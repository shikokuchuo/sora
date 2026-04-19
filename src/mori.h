#ifndef MORI_H
#define MORI_H

#include <Rversion.h>
#include <Rinternals.h>
#include <R_ext/Altrep.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Types -----------------------------------------------------------------------

typedef struct mori_shm_s {
  void *addr;
  size_t size;
  char name[48];
#ifdef _WIN32
  void *handle;
#endif
} mori_shm;

typedef struct mori_buf_s {
  unsigned char *buf;
  size_t len;
  size_t cur;
} mori_buf;

typedef struct mori_vec_s {
  const void *data;
  R_xlen_t length;
  int32_t index;   /* -1 = standalone, >= 0 = element of ALTLIST */
} mori_vec;

typedef struct mori_list_view_s {
  unsigned char *base;       /* points to child MORL start */
  int64_t region_size;       /* bounds all reads within this region */
  int32_t n_elements;
  int32_t index;             /* -1 = root, >= 0 = sub-list */
} mori_list_view;

// shm.c -----------------------------------------------------------------------

int mori_shm_create(mori_shm *shm, size_t size);
int mori_shm_open(mori_shm *shm, const char *name);
void mori_shm_close(mori_shm *shm, int unlink);
mori_shm *mori_shm_create_heap(size_t size);
mori_shm *mori_shm_open_heap(const char *name);
void mori_shm_finalizer(SEXP ptr);
void mori_host_finalizer(SEXP ptr);

// serialize.c -----------------------------------------------------------------

size_t mori_serialize_count(SEXP object);
void mori_serialize_into(unsigned char *dst, size_t size, SEXP object);
SEXP mori_unserialize_from(unsigned char *src, size_t size);

static inline size_t mori_sizeof_elt(int type) {
  switch (type) {
  case REALSXP:  return sizeof(double);
  case INTSXP:   return sizeof(int);
  case LGLSXP:   return sizeof(int);
  case RAWSXP:   return 1;
  case CPLXSXP:  return sizeof(Rcomplex);
  default:       return 0;
  }
}

// altrep.c --------------------------------------------------------------------

void mori_altrep_init(DllInfo *dll);

// Alignment macro -------------------------------------------------------------

#define MORI_ALIGN64(x) (((x) + 63) & ~(size_t)63)

#endif /* MORI_H */
