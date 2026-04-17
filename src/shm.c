#include <stdlib.h>
#include <stdio.h>
#include "mori.h"

// Platform-specific SHM implementations --------------------------------------

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static void mori_shm_name(char *name, size_t size) {
  static unsigned int counter = 0;
  snprintf(name, size, "Local\\mori_%lx_%x",
           (unsigned long) GetCurrentProcessId(), counter++);
}

int mori_shm_create(mori_shm *shm, size_t size) {

  shm->addr = NULL;
  shm->size = 0;
  shm->handle = NULL;
  mori_shm_name(shm->name, sizeof(shm->name));

  DWORD hi = (DWORD) ((uint64_t) size >> 32);
  DWORD lo = (DWORD) (size & 0xFFFFFFFF);

  HANDLE h = CreateFileMappingA(
    INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, hi, lo, shm->name
  );
  if (!h) return -1;

  void *addr = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!addr) {
    CloseHandle(h);
    return -1;
  }

  shm->addr = addr;
  shm->size = size;
  shm->handle = h;
  return 0;
}

int mori_shm_open(mori_shm *shm, const char *name) {

  shm->addr = NULL;
  shm->size = 0;
  shm->handle = NULL;
  strncpy(shm->name, name, sizeof(shm->name) - 1);
  shm->name[sizeof(shm->name) - 1] = '\0';

  HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
  if (!h) return -1;

  void *addr = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
  if (!addr) {
    CloseHandle(h);
    return -1;
  }

  MEMORY_BASIC_INFORMATION mbi;
  VirtualQuery(addr, &mbi, sizeof(mbi));

  shm->addr = addr;
  shm->size = mbi.RegionSize;
  shm->handle = h;
  return 0;
}

void mori_shm_close(mori_shm *shm, int unlink) {
  (void) unlink;
  if (shm->addr) UnmapViewOfFile(shm->addr);
  if (shm->handle) CloseHandle(shm->handle);
  shm->addr = NULL;
  shm->handle = NULL;
}

#else /* POSIX */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

/* Linux: go through /dev/shm directly to avoid the -lrt link dependency
   that shm_open/shm_unlink would introduce. macOS has them in libc. */

#ifdef __linux__

static int mori_shm_os_open(const char *name, int flags, mode_t mode) {
  char path[64];
  snprintf(path, sizeof(path), "/dev/shm%s", name);
  return open(path, flags, mode);
}

static int mori_shm_os_unlink(const char *name) {
  char path[64];
  snprintf(path, sizeof(path), "/dev/shm%s", name);
  return unlink(path);
}

#else /* macOS / other POSIX */

static int mori_shm_os_open(const char *name, int flags, mode_t mode) {
  return shm_open(name, flags, mode);
}

static int mori_shm_os_unlink(const char *name) {
  return shm_unlink(name);
}

#endif

static void mori_shm_name(char *name, size_t size) {
  static unsigned int counter = 0;
  snprintf(name, size, "/mori_%x_%x", (unsigned) getpid(), counter++);
}

int mori_shm_create(mori_shm *shm, size_t size) {

  shm->addr = NULL;
  shm->size = 0;
  mori_shm_name(shm->name, sizeof(shm->name));

  int fd = mori_shm_os_open(shm->name, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) return -1;

  if (ftruncate(fd, (off_t) size) != 0) {
    close(fd);
    mori_shm_os_unlink(shm->name);
    return -1;
  }

  void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, fd, 0);
  if (addr == MAP_FAILED) {
    close(fd);
    mori_shm_os_unlink(shm->name);
    return -1;
  }

  close(fd);

#ifdef MADV_HUGEPAGE
  if (size >= 2 * 1024 * 1024)
    madvise(addr, size, MADV_HUGEPAGE);
#elif defined(__APPLE__)
  madvise(addr, size, MADV_WILLNEED);
#endif

  shm->addr = addr;
  shm->size = size;
  return 0;
}

int mori_shm_open(mori_shm *shm, const char *name) {

  shm->addr = NULL;
  shm->size = 0;
  strncpy(shm->name, name, sizeof(shm->name) - 1);
  shm->name[sizeof(shm->name) - 1] = '\0';

  int fd = mori_shm_os_open(name, O_RDONLY, 0);
  if (fd < 0) return -1;

  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return -1;
  }
  size_t size = (size_t) st.st_size;

  void *addr = mmap(NULL, size, PROT_READ,
                     MAP_SHARED | MAP_POPULATE, fd, 0);
  if (addr == MAP_FAILED) {
    close(fd);
    return -1;
  }

  close(fd);

#ifdef MADV_HUGEPAGE
  if (size >= 2 * 1024 * 1024)
    madvise(addr, size, MADV_HUGEPAGE);
#elif defined(__APPLE__)
  madvise(addr, size, MADV_WILLNEED);
#endif

  shm->addr = addr;
  shm->size = size;
  return 0;
}

void mori_shm_close(mori_shm *shm, int unlink) {
  if (shm->addr) munmap(shm->addr, shm->size);
  if (unlink) mori_shm_os_unlink(shm->name);
  shm->addr = NULL;
}

#endif /* _WIN32 */

// Platform-independent finalizers --------------------------------------------

/* Daemon-side finalizer: unmap only, don't unlink (host manages lifetime) */
void mori_shm_finalizer(SEXP ptr) {
  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(ptr);
  if (shm) {
    mori_shm_close(shm, 0);
    free(shm);
    R_ClearExternalPtr(ptr);
  }
}

/* Host-side finalizer: releases the SHM name/handle */
void mori_host_finalizer(SEXP ptr) {
  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(ptr);
  if (shm) {
#ifdef _WIN32
    if (shm->handle) CloseHandle(shm->handle);
#else
    if (shm->name[0]) mori_shm_os_unlink(shm->name);
#endif
    free(shm);
    R_ClearExternalPtr(ptr);
  }
}
