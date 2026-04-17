#' Create a Shared Object
#'
#' Write an R object into shared memory and return a version that other
#' processes on the same machine can map without copying.
#'
#' @param x an R object.
#'
#' @return For atomic vectors (including character vectors and those with
#'   attributes such as names, dim, class, or levels) and lists or data
#'   frames whose elements are such vectors, an ALTREP-backed object that
#'   reads directly from shared memory. For any other object (environments,
#'   closures, language objects, `NULL`), the input is returned
#'   unchanged with no shared memory region created.
#'
#' @details
#' Attributes are stored alongside the data in the shared memory region
#' and restored on the consumer side. Character vectors use a packed
#' layout and elements are materialised lazily on access. When serialised
#' (e.g. by [serialize()] or across a `mirai()` call), a
#' shared object is represented compactly by its SHM name (~30 bytes)
#' rather than by its contents.
#'
#' The shared memory region is managed automatically. It stays alive as
#' long as the returned object (or any element extracted from it) is
#' referenced in R, and is freed by the garbage collector when no
#' references remain.
#'
#' **Important**: always assign the result of `share()` to a
#' variable. The shared memory is kept alive by the R object reference —
#' if the result is used as a temporary (not assigned), the garbage
#' collector may free the shared memory before a consumer process has
#' mapped it.
#'
#' @examples
#' x <- share(rnorm(100))
#' sum(x)
#'
#' @seealso [map_shared()] to open a shared region by name,
#'   [shared_name()] to extract the SHM name.
#'
#' @export
share <- function(x) .Call(sora_create, x)

#' Open Shared Memory by Name
#'
#' Open a shared memory region identified by a name string and return an
#' ALTREP-backed R object that reads directly from shared memory.
#'
#' @param name a character string name identifying the shared memory
#'   region, as returned by [shared_name()].
#'
#' @return The R object stored in the shared memory region, or `NULL` if
#'   `name` is not a valid shared memory name (wrong type, length, `NA`,
#'   or missing the `sora` prefix). If `name` is well-formed but the
#'   region is absent or corrupted, an error is raised.
#'
#' @examples
#' x <- share(1:100)
#' nm <- shared_name(x)
#' y <- map_shared(nm)
#' sum(y)
#'
#' @seealso [share()] to create a shared object, [shared_name()] to
#'   extract the name.
#'
#' @export
map_shared <- function(name) .Call(sora_shm_open_and_wrap, name)

#' Test if an Object is Shared
#'
#' Returns `TRUE` if `x` is an ALTREP object backed by shared memory
#' (created by [share()] or [map_shared()]), `FALSE` otherwise.
#'
#' @param x an R object.
#'
#' @return `TRUE` or `FALSE`.
#'
#' @examples
#' x <- share(rnorm(100))
#' is_shared(x)
#' is_shared(rnorm(100))
#'
#' @export
is_shared <- function(x) .Call(sora_is_shared, x)

#' Extract Shared Memory Name
#'
#' Extract the SHM region name from a shared object. This name can be
#' passed to [map_shared()] to open the same region in another process.
#'
#' @param x a shared object as returned by [share()] or [map_shared()].
#'
#' @return A character string identifying the shared memory region, or
#'   the empty string `""` if `x` is not a shared object.
#'
#' @examples
#' x <- share(rnorm(100))
#' shared_name(x)
#'
#' @seealso [map_shared()] to open a shared region by name.
#'
#' @export
shared_name <- function(x) .Call(sora_shm_name, x)
