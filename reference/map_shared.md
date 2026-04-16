# Open Shared Memory by Name

Open a shared memory region identified by a name string and return an
ALTREP-backed R object that reads directly from shared memory.

## Usage

``` r
map_shared(name)
```

## Arguments

- name:

  a character string name identifying the shared memory region, as
  returned by
  [`shared_name()`](https://shikokuchuo.net/sora/reference/shared_name.md).

## Value

The R object stored in the shared memory region, or `NULL` if `name` is
not a valid shared memory name (wrong type, length, `NA`, or missing the
`sora` prefix). If `name` is well-formed but the region is absent or
corrupted, an error is raised.

## See also

[`sora()`](https://shikokuchuo.net/sora/reference/sora.md) to create a
shared object,
[`shared_name()`](https://shikokuchuo.net/sora/reference/shared_name.md)
to extract the name.

## Examples

``` r
x <- sora(1:100)
nm <- shared_name(x)
y <- map_shared(nm)
sum(y)
#> [1] 5050
```
