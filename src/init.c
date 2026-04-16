#include <R_ext/Rdynload.h>
#include "sora.h"

SEXP sora_create_call(SEXP);
SEXP sora_shm_open_and_wrap_call(SEXP);
SEXP sora_is_shared_call(SEXP);
SEXP sora_shm_name_call(SEXP);

static const R_CallMethodDef CallEntries[] = {
  {"sora_create",             (DL_FUNC) &sora_create_call,             1},
  {"sora_shm_open_and_wrap",  (DL_FUNC) &sora_shm_open_and_wrap_call,  1},
  {"sora_is_shared",          (DL_FUNC) &sora_is_shared_call,          1},
  {"sora_shm_name",           (DL_FUNC) &sora_shm_name_call,           1},
  {NULL, NULL, 0}
};

void R_init_sora(DllInfo *dll) {
  R_registerRoutines(dll, NULL, CallEntries, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
  sora_altrep_init(dll);
}
