#include "mori.h"

// Counting pass: compute exact serialized size -------------------------------

static void mori_count_bytes(R_outpstream_t stream, void *src, int len) {
  mori_buf *buf = (mori_buf *) stream->data;
  buf->cur += (size_t) len;
}

size_t mori_serialize_count(SEXP object) {

  mori_buf buf = {.buf = NULL, .len = 0, .cur = 0};
  struct R_outpstream_st out;

  R_InitOutPStream(&out, (R_pstream_data_t) &buf, R_pstream_binary_format,
                   3, NULL, mori_count_bytes, NULL, R_NilValue);
  R_Serialize(object, &out);

  return buf.cur;
}

// Fixed-buffer write: no realloc, no bounds check ----------------------------
// Safe because the counting pass guarantees exact size.

static void mori_write_fixed(R_outpstream_t stream, void *src, int len) {
  mori_buf *buf = (mori_buf *) stream->data;
  memcpy(buf->buf + buf->cur, src, (size_t) len);
  buf->cur += (size_t) len;
}

void mori_serialize_into(unsigned char *dst, size_t size, SEXP object) {

  mori_buf buf = {.buf = dst, .len = size, .cur = 0};
  struct R_outpstream_st out;

  R_InitOutPStream(&out, (R_pstream_data_t) &buf, R_pstream_binary_format,
                   3, NULL, mori_write_fixed, NULL, R_NilValue);
  R_Serialize(object, &out);
}

// Read callbacks for unserialize-from-buffer ---------------------------------

static void mori_read_bytes(R_inpstream_t stream, void *dst, int len) {
  mori_buf *buf = (mori_buf *) stream->data;
  size_t n = (size_t) len;
  if (buf->cur + n > buf->len) n = buf->len - buf->cur;
  memcpy(dst, buf->buf + buf->cur, n);
  buf->cur += n;
}

SEXP mori_unserialize_from(unsigned char *src, size_t size) {

  mori_buf buf = {.buf = src, .len = size, .cur = 0};
  struct R_inpstream_st in;

  R_InitInPStream(&in, (R_pstream_data_t) &buf, R_pstream_binary_format,
                  NULL, mori_read_bytes, NULL, R_NilValue);
  return R_Unserialize(&in);
}

