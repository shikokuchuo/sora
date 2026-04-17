# Create a Shared Object

Write an R object into shared memory and return a version that other
processes on the same machine can map without copying.

## Usage

``` r
share(x)
```

## Arguments

- x:

  an R object.

## Value

For atomic vectors (including character vectors and those with
attributes such as names, dim, class, or levels) and lists or data
frames whose elements are such vectors, an ALTREP-backed object that
reads directly from shared memory. For any other object (environments,
closures, language objects, `NULL`), the input is returned unchanged
with no shared memory region created.

## Details

Attributes are stored alongside the data in the shared memory region and
restored on the consumer side. Character vectors use a packed layout and
elements are materialised lazily on access. When serialised (e.g. by
[`serialize()`](https://rdrr.io/r/base/serialize.html) or across a
[`mirai()`](https://mirai.r-lib.org/reference/mirai.html) call), a
shared object is represented compactly by its SHM name (~30 bytes)
rather than by its contents.

The shared memory region is managed automatically. It stays alive as
long as the returned object (or any element extracted from it) is
referenced in R, and is freed by the garbage collector when no
references remain.

**Important**: always assign the result of `share()` to a variable. The
shared memory is kept alive by the R object reference — if the result is
used as a temporary (not assigned), the garbage collector may free the
shared memory before a consumer process has mapped it.

## See also

[`map_shared()`](https://shikokuchuo.net/sora/reference/map_shared.md)
to open a shared region by name,
[`shared_name()`](https://shikokuchuo.net/sora/reference/shared_name.md)
to extract the SHM name.

## Examples

``` r
x <- share(rnorm(100))
sum(x)
#> [1] -3.00474
```
