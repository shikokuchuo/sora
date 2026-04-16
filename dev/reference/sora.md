# Share an R Object via Shared Memory

Write an R object into shared memory and return a shared version of the
object. For Tier 2 objects, the result is ALTREP-backed, providing
zero-copy access to the shared memory pages. For Tier 1 objects, the
result is a plain R object.

## Usage

``` r
sora(x)
```

## Arguments

- x:

  an R object.

## Value

The shared object. For Tier 2 objects (atomic vectors, character
vectors, lists/data frames with atomic columns), an ALTREP-backed
object. For Tier 1 objects (environments, closures, language objects),
the object is returned unchanged.

## Details

Two tiers are used depending on the object type:

- **Tier 2 (zero-copy)**: Atomic vectors (including those with
  attributes such as names, dim, class, or levels), character vectors,
  and lists/data frames with atomic columns are backed by ALTREP,
  providing direct access to shared memory pages. Character strings are
  accessed lazily per element. ALTREP objects serialize compactly as the
  SHM name (~30 bytes). Attributes are serialized into the SHM region
  alongside the data.

- **Tier 1**: All other R objects are returned unchanged.

The shared memory region is managed automatically. It stays alive as
long as the returned object (or any element extracted from it) is
referenced in R, and is freed by the garbage collector when no
references remain.

**Important**: always assign the result of `sora()` to a variable. The
shared memory is kept alive by the R object reference — if the result is
used as a temporary (not assigned), the garbage collector may free the
shared memory before a consumer process has mapped it.

## See also

[`map_shared()`](https://shikokuchuo.github.io/sora/dev/reference/map_shared.md)
to open a shared region by name,
[`shared_name()`](https://shikokuchuo.github.io/sora/dev/reference/shared_name.md)
to extract the SHM name.

## Examples

``` r
x <- sora(rnorm(100))
sum(x)
#> [1] 18.85989
```
