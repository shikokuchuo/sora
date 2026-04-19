#include <stdlib.h>
#include "mori.h"

// Global ALTREP class handles and sentinel ------------------------------------

static R_altrep_class_t mori_list_class;
static R_altrep_class_t mori_real_class;
static R_altrep_class_t mori_integer_class;
static R_altrep_class_t mori_logical_class;
static R_altrep_class_t mori_raw_class;
static R_altrep_class_t mori_complex_class;
static R_altrep_class_t mori_string_class;
static SEXP mori_tag;        /* ALTLIST cache sentinel (VECSXP marker, not an extptr tag) */
static SEXP mori_shm_tag;    /* tag on SHM mapping extptrs (addr is mori_shm *) */
static SEXP mori_host_tag;   /* tag on host-only unlink extptrs (addr is mori_shm *) */
static SEXP mori_owned_tag;  /* tag on every mori ALTREP data1 extptr; addr type dispatched via TYPEOF(x) */

// Element directory (for list SHM layout) -------------------------------------

typedef struct {
  int64_t data_offset;
  int64_t data_size;
  int32_t sexptype;
  int32_t attrs_size;
  int64_t length;
} mori_elem;

// SHM eligibility: any atomic vector (attributes stored separately) ---------

static inline int mori_shm_eligible(int type) {
  return type == REALSXP || type == INTSXP || type == LGLSXP ||
         type == RAWSXP || type == CPLXSXP || type == STRSXP;
}

// Type-dispatched data pointer (avoids non-API DATAPTR) -----------------------

static inline void *mori_data_ptr(SEXP x) {
  switch (TYPEOF(x)) {
  case REALSXP:  return (void *) REAL(x);
  case INTSXP:   return (void *) INTEGER(x);
  case LGLSXP:   return (void *) LOGICAL(x);
  case RAWSXP:   return (void *) RAW(x);
  case CPLXSXP:  return (void *) COMPLEX(x);
  default:       return (void *) DATAPTR_RO(x);
  }
}

// Attribute helpers for API compliance ----------------------------------------

/* Named list on R >= 4.6.0, pairlist otherwise. R_NilValue if none.
   Caller must PROTECT. */
static inline SEXP mori_get_attrs_for_serialize(SEXP x) {
#if R_VERSION >= R_Version(4, 6, 0)
  return ANY_ATTRIB(x) ? R_getAttributes(x) : R_NilValue;
#else
  return ATTRIB(x);
#endif
}

/* Sets class last to avoid validation ordering issues. */
static void mori_set_attrs_from(SEXP result, SEXP attrs) {
#if R_VERSION >= R_Version(4, 6, 0)
  SEXP names = PROTECT(Rf_getAttrib(attrs, R_NamesSymbol));
  R_xlen_t n = XLENGTH(attrs), class_idx = -1;
  for (R_xlen_t i = 0; i < n; i++) {
    SEXP nm = Rf_installChar(STRING_ELT(names, i));
    if (nm == R_ClassSymbol) { class_idx = i; continue; }
    Rf_setAttrib(result, nm, VECTOR_ELT(attrs, i));
  }
  if (class_idx >= 0)
    Rf_classgets(result, VECTOR_ELT(attrs, class_idx));
  UNPROTECT(1);
#else
  SET_ATTRIB(result, attrs);
  if (Rf_getAttrib(result, R_ClassSymbol) != R_NilValue)
    SET_OBJECT(result, 1);
#endif
}

static inline void mori_restore_attrs(SEXP result, unsigned char *buf,
                                      size_t size) {
  SEXP attrs = PROTECT(mori_unserialize_from(buf, size));
  mori_set_attrs_from(result, attrs);
  UNPROTECT(1);
}

// SHM name validation for unserialize dispatch --------------------------------

static inline int mori_is_shm_name(const char *s) {
#ifdef _WIN32
  return strncmp(s, "Local\\mori_", 11) == 0;
#else
  return s[0] == '/' && strncmp(s + 1, "mori_", 5) == 0;
#endif
}

// Generic finalizer for mori_owned_tag extptrs (vec / str / view) ------------

static void mori_owned_finalizer(SEXP ptr) {
  void *v = R_ExternalPtrAddr(ptr);
  if (v) {
    free(v);
    R_ClearExternalPtr(ptr);
  }
}

// ALTREP atomic vector methods (shared by all 5 types) ------------------------

/*
 * Layout:
 *   data1 = extptr (tag = mori_owned_tag, addr = mori_vec *)
 *           protected value keeps the parent SHM extptr alive (shm_tag for
 *           standalone, parent view for element within an ALTLIST)
 *   data2 = R_NilValue while SHM-backed; regular SEXP after materialization
 */

static R_xlen_t mori_vec_Length(SEXP x) {
  if (R_altrep_data2(x) != R_NilValue)
    return XLENGTH(R_altrep_data2(x));
  mori_vec *v = (mori_vec *) R_ExternalPtrAddr(R_altrep_data1(x));
  return v->length;
}

static const void *mori_vec_Dataptr_or_null(SEXP x) {
  if (R_altrep_data2(x) != R_NilValue)
    return DATAPTR_RO(R_altrep_data2(x));
  mori_vec *v = (mori_vec *) R_ExternalPtrAddr(R_altrep_data1(x));
  return v->data;
}

static void *mori_vec_Dataptr(SEXP x, Rboolean writable) {

  if (R_altrep_data2(x) != R_NilValue)
    return mori_data_ptr(R_altrep_data2(x));

  mori_vec *v = (mori_vec *) R_ExternalPtrAddr(R_altrep_data1(x));

  if (!writable)
    return (void *) v->data;

  /* COW: materialize to a regular R vector */
  R_xlen_t n = v->length;
  int type = TYPEOF(x);
  SEXP mat = PROTECT(Rf_allocVector(type, n));
  memcpy(mori_data_ptr(mat), v->data, (size_t) n * mori_sizeof_elt(type));
  DUPLICATE_ATTRIB(mat, x);
  R_set_altrep_data2(x, mat);
  UNPROTECT(1);

  return mori_data_ptr(mat);
}

/* keeper: SEXP kept alive via the extptr's protected slot (parent SHM). */
static SEXP mori_make_vector(const void *data, R_xlen_t length,
                             int sexptype, SEXP keeper) {

  R_altrep_class_t cls;
  switch (sexptype) {
  case REALSXP:  cls = mori_real_class;    break;
  case INTSXP:   cls = mori_integer_class; break;
  case LGLSXP:   cls = mori_logical_class; break;
  case RAWSXP:   cls = mori_raw_class;     break;
  case CPLXSXP:  cls = mori_complex_class; break;
  default:       Rf_error("mori:unsupported ALTREP type %d", sexptype);
  }

  mori_vec *v = (mori_vec *) malloc(sizeof(mori_vec));
  if (!v) Rf_error("mori:allocation failure");
  v->data = data;
  v->length = length;
  v->index = -1;

  SEXP ptr = PROTECT(R_MakeExternalPtr(v, mori_owned_tag, keeper));
  R_RegisterCFinalizerEx(ptr, mori_owned_finalizer, TRUE);

  SEXP result = R_new_altrep(cls, ptr, R_NilValue);
  UNPROTECT(1);
  return result;
}

// ALTSTRING methods -----------------------------------------------------------

/*
 * String entry in offset table (16 bytes per string):
 *   str_offset(int64) + str_length(int32) + str_encoding(int32)
 * str_length < 0 means NA_STRING.
 *
 * Layout:
 *   data1 = extptr (tag = mori_owned_tag, addr = mori_str *)
 *           protected value keeps the parent SHM extptr alive (shm_tag for
 *           standalone, parent view for element within an ALTLIST)
 *   data2 = R_NilValue while SHM-backed; regular STRSXP after materialization
 */

typedef struct {
  const unsigned char *table;
  const unsigned char *data;
  R_xlen_t length;
  int32_t index;   /* -1 = standalone, >= 0 = element of ALTLIST */
} mori_str;

static inline SEXP mori_string_elt_shm(mori_str *s, R_xlen_t i) {
  const unsigned char *entry = s->table + 16 * (size_t) i;
  int64_t str_offset;
  int32_t str_length, str_encoding;
  memcpy(&str_offset, entry, 8);
  memcpy(&str_length, entry + 8, 4);
  memcpy(&str_encoding, entry + 12, 4);

  if (str_length < 0) return NA_STRING;

  return Rf_mkCharLenCE((const char *) (s->data + str_offset),
                        str_length, (cetype_t) str_encoding);
}

static R_xlen_t mori_string_Length(SEXP x) {
  if (R_altrep_data2(x) != R_NilValue)
    return XLENGTH(R_altrep_data2(x));
  mori_str *s = (mori_str *) R_ExternalPtrAddr(R_altrep_data1(x));
  return s->length;
}

static SEXP mori_string_Elt(SEXP x, R_xlen_t i) {
  if (R_altrep_data2(x) != R_NilValue)
    return STRING_ELT(R_altrep_data2(x), i);
  mori_str *s = (mori_str *) R_ExternalPtrAddr(R_altrep_data1(x));
  return mori_string_elt_shm(s, i);
}

static const void *mori_string_Dataptr_or_null(SEXP x) {
  if (R_altrep_data2(x) != R_NilValue)
    return DATAPTR_RO(R_altrep_data2(x));
  return NULL;
}

static void *mori_string_Dataptr(SEXP x, Rboolean writable) {
  if (R_altrep_data2(x) != R_NilValue)
    return mori_data_ptr(R_altrep_data2(x));

  mori_str *s = (mori_str *) R_ExternalPtrAddr(R_altrep_data1(x));
  R_xlen_t n = s->length;
  SEXP mat = PROTECT(Rf_allocVector(STRSXP, n));
  for (R_xlen_t i = 0; i < n; i++)
    SET_STRING_ELT(mat, i, mori_string_elt_shm(s, i));
  R_set_altrep_data2(x, mat);
  UNPROTECT(1);

  return mori_data_ptr(mat);
}

static SEXP mori_string_Duplicate(SEXP x, Rboolean deep) {
  (void) deep;
  R_xlen_t n = XLENGTH(x);
  SEXP result = PROTECT(Rf_allocVector(STRSXP, n));
  for (R_xlen_t i = 0; i < n; i++)
    SET_STRING_ELT(result, i, STRING_ELT(x, i));
  DUPLICATE_ATTRIB(result, x);
  UNPROTECT(1);
  return result;
}

/* region_base: points to the offset table.
   keeper: SEXP kept alive via the extptr's protected slot (parent SHM). */
static SEXP mori_make_string(const unsigned char *region_base,
                             R_xlen_t n, SEXP keeper) {

  mori_str *s = (mori_str *) malloc(sizeof(mori_str));
  if (!s) Rf_error("mori:allocation failure");

  size_t table_size = 16 * (size_t) n;
  s->table = region_base;
  s->data = region_base + MORI_ALIGN64(table_size);
  s->length = n;
  s->index = -1;

  SEXP ptr = PROTECT(R_MakeExternalPtr(s, mori_owned_tag, keeper));
  R_RegisterCFinalizerEx(ptr, mori_owned_finalizer, TRUE);

  SEXP result = R_new_altrep(mori_string_class, ptr, R_NilValue);
  UNPROTECT(1);
  return result;
}

// Forward declarations --------------------------------------------------------

static SEXP mori_make_list_view(unsigned char *base, int64_t region_size,
                                int32_t index, SEXP keeper);

// Element extraction helper (shared by list Elt and open_path) ----------------

static SEXP mori_unwrap_element(unsigned char *base, int64_t region_size,
                                int32_t index, SEXP keeper) {

  unsigned char *dir = base + 24 + 32 * (size_t) index;
  int64_t data_offset, data_size, length;
  int32_t sexptype, attrs_size;
  memcpy(&data_offset, dir, 8);
  memcpy(&data_size, dir + 8, 8);
  memcpy(&sexptype, dir + 16, 4);
  memcpy(&attrs_size, dir + 20, 4);
  memcpy(&length, dir + 24, 8);

  /* Bounds-check the data region against the enclosing region */
  if (data_offset < 0 || data_size < 0 ||
      data_offset + data_size > region_size)
    Rf_error("mori:element data out of bounds");
  if (attrs_size > 0 && attrs_size > data_size)
    Rf_error("mori:element attrs size larger than data");

  SEXP result;
  if (sexptype == VECSXP) {
    /* Nested MORL at base+data_offset; attrs live inside child region */
    result = PROTECT(mori_make_list_view(
      base + data_offset, data_size, index, keeper
    ));
  } else if (sexptype == STRSXP) {
    result = PROTECT(mori_make_string(
      base + data_offset, (R_xlen_t) length, keeper
    ));
    ((mori_str *) R_ExternalPtrAddr(R_altrep_data1(result)))->index = index;
  } else if (sexptype != 0) {
    result = PROTECT(mori_make_vector(
      base + data_offset, (R_xlen_t) length, sexptype, keeper
    ));
    ((mori_vec *) R_ExternalPtrAddr(R_altrep_data1(result)))->index = index;
  } else {
    result = PROTECT(mori_unserialize_from(
      base + (size_t) data_offset, (size_t) data_size
    ));
  }

  if (attrs_size > 0 && sexptype != 0 && sexptype != VECSXP) {
    size_t attrs_off = (size_t)(data_offset + data_size - attrs_size);
    mori_restore_attrs(result, base + attrs_off, (size_t) attrs_size);
  }

  UNPROTECT(1);
  return result;
}

// ALTLIST methods -------------------------------------------------------------

/*
 * Layout:
 *   data1 = extptr (tag = mori_owned_tag, addr = mori_list_view *)
 *           prot: parent extptr (shm_tag for root; parent view for sub-list)
 *   data2 = R_NilValue or cache VECSXP (length n, initialized to mori_tag)
 *
 * MORL region layout (same whether root SHM or nested inside a parent):
 *   Bytes 0-3:   uint32_t magic (0x4D4F524C "MORL")
 *   Bytes 4-7:   int32_t  n_elements
 *   Bytes 8-15:  int64_t  attrs_offset
 *   Bytes 16-23: int64_t  attrs_size
 *   Byte 24+:    element directory (32 bytes per element)
 */

/* keeper: parent extptr (shm_tag for root, owned_tag for sub-list).
   Validates the MORL header before touching it so corrupt input errors
   cleanly rather than SEGV. */
static SEXP mori_make_list_view(unsigned char *base, int64_t region_size,
                                int32_t index, SEXP keeper) {

  if (region_size < 24)
    Rf_error("mori:nested list region too small");

  uint32_t magic;
  memcpy(&magic, base, 4);
  if (magic != 0x4D4F524Cu)
    Rf_error("mori:invalid nested list magic bytes");

  int32_t n;
  int64_t attrs_offset, attrs_size;
  memcpy(&n, base + 4, 4);
  memcpy(&attrs_offset, base + 8, 8);
  memcpy(&attrs_size, base + 16, 8);

  if (n < 0 || (int64_t) 24 + (int64_t) 32 * n > region_size)
    Rf_error("mori:nested list directory out of bounds");
  if (attrs_offset < 0 || attrs_size < 0 ||
      attrs_offset + attrs_size > region_size)
    Rf_error("mori:nested list attrs out of bounds");

  mori_list_view *v = (mori_list_view *) malloc(sizeof(mori_list_view));
  if (!v) Rf_error("mori:allocation failure");
  v->base = base;
  v->region_size = region_size;
  v->n_elements = n;
  v->index = index;

  SEXP ptr = PROTECT(R_MakeExternalPtr(v, mori_owned_tag, keeper));
  R_RegisterCFinalizerEx(ptr, mori_owned_finalizer, TRUE);

  /* Cache is allocated lazily on first Elt access */
  SEXP result = PROTECT(R_new_altrep(mori_list_class, ptr, R_NilValue));

  if (attrs_size > 0)
    mori_restore_attrs(result, base + (size_t) attrs_offset,
                       (size_t) attrs_size);

  UNPROTECT(2);
  return result;
}

static R_xlen_t mori_list_Length(SEXP x) {
  mori_list_view *v = (mori_list_view *) R_ExternalPtrAddr(R_altrep_data1(x));
  return v != NULL ? v->n_elements : 0;
}

static SEXP mori_list_Elt(SEXP x, R_xlen_t i) {

  SEXP d1 = R_altrep_data1(x);
  mori_list_view *view = (mori_list_view *) R_ExternalPtrAddr(d1);

  /* Lazy cache allocation */
  SEXP cache = R_altrep_data2(x);
  if (cache == R_NilValue) {
    cache = PROTECT(Rf_allocVector(VECSXP, view->n_elements));
    for (int32_t k = 0; k < view->n_elements; k++)
      SET_VECTOR_ELT(cache, k, mori_tag);
    R_set_altrep_data2(x, cache);
    UNPROTECT(1);
  }

  SEXP cached = VECTOR_ELT(cache, i);
  if (cached != mori_tag) return cached;

  SEXP result = mori_unwrap_element(view->base, view->region_size,
                                    (int32_t) i, d1);
  SET_VECTOR_ELT(cache, i, result);
  return result;
}

/* NULL forces R to use Elt() for element access */
static const void *mori_list_Dataptr_or_null(SEXP x) {
  return NULL;
}

/* Full materialization fallback */
static void *mori_list_Dataptr(SEXP x, Rboolean writable) {
  R_xlen_t n = mori_list_Length(x);
  for (R_xlen_t i = 0; i < n; i++)
    mori_list_Elt(x, i);
  if (R_altrep_data2(x) == R_NilValue) {
    SEXP cache = PROTECT(Rf_allocVector(VECSXP, n));
    R_set_altrep_data2(x, cache);
    UNPROTECT(1);
  }
  return mori_data_ptr(R_altrep_data2(x));
}

/* COW: modification produces a regular list */
static SEXP mori_list_Duplicate(SEXP x, Rboolean deep) {
  R_xlen_t n = mori_list_Length(x);
  SEXP result = PROTECT(Rf_allocVector(VECSXP, n));
  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = mori_list_Elt(x, i);
    SET_VECTOR_ELT(result, i, deep ? Rf_duplicate(elt) : elt);
  }
  DUPLICATE_ATTRIB(result, x);
  UNPROTECT(1);
  return result;
}

static SEXP mori_dispatch_by_magic(SEXP shm_ptr, const char *err_name);

// SHM keeper wrappers: transfer heap mori_shm ownership to R -----------------

/* Consumer-side: single shm_tag extptr with munmap-only finalizer. Takes
   ownership of heap; the returned SEXP's finalizer frees it on GC. */
static SEXP mori_wrap_shm_consumer(mori_shm *heap) {
  SEXP ptr = R_MakeExternalPtr(heap, mori_shm_tag, R_NilValue);
  R_RegisterCFinalizerEx(ptr, mori_shm_finalizer, TRUE);
  return ptr;
}

/* Producer-side: shm_tag extptr (munmap finalizer) chained to a host_tag
   extptr (unlink / CloseHandle finalizer) via its protected slot. Takes
   ownership of heap. The host copy gets the name / Windows handle; heap
   keeps only the mapping, so munmap and unlink fire independently. */
static SEXP mori_wrap_shm_producer(mori_shm *heap) {

  mori_shm *host = (mori_shm *) malloc(sizeof(mori_shm));
  if (!host) Rf_error("mori:allocation failure");
  memcpy(host, heap, sizeof(mori_shm));
  host->addr = NULL;
  host->size = 0;
#ifdef _WIN32
  heap->handle = NULL;
#endif

  SEXP host_ptr = PROTECT(R_MakeExternalPtr(host, mori_host_tag, R_NilValue));
  R_RegisterCFinalizerEx(host_ptr, mori_host_finalizer, TRUE);

  SEXP shm_ptr = R_MakeExternalPtr(heap, mori_shm_tag, host_ptr);
  R_RegisterCFinalizerEx(shm_ptr, mori_shm_finalizer, TRUE);

  UNPROTECT(1);
  return shm_ptr;
}

static SEXP mori_make_result(mori_shm *heap) {
  SEXP shm_ptr = PROTECT(mori_wrap_shm_producer(heap));
  SEXP result = mori_dispatch_by_magic(shm_ptr, NULL);
  UNPROTECT(1);
  return result;
}

// String write helper (shared by standalone and list paths) -------------------

/* Returns total bytes written (including alignment padding). */
static size_t mori_write_strings(unsigned char *dest, SEXP x) {
  R_xlen_t n = XLENGTH(x);
  size_t table_size = 16 * (size_t) n;
  size_t data_start = MORI_ALIGN64(table_size);

  /* Zero-fill alignment gap */
  if (data_start > table_size)
    memset(dest + table_size, 0, data_start - table_size);

  size_t cur = 0;
  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = STRING_ELT(x, i);
    unsigned char *tbl = dest + 16 * (size_t) i;

    if (elt == NA_STRING) {
      int64_t off = 0;
      int32_t len = -1, enc = 0;
      memcpy(tbl, &off, 8);
      memcpy(tbl + 8, &len, 4);
      memcpy(tbl + 12, &enc, 4);
    } else {
      int32_t slen = (int32_t) LENGTH(elt);
      int64_t off = (int64_t) cur;
      int32_t enc = (int32_t) Rf_getCharCE(elt);
      memcpy(tbl, &off, 8);
      memcpy(tbl + 8, &slen, 4);
      memcpy(tbl + 12, &enc, 4);
      memcpy(dest + data_start + cur, CHAR(elt), (size_t) slen);
      cur += (size_t) slen;
    }
  }

  return data_start + cur;
}

static size_t mori_string_data_size(SEXP x) {
  R_xlen_t n = XLENGTH(x);
  size_t table_size = 16 * (size_t) n;
  size_t str_bytes = 0;
  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = STRING_ELT(x, i);
    if (elt != NA_STRING)
      str_bytes += (size_t) LENGTH(elt);
  }
  return MORI_ALIGN64(table_size) + str_bytes;
}

// .Call entry points: host-side SHM creation ---------------------------------

// Recursive size/write helpers for nested list regions -----------------------

static size_t mori_nested_size(SEXP x);
static size_t mori_nested_write(unsigned char *base, SEXP x);

/* Total bytes occupied by a MORL region for VECSXP x, including header,
   directory, elements (recursing into VECSXP/LISTSXP children), trailing
   attrs, and all 64-byte alignment padding. Caller passes a VECSXP; any
   LISTSXP children are coerced locally during recursion. */
static size_t mori_nested_size(SEXP x) {

  R_xlen_t n = XLENGTH(x);
  size_t total = MORI_ALIGN64(24 + 32 * (size_t) n);

  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = VECTOR_ELT(x, i);
    int type = TYPEOF(elt);
    size_t elt_size;

    if (type == LISTSXP || type == VECSXP) {
      SEXP coerced = (type == LISTSXP) ? Rf_coerceVector(elt, VECSXP) : elt;
      PROTECT(coerced);
      elt_size = mori_nested_size(coerced);
      UNPROTECT(1);
    } else if (mori_shm_eligible(type)) {
      size_t raw_size = (type == STRSXP) ?
        mori_string_data_size(elt) :
        (size_t) XLENGTH(elt) * mori_sizeof_elt(type);
      SEXP elt_attrs = PROTECT(mori_get_attrs_for_serialize(elt));
      size_t attrs_size = (elt_attrs != R_NilValue) ?
        mori_serialize_count(elt_attrs) : 0;
      UNPROTECT(1);
      elt_size = raw_size + attrs_size;
    } else {
      elt_size = mori_serialize_count(elt);
    }

    total += MORI_ALIGN64(elt_size);
  }

  SEXP attrs = PROTECT(mori_get_attrs_for_serialize(x));
  size_t attrs_size = (attrs != R_NilValue) ?
    mori_serialize_count(attrs) : 0;
  UNPROTECT(1);
  total += MORI_ALIGN64(attrs_size);

  return total;
}

/* Writes a complete MORL region for VECSXP x starting at base. Returns
   total bytes written (must equal mori_nested_size(x)). */
static size_t mori_nested_write(unsigned char *base, SEXP x) {

  R_xlen_t n = XLENGTH(x);
  size_t cur = MORI_ALIGN64(24 + 32 * (size_t) n);

  mori_elem *elems = (n > 0) ?
    (mori_elem *) R_alloc((size_t) n, sizeof(mori_elem)) : NULL;

  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = VECTOR_ELT(x, i);
    int type = TYPEOF(elt);

    elems[i].data_offset = (int64_t) cur;

    if (type == LISTSXP || type == VECSXP) {
      SEXP coerced = (type == LISTSXP) ? Rf_coerceVector(elt, VECSXP) : elt;
      PROTECT(coerced);
      size_t written = mori_nested_write(base + cur, coerced);
      elems[i].sexptype = VECSXP;
      elems[i].attrs_size = 0;
      elems[i].length = (int64_t) XLENGTH(coerced);
      elems[i].data_size = (int64_t) written;
      UNPROTECT(1);
      cur += MORI_ALIGN64(written);
    } else if (mori_shm_eligible(type)) {
      size_t raw_size = (type == STRSXP) ?
        mori_string_data_size(elt) :
        (size_t) XLENGTH(elt) * mori_sizeof_elt(type);

      SEXP elt_attrs = PROTECT(mori_get_attrs_for_serialize(elt));
      size_t attrs_size = (elt_attrs != R_NilValue) ?
        mori_serialize_count(elt_attrs) : 0;

      elems[i].sexptype = type;
      elems[i].attrs_size = (int32_t) attrs_size;
      elems[i].length = (int64_t) XLENGTH(elt);
      elems[i].data_size = (int64_t) (raw_size + attrs_size);

      if (type == STRSXP) {
        size_t written = mori_write_strings(base + cur, elt);
        if (attrs_size > 0)
          mori_serialize_into(base + cur + written, attrs_size, elt_attrs);
      } else {
        memcpy(base + cur, DATAPTR_RO(elt), raw_size);
        if (attrs_size > 0)
          mori_serialize_into(base + cur + raw_size, attrs_size, elt_attrs);
      }

      UNPROTECT(1);
      cur += MORI_ALIGN64((size_t) elems[i].data_size);
    } else {
      size_t elt_size = mori_serialize_count(elt);
      mori_serialize_into(base + cur, elt_size, elt);
      elems[i].sexptype = 0;
      elems[i].attrs_size = 0;
      elems[i].length = 0;
      elems[i].data_size = (int64_t) elt_size;
      cur += MORI_ALIGN64(elt_size);
    }
  }

  SEXP list_attrs = PROTECT(mori_get_attrs_for_serialize(x));
  size_t attrs_size = (list_attrs != R_NilValue) ?
    mori_serialize_count(list_attrs) : 0;
  int64_t attrs_offset = (int64_t) cur;
  if (attrs_size > 0)
    mori_serialize_into(base + cur, attrs_size, list_attrs);
  cur += MORI_ALIGN64(attrs_size);
  UNPROTECT(1);

  /* Write header */
  uint32_t magic = 0x4D4F524Cu;
  int32_t n32 = (int32_t) n;
  int64_t as64 = (int64_t) attrs_size;
  memcpy(base, &magic, 4);
  memcpy(base + 4, &n32, 4);
  memcpy(base + 8, &attrs_offset, 8);
  memcpy(base + 16, &as64, 8);

  /* Write directory */
  for (R_xlen_t i = 0; i < n; i++) {
    unsigned char *d = base + 24 + 32 * (size_t) i;
    memcpy(d, &elems[i].data_offset, 8);
    memcpy(d + 8, &elems[i].data_size, 8);
    memcpy(d + 16, &elems[i].sexptype, 4);
    memcpy(d + 20, &elems[i].attrs_size, 4);
    memcpy(d + 24, &elems[i].length, 8);
  }

  return cur;
}

/* Write list/data frame to SHM (with transparent nested VECSXP) */
static SEXP mori_shm_create_list_call(SEXP x) {

  /* Coerce top-level pairlists to VECSXP so VECTOR_ELT/XLENGTH work */
  if (TYPEOF(x) == LISTSXP) {
    x = PROTECT(Rf_coerceVector(x, VECSXP));
  } else {
    PROTECT(x);
  }

  size_t total = mori_nested_size(x);

  mori_shm *shm = mori_shm_create_heap(total);
  if (!shm)
    Rf_error("mori:failed to create shared memory");

  mori_nested_write((unsigned char *) shm->addr, x);

  UNPROTECT(1);
  return mori_make_result(shm);
}

/* Write atomic vector to SHM: 64-byte header + data (64-byte aligned) + attrs */
static SEXP mori_shm_create_vector_call(SEXP x) {

  int type = TYPEOF(x);
  R_xlen_t n = XLENGTH(x);
  size_t elt_size = mori_sizeof_elt(type);
  size_t data_size = (size_t) n * elt_size;

  SEXP attrs = PROTECT(mori_get_attrs_for_serialize(x));
  size_t attrs_size = (attrs != R_NilValue) ? mori_serialize_count(attrs) : 0;
  size_t total = 64 + data_size + attrs_size;

  mori_shm *shm = mori_shm_create_heap(total);
  if (!shm)
    Rf_error("mori:failed to create shared memory");

  unsigned char *base = (unsigned char *) shm->addr;

  /* Zero-fill header, then write fields */
  memset(base, 0, 64);
  uint32_t magic = 0x4D4F5248u;
  int32_t sexptype = (int32_t) type;
  int64_t length = (int64_t) n;
  int64_t as64 = (int64_t) attrs_size;
  memcpy(base, &magic, 4);
  memcpy(base + 4, &sexptype, 4);
  memcpy(base + 8, &length, 8);
  memcpy(base + 16, &as64, 8);

  memcpy(base + 64, DATAPTR_RO(x), data_size);

  if (attrs_size > 0)
    mori_serialize_into(base + 64 + data_size, attrs_size, attrs);

  UNPROTECT(1);
  return mori_make_result(shm);
}

/* Write character vector to SHM: 24-byte header + offset table + strings + attrs */
static SEXP mori_shm_create_string_call(SEXP x) {

  R_xlen_t n = XLENGTH(x);
  size_t header_size = 24;
  size_t str_size = mori_string_data_size(x);

  SEXP attrs = PROTECT(mori_get_attrs_for_serialize(x));
  size_t attrs_size = (attrs != R_NilValue) ? mori_serialize_count(attrs) : 0;
  size_t total = header_size + str_size + attrs_size;

  mori_shm *shm = mori_shm_create_heap(total);
  if (!shm)
    Rf_error("mori:failed to create shared memory");

  unsigned char *base = (unsigned char *) shm->addr;

  /* Write header */
  memset(base, 0, header_size);
  uint32_t magic = 0x4D4F5253u;
  int32_t as32 = (int32_t) attrs_size;
  int64_t n64 = (int64_t) n;
  int64_t sd = (int64_t) str_size;
  memcpy(base, &magic, 4);
  memcpy(base + 4, &as32, 4);
  memcpy(base + 8, &n64, 8);
  memcpy(base + 16, &sd, 8);

  mori_write_strings(base + header_size, x);

  if (attrs_size > 0)
    mori_serialize_into(base + header_size + str_size, attrs_size, attrs);

  UNPROTECT(1);
  return mori_make_result(shm);
}

/* Unified entry point: dispatch by type */
SEXP mori_create(SEXP x) {
  if (ALTREP(x)) {
    SEXP d1 = R_altrep_data1(x);
    if (TYPEOF(d1) == EXTPTRSXP && R_ExternalPtrTag(d1) == mori_owned_tag)
      return x;
  }
  int type = TYPEOF(x);
  if (type == VECSXP || type == LISTSXP)
    return mori_shm_create_list_call(x);
  if (type == STRSXP)
    return mori_shm_create_string_call(x);
  if (mori_shm_eligible(type))
    return mori_shm_create_vector_call(x);
  return x;
}

// .Call entry points: daemon-side SHM open and wrap --------------------------

/* All three open_* take an already-wrapped shm_ptr (shm_tag extptr) that the
   caller has PROTECTed. The shm_ptr flows through as the keeper for the
   returned ALTREP's data1 extptr, pinning the mapping to its lifetime. */

static SEXP mori_open_list(SEXP shm_ptr) {
  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(shm_ptr);
  return mori_make_list_view(
    (unsigned char *) shm->addr, (int64_t) shm->size, -1, shm_ptr
  );
}

static SEXP mori_open_vector(SEXP shm_ptr) {

  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(shm_ptr);
  unsigned char *base = (unsigned char *) shm->addr;
  int32_t sexptype;
  int64_t length, attrs_size;
  memcpy(&sexptype, base + 4, 4);
  memcpy(&length, base + 8, 8);
  memcpy(&attrs_size, base + 16, 8);

  SEXP result = PROTECT(mori_make_vector(
    base + 64, (R_xlen_t) length, sexptype, shm_ptr
  ));

  if (attrs_size > 0) {
    size_t data_bytes = (size_t) length * mori_sizeof_elt(sexptype);
    mori_restore_attrs(result, base + 64 + data_bytes, (size_t) attrs_size);
  }

  UNPROTECT(1);
  return result;
}

static SEXP mori_open_string(SEXP shm_ptr) {

  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(shm_ptr);
  unsigned char *base = (unsigned char *) shm->addr;
  int32_t attrs_size;
  int64_t n, str_data_size;
  memcpy(&attrs_size, base + 4, 4);
  memcpy(&n, base + 8, 8);
  memcpy(&str_data_size, base + 16, 8);

  SEXP result = PROTECT(mori_make_string(
    base + 24, (R_xlen_t) n, shm_ptr
  ));

  if (attrs_size > 0)
    mori_restore_attrs(result, base + 24 + (size_t) str_data_size,
                       (size_t) attrs_size);

  UNPROTECT(1);
  return result;
}

/* Dispatch on magic bytes and wrap in the appropriate ALTREP class.
   On unknown magic, errors — GC cleans up shm_ptr's finalizer after longjmp.
   err_name is used in the error message ("" if caller has no name to report). */
static SEXP mori_dispatch_by_magic(SEXP shm_ptr, const char *err_name) {
  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(shm_ptr);
  unsigned char *base = (unsigned char *) shm->addr;
  uint32_t magic;
  memcpy(&magic, base, 4);
  if (magic == 0x4D4F524Cu) return mori_open_list(shm_ptr);
  if (magic == 0x4D4F5248u) return mori_open_vector(shm_ptr);
  if (magic == 0x4D4F5253u) return mori_open_string(shm_ptr);
  Rf_error("mori:invalid or corrupted shared memory region: '%s'",
           err_name != NULL ? err_name : "");
}

/* Open SHM by name, inspect magic, dispatch to appropriate wrapper.
   Malformed input (wrong type/length, NA, or not a mori SHM name) returns
   NULL silently. A well-formed name that fails to open or has unexpected
   magic bytes errors with a specific message. */
SEXP mori_shm_open_and_wrap(SEXP name) {

  if (TYPEOF(name) != STRSXP || XLENGTH(name) != 1)
    return R_NilValue;
  SEXP nm_sxp = STRING_ELT(name, 0);
  if (nm_sxp == NA_STRING)
    return R_NilValue;
  const char *nm = CHAR(nm_sxp);
  if (!mori_is_shm_name(nm))
    return R_NilValue;

  mori_shm *heap = mori_shm_open_heap(nm);
  if (!heap)
    Rf_error("mori:shared memory region not found: '%s'", nm);

  SEXP shm_ptr = PROTECT(mori_wrap_shm_consumer(heap));
  SEXP result = mori_dispatch_by_magic(shm_ptr, nm);
  UNPROTECT(1);
  return result;
}

SEXP mori_is_shared(SEXP x) {
  if (!ALTREP(x)) return Rf_ScalarLogical(0);
  SEXP d1 = R_altrep_data1(x);
  return Rf_ScalarLogical(
    TYPEOF(d1) == EXTPTRSXP &&
    R_ExternalPtrTag(d1) == mori_owned_tag
  );
}

SEXP mori_shm_name(SEXP x) {
  if (!ALTREP(x)) return R_BlankScalarString;
  SEXP d1 = R_altrep_data1(x);
  if (TYPEOF(d1) != EXTPTRSXP ||
      R_ExternalPtrTag(d1) != mori_owned_tag)
    return R_BlankScalarString;
  SEXP prot = R_ExternalPtrProtected(d1);
  if (R_ExternalPtrTag(prot) != mori_shm_tag)
    return R_BlankScalarString;  /* element or sub-list */
  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(prot);
  return Rf_mkString(shm->name);
}

// ALTREP serialization hooks --------------------------------------------------

/* Walks the keeper chain through owned-tag view extptrs and returns
   list(name, path) where path is an INTSXP of (collected indices +
   leaf_index) in top-down order. The only legitimate R_NilValue return is
   MORI_MAX_PATH overflow: any ALTREP holding this chain keeps every hop
   (and the root shm->name) live, so other reachability guards are
   unreachable. */
#define MORI_MAX_PATH 64

static SEXP mori_build_path(SEXP keeper_extptr, int32_t leaf_index) {

  int32_t collected[MORI_MAX_PATH];
  int n_collected = 0;

  SEXP hop = keeper_extptr;
  while (R_ExternalPtrTag(hop) == mori_owned_tag) {
    mori_list_view *view = (mori_list_view *) R_ExternalPtrAddr(hop);
    if (view->index >= 0) {
      if (n_collected >= MORI_MAX_PATH) return R_NilValue;
      collected[n_collected++] = view->index;
    }
    hop = R_ExternalPtrProtected(hop);
  }

  mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(hop);

  SEXP path = PROTECT(Rf_allocVector(INTSXP, n_collected + 1));
  int *pp = INTEGER(path);
  for (int i = 0; i < n_collected; i++)
    pp[i] = collected[n_collected - 1 - i];
  pp[n_collected] = leaf_index;

  SEXP state = PROTECT(Rf_allocVector(VECSXP, 2));
  SET_VECTOR_ELT(state, 0, Rf_mkString(shm->name));
  SET_VECTOR_ELT(state, 1, path);

  UNPROTECT(2);
  return state;
}

static SEXP mori_vec_Serialized_state(SEXP x) {
  SEXP data2 = R_altrep_data2(x);
  if (data2 != R_NilValue) return data2;  /* COW-materialized copy */

  SEXP data1 = R_altrep_data1(x);
  mori_vec *v = (mori_vec *) R_ExternalPtrAddr(data1);

  if (v->index == -1) {
    /* Standalone: prot is shm extptr by construction; name is live. */
    mori_shm *shm = (mori_shm *)
      R_ExternalPtrAddr(R_ExternalPtrProtected(data1));
    return Rf_mkString(shm->name);
  }

  /* Element: walk keeper chain to build (name, path). */
  SEXP state = mori_build_path(R_ExternalPtrProtected(data1), v->index);
  if (state != R_NilValue) return state;

  /* Only reachable when nesting depth exceeds MORI_MAX_PATH: materialize. */
  R_xlen_t n = v->length;
  int type = TYPEOF(x);
  SEXP mat = PROTECT(Rf_allocVector(type, n));
  memcpy(mori_data_ptr(mat), v->data, (size_t) n * mori_sizeof_elt(type));
  UNPROTECT(1);
  return mat;
}

static SEXP mori_string_Serialized_state(SEXP x) {
  SEXP data2 = R_altrep_data2(x);
  if (data2 != R_NilValue) return data2;  /* COW-materialized copy */

  SEXP data1 = R_altrep_data1(x);
  mori_str *s = (mori_str *) R_ExternalPtrAddr(data1);

  if (s->index == -1) {
    /* Standalone: prot is shm extptr by construction; name is live. */
    mori_shm *shm = (mori_shm *)
      R_ExternalPtrAddr(R_ExternalPtrProtected(data1));
    return Rf_mkString(shm->name);
  }

  /* Element: walk keeper chain to build (name, path). */
  SEXP state = mori_build_path(R_ExternalPtrProtected(data1), s->index);
  if (state != R_NilValue) return state;

  /* Only reachable when nesting depth exceeds MORI_MAX_PATH: materialize. */
  R_xlen_t n = s->length;
  SEXP mat = PROTECT(Rf_allocVector(STRSXP, n));
  for (R_xlen_t i = 0; i < n; i++)
    SET_STRING_ELT(mat, i, mori_string_elt_shm(s, i));
  UNPROTECT(1);
  return mat;
}

static SEXP mori_list_Serialized_state(SEXP x) {
  SEXP data1 = R_altrep_data1(x);
  if (R_ExternalPtrTag(data1) != mori_owned_tag)
    return Rf_allocVector(VECSXP, 0);

  mori_list_view *view = (mori_list_view *) R_ExternalPtrAddr(data1);
  if (view == NULL) return Rf_allocVector(VECSXP, 0);

  if (view->index == -1) {
    /* Root list: compact name scalar */
    SEXP shm_ptr = R_ExternalPtrProtected(data1);
    if (TYPEOF(shm_ptr) == EXTPTRSXP &&
        R_ExternalPtrTag(shm_ptr) == mori_shm_tag) {
      mori_shm *shm = (mori_shm *) R_ExternalPtrAddr(shm_ptr);
      if (shm != NULL && shm->name[0] != '\0')
        return Rf_mkString(shm->name);
    }
  } else {
    /* Sub-list: build (name, path) */
    SEXP state = mori_build_path(R_ExternalPtrProtected(data1), view->index);
    if (state != R_NilValue) return state;
  }

  /* Fallback: materialize */
  R_xlen_t n = view->n_elements;
  SEXP mat = PROTECT(Rf_allocVector(VECSXP, n));
  for (R_xlen_t i = 0; i < n; i++)
    SET_VECTOR_ELT(mat, i, mori_list_Elt(x, i));
  DUPLICATE_ATTRIB(mat, x);
  UNPROTECT(1);
  return mat;
}

/* Open parent SHM and walk the path, returning the leaf element.
   path is an INTSXP of length >= 1. Intermediate steps must be VECSXP
   children (nested MORL regions); the final step is the leaf. */
static SEXP mori_open_path(SEXP name, SEXP path_sxp) {

  const char *nm = CHAR(STRING_ELT(name, 0));

  mori_shm *heap = mori_shm_open_heap(nm);
  if (!heap)
    Rf_error("mori:failed to open shared memory '%s'", nm);

  SEXP shm_ptr = PROTECT(mori_wrap_shm_consumer(heap));

  unsigned char *base = (unsigned char *) heap->addr;
  uint32_t magic;
  memcpy(&magic, base, 4);
  if (magic != 0x4D4F524Cu)
    Rf_error("mori:not a list region");  /* GC reclaims shm_ptr via longjmp */

  int path_len = (int) XLENGTH(path_sxp);
  const int *path = INTEGER(path_sxp);

  SEXP keeper = shm_ptr;
  unsigned char *cur_base = base;
  int64_t cur_region_size = (int64_t) heap->size;
  int32_t cur_n;
  memcpy(&cur_n, cur_base + 4, 4);

  PROTECT_INDEX child_idx;
  SEXP child = R_NilValue;
  PROTECT_WITH_INDEX(child, &child_idx);

  for (int k = 0; k < path_len - 1; k++) {
    int idx = path[k];
    if (idx < 0 || idx >= cur_n)
      Rf_error("mori:path index out of bounds");

    unsigned char *dir = cur_base + 24 + 32 * (size_t) idx;
    int64_t data_offset, data_size;
    int32_t sexptype;
    memcpy(&data_offset, dir, 8);
    memcpy(&data_size, dir + 8, 8);
    memcpy(&sexptype, dir + 16, 4);

    if (sexptype != VECSXP)
      Rf_error("mori:path step is not a nested list");
    if (data_offset < 0 || data_size < 0 ||
        data_offset + data_size > cur_region_size)
      Rf_error("mori:nested region out of bounds");

    REPROTECT(child = mori_make_list_view(
      cur_base + data_offset, data_size, idx, keeper
    ), child_idx);
    keeper = R_altrep_data1(child);

    cur_base = cur_base + data_offset;
    cur_region_size = data_size;
    memcpy(&cur_n, cur_base + 4, 4);
  }

  int leaf_idx = path[path_len - 1];
  if (leaf_idx < 0 || leaf_idx >= cur_n)
    Rf_error("mori:leaf index out of bounds");

  SEXP result = mori_unwrap_element(cur_base, cur_region_size,
                                    leaf_idx, keeper);
  UNPROTECT(2);
  return result;
}

static SEXP mori_Unserialize(SEXP class_info, SEXP state) {
  (void) class_info;
  /* Compact state: SHM name string → open and wrap (root) */
  if (TYPEOF(state) == STRSXP && XLENGTH(state) == 1 &&
      STRING_ELT(state, 0) != NA_STRING &&
      mori_is_shm_name(CHAR(STRING_ELT(state, 0))))
    return mori_shm_open_and_wrap(state);
  /* Path reference: list(name, int_path) where int_path has length >= 1.
     Length 1 is wire-compatible with the previous (name, scalar_int) form. */
  if (TYPEOF(state) == VECSXP && XLENGTH(state) == 2 &&
      TYPEOF(VECTOR_ELT(state, 0)) == STRSXP &&
      XLENGTH(VECTOR_ELT(state, 0)) == 1 &&
      STRING_ELT(VECTOR_ELT(state, 0), 0) != NA_STRING &&
      TYPEOF(VECTOR_ELT(state, 1)) == INTSXP &&
      XLENGTH(VECTOR_ELT(state, 1)) >= 1 &&
      mori_is_shm_name(CHAR(STRING_ELT(VECTOR_ELT(state, 0), 0))))
    return mori_open_path(VECTOR_ELT(state, 0), VECTOR_ELT(state, 1));
  /* Expanded state: materialized data → return as-is
     (R restores ALTREP attributes separately) */
  return state;
}

// ALTREP class registration ---------------------------------------------------

typedef R_altrep_class_t (*mori_make_class_fn)(const char *, const char *,
                                               DllInfo *);

/* Register one of the 5 atomic ALTREP vector classes, all sharing the same
   method set (mori_vec_*). */
static R_altrep_class_t mori_register_vec_class(mori_make_class_fn make,
                                                const char *name,
                                                DllInfo *dll) {
  R_altrep_class_t cls = make(name, "mori", dll);
  R_set_altrep_Length_method(cls, mori_vec_Length);
  R_set_altvec_Dataptr_method(cls, mori_vec_Dataptr);
  R_set_altvec_Dataptr_or_null_method(cls, mori_vec_Dataptr_or_null);
  R_set_altrep_Serialized_state_method(cls, mori_vec_Serialized_state);
  R_set_altrep_Unserialize_method(cls, mori_Unserialize);
  return cls;
}

void mori_altrep_init(DllInfo *dll) {

  mori_tag = Rf_install("mori");
  mori_shm_tag = Rf_install("mori_shm");
  mori_host_tag = Rf_install("mori_host");
  mori_owned_tag = Rf_install("mori_owned");

  /* ALTLIST class */
  mori_list_class = R_make_altlist_class("mori_list", "mori", dll);
  R_set_altrep_Length_method(mori_list_class, mori_list_Length);
  R_set_altrep_Duplicate_method(mori_list_class, mori_list_Duplicate);
  R_set_altvec_Dataptr_method(mori_list_class, mori_list_Dataptr);
  R_set_altvec_Dataptr_or_null_method(mori_list_class,
                                      mori_list_Dataptr_or_null);
  R_set_altlist_Elt_method(mori_list_class, mori_list_Elt);
  R_set_altrep_Serialized_state_method(mori_list_class,
                                       mori_list_Serialized_state);
  R_set_altrep_Unserialize_method(mori_list_class, mori_Unserialize);

  /* ALTREP atomic vector classes (all share the same mori_vec_* methods) */
  mori_real_class    = mori_register_vec_class(R_make_altreal_class,
                                               "mori_real",    dll);
  mori_integer_class = mori_register_vec_class(R_make_altinteger_class,
                                               "mori_integer", dll);
  mori_logical_class = mori_register_vec_class(R_make_altlogical_class,
                                               "mori_logical", dll);
  mori_raw_class     = mori_register_vec_class(R_make_altraw_class,
                                               "mori_raw",     dll);
  mori_complex_class = mori_register_vec_class(R_make_altcomplex_class,
                                               "mori_complex", dll);

  /* ALTSTRING class */
  mori_string_class = R_make_altstring_class("mori_string", "mori", dll);
  R_set_altrep_Length_method(mori_string_class, mori_string_Length);
  R_set_altrep_Duplicate_method(mori_string_class, mori_string_Duplicate);
  R_set_altvec_Dataptr_method(mori_string_class, mori_string_Dataptr);
  R_set_altvec_Dataptr_or_null_method(mori_string_class,
                                      mori_string_Dataptr_or_null);
  R_set_altstring_Elt_method(mori_string_class, mori_string_Elt);
  R_set_altrep_Serialized_state_method(mori_string_class,
                                       mori_string_Serialized_state);
  R_set_altrep_Unserialize_method(mori_string_class, mori_Unserialize);
}
