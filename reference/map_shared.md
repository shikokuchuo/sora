# Open Shared Memory by Name

Open a shared memory region identified by a name string and return the R
object. For Tier 2 objects, the result is ALTREP-backed, providing
zero-copy access to the shared memory pages.

## Usage

``` r
map_shared(name)
```

## Arguments

- name:

  a character string name identifying the shared memory region, as
  returned by
  [`shared_name()`](https://shikokuchuo.github.io/sora/reference/shared_name.md).

## Value

The R object stored in the shared memory region.

## See also

[`sora()`](https://shikokuchuo.github.io/sora/reference/sora.md) to
create a shared object,
[`shared_name()`](https://shikokuchuo.github.io/sora/reference/shared_name.md)
to extract the name.

## Examples

``` r
x <- sora(1:100)
nm <- shared_name(x)
y <- map_shared(nm)
sum(y)
#> [1] 5050
```
