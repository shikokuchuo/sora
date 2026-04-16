#ifndef SORA_H
#define SORA_H

#include <Rversion.h>
#include <Rinternals.h>
#include <R_ext/Altrep.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- Types --- */

typedef struct sora_shm_s {
  void *addr;
  size_t size;
  char name[48];
#ifdef _WIN32
  void *handle;
#endif
} sora_shm;

typedef struct sora_buf_s {
  unsigned char *buf;
  size_t len;
  size_t cur;
} sora_buf;

typedef struct sora_vec_s {
  const void *data;
  R_xlen_t length;
  int32_t index;   /* -1 = standalone, >= 0 = element of ALTLIST */
} sora_vec;

/* --- shm.c --- */

int sora_shm_create(sora_shm *shm, size_t size);
int sora_shm_open(sora_shm *shm, const char *name);
void sora_shm_close(sora_shm *shm, int unlink);
void sora_shm_finalizer(SEXP ptr);
void sora_host_finalizer(SEXP ptr);

/* --- serialize.c --- */

size_t sora_serialize_count(SEXP object);
void sora_serialize_into(unsigned char *dst, size_t size, SEXP object);
SEXP sora_unserialize_from(unsigned char *src, size_t size);
size_t sora_sizeof_elt(int type);

/* --- altrep.c --- */

void sora_altrep_init(DllInfo *dll);

/* --- Alignment macro --- */

#define SORA_ALIGN64(x) (((x) + 63) & ~(size_t)63)

#endif /* SORA_H */
