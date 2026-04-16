#include "sora.h"

/* ================================================================
   Counting pass: compute exact serialized size
   ================================================================ */

static void sora_count_bytes(R_outpstream_t stream, void *src, int len) {
  sora_buf *buf = (sora_buf *) stream->data;
  buf->cur += (size_t) len;
}

size_t sora_serialize_count(SEXP object) {

  sora_buf buf = {.buf = NULL, .len = 0, .cur = 0};
  struct R_outpstream_st out;

  R_InitOutPStream(&out, (R_pstream_data_t) &buf, R_pstream_binary_format,
                   3, NULL, sora_count_bytes, NULL, R_NilValue);
  R_Serialize(object, &out);

  return buf.cur;
}

/* ================================================================
   Fixed-buffer write: no realloc, no bounds check.
   Safe because the counting pass guarantees exact size.
   ================================================================ */

static void sora_write_fixed(R_outpstream_t stream, void *src, int len) {
  sora_buf *buf = (sora_buf *) stream->data;
  memcpy(buf->buf + buf->cur, src, (size_t) len);
  buf->cur += (size_t) len;
}

void sora_serialize_into(unsigned char *dst, size_t size, SEXP object) {

  sora_buf buf = {.buf = dst, .len = size, .cur = 0};
  struct R_outpstream_st out;

  R_InitOutPStream(&out, (R_pstream_data_t) &buf, R_pstream_binary_format,
                   3, NULL, sora_write_fixed, NULL, R_NilValue);
  R_Serialize(object, &out);
}

/* ================================================================
   Read callbacks for unserialize-from-buffer
   ================================================================ */

static int sora_read_char(R_inpstream_t stream) {
  sora_buf *buf = (sora_buf *) stream->data;
  return (buf->cur < buf->len)
    ? (unsigned char) buf->buf[buf->cur++]
    : -1;
}

static void sora_read_bytes(R_inpstream_t stream, void *dst, int len) {
  sora_buf *buf = (sora_buf *) stream->data;
  size_t n = (size_t) len;
  if (buf->cur + n > buf->len) n = buf->len - buf->cur;
  memcpy(dst, buf->buf + buf->cur, n);
  buf->cur += n;
}

SEXP sora_unserialize_from(unsigned char *src, size_t size) {

  sora_buf buf = {.buf = src, .len = size, .cur = 0};
  struct R_inpstream_st in;

  R_InitInPStream(&in, (R_pstream_data_t) &buf, R_pstream_any_format,
                  sora_read_char, sora_read_bytes, NULL, R_NilValue);
  return R_Unserialize(&in);
}

/* ================================================================
   Element size for atomic types
   ================================================================ */

size_t sora_sizeof_elt(int type) {
  switch (type) {
  case REALSXP:  return sizeof(double);
  case INTSXP:   return sizeof(int);
  case LGLSXP:   return sizeof(int);
  case RAWSXP:   return 1;
  case CPLXSXP:  return sizeof(Rcomplex);
  default:       return 0;
  }
}
