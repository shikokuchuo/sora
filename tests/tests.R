# sora unit tests — minitest framework

test_true <- function(x) invisible(stopifnot(isTRUE(x)))
test_false <- function(x) invisible(stopifnot(identical(x, FALSE)))
test_null <- function(x) invisible(stopifnot(is.null(x)))
test_equal <- function(x, y) invisible(stopifnot(all.equal(x, y)))
test_identical <- function(x, y) invisible(stopifnot(identical(x, y)))
test_class <- function(x, class) invisible(stopifnot(inherits(x, class)))
test_error <- function(x, msg = "") {
  tryCatch(
    {force(x); stop("expected error")},
    error = function(e) invisible(stopifnot(grepl(msg, conditionMessage(e))))
  )
}

library(sora)

# Unit tests: Tier 1 — pass-through for non-ALTREP types

x <- y ~ x + z
test_identical(share(x), x)

x <- NULL
test_null(share(x))

# Unit tests: Tier 2 — bare vector round-trip via map_shared

# Double
x <- as.double(1:1000)
sx <- share(x)
y <- map_shared(shared_name(sx))
test_equal(length(y), length(x))
test_equal(sum(y), sum(x))
test_identical(x, y[])

# Integer
x <- 1:1000L
sx <- share(x)
y <- map_shared(shared_name(sx))
test_identical(x, y[])

# Logical
x <- c(TRUE, FALSE, NA, TRUE, FALSE)
sx <- share(x)
y <- map_shared(shared_name(sx))
test_identical(x, y[])

# Raw
x <- as.raw(0:255)
sx <- share(x)
y <- map_shared(shared_name(sx))
test_identical(x, y[])

# Complex
x <- complex(10, real = 1:10, imaginary = 10:1)
sx <- share(x)
y <- map_shared(shared_name(sx))
test_identical(x, y[])

# Matrix (dim attribute)
m <- matrix(as.double(1:12), nrow = 3)
sm <- share(m)
n <- map_shared(shared_name(sm))
test_identical(dim(m), dim(n))
test_identical(as.double(m), as.double(n))

# Empty vector
x <- double(0)
sx <- share(x)
y <- map_shared(shared_name(sx))
test_equal(length(y), 0L)

# Unit tests: Tier 2 — ALTLIST round-trip

# Data frame with numeric and integer columns
df <- data.frame(a = as.double(1:100), b = 1:100, c = rep(TRUE, 100))
sdf <- share(df)
df2 <- map_shared(shared_name(sdf))
test_class(df2, "data.frame")
test_equal(nrow(df2), 100L)
test_identical(df$a, df2$a[])
test_identical(df$b, df2$b[])
test_identical(df$c, df2$c[])
test_identical(names(df), names(df2))

# Mixed list: atomic + character
lst <- list(x = as.double(1:50), y = letters, z = 1:10)
slst <- share(lst)
lst2 <- map_shared(shared_name(slst))
test_identical(lst$x, lst2$x[])
test_identical(lst$y, lst2$y)
test_identical(lst$z, lst2$z[])

# List with NULL elements (sentinel test)
lst <- list(NULL, 1:3, NULL)
slst <- share(lst)
lst2 <- map_shared(shared_name(slst))
test_null(lst2[[1]])
test_identical(lst[[2]], lst2[[2]][])
test_null(lst2[[3]])
# Access again to verify caching works
test_null(lst2[[1]])

# ALTLIST Duplicate
df <- data.frame(a = as.double(1:10), b = 1:10)
sdf <- share(df)
df2 <- map_shared(shared_name(sdf))
df3 <- df2
df3$a <- df3$a * 2
test_class(df3, "data.frame")
test_equal(df3$a, df$a * 2)

# Unit tests: ALTREP COW (copy-on-write)

x <- as.double(1:100)
sx <- share(x)
y <- map_shared(shared_name(sx))
original <- y[1]

# Write should trigger materialization
y2 <- y
y2[1] <- 999
test_equal(y2[1], 999)

# SHM data unchanged — open again and verify
z <- map_shared(shared_name(sx))
test_equal(z[1], original)

# Unit tests: R API — share / map_shared / shared_name / is_shared

# Return format — share returns usable objects
x <- share(as.double(1:10))
test_true(is.double(x))
test_equal(length(x), 10L)

# Bare vector round-trip
x <- as.double(1:100)
y <- share(x)
test_identical(x, y[])

# Integer vector
x <- 1:1000L
y <- share(x)
test_identical(x, y[])

# Data frame
df <- data.frame(a = as.double(1:50), b = 1:50)
df2 <- share(df)
test_class(df2, "data.frame")
test_identical(df$a, df2$a[])
test_identical(df$b, df2$b[])

# Character vector (Tier 2 ALTSTRING)
x <- letters
y <- share(x)
test_identical(x, y)

# Matrix
m <- matrix(1:12, nrow = 3)
n <- share(m)
test_identical(dim(m), dim(n))
test_identical(as.integer(m), as.integer(n))

# shared_name + map_shared round-trip
x <- share(as.double(1:50))
nm <- shared_name(x)
test_true(is.character(nm))
test_true(nchar(nm) > 0L)
y <- map_shared(nm)
test_identical(x[], y[])

# is_shared
x <- share(1:10)
test_true(is_shared(x))
test_false(is_shared(1:10))

# Unit tests: Tier 2 — ALTSTRING round-trip

# Basic string vector
x <- c("hello", "world", "foo")
sx <- share(x)
y <- map_shared(shared_name(sx))
test_identical(x, y)

# String vector with NA
x <- c("a", NA, "b", NA, "c")
sx <- share(x)
y <- map_shared(shared_name(sx))
test_identical(x, y)
test_true(is.na(y[2]))
test_true(is.na(y[4]))

# Empty strings
x <- c("", "", "notempty", "")
sx <- share(x)
y <- map_shared(shared_name(sx))
test_identical(x, y)

# Empty character vector
x <- character(0)
sx <- share(x)
y <- map_shared(shared_name(sx))
test_equal(length(y), 0L)
test_identical(x, y)

# UTF-8 strings
x <- c("\u00e9", "\u00fc", "\u2603")
sx <- share(x)
y <- map_shared(shared_name(sx))
test_identical(x, y)

# Large character vector
x <- paste0("str_", seq_len(10000))
sx <- share(x)
y <- map_shared(shared_name(sx))
test_equal(length(y), 10000L)
test_identical(x[1], y[1])
test_identical(x[10000], y[10000])
test_identical(x, y)

# Character column in ALTLIST (data frame)
df <- data.frame(id = c("a", "b", "c"), val = 1:3, stringsAsFactors = FALSE)
sdf <- share(df)
df2 <- map_shared(shared_name(sdf))
test_class(df2, "data.frame")
test_identical(df$id, df2$id)
test_identical(df$val, df2$val[])

# Character element in list
lst <- list(x = letters, y = 1:10, z = c(NA, "test"))
slst <- share(lst)
lst2 <- map_shared(shared_name(slst))
test_identical(lst$x, lst2$x)
test_identical(lst$y, lst2$y[])
test_identical(lst$z, lst2$z)

# COW on string vector
x <- c("original", "data")
sx <- share(x)
y <- map_shared(shared_name(sx))
y2 <- y
y2[1] <- "modified"
test_identical(y2[1], "modified")
# SHM unchanged
z <- map_shared(shared_name(sx))
test_identical(z[1], "original")

# Unit tests: ALTREP serialization hooks

# Standalone double vector round-trip
x <- share(rnorm(1000))
buf <- serialize(x, NULL)
y <- unserialize(buf)
test_equal(length(y), 1000L)
test_identical(x[], y[])

# Standalone integer round-trip
x <- share(1:500L)
buf <- serialize(x, NULL)
y <- unserialize(buf)
test_identical(x[], y[])

# Standalone string round-trip
x <- share(letters)
buf <- serialize(x, NULL)
y <- unserialize(buf)
test_identical(x[], y[])

# List/data frame round-trip
df <- data.frame(a = as.double(1:100), b = 1:100)
x <- share(df)
buf <- serialize(x, NULL)
y <- unserialize(buf)
test_class(y, "data.frame")
test_identical(x$a[], y$a[])
test_identical(x$b[], y$b[])

# Matrix with dim round-trip
m <- matrix(rnorm(120), nrow = 10)
x <- share(m)
buf <- serialize(x, NULL)
y <- unserialize(buf)
test_identical(dim(x), dim(y))
test_identical(x[], y[])

# COW-materialized falls back to normal serialization
x <- share(as.double(1:100))
x[1] <- 999
buf <- serialize(x, NULL)
y <- unserialize(buf)
test_equal(y[1], 999)
test_equal(length(y), 100L)
# Normal serialization: should be larger than compact
test_true(length(buf) > 200)

# Element vector from list serializes compactly
df <- share(data.frame(a = as.double(1:100), b = 1:100))
col <- df$a
buf <- serialize(col, NULL)
test_true(length(buf) < 1000)
y <- unserialize(buf)
test_identical(col[], y[])

# Element string from list serializes compactly
x <- share(list(a = 1:100, b = letters))
elem <- x[[2]]
buf <- serialize(elem, NULL)
test_true(length(buf) < 1000)
y <- unserialize(buf)
test_identical(elem[], y[])

# COW-materialized element falls back to normal serialization
x <- share(list(a = 1:10))
elem <- x[[1]]
elem[1] <- 99L
buf <- serialize(elem, NULL)
y <- unserialize(buf)
test_identical(as.integer(y), c(99L, 2:10))

# Compact serialization: shared bytes << full data
x <- share(rnorm(1e5))
buf <- serialize(x, NULL)
test_true(length(buf) < 1000)

# Unit tests: Tier 2 — attributed vectors

# Named double vector
x <- c(a = 1, b = 2, c = 3)
y <- share(x)
test_identical(x, y[])
test_identical(names(x), names(y))

# Named integer vector
x <- c(a = 1L, b = 2L, c = 3L)
y <- share(x)
test_identical(x, y[])
test_identical(names(x), names(y))

# Factor
x <- factor(c("b", "a", "c", "a", "b"))
y <- share(x)
test_class(y, "factor")
test_identical(levels(x), levels(y))
test_identical(as.integer(x), as.integer(y))

# Date
x <- as.Date("2024-01-01") + 0:9
y <- share(x)
test_class(y, "Date")
test_identical(x, y[])

# POSIXct
x <- as.POSIXct("2024-01-01 12:00:00", tz = "UTC") + 0:4
y <- share(x)
test_class(y, "POSIXct")
test_identical(x, y[])

# Named character vector
x <- c(a = "hello", b = "world")
y <- share(x)
test_identical(x, y)
test_identical(names(x), names(y))

# Factor column in data frame (Tier 2 via ALTLIST)
df <- data.frame(id = factor(c("x", "y", "z")), val = 1:3)
df2 <- share(df)
test_class(df2, "data.frame")
test_class(df2$id, "factor")
test_identical(levels(df$id), levels(df2$id))
test_identical(as.integer(df$id), as.integer(df2$id))
test_identical(df$val, df2$val[])

# Date column in data frame
df <- data.frame(date = as.Date("2024-01-01") + 0:4, val = 1:5)
df2 <- share(df)
test_class(df2, "data.frame")
test_class(df2$date, "Date")
test_identical(df$date, df2$date[])
test_identical(df$val, df2$val[])

# map_shared preserves attrs
x <- share(c(a = 1, b = 2, c = 3))
nm <- shared_name(x)
y <- map_shared(nm)
test_identical(names(x), names(y))
test_identical(x[], y[])

# map_shared preserves factor
x <- share(factor(c("a", "b", "c")))
nm <- shared_name(x)
y <- map_shared(nm)
test_class(y, "factor")
test_identical(levels(x), levels(y))

# Serialization round-trip with named vector
x <- share(c(a = 1, b = 2, c = 3))
buf <- serialize(x, NULL)
y <- unserialize(buf)
test_identical(names(x), names(y))
test_identical(x[], y[])

# Serialization round-trip with factor
x <- share(factor(c("x", "y", "x")))
buf <- serialize(x, NULL)
y <- unserialize(buf)
test_class(y, "factor")
test_identical(levels(x), levels(y))
test_identical(as.integer(x), as.integer(y))

# Serialization round-trip with Date
x <- share(as.Date("2024-01-01") + 0:4)
buf <- serialize(x, NULL)
y <- unserialize(buf)
test_class(y, "Date")
test_identical(x[], y[])

# Serialization round-trip with named character
x <- share(c(a = "hello", b = "world"))
buf <- serialize(x, NULL)
y <- unserialize(buf)
test_identical(names(x), names(y))
test_identical(x[], y[])

# Data frame with factor + Date columns round-trip
df <- data.frame(
  id = factor(c("a", "b")),
  date = as.Date("2024-01-01") + 0:1,
  val = c(10, 20)
)
x <- share(df)
buf <- serialize(x, NULL)
y <- unserialize(buf)
test_class(y, "data.frame")
test_class(y$id, "factor")
test_class(y$date, "Date")
test_identical(df$id, y$id)
test_identical(df$date, y$date[])
test_identical(df$val, y$val[])

# Element from list with factor column
df <- share(data.frame(id = factor(c("a", "b", "c")), val = 1:3))
col <- df$id
buf <- serialize(col, NULL)
y <- unserialize(buf)
test_class(y, "factor")
test_identical(levels(col), levels(y))
test_identical(as.integer(col), as.integer(y))

# Unit tests: GC-based automatic cleanup

# Shared object can be GC'd without explicit unshare
x <- share(1:100)
nm <- shared_name(x)
rm(x)
gc()
# SHM should be unlinked — map_shared should fail
test_error(map_shared(nm), "not found")

# Shared list GC cleans up SHM
x <- share(data.frame(a = 1:10, b = as.double(1:10)))
nm <- shared_name(x)
rm(x)
gc()
test_error(map_shared(nm), "not found")

# Shared string GC cleans up SHM
x <- share(letters)
nm <- shared_name(x)
rm(x)
gc()
test_error(map_shared(nm), "not found")

# Element keeps parent SHM alive
x <- share(data.frame(a = 1:10, b = as.double(1:10)))
nm <- shared_name(x)
col <- x$a
rm(x)
gc()
# Parent SHM should still be accessible via element reference
y <- map_shared(nm)
test_class(y, "data.frame")
rm(col, y)
gc()
# Now it should be gone
test_error(map_shared(nm), "not found")

# Unit tests: Pairlist input (coerced to VECSXP)

pl <- pairlist(a = 1, b = 2L, c = "hello")
spl <- share(pl)
test_equal(length(spl), 3L)
test_identical(spl[[1]][], 1)
test_identical(spl[[2]][], 2L)
test_identical(spl[[3]], "hello")

# Pairlist via map_shared
nm <- shared_name(spl)
spl2 <- map_shared(nm)
test_identical(spl[[1]][], spl2[[1]][])
test_identical(spl[[2]][], spl2[[2]][])
test_identical(spl[[3]], spl2[[3]])

# Unit tests: Tier 1 elements in list

lst <- list(fn = sum, val = 1:5, env = globalenv())
slst <- share(lst)
test_identical(slst$fn, sum)
test_identical(slst$val[], 1:5)
test_identical(slst$env, globalenv())

# Tier 1 list serialize round-trip
buf <- serialize(slst, NULL)
y <- unserialize(buf)
test_identical(y$fn, sum)
test_identical(y$val[], 1:5)

# Unit tests: Named character vector inside a list

lst <- list(x = c(a = "hello", b = "world"), y = 1:3)
slst <- share(lst)
test_identical(slst$x, c(a = "hello", b = "world"))
test_identical(names(slst$x), c("a", "b"))
test_identical(slst$y[], 1:3)

# map_shared round-trip preserves string element attrs
nm <- shared_name(slst)
slst2 <- map_shared(nm)
test_identical(slst2$x, c(a = "hello", b = "world"))
test_identical(names(slst2$x), c("a", "b"))

# Element compact serialization preserves string attrs
elem <- slst[[1]]
buf <- serialize(elem, NULL)
y <- unserialize(buf)
test_identical(y, c(a = "hello", b = "world"))
test_identical(names(y), c("a", "b"))

# Unit tests: String vector Dataptr materialization

# make.unique() uses STRING_PTR_RO internally, triggering sora_string_Dataptr
x <- share(c("a", "a", "b"))
y <- map_shared(shared_name(x))
z <- make.unique(y)
test_identical(z, c("a", "a.1", "b"))
# After materialization: Length, Elt use data2 paths
test_equal(length(y), 3L)
test_identical(y[1], "a")
test_identical(y[], c("a", "a", "b"))

# duplicated() on string ALTREP
x2 <- share(c("x", "y", "x"))
y2 <- map_shared(shared_name(x2))
d <- duplicated(y2)
test_identical(d, c(FALSE, FALSE, TRUE))

# Unit tests: COW-materialized string serialization

x <- share(c("alpha", "beta", "gamma"))
y <- map_shared(shared_name(x))
y2 <- y
y2[1] <- "delta"
buf <- serialize(y2, NULL)
z <- unserialize(buf)
test_identical(z, c("delta", "beta", "gamma"))

# Unit tests: shared_name on various types

# shared_name on list/data frame
df <- share(data.frame(a = 1:3))
nm <- shared_name(df)
test_true(is.character(nm))
test_true(nchar(nm) > 0L)

# shared_name on string vector
s <- share(letters)
nm <- shared_name(s)
test_true(is.character(nm))

# shared_name returns "" on non-shared object (including plain strings)
test_identical(shared_name(1:10), "")
test_identical(shared_name(NULL), "")
test_identical(shared_name(list(a = 1)), "")
test_identical(shared_name("a"), "")
test_identical(shared_name(nm), "")

# Unit tests: map_shared on invalid input

# Malformed input returns NULL silently (symmetric with shared_name -> "")
test_null(map_shared(""))
test_null(map_shared("junk"))
test_null(map_shared(NULL))
test_null(map_shared(NA_character_))
test_null(map_shared(character(0)))
test_null(map_shared(c("a", "b")))
test_null(map_shared(1L))
test_null(map_shared(list("a")))

# Round-trip through sentinels composes without tryCatch
test_null(map_shared(shared_name(1:10)))

# Well-formed name but region absent errors (platform-specific prefix)
bogus <- if (.Platform$OS.type == "windows") "Local\\sora_nonexistent_xyz" else "/sora_nonexistent_xyz"
test_error(map_shared(bogus), "not found")

# Unit tests: is_shared on various types

# Shared list
df <- share(data.frame(a = 1:3))
test_true(is_shared(df))

# Shared string
s <- share(letters)
test_true(is_shared(s))

# Various non-shared types
test_false(is_shared(NULL))
test_false(is_shared("hello"))
test_false(is_shared(data.frame(a = 1)))
test_false(is_shared(list(1, 2)))

# Unit tests: Materialized vec Dataptr_or_null

x <- share(as.double(1:100))
x[1] <- 999
# sum() uses REAL_RO which goes through Dataptr_or_null on the materialized vec
test_equal(sum(x), 999 + sum(2:100))

