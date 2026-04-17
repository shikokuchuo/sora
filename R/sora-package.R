#' sora: Shared Objects for R Applications
#'
#' Share R objects via shared memory with [share()], access them in
#' other processes with [map_shared()], using R's ALTREP framework for
#' zero-copy memory-mapped access. Shared objects serialize compactly
#' via ALTREP serialization hooks. Shared memory is automatically freed
#' when the R object is garbage collected.
#'
#' @useDynLib sora, .registration = TRUE
"_PACKAGE"
