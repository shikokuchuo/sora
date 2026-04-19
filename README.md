
<!-- README.md is generated from README.Rmd. Please edit that file -->

# mori

<!-- badges: start -->

[![Lifecycle:
experimental](https://img.shields.io/badge/lifecycle-experimental-orange.svg)](https://lifecycle.r-lib.org/articles/stages.html#experimental)
[![R-CMD-check](https://github.com/shikokuchuo/mori/actions/workflows/R-CMD-check.yaml/badge.svg)](https://github.com/shikokuchuo/mori/actions/workflows/R-CMD-check.yaml)
[![Codecov test
coverage](https://codecov.io/gh/shikokuchuo/mori/graph/badge.svg)](https://app.codecov.io/gh/shikokuchuo/mori)
<!-- badges: end -->

      ________
     /\ mori  \
    /  \       \
    \  /  森   /
     \/_______/

Shared Memory for R Objects

→ `share()` writes an R object into shared memory and returns a shared
version

→ ALTREP serialization hooks — shared objects serialize compactly and
work transparently with `serialize()` and `mirai()`

→ ALTREP-backed lazy access — a 100-column data frame is one `mmap`;
columns materialize on first touch

→ OS-level shared memory (POSIX / Win32) — pure C, no external
dependencies; read-only in other processes, preventing corruption of
shared data

→ Automatic cleanup — shared memory is freed when the R object is
garbage collected

<br />

## Installation

``` r
install.packages("mori")
```

## Quick Start

`share()` writes an R object once into shared memory and returns a
zero-copy ALTREP view. Shared objects serialize compactly via ALTREP
serialization hooks, working transparently with mirai and any R
serialization path. Shared memory is automatically freed when the object
is garbage collected.

``` r
library(mori)

# Share a vector — returns an ALTREP-backed object
x <- share(rnorm(1e6))
mean(x)
#> [1] 0.0007940251

# Serialized form is ~100 bytes, not ~8 MB
x |> serialize(NULL) |> length()
#> [1] 124
```

## Sharing by Name

`shared_name()` extracts the SHM name from a shared object.
`map_shared()` opens a shared region by name — useful for accessing the
same data from another process without serialization:

``` r
x <- share(1:1e6)

# Extract the SHM name
nm <- shared_name(x)
nm
#> [1] "/mori_10d8_1"

# Another process can map the same region by name
y <- map_shared(nm)
identical(x[], y[])
#> [1] TRUE
```

## Use with mirai

Shared objects can be sent to local daemons — the ALTREP serialization
hooks ensure only the SHM name crosses the wire, and the worker maps the
same physical memory.

``` r
library(lobstr)
library(mirai)

daemons(1)

x <- share(rnorm(1e6))

# Worker maps the same shared memory — 0 bytes copied
m <- mirai(list(mean = mean(x), size = lobstr::obj_size(x)), x = x)
m[]
#> $mean
#> [1] 0.000601325
#> 
#> $size
#> 840 B

daemons(0)
```

Elements of a shared list also serialize compactly — each element
travels as a reference to its position in the parent shared region, not
as the full data:

``` r
daemons(3)

# Share a list — all 3 vectors in a single shared region
x <- list(a = rnorm(1e6), b = rnorm(1e6), c = rnorm(1e6)) |> share()

# Each element is sent as (parent_name, index) — zero-copy on the worker
mirai_map(x, \(v) lobstr::obj_size(v) |> format())[.flat]
#>       a       b       c 
#> "840 B" "840 B" "840 B"

daemons(0)
```

## Why mori

Parallel computing multiplies memory. When 8 workers each need the same
210 MB dataset, that is 1.7 GB of serialization, transfer, and
deserialization — plus 8 separate copies consuming RAM.

mori eliminates all of it. `share()` writes data into shared memory
once. Each worker maps the same physical pages, receiving a reference of
~300 bytes instead of the full dataset — a payload ~700,000 times
smaller, which translates into a significant saving in total runtime:

``` r
daemons(8)

# 200 MB data frame — 5 columns × 5M rows
df <- as.data.frame(matrix(rnorm(25e6), ncol = 5))
shared_df <- share(df)

boot_mean <- \(i, data) colMeans(data[sample(nrow(data), replace = TRUE), ])

# Without mori — each daemon deserializes a full copy
mirai_map(1:8, boot_mean, data = df)[] |> system.time()
#>    user  system elapsed 
#>   2.208  40.825   6.098

# With mori — each daemon maps the same shared memory
mirai_map(1:8, boot_mean, data = shared_df)[] |> system.time()
#>    user  system elapsed 
#>   1.456  28.489   4.087

daemons(0)
```

## How It Works

### What gets shared

All atomic vector types and lists / data frames are written directly
into shared memory, with attributes (`class`, `names`, `dim`, `levels`,
`tzone`, …) preserved end-to-end. Pairlists are coerced to lists.
`share()` returns ALTREP wrappers that point into the shared pages — no
deserialization, no per-process memory allocation.

All other R objects (environments, closures, language objects) are
returned unchanged by `share()` — no shared memory region is created.

<figure>
<img src="man/figures/mori-diagram.svg"
alt="Diagram showing share() writing an object once into OS-backed shared memory, which is then memory-mapped by other processes using zero-copy ALTREP wrappers" />
<figcaption aria-hidden="true">Diagram showing share() writing an object
once into OS-backed shared memory, which is then memory-mapped by other
processes using zero-copy ALTREP wrappers</figcaption>
</figure>

### Lazy access

A data frame with 10 columns lives in a single shared region; a task
that touches 3 columns pays for 3. Character strings are accessed lazily
per element.

### Lifetime

Shared memory is managed by R’s garbage collector. The SHM region stays
alive as long as the shared object (or any element extracted from it) is
referenced in R. When no references remain, the garbage collector frees
the shared memory automatically.

**Important:** Always assign the result of `share()` to a variable. The
shared memory is kept alive by the R object reference — if the result is
used as a temporary (not assigned), the garbage collector may free the
shared memory before a consumer process has mapped it.

### Copy-on-write

Shared data is mapped read-only. Mutations are always local — R’s
copy-on-write mechanism ensures other processes continue reading the
original shared data:

- **Structural changes** to a list or data frame (adding, removing, or
  reordering elements) produce a regular R list. The shared region is
  unaffected.
- **Modifying values** within a shared vector (e.g., `X[1] <- 0`)
  materializes just that vector into a private copy. Other vectors in
  the same shared region stay zero-copy.

–

Please note that the mori project is released with a [Contributor Code
of Conduct](https://shikokuchuo.net/mori/CODE_OF_CONDUCT.html). By
contributing to this project, you agree to abide by its terms.
