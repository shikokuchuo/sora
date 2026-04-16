#' Share an R Object via Shared Memory
#'
#' Write an R object into shared memory and return a shared version of the
#' object. For Tier 2 objects, the result is ALTREP-backed, providing
#' zero-copy access to the shared memory pages. For Tier 1 objects, the
#' result is a plain R object.
#'
#' @param x an R object.
#'
#' @return The shared object. For Tier 2 objects (atomic vectors,
#'   character vectors, lists/data frames with atomic columns), an
#'   ALTREP-backed object. For Tier 1 objects (environments, closures,
#'   language objects), the object is returned unchanged.
#'
#' @details
#' Two tiers are used depending on the object type:
#' \itemize{
#'   \item \strong{Tier 2 (zero-copy)}: Atomic vectors (including those
#'     with attributes such as names, dim, class, or levels), character
#'     vectors, and lists/data frames with atomic columns are backed by
#'     ALTREP, providing direct access to shared memory pages. Character
#'     strings are accessed lazily per element. ALTREP objects serialize
#'     compactly as the SHM name (~30 bytes). Attributes are serialized
#'     into the SHM region alongside the data.
#'   \item \strong{Tier 1}: All other R objects are returned unchanged.
#' }
#'
#' The shared memory region is managed automatically. It stays alive as
#' long as the returned object (or any element extracted from it) is
#' referenced in R, and is freed by the garbage collector when no
#' references remain.
#'
#' \strong{Important}: always assign the result of \code{sora()} to a
#' variable. The shared memory is kept alive by the R object reference —
#' if the result is used as a temporary (not assigned), the garbage
#' collector may free the shared memory before a consumer process has
#' mapped it.
#'
#' @examples
#' x <- sora(rnorm(100))
#' sum(x)
#'
#' @seealso [map_shared()] to open a shared region by name,
#'   [shared_name()] to extract the SHM name.
#'
#' @export
sora <- function(x) .Call(sora_create, x)

#' Open Shared Memory by Name
#'
#' Open a shared memory region identified by a name string and return
#' the R object. For Tier 2 objects, the result is ALTREP-backed,
#' providing zero-copy access to the shared memory pages.
#'
#' @param name a character string name identifying the shared memory
#'   region, as returned by [shared_name()].
#'
#' @return The R object stored in the shared memory region.
#'
#' @examples
#' x <- sora(1:100)
#' nm <- shared_name(x)
#' y <- map_shared(nm)
#' sum(y)
#'
#' @seealso [sora()] to create a shared object, [shared_name()] to
#'   extract the name.
#'
#' @export
map_shared <- function(name) .Call(sora_shm_open_and_wrap, name)

#' Test if an Object is Shared
#'
#' Returns `TRUE` if `x` is an ALTREP object backed by shared memory
#' (created by [sora()] or [map_shared()]), `FALSE` otherwise.
#'
#' @param x an R object.
#'
#' @return `TRUE` or `FALSE`.
#'
#' @examples
#' x <- sora(rnorm(100))
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
#' @param x a shared object as returned by [sora()] or [map_shared()].
#'
#' @return A character string identifying the shared memory region.
#'
#' @examples
#' x <- sora(rnorm(100))
#' shared_name(x)
#'
#' @seealso [map_shared()] to open a shared region by name.
#'
#' @export
shared_name <- function(x) .Call(sora_shm_name, x)
