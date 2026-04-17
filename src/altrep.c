#include <stdlib.h>
#include "sora.h"

// Global ALTREP class handles and sentinel ------------------------------------

static R_altrep_class_t sora_list_class;
static R_altrep_class_t sora_real_class;
static R_altrep_class_t sora_integer_class;
static R_altrep_class_t sora_logical_class;
static R_altrep_class_t sora_raw_class;
static R_altrep_class_t sora_complex_class;
static R_altrep_class_t sora_string_class;
static SEXP sora_tag;

// Element directory (for list SHM layout) -------------------------------------

typedef struct {
  int64_t data_offset;
  int64_t data_size;
  int32_t sexptype;
  int32_t attrs_size;
  int64_t length;
} sora_elem;

// Zero-copy eligibility: any atomic vector (attributes stored separately) ----

static int sora_shm_eligible(int type) {
  return type == REALSXP || type == INTSXP || type == LGLSXP ||
         type == RAWSXP || type == CPLXSXP || type == STRSXP;
}

// Type-dispatched data pointer (avoids non-API DATAPTR) -----------------------

static inline void *sora_data_ptr(SEXP x) {
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
static inline SEXP sora_get_attrs_for_serialize(SEXP x) {
#if R_VERSION >= R_Version(4, 6, 0)
  return ANY_ATTRIB(x) ? R_getAttributes(x) : R_NilValue;
#else
  return ATTRIB(x);
#endif
}

/* Sets class last to avoid validation ordering issues. */
static void sora_set_attrs_from(SEXP result, SEXP attrs) {
#if R_VERSION >= R_Version(4, 6, 0)
  SEXP names = Rf_getAttrib(attrs, R_NamesSymbol);
  R_xlen_t n = XLENGTH(attrs), class_idx = -1;
  for (R_xlen_t i = 0; i < n; i++) {
    SEXP nm = Rf_installChar(STRING_ELT(names, i));
    if (nm == R_ClassSymbol) { class_idx = i; continue; }
    Rf_setAttrib(result, nm, VECTOR_ELT(attrs, i));
  }
  if (class_idx >= 0)
    Rf_classgets(result, VECTOR_ELT(attrs, class_idx));
#else
  SET_ATTRIB(result, attrs);
  if (Rf_getAttrib(result, R_ClassSymbol) != R_NilValue)
    SET_OBJECT(result, 1);
#endif
}

static void sora_restore_attrs(SEXP result, unsigned char *buf, size_t size) {
  SEXP attrs = PROTECT(sora_unserialize_from(buf, size));
  sora_set_attrs_from(result, attrs);
  UNPROTECT(1);
}

// SHM name validation for unserialize dispatch --------------------------------

static int sora_is_shm_name(const char *s) {
#ifdef _WIN32
  return strncmp(s, "Local\\sora_", 11) == 0;
#else
  return s[0] == '/' && strncmp(s + 1, "sora_", 5) == 0;
#endif
}

// Vec finalizer: frees the sora_vec struct only -------------------------------

static void sora_vec_finalizer(SEXP ptr) {
  void *v = R_ExternalPtrAddr(ptr);
  if (v) {
    free(v);
    R_ClearExternalPtr(ptr);
  }
}

// ALTREP atomic vector methods (shared by all 5 types) ------------------------

/*
 * Layout:
 *   data1 = extptr to sora_vec { data, length }
 *           protected value keeps the parent SHM extptr alive
 *   data2 = R_NilValue while SHM-backed; regular SEXP after materialization
 */

static R_xlen_t sora_vec_Length(SEXP x) {
  if (R_altrep_data2(x) != R_NilValue)
    return XLENGTH(R_altrep_data2(x));
  sora_vec *v = (sora_vec *) R_ExternalPtrAddr(R_altrep_data1(x));
  return v->length;
}

static const void *sora_vec_Dataptr_or_null(SEXP x) {
  if (R_altrep_data2(x) != R_NilValue)
    return DATAPTR_RO(R_altrep_data2(x));
  sora_vec *v = (sora_vec *) R_ExternalPtrAddr(R_altrep_data1(x));
  return v->data;
}

static void *sora_vec_Dataptr(SEXP x, Rboolean writable) {

  if (R_altrep_data2(x) != R_NilValue)
    return sora_data_ptr(R_altrep_data2(x));

  sora_vec *v = (sora_vec *) R_ExternalPtrAddr(R_altrep_data1(x));

  if (!writable)
    return (void *) v->data;

  /* COW: materialize to a regular R vector */
  R_xlen_t n = v->length;
  int type = TYPEOF(x);
  SEXP mat = PROTECT(Rf_allocVector(type, n));
  memcpy(sora_data_ptr(mat), v->data, (size_t) n * sora_sizeof_elt(type));
  DUPLICATE_ATTRIB(mat, x);
  R_set_altrep_data2(x, mat);
  UNPROTECT(1);

  return sora_data_ptr(mat);
}

/* keeper: SEXP kept alive via the extptr's protected slot (parent SHM). */
static SEXP sora_make_vector(const void *data, R_xlen_t length,
                             int sexptype, SEXP keeper) {

  sora_vec *v = (sora_vec *) malloc(sizeof(sora_vec));
  if (!v) Rf_error("sora: allocation failure");
  v->data = data;
  v->length = length;
  v->index = -1;

  SEXP ptr = PROTECT(R_MakeExternalPtr(v, R_NilValue, keeper));
  R_RegisterCFinalizerEx(ptr, sora_vec_finalizer, TRUE);

  R_altrep_class_t cls;
  switch (sexptype) {
  case REALSXP:  cls = sora_real_class;    break;
  case INTSXP:   cls = sora_integer_class; break;
  case LGLSXP:   cls = sora_logical_class; break;
  case RAWSXP:   cls = sora_raw_class;     break;
  case CPLXSXP:  cls = sora_complex_class; break;
  default:
    free(v);
    UNPROTECT(1);
    Rf_error("sora: unsupported ALTREP type %d", sexptype);
  }

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
 *   data1 = extptr to sora_str { table, data, length }
 *           protected value keeps the parent SHM extptr alive
 *   data2 = R_NilValue while SHM-backed; regular STRSXP after materialization
 */

typedef struct {
  const unsigned char *table;
  const unsigned char *data;
  R_xlen_t length;
  int32_t index;   /* -1 = standalone, >= 0 = element of ALTLIST */
} sora_str;

static SEXP sora_string_elt_shm(sora_str *s, R_xlen_t i) {
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

static R_xlen_t sora_string_Length(SEXP x) {
  if (R_altrep_data2(x) != R_NilValue)
    return XLENGTH(R_altrep_data2(x));
  sora_str *s = (sora_str *) R_ExternalPtrAddr(R_altrep_data1(x));
  return s->length;
}

static SEXP sora_string_Elt(SEXP x, R_xlen_t i) {
  if (R_altrep_data2(x) != R_NilValue)
    return STRING_ELT(R_altrep_data2(x), i);
  sora_str *s = (sora_str *) R_ExternalPtrAddr(R_altrep_data1(x));
  return sora_string_elt_shm(s, i);
}

static const void *sora_string_Dataptr_or_null(SEXP x) {
  if (R_altrep_data2(x) != R_NilValue)
    return DATAPTR_RO(R_altrep_data2(x));
  return NULL;
}

static void *sora_string_Dataptr(SEXP x, Rboolean writable) {
  if (R_altrep_data2(x) != R_NilValue)
    return sora_data_ptr(R_altrep_data2(x));

  sora_str *s = (sora_str *) R_ExternalPtrAddr(R_altrep_data1(x));
  R_xlen_t n = s->length;
  SEXP mat = PROTECT(Rf_allocVector(STRSXP, n));
  for (R_xlen_t i = 0; i < n; i++)
    SET_STRING_ELT(mat, i, sora_string_elt_shm(s, i));
  R_set_altrep_data2(x, mat);
  UNPROTECT(1);

  return sora_data_ptr(mat);
}

static SEXP sora_string_Duplicate(SEXP x, Rboolean deep) {
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
static SEXP sora_make_string(const unsigned char *region_base,
                             R_xlen_t n, SEXP keeper) {

  sora_str *s = (sora_str *) malloc(sizeof(sora_str));
  if (!s) Rf_error("sora: allocation failure");

  size_t table_size = 16 * (size_t) n;
  s->table = region_base;
  s->data = region_base + SORA_ALIGN64(table_size);
  s->length = n;
  s->index = -1;

  SEXP ptr = PROTECT(R_MakeExternalPtr(s, R_NilValue, keeper));
  R_RegisterCFinalizerEx(ptr, sora_vec_finalizer, TRUE);

  SEXP result = R_new_altrep(sora_string_class, ptr, R_NilValue);
  UNPROTECT(1);
  return result;
}

// Element extraction helper (shared by list Elt and open_element) -------------

static SEXP sora_unwrap_element(unsigned char *base, int32_t index,
                                SEXP shm_ptr) {

  unsigned char *dir = base + 24 + 32 * (size_t) index;
  int64_t data_offset, data_size, length;
  int32_t sexptype, attrs_size;
  memcpy(&data_offset, dir, 8);
  memcpy(&data_size, dir + 8, 8);
  memcpy(&sexptype, dir + 16, 4);
  memcpy(&attrs_size, dir + 20, 4);
  memcpy(&length, dir + 24, 8);

  SEXP result;
  if (sexptype == STRSXP) {
    result = PROTECT(sora_make_string(
      base + data_offset, (R_xlen_t) length, shm_ptr
    ));
    ((sora_str *) R_ExternalPtrAddr(R_altrep_data1(result)))->index = index;
  } else if (sexptype != 0) {
    result = PROTECT(sora_make_vector(
      base + data_offset, (R_xlen_t) length, sexptype, shm_ptr
    ));
    ((sora_vec *) R_ExternalPtrAddr(R_altrep_data1(result)))->index = index;
  } else {
    result = PROTECT(sora_unserialize_from(
      base + (size_t) data_offset, (size_t) data_size
    ));
  }

  if (attrs_size > 0 && sexptype != 0) {
    size_t attrs_off = (size_t)(data_offset + data_size - attrs_size);
    sora_restore_attrs(result, base + attrs_off, (size_t) attrs_size);
  }

  UNPROTECT(1);
  return result;
}

// ALTLIST methods -------------------------------------------------------------

/*
 * Layout:
 *   data1 = extptr to sora_shm (daemon-side, mmap'd read-only)
 *   data2 = cache VECSXP (length n, initialized to sora_tag)
 *
 * SHM layout:
 *   Bytes 0-3:   uint32_t magic (0x534F524C "SORL")
 *   Bytes 4-7:   int32_t  n_elements
 *   Bytes 8-15:  int64_t  attrs_offset
 *   Bytes 16-23: int64_t  attrs_size
 *   Byte 24+:    element directory (32 bytes per element)
 */

static R_xlen_t sora_list_Length(SEXP x) {
  return XLENGTH(R_altrep_data2(x));
}

static SEXP sora_list_Elt(SEXP x, R_xlen_t i) {

  SEXP cache = R_altrep_data2(x);
  SEXP cached = VECTOR_ELT(cache, i);
  if (cached != sora_tag) return cached;

  SEXP shm_ptr = R_altrep_data1(x);
  sora_shm *shm = (sora_shm *) R_ExternalPtrAddr(shm_ptr);
  unsigned char *base = (unsigned char *) shm->addr;

  SEXP result = sora_unwrap_element(base, (int32_t) i, shm_ptr);
  SET_VECTOR_ELT(cache, i, result);
  return result;
}

/* NULL forces R to use Elt() for element access */
static const void *sora_list_Dataptr_or_null(SEXP x) {
  return NULL;
}

/* Full materialization fallback */
static void *sora_list_Dataptr(SEXP x, Rboolean writable) {
  R_xlen_t n = XLENGTH(x);
  for (R_xlen_t i = 0; i < n; i++)
    sora_list_Elt(x, i);
  return sora_data_ptr(R_altrep_data2(x));
}

/* COW: modification produces a regular list */
static SEXP sora_list_Duplicate(SEXP x, Rboolean deep) {
  R_xlen_t n = XLENGTH(x);
  SEXP result = PROTECT(Rf_allocVector(VECSXP, n));
  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = sora_list_Elt(x, i);
    SET_VECTOR_ELT(result, i, deep ? Rf_duplicate(elt) : elt);
  }
  DUPLICATE_ATTRIB(result, x);
  UNPROTECT(1);
  return result;
}

static SEXP sora_open_list(sora_shm *shm_stack);
static SEXP sora_open_vector(sora_shm *shm_stack);
static SEXP sora_open_string(sora_shm *shm_stack);

// Host-side result helper: GC-chained SHM cleanup -----------------------------

static SEXP sora_make_result(sora_shm *shm) {

  /* Heap-allocate for host finalizer: unlink only, no munmap */
  sora_shm *host = (sora_shm *) malloc(sizeof(sora_shm));
  if (!host) Rf_error("sora: allocation failure");
  memcpy(host, shm, sizeof(sora_shm));
  host->addr = NULL;
  host->size = 0;
#ifdef _WIN32
  shm->handle = NULL;
#endif

  SEXP host_ptr = PROTECT(R_MakeExternalPtr(host, R_NilValue, R_NilValue));
  R_RegisterCFinalizerEx(host_ptr, sora_host_finalizer, TRUE);

  /* Dispatch on magic bytes to wrap in ALTREP */
  unsigned char *base = (unsigned char *) shm->addr;
  uint32_t magic;
  memcpy(&magic, base, 4);

  SEXP result;
  if (magic == 0x534F524Cu) {
    result = PROTECT(sora_open_list(shm));
    /* Chain: list data1 (shm_ptr) protects host_ptr */
    R_SetExternalPtrProtected(R_altrep_data1(result), host_ptr);
  } else if (magic == 0x534F5248u) {
    result = PROTECT(sora_open_vector(shm));
    /* Chain: vec data1's keeper (shm_ptr) protects host_ptr */
    R_SetExternalPtrProtected(
      R_ExternalPtrProtected(R_altrep_data1(result)), host_ptr);
  } else if (magic == 0x534F5253u) {
    result = PROTECT(sora_open_string(shm));
    /* Chain: str data1's keeper (shm_ptr) protects host_ptr */
    R_SetExternalPtrProtected(
      R_ExternalPtrProtected(R_altrep_data1(result)), host_ptr);
  } else {
    /* Unknown magic: deserialize as raw bytes and release SHM immediately */
    result = PROTECT(sora_unserialize_from(base, shm->size));
    sora_shm_close(shm, 0);
    sora_host_finalizer(host_ptr);
  }

  UNPROTECT(2);
  return result;
}

// String write helper (shared by standalone and list paths) -------------------

/* Returns total bytes written (including alignment padding). */
static size_t sora_write_strings(unsigned char *dest, SEXP x) {
  R_xlen_t n = XLENGTH(x);
  size_t table_size = 16 * (size_t) n;
  size_t data_start = SORA_ALIGN64(table_size);

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

static size_t sora_string_data_size(SEXP x) {
  R_xlen_t n = XLENGTH(x);
  size_t table_size = 16 * (size_t) n;
  size_t str_bytes = 0;
  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = STRING_ELT(x, i);
    if (elt != NA_STRING)
      str_bytes += (size_t) LENGTH(elt);
  }
  return SORA_ALIGN64(table_size) + str_bytes;
}

// .Call entry points: host-side SHM creation ---------------------------------

/* Zero-copy: list/data frame to SHM */
static SEXP sora_shm_create_list_call(SEXP x) {

  /* Coerce pairlists to VECSXP so VECTOR_ELT/XLENGTH work uniformly */
  if (TYPEOF(x) == LISTSXP) {
    x = PROTECT(Rf_coerceVector(x, VECSXP));
  } else {
    PROTECT(x);
  }

  R_xlen_t n = XLENGTH(x);
  size_t dir_size = 24 + 32 * (size_t) n;
  size_t total = SORA_ALIGN64(dir_size);

  sora_elem *elems = (sora_elem *) R_alloc((size_t) n, sizeof(sora_elem));

  /* First pass: compute element sizes and offsets */
  for (R_xlen_t i = 0; i < n; i++) {
    SEXP elt = VECTOR_ELT(x, i);
    int type = TYPEOF(elt);

    if (sora_shm_eligible(type)) {
      elems[i].sexptype = type;
      elems[i].length = (int64_t) XLENGTH(elt);

      size_t raw_size;
      if (type == STRSXP)
        raw_size = sora_string_data_size(elt);
      else
        raw_size = (size_t) XLENGTH(elt) * sora_sizeof_elt(type);

      SEXP elt_attrs = PROTECT(sora_get_attrs_for_serialize(elt));
      if (elt_attrs != R_NilValue) {
        size_t elt_attrs_size = sora_serialize_count(elt_attrs);
        elems[i].attrs_size = (int32_t) elt_attrs_size;
        elems[i].data_size = (int64_t)(raw_size + elt_attrs_size);
      } else {
        elems[i].attrs_size = 0;
        elems[i].data_size = (int64_t) raw_size;
      }
      UNPROTECT(1);
    } else {
      elems[i].sexptype = 0;
      elems[i].attrs_size = 0;
      elems[i].length = 0;
      elems[i].data_size = (int64_t) sora_serialize_count(elt);
    }
    elems[i].data_offset = (int64_t) total;
    total += SORA_ALIGN64((size_t) elems[i].data_size);
  }

  /* Compute serialized attributes size */
  SEXP list_attrs = PROTECT(sora_get_attrs_for_serialize(x));
  size_t attrs_size = (list_attrs != R_NilValue) ?
    sora_serialize_count(list_attrs) : 0;
  size_t attrs_offset = total;
  total += SORA_ALIGN64(attrs_size);

  sora_shm shm;
  if (sora_shm_create(&shm, total) != 0)
    Rf_error("sora: failed to create shared memory");

  unsigned char *base = (unsigned char *) shm.addr;

  /* Write header */
  uint32_t magic = 0x534F524Cu;
  int32_t n32 = (int32_t) n;
  int64_t ao = (int64_t) attrs_offset, as = (int64_t) attrs_size;
  memcpy(base, &magic, 4);
  memcpy(base + 4, &n32, 4);
  memcpy(base + 8, &ao, 8);
  memcpy(base + 16, &as, 8);

  /* Write element directory and data */
  for (R_xlen_t i = 0; i < n; i++) {
    unsigned char *d = base + 24 + 32 * (size_t) i;
    memcpy(d, &elems[i].data_offset, 8);
    memcpy(d + 8, &elems[i].data_size, 8);
    memcpy(d + 16, &elems[i].sexptype, 4);
    memcpy(d + 20, &elems[i].attrs_size, 4);
    memcpy(d + 24, &elems[i].length, 8);

    SEXP elt = VECTOR_ELT(x, i);
    if (elems[i].sexptype == STRSXP) {
      size_t written = sora_write_strings(base + elems[i].data_offset, elt);
      if (elems[i].attrs_size > 0) {
        SEXP ea = PROTECT(sora_get_attrs_for_serialize(elt));
        sora_serialize_into(base + elems[i].data_offset + written,
                            (size_t) elems[i].attrs_size, ea);
        UNPROTECT(1);
      }
    } else if (elems[i].sexptype != 0) {
      size_t raw_size = (size_t) XLENGTH(elt) *
                        sora_sizeof_elt(elems[i].sexptype);
      memcpy(base + elems[i].data_offset, DATAPTR_RO(elt), raw_size);
      if (elems[i].attrs_size > 0) {
        SEXP ea = PROTECT(sora_get_attrs_for_serialize(elt));
        sora_serialize_into(base + elems[i].data_offset + raw_size,
                            (size_t) elems[i].attrs_size, ea);
        UNPROTECT(1);
      }
    } else {
      sora_serialize_into(base + elems[i].data_offset,
                          (size_t) elems[i].data_size, elt);
    }
  }

  /* Write serialized attributes */
  if (attrs_size > 0)
    sora_serialize_into(base + attrs_offset, attrs_size, list_attrs);

  UNPROTECT(2);
  return sora_make_result(&shm);
}

/* Zero-copy: atomic vector → 64-byte header + data (64-byte aligned) + attrs */
static SEXP sora_shm_create_vector_call(SEXP x) {

  int type = TYPEOF(x);
  R_xlen_t n = XLENGTH(x);
  size_t elt_size = sora_sizeof_elt(type);
  size_t data_size = (size_t) n * elt_size;

  SEXP attrs = PROTECT(sora_get_attrs_for_serialize(x));
  size_t attrs_size = (attrs != R_NilValue) ? sora_serialize_count(attrs) : 0;
  size_t total = 64 + data_size + attrs_size;

  sora_shm shm;
  if (sora_shm_create(&shm, total) != 0)
    Rf_error("sora: failed to create shared memory");

  unsigned char *base = (unsigned char *) shm.addr;

  /* Zero-fill header, then write fields */
  memset(base, 0, 64);
  uint32_t magic = 0x534F5248u;
  int32_t sexptype = (int32_t) type;
  int64_t length = (int64_t) n;
  int64_t as64 = (int64_t) attrs_size;
  memcpy(base, &magic, 4);
  memcpy(base + 4, &sexptype, 4);
  memcpy(base + 8, &length, 8);
  memcpy(base + 16, &as64, 8);

  memcpy(base + 64, DATAPTR_RO(x), data_size);

  if (attrs_size > 0)
    sora_serialize_into(base + 64 + data_size, attrs_size, attrs);

  UNPROTECT(1);
  return sora_make_result(&shm);
}

/* Zero-copy: character vector → 24-byte header + offset table + strings + attrs */
static SEXP sora_shm_create_string_call(SEXP x) {

  R_xlen_t n = XLENGTH(x);
  size_t header_size = 24;
  size_t str_size = sora_string_data_size(x);

  SEXP attrs = PROTECT(sora_get_attrs_for_serialize(x));
  size_t attrs_size = (attrs != R_NilValue) ? sora_serialize_count(attrs) : 0;
  size_t total = header_size + str_size + attrs_size;

  sora_shm shm;
  if (sora_shm_create(&shm, total) != 0)
    Rf_error("sora: failed to create shared memory");

  unsigned char *base = (unsigned char *) shm.addr;

  /* Write header */
  memset(base, 0, header_size);
  uint32_t magic = 0x534F5253u;
  int32_t as32 = (int32_t) attrs_size;
  int64_t n64 = (int64_t) n;
  int64_t sd = (int64_t) str_size;
  memcpy(base, &magic, 4);
  memcpy(base + 4, &as32, 4);
  memcpy(base + 8, &n64, 8);
  memcpy(base + 16, &sd, 8);

  sora_write_strings(base + header_size, x);

  if (attrs_size > 0)
    sora_serialize_into(base + header_size + str_size, attrs_size, attrs);

  UNPROTECT(1);
  return sora_make_result(&shm);
}

/* Unified entry point: dispatch by type */
SEXP sora_create(SEXP x) {
  int type = TYPEOF(x);
  if (type == VECSXP || type == LISTSXP)
    return sora_shm_create_list_call(x);
  if (type == STRSXP)
    return sora_shm_create_string_call(x);
  if (sora_shm_eligible(type))
    return sora_shm_create_vector_call(x);
  return x;
}

// .Call entry points: daemon-side SHM open and wrap --------------------------

/* Open SHM and create ALTLIST wrapper */
static SEXP sora_open_list(sora_shm *shm_stack) {

  sora_shm *shm = (sora_shm *) malloc(sizeof(sora_shm));
  if (!shm) Rf_error("sora: allocation failure");
  memcpy(shm, shm_stack, sizeof(sora_shm));

  SEXP shm_ptr = PROTECT(R_MakeExternalPtr(shm, sora_tag, R_NilValue));
  R_RegisterCFinalizerEx(shm_ptr, sora_shm_finalizer, TRUE);

  unsigned char *base = (unsigned char *) shm->addr;
  int32_t n;
  int64_t attrs_offset, attrs_size;
  memcpy(&n, base + 4, 4);
  memcpy(&attrs_offset, base + 8, 8);
  memcpy(&attrs_size, base + 16, 8);

  /* Cache filled with sentinel (sora_tag means "not yet accessed") */
  SEXP cache = PROTECT(Rf_allocVector(VECSXP, (R_xlen_t) n));
  for (int32_t i = 0; i < n; i++)
    SET_VECTOR_ELT(cache, i, sora_tag);

  SEXP result = PROTECT(R_new_altrep(sora_list_class, shm_ptr, cache));

  if (attrs_size > 0)
    sora_restore_attrs(result, base + (size_t) attrs_offset,
                       (size_t) attrs_size);

  UNPROTECT(3);
  return result;
}

/* Open SHM and create ALTREP vector wrapper */
static SEXP sora_open_vector(sora_shm *shm_stack) {

  sora_shm *shm = (sora_shm *) malloc(sizeof(sora_shm));
  if (!shm) Rf_error("sora: allocation failure");
  memcpy(shm, shm_stack, sizeof(sora_shm));

  SEXP shm_ptr = PROTECT(R_MakeExternalPtr(shm, sora_tag, R_NilValue));
  R_RegisterCFinalizerEx(shm_ptr, sora_shm_finalizer, TRUE);

  unsigned char *base = (unsigned char *) shm->addr;
  int32_t sexptype;
  int64_t length, attrs_size;
  memcpy(&sexptype, base + 4, 4);
  memcpy(&length, base + 8, 8);
  memcpy(&attrs_size, base + 16, 8);

  SEXP result = PROTECT(sora_make_vector(
    base + 64, (R_xlen_t) length, sexptype, shm_ptr
  ));
  R_SetExternalPtrTag(R_altrep_data1(result), Rf_mkChar(shm->name));

  /* Restore attributes */
  if (attrs_size > 0) {
    size_t data_bytes = (size_t) length * sora_sizeof_elt(sexptype);
    sora_restore_attrs(result, base + 64 + data_bytes, (size_t) attrs_size);
  }

  UNPROTECT(2);
  return result;
}

/* Open SHM and create ALTREP string wrapper */
static SEXP sora_open_string(sora_shm *shm_stack) {

  sora_shm *shm = (sora_shm *) malloc(sizeof(sora_shm));
  if (!shm) Rf_error("sora: allocation failure");
  memcpy(shm, shm_stack, sizeof(sora_shm));

  SEXP shm_ptr = PROTECT(R_MakeExternalPtr(shm, sora_tag, R_NilValue));
  R_RegisterCFinalizerEx(shm_ptr, sora_shm_finalizer, TRUE);

  unsigned char *base = (unsigned char *) shm->addr;
  int32_t attrs_size;
  int64_t n, str_data_size;
  memcpy(&attrs_size, base + 4, 4);
  memcpy(&n, base + 8, 8);
  memcpy(&str_data_size, base + 16, 8);

  SEXP result = PROTECT(sora_make_string(
    base + 24, (R_xlen_t) n, shm_ptr
  ));
  R_SetExternalPtrTag(R_altrep_data1(result), Rf_mkChar(shm->name));

  /* Restore attributes */
  if (attrs_size > 0)
    sora_restore_attrs(result, base + 24 + (size_t) str_data_size,
                       (size_t) attrs_size);

  UNPROTECT(2);
  return result;
}

/* Open SHM by name, inspect magic, dispatch to appropriate wrapper.
   Malformed input (wrong type/length, NA, or not a sora SHM name) returns
   NULL silently. A well-formed name that fails to open or has unexpected
   magic bytes errors with a specific message. */
SEXP sora_shm_open_and_wrap(SEXP name) {

  if (TYPEOF(name) != STRSXP || XLENGTH(name) != 1)
    return R_NilValue;
  SEXP nm_sxp = STRING_ELT(name, 0);
  if (nm_sxp == NA_STRING)
    return R_NilValue;
  const char *nm = CHAR(nm_sxp);
  if (!sora_is_shm_name(nm))
    return R_NilValue;

  sora_shm shm;
  if (sora_shm_open(&shm, nm) != 0)
    Rf_error("sora: shared memory region not found: '%s'", nm);

  unsigned char *base = (unsigned char *) shm.addr;
  uint32_t magic;
  memcpy(&magic, base, 4);

  if (magic == 0x534F524Cu) {
    /* SORL: ALTLIST */
    return sora_open_list(&shm);
  } else if (magic == 0x534F5248u) {
    /* SORH: bare ALTREP vector */
    return sora_open_vector(&shm);
  } else if (magic == 0x534F5253u) {
    /* SORS: ALTREP string vector */
    return sora_open_string(&shm);
  }

  sora_shm_close(&shm, 0);
  Rf_error("sora: invalid or corrupted shared memory region: '%s'", nm);
}

SEXP sora_is_shared(SEXP x) {
  if (ALTREP(x)) {
    SEXP d1 = R_altrep_data1(x);
    if (TYPEOF(d1) == EXTPTRSXP) {
      /* List: data1 is shm_ptr directly */
      if (R_ExternalPtrTag(d1) == sora_tag)
        return Rf_ScalarLogical(1);
      /* Vec/str: data1's prot is shm_ptr */
      SEXP prot = R_ExternalPtrProtected(d1);
      if (TYPEOF(prot) == EXTPTRSXP &&
          R_ExternalPtrTag(prot) == sora_tag)
        return Rf_ScalarLogical(1);
    }
  }
  return Rf_ScalarLogical(0);
}

SEXP sora_shm_name(SEXP x) {
  if (ALTREP(x)) {
    SEXP d1 = R_altrep_data1(x);
    if (TYPEOF(d1) == EXTPTRSXP) {
      /* Vec/str: data1 tag is the name */
      SEXP tag = R_ExternalPtrTag(d1);
      if (tag != R_NilValue && TYPEOF(tag) == CHARSXP)
        return Rf_ScalarString(tag);
      /* List: data1 is shm_ptr, name is in C struct */
      if (tag == sora_tag) {
        sora_shm *shm = (sora_shm *) R_ExternalPtrAddr(d1);
        if (shm && shm->name[0] != '\0') return Rf_mkString(shm->name);
      }
    }
  }
  return R_BlankScalarString;
}

// ALTREP serialization hooks --------------------------------------------------

static SEXP sora_vec_Serialized_state(SEXP x) {
  SEXP data1 = R_altrep_data1(x);
  SEXP tag = R_ExternalPtrTag(data1);

  /* Standalone with valid SHM → compact name */
  if (tag != R_NilValue && TYPEOF(tag) == CHARSXP &&
      R_altrep_data2(x) == R_NilValue &&
      R_ExternalPtrAddr(data1) != NULL)
    return Rf_ScalarString(tag);

  /* COW-materialized → return materialized copy */
  if (R_altrep_data2(x) != R_NilValue)
    return R_altrep_data2(x);

  /* Element of ALTLIST with valid parent SHM → compact (name, index) */
  sora_vec *v = (sora_vec *) R_ExternalPtrAddr(data1);
  if (v != NULL && v->index >= 0) {
    SEXP keeper = R_ExternalPtrProtected(data1);
    sora_shm *parent = (sora_shm *) R_ExternalPtrAddr(keeper);
    if (parent != NULL && parent->name[0] != '\0') {
      SEXP state = PROTECT(Rf_allocVector(VECSXP, 2));
      SET_VECTOR_ELT(state, 0, Rf_mkString(parent->name));
      SET_VECTOR_ELT(state, 1, Rf_ScalarInteger(v->index));
      UNPROTECT(1);
      return state;
    }
  }

  /* Closed, detached, or invalidated → materialize from SHM */
  if (v == NULL)
    return Rf_allocVector(TYPEOF(x), 0);
  R_xlen_t n = v->length;
  int type = TYPEOF(x);
  SEXP mat = PROTECT(Rf_allocVector(type, n));
  memcpy(sora_data_ptr(mat), v->data, (size_t) n * sora_sizeof_elt(type));
  UNPROTECT(1);
  return mat;
}

static SEXP sora_string_Serialized_state(SEXP x) {
  SEXP data1 = R_altrep_data1(x);
  SEXP tag = R_ExternalPtrTag(data1);

  if (tag != R_NilValue && TYPEOF(tag) == CHARSXP &&
      R_altrep_data2(x) == R_NilValue &&
      R_ExternalPtrAddr(data1) != NULL)
    return Rf_ScalarString(tag);

  if (R_altrep_data2(x) != R_NilValue)
    return R_altrep_data2(x);

  /* Element of ALTLIST with valid parent SHM → compact (name, index) */
  sora_str *s = (sora_str *) R_ExternalPtrAddr(data1);
  if (s != NULL && s->index >= 0) {
    SEXP keeper = R_ExternalPtrProtected(data1);
    sora_shm *parent = (sora_shm *) R_ExternalPtrAddr(keeper);
    if (parent != NULL && parent->name[0] != '\0') {
      SEXP state = PROTECT(Rf_allocVector(VECSXP, 2));
      SET_VECTOR_ELT(state, 0, Rf_mkString(parent->name));
      SET_VECTOR_ELT(state, 1, Rf_ScalarInteger(s->index));
      UNPROTECT(1);
      return state;
    }
  }

  /* Closed, detached, or invalidated → materialize from SHM */
  if (s == NULL)
    return Rf_allocVector(STRSXP, 0);
  R_xlen_t n = s->length;
  SEXP mat = PROTECT(Rf_allocVector(STRSXP, n));
  for (R_xlen_t i = 0; i < n; i++)
    SET_STRING_ELT(mat, i, sora_string_elt_shm(s, i));
  UNPROTECT(1);
  return mat;
}

static SEXP sora_list_Serialized_state(SEXP x) {
  SEXP data1 = R_altrep_data1(x);
  sora_shm *shm = (sora_shm *) R_ExternalPtrAddr(data1);

  if (R_ExternalPtrTag(data1) == sora_tag && shm != NULL &&
      shm->name[0] != '\0')
    return Rf_mkString(shm->name);

  /* Invalidated (SHM still mapped) → materialize */
  if (R_ExternalPtrTag(data1) == sora_tag && shm != NULL) {
    R_xlen_t n = XLENGTH(x);
    SEXP mat = PROTECT(Rf_allocVector(VECSXP, n));
    for (R_xlen_t i = 0; i < n; i++)
      SET_VECTOR_ELT(mat, i, sora_list_Elt(x, i));
    DUPLICATE_ATTRIB(mat, x);
    UNPROTECT(1);
    return mat;
  }

  /* Closed (extptr cleared) → empty */
  return Rf_allocVector(VECSXP, 0);
}

/* Open parent SHM and extract a single element by index */
static SEXP sora_open_element(SEXP name, int32_t index) {

  const char *nm = CHAR(STRING_ELT(name, 0));

  sora_shm shm_stack;
  if (sora_shm_open(&shm_stack, nm) != 0)
    Rf_error("sora: failed to open shared memory '%s'", nm);

  unsigned char *base = (unsigned char *) shm_stack.addr;
  uint32_t magic;
  memcpy(&magic, base, 4);
  if (magic != 0x534F524Cu) {
    sora_shm_close(&shm_stack, 0);
    Rf_error("sora: not a list region");
  }

  sora_shm *shm = (sora_shm *) malloc(sizeof(sora_shm));
  if (!shm) {
    sora_shm_close(&shm_stack, 0);
    Rf_error("sora: allocation failure");
  }
  memcpy(shm, &shm_stack, sizeof(sora_shm));

  SEXP shm_ptr = PROTECT(R_MakeExternalPtr(shm, sora_tag, R_NilValue));
  R_RegisterCFinalizerEx(shm_ptr, sora_shm_finalizer, TRUE);

  SEXP result = sora_unwrap_element(base, index, shm_ptr);
  UNPROTECT(1);
  return result;
}

static SEXP sora_Unserialize(SEXP class_info, SEXP state) {
  (void) class_info;
  /* Compact state: SHM name string → open and wrap */
  if (TYPEOF(state) == STRSXP && XLENGTH(state) == 1 &&
      sora_is_shm_name(CHAR(STRING_ELT(state, 0))))
    return sora_shm_open_and_wrap(state);
  /* Element reference: list(name, index) → open parent, extract element */
  if (TYPEOF(state) == VECSXP && XLENGTH(state) == 2 &&
      TYPEOF(VECTOR_ELT(state, 0)) == STRSXP &&
      XLENGTH(VECTOR_ELT(state, 0)) == 1 &&
      TYPEOF(VECTOR_ELT(state, 1)) == INTSXP &&
      sora_is_shm_name(CHAR(STRING_ELT(VECTOR_ELT(state, 0), 0))))
    return sora_open_element(VECTOR_ELT(state, 0),
                             INTEGER(VECTOR_ELT(state, 1))[0]);
  /* Expanded state: materialized data → return as-is
     (R restores ALTREP attributes separately) */
  return state;
}

// ALTREP class registration ---------------------------------------------------

void sora_altrep_init(DllInfo *dll) {

  sora_tag = Rf_install("sora");

  /* ALTLIST class */
  sora_list_class = R_make_altlist_class("sora_list", "sora", dll);
  R_set_altrep_Length_method(sora_list_class, sora_list_Length);
  R_set_altrep_Duplicate_method(sora_list_class, sora_list_Duplicate);
  R_set_altvec_Dataptr_method(sora_list_class, sora_list_Dataptr);
  R_set_altvec_Dataptr_or_null_method(sora_list_class,
                                      sora_list_Dataptr_or_null);
  R_set_altlist_Elt_method(sora_list_class, sora_list_Elt);
  R_set_altrep_Serialized_state_method(sora_list_class,
                                       sora_list_Serialized_state);
  R_set_altrep_Unserialize_method(sora_list_class, sora_Unserialize);

  /* ALTREP real class */
  sora_real_class = R_make_altreal_class("sora_real", "sora", dll);
  R_set_altrep_Length_method(sora_real_class, sora_vec_Length);
  R_set_altvec_Dataptr_method(sora_real_class, sora_vec_Dataptr);
  R_set_altvec_Dataptr_or_null_method(sora_real_class,
                                      sora_vec_Dataptr_or_null);
  R_set_altrep_Serialized_state_method(sora_real_class,
                                       sora_vec_Serialized_state);
  R_set_altrep_Unserialize_method(sora_real_class, sora_Unserialize);

  /* ALTREP integer class */
  sora_integer_class = R_make_altinteger_class("sora_integer", "sora", dll);
  R_set_altrep_Length_method(sora_integer_class, sora_vec_Length);
  R_set_altvec_Dataptr_method(sora_integer_class, sora_vec_Dataptr);
  R_set_altvec_Dataptr_or_null_method(sora_integer_class,
                                      sora_vec_Dataptr_or_null);
  R_set_altrep_Serialized_state_method(sora_integer_class,
                                       sora_vec_Serialized_state);
  R_set_altrep_Unserialize_method(sora_integer_class, sora_Unserialize);

  /* ALTREP logical class */
  sora_logical_class = R_make_altlogical_class("sora_logical", "sora", dll);
  R_set_altrep_Length_method(sora_logical_class, sora_vec_Length);
  R_set_altvec_Dataptr_method(sora_logical_class, sora_vec_Dataptr);
  R_set_altvec_Dataptr_or_null_method(sora_logical_class,
                                      sora_vec_Dataptr_or_null);
  R_set_altrep_Serialized_state_method(sora_logical_class,
                                       sora_vec_Serialized_state);
  R_set_altrep_Unserialize_method(sora_logical_class, sora_Unserialize);

  /* ALTREP raw class */
  sora_raw_class = R_make_altraw_class("sora_raw", "sora", dll);
  R_set_altrep_Length_method(sora_raw_class, sora_vec_Length);
  R_set_altvec_Dataptr_method(sora_raw_class, sora_vec_Dataptr);
  R_set_altvec_Dataptr_or_null_method(sora_raw_class,
                                      sora_vec_Dataptr_or_null);
  R_set_altrep_Serialized_state_method(sora_raw_class,
                                       sora_vec_Serialized_state);
  R_set_altrep_Unserialize_method(sora_raw_class, sora_Unserialize);

  /* ALTREP complex class */
  sora_complex_class = R_make_altcomplex_class("sora_complex", "sora", dll);
  R_set_altrep_Length_method(sora_complex_class, sora_vec_Length);
  R_set_altvec_Dataptr_method(sora_complex_class, sora_vec_Dataptr);
  R_set_altvec_Dataptr_or_null_method(sora_complex_class,
                                      sora_vec_Dataptr_or_null);
  R_set_altrep_Serialized_state_method(sora_complex_class,
                                       sora_vec_Serialized_state);
  R_set_altrep_Unserialize_method(sora_complex_class, sora_Unserialize);

  /* ALTSTRING class */
  sora_string_class = R_make_altstring_class("sora_string", "sora", dll);
  R_set_altrep_Length_method(sora_string_class, sora_string_Length);
  R_set_altrep_Duplicate_method(sora_string_class, sora_string_Duplicate);
  R_set_altvec_Dataptr_method(sora_string_class, sora_string_Dataptr);
  R_set_altvec_Dataptr_or_null_method(sora_string_class,
                                      sora_string_Dataptr_or_null);
  R_set_altstring_Elt_method(sora_string_class, sora_string_Elt);
  R_set_altrep_Serialized_state_method(sora_string_class,
                                       sora_string_Serialized_state);
  R_set_altrep_Unserialize_method(sora_string_class, sora_Unserialize);
}
