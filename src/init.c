#include <R_ext/Rdynload.h>
#include "mori.h"

SEXP mori_create(SEXP);
SEXP mori_shm_open_and_wrap(SEXP);
SEXP mori_is_shared(SEXP);
SEXP mori_shm_name(SEXP);

static const R_CallMethodDef CallEntries[] = {
  {"mori_create",             (DL_FUNC) &mori_create,             1},
  {"mori_shm_open_and_wrap",  (DL_FUNC) &mori_shm_open_and_wrap,  1},
  {"mori_is_shared",          (DL_FUNC) &mori_is_shared,          1},
  {"mori_shm_name",           (DL_FUNC) &mori_shm_name,           1},
  {NULL, NULL, 0}
};

void R_init_mori(DllInfo *dll) {
  R_registerRoutines(dll, NULL, CallEntries, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
  mori_altrep_init(dll);
}
