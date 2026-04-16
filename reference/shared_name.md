# Extract Shared Memory Name

Extract the SHM region name from a shared object. This name can be
passed to
[`map_shared()`](https://shikokuchuo.github.io/sora/reference/map_shared.md)
to open the same region in another process.

## Usage

``` r
shared_name(x)
```

## Arguments

- x:

  a shared object as returned by
  [`sora()`](https://shikokuchuo.github.io/sora/reference/sora.md) or
  [`map_shared()`](https://shikokuchuo.github.io/sora/reference/map_shared.md).

## Value

A character string identifying the shared memory region.

## See also

[`map_shared()`](https://shikokuchuo.github.io/sora/reference/map_shared.md)
to open a shared region by name.

## Examples

``` r
x <- sora(rnorm(100))
shared_name(x)
#> [1] "/sora_19a7_2"
```
