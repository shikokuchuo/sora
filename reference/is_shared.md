# Test if an Object is Shared

Returns `TRUE` if `x` is an ALTREP object backed by shared memory
(created by
[`sora()`](https://shikokuchuo.github.io/sora/reference/sora.md) or
[`map_shared()`](https://shikokuchuo.github.io/sora/reference/map_shared.md)),
`FALSE` otherwise.

## Usage

``` r
is_shared(x)
```

## Arguments

- x:

  an R object.

## Value

`TRUE` or `FALSE`.

## Examples

``` r
x <- sora(rnorm(100))
is_shared(x)
#> [1] TRUE
is_shared(rnorm(100))
#> [1] FALSE
```
