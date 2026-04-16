# sora

``` R
  ________
 /\ sora  \
/  \       \
\  /  空   /
 \/_______/
```

Shared Objects for R Applications

→ [`sora()`](https://shikokuchuo.net/sora/reference/sora.md) writes an R
object into shared memory and returns a shared version

→ ALTREP serialization hooks — shared objects serialize compactly

→ ALTREP-backed lazy access — columns materialize on first touch, not
before

→ OS-level shared memory (POSIX / Win32), no external dependencies

→ Automatic cleanup — shared memory is freed by R’s garbage collector

  

### Installation

``` r
install.packages("sora")
```

### Quick Start

[`sora()`](https://shikokuchuo.net/sora/reference/sora.md) writes an R
object into shared memory and returns a shared version backed by
zero-copy ALTREP. Shared objects serialize compactly via ALTREP
serialization hooks, working transparently with mirai and any R
serialization path. Shared memory is automatically freed when the object
is garbage collected.

``` r
library(sora)

# Share a vector — returns an ALTREP-backed object
x <- sora(rnorm(1e6))
mean(x)
#> [1] -0.0003908872

# Serialized form is ~100 bytes, not ~8 MB
length(serialize(x, NULL))
#> [1] 124
```

### Sharing by Name

[`shared_name()`](https://shikokuchuo.net/sora/reference/shared_name.md)
extracts the SHM name from a shared object.
[`map_shared()`](https://shikokuchuo.net/sora/reference/map_shared.md)
opens a shared region by name — useful for accessing the same data from
another process without serialization:

``` r
x <- sora(1:1e6)

# Extract the SHM name
nm <- shared_name(x)
nm
#> [1] "/sora_9d7c_1"

# Another process can map the same region by name
y <- map_shared(nm)
identical(x[], y[])
#> [1] TRUE
```

### Use with mirai

Shared objects can be sent to local daemons — the ALTREP serialization
hooks ensure only the SHM name crosses the wire, and the worker maps the
same physical memory.

``` r
library(lobstr)
library(mirai)

daemons(1)

x <- sora(rnorm(1e6))

# Worker maps the same shared memory — 0 bytes copied
m <- mirai(list(mean = mean(x), size = lobstr::obj_size(x)), x = x)
m[]
#> $mean
#> [1] -5.607519e-05
#> 
#> $size
#> 792 B

daemons(0)
```

Elements of a shared list also serialize compactly — each element
travels as a reference to its position in the parent shared region, not
as the full data:

``` r
daemons(3)

# Share a list — all 3 vectors in a single shared region
x <- sora(list(a = rnorm(1e6), b = rnorm(1e6), c = rnorm(1e6)))

# Each element is sent as (parent_name, index) — zero-copy on the worker
mirai_map(x, \(v) list(mean = mean(v), size = lobstr::obj_size(v)))[.flat]
#> $a.mean
#> [1] -0.000414157
#> 
#> $a.size
#> 728 B
#> 
#> $b.mean
#> [1] -0.001099396
#> 
#> $b.size
#> 728 B
#> 
#> $c.mean
#> [1] -0.0008098337
#> 
#> $c.size
#> 728 B

daemons(0)
```

### Why sora

Parallel computing multiplies memory. When 8 workers each need the same
210 MB dataset, that is 1.7 GB of serialization, transfer, and
deserialization — plus 8 separate copies consuming RAM.

sora eliminates all of it.
[`sora()`](https://shikokuchuo.net/sora/reference/sora.md) writes data
into shared memory once. Each worker maps the same physical pages,
receiving a reference of ~300 bytes instead of the full dataset — a
payload ~700,000 times smaller, which translates into a significant
saving in total runtime:

``` r
daemons(8)

set.seed(42)
n <- 5000000L
df <- data.frame(
  x1 = rnorm(n), x2 = rnorm(n), x3 = rnorm(n),
  x4 = runif(n), x5 = runif(n),
  group = sample.int(100L, n, replace = TRUE)
)

shared_df <- sora(df)

# Per-task payload: ~210 MB vs ~300 bytes
length(serialize(df, NULL))
#> [1] 220000263
length(serialize(shared_df, NULL))
#> [1] 304

boot_means <- function(seed, data) {
  set.seed(seed)
  idx <- sample.int(nrow(data), replace = TRUE)
  colMeans(data[idx, ])
}

seeds <- seq_len(8L)

# Without sora — each daemon deserializes a full copy
system.time(mirai_map(seeds, boot_means, data = df)[])
#>    user  system elapsed 
#>   2.277  41.268   6.574

# With sora — each daemon maps the same shared memory
system.time(mirai_map(seeds, boot_means, data = shared_df)[])
#>    user  system elapsed 
#>   1.443  29.058   4.364

daemons(0)
```

### How It Works

#### What gets shared

All atomic vector types and lists / data frames are written directly
into shared memory, with attributes (`class`, `names`, `dim`, `levels`,
`tzone`, …) preserved end-to-end. Pairlists are coerced to lists.
[`sora()`](https://shikokuchuo.net/sora/reference/sora.md) returns
ALTREP wrappers that point into the shared pages — no deserialization,
no per-process memory allocation.

All other R objects (environments, closures, language objects) are
returned unchanged by
[`sora()`](https://shikokuchuo.net/sora/reference/sora.md) — no shared
memory region is created.

![Diagram showing sora() writing an object once into OS-backed shared
memory, which is then memory-mapped by other processes using zero-copy
ALTREP wrappers](reference/figures/sora-diagram.svg)

Diagram showing sora() writing an object once into OS-backed shared
memory, which is then memory-mapped by other processes using zero-copy
ALTREP wrappers

#### Lazy access

A data frame with 10 columns lives in a single shared region; a task
that touches 3 columns pays for 3. Character strings are accessed lazily
per element.

#### Lifetime

Shared memory is managed by R’s garbage collector. The SHM region stays
alive as long as the shared object (or any element extracted from it) is
referenced in R. When no references remain, the garbage collector frees
the shared memory automatically.

**Important:** Always assign the result of
[`sora()`](https://shikokuchuo.net/sora/reference/sora.md) to a
variable. The shared memory is kept alive by the R object reference — if
the result is used as a temporary (not assigned), the garbage collector
may free the shared memory before a consumer process has mapped it.

#### Copy-on-write

Shared data is mapped read-only. Mutations are always local — R’s
copy-on-write mechanism ensures other processes continue reading the
original shared data:

- **Structural changes** to a list or data frame (adding, removing, or
  reordering elements) produce a regular R list. The shared region is
  unaffected.
- **Modifying values** within a shared vector (e.g., `X[1] <- 0`)
  materializes just that vector into a private copy. Other vectors in
  the same shared region stay zero-copy.

### Design Highlights

#### Transparent IPC

All atomic vector types (via ALTREAL, ALTINTEGER, ALTLOGICAL, ALTRAW,
ALTCOMPLEX, ALTSTRING) and lists / data frames (via ALTLIST) work with
[`serialize()`](https://rdrr.io/r/base/serialize.html) and
[`mirai()`](https://mirai.r-lib.org/reference/mirai.html) — no
descriptor or attach step.

#### Zero dependencies

Pure C against OS APIs (POSIX SHM, Win32 file mappings) — no Rcpp, no
Boost, no C++ on the build path.

#### Single SHM region per compound object

A 100-column data frame is one `mmap`, not 100.

#### Read-only consumer mappings

`PROT_READ` is OS-enforced; a buggy worker cannot corrupt the shared
region.

#### GC-driven lifetime

Finalizers chain `munmap` + `unlink` through R’s external pointer
hierarchy, with session-exit cleanup — no stranded regions, no
descriptor files to manage.

–

Please note that the sora project is released with a [Contributor Code
of Conduct](https://shikokuchuo.net/sora/CODE_OF_CONDUCT.html). By
contributing to this project, you agree to abide by its terms.
