test_that("nested list round-trips via map_shared", {
  x <- list(a = 1:5, b = list(c = 6:10, d = letters))
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_identical(x$a, y$a[])
  expect_identical(x$b$c, y$b$c[])
  expect_identical(x$b$d, y$b$d)
})

test_that("inner list on consumer is still an ALTLIST", {
  x <- list(inner = list(a = 1:3))
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_true(is_shared(y$inner))
  expect_true(is_shared(y$inner$a))
})

test_that("data frame with list column round-trips", {
  df <- data.frame(a = 1:3)
  df$b <- list(1:5, 6:10, 11:15)
  sdf <- share(df)
  df2 <- map_shared(shared_name(sdf))
  expect_s3_class(df2, "data.frame")
  expect_identical(df2$a[], 1:3)
  expect_identical(df2$b[[1]][], 1:5)
  expect_identical(df2$b[[2]][], 6:10)
  expect_identical(df2$b[[3]][], 11:15)
})

test_that("list of data frames (3 levels) round-trips", {
  x <- list(
    df1 = data.frame(a = 1:3, b = 4:6),
    df2 = data.frame(x = 7:9, y = 10:12)
  )
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_s3_class(y$df1, "data.frame")
  expect_s3_class(y$df2, "data.frame")
  expect_identical(x$df1$a, y$df1$a[])
  expect_identical(x$df2$y, y$df2$y[])
})

test_that("named sub-list preserves names", {
  x <- list(outer = list(first = 1:3, second = 4:6))
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_identical(names(y$outer), c("first", "second"))
})

test_that("classed sub-list preserves class", {
  inner <- structure(list(a = 1:3), class = "myclass")
  x <- list(wrapped = inner)
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_s3_class(y$wrapped, "myclass")
  expect_identical(y$wrapped$a[], 1:3)
})

test_that("factor inside a sub-list is preserved", {
  x <- list(deep = list(f = factor(c("a", "b", "a"))))
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_s3_class(y$deep$f, "factor")
  expect_identical(levels(y$deep$f), c("a", "b"))
  expect_identical(as.integer(y$deep$f), c(1L, 2L, 1L))
})

test_that("empty sub-list round-trips", {
  x <- list(empty = list())
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_identical(length(y$empty), 0L)
})

test_that("NULL elements inside a sub-list round-trip", {
  x <- list(outer = list(NULL, 1:3, NULL))
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_null(y$outer[[1]])
  expect_identical(y$outer[[2]][], 1:3)
  expect_null(y$outer[[3]])
})

test_that("sub-list alone round-trips through serialize (length>=1 path)", {
  x <- share(list(outer = list(inner = 1:10)))
  sub <- x$outer
  buf <- serialize(sub, NULL)
  expect_lt(length(buf), 1000)
  y <- unserialize(buf)
  expect_identical(y$inner[], 1:10)
  expect_true(is_shared(y))
})

test_that("vec inside sub-list round-trips through serialize (depth-2 path)", {
  x <- share(list(outer = list(inner = 1:10)))
  vec <- x$outer$inner
  buf <- serialize(vec, NULL)
  expect_lt(length(buf), 1000)
  y <- unserialize(buf)
  expect_identical(y[], 1:10)
})

test_that("COW of sub-list does not touch parent SHM", {
  x <- share(list(outer = list(a = 1:5, b = 6:10)))
  nm <- shared_name(x)
  y <- x$outer
  y$a[1] <- 99L
  expect_identical(y$a[1], 99L)
  x2 <- map_shared(nm)
  expect_identical(x2$outer$a[], 1:5)
})

test_that("is_shared TRUE for sub-lists and nested elements", {
  x <- share(list(outer = list(a = 1:5, b = letters)))
  expect_true(is_shared(x))
  expect_true(is_shared(x$outer))
  expect_true(is_shared(x$outer$a))
  expect_true(is_shared(x$outer$b))
})

test_that("shared_name returns root name for root, blank for sub-lists", {
  x <- share(list(outer = list(a = 1:5)))
  nm <- shared_name(x)
  expect_true(nzchar(nm))
  expect_identical(shared_name(x$outer), "")
})

test_that("deeply nested structure (3 list levels) round-trips", {
  x <- share(list(a = list(b = list(c = 1:5))))
  y <- map_shared(shared_name(x))
  expect_identical(y$a$b$c[], 1:5)
  expect_true(is_shared(y$a$b))
})

test_that("nested element keeps parent SHM alive through GC", {
  x <- share(list(outer = list(a = 1:10)))
  nm <- shared_name(x)
  deep <- x$outer$a
  rm(x)
  gc()
  y <- map_shared(nm)
  expect_identical(y$outer$a[], 1:10)
  expect_identical(deep[], 1:10)
})

test_that("nested sub-list survives serialize → unserialize round-trip", {
  x <- share(list(outer = list(a = 1:5, b = 6:10)))
  sub <- x$outer
  y <- unserialize(serialize(sub, NULL))
  expect_identical(y$a[], 1:5)
  expect_identical(y$b[], 6:10)
})

test_that("pairlist nested inside a shared list is coerced and accessible", {
  x <- share(list(outer = pairlist(a = 1L, b = "hi", c = 3:5)))
  expect_identical(x$outer$a[], 1L)
  expect_identical(x$outer$b, "hi")
  expect_identical(x$outer$c[], 3:5)
  y <- map_shared(shared_name(x))
  expect_identical(y$outer$a[], 1L)
  expect_identical(y$outer$b, "hi")
  expect_identical(y$outer$c[], 3:5)
})

test_that("depth-3 path serialize → unserialize works", {
  x <- share(list(a = list(b = list(c = 1:50))))
  vec <- x$a$b$c
  y <- unserialize(serialize(vec, NULL))
  expect_identical(y[], 1:50)
  sub <- x$a$b
  y2 <- unserialize(serialize(sub, NULL))
  expect_identical(y2$c[], 1:50)
})

test_that("string leaf inside sub-list serialize → unserialize works", {
  x <- share(list(outer = list(labels = c("alpha", "beta", "gamma"))))
  s <- x$outer$labels
  y <- unserialize(serialize(s, NULL))
  expect_identical(y[], c("alpha", "beta", "gamma"))
})

test_that("re-serialization of an unserialized nested leaf works", {
  x <- share(list(a = list(b = list(c = 1:100))))
  vec <- x$a$b$c
  y <- unserialize(serialize(vec, NULL))
  z <- unserialize(serialize(y, NULL))
  expect_identical(z[], 1:100)
  sub <- x$a$b
  y2 <- unserialize(serialize(sub, NULL))
  z2 <- unserialize(serialize(y2, NULL))
  expect_identical(z2$c[], 1:100)
})

test_that("sub-list reference alone keeps root SHM alive through GC", {
  x <- share(list(outer = list(a = 1:10, b = letters)))
  nm <- shared_name(x)
  sub <- x$outer
  rm(x)
  gc()
  y <- map_shared(nm)
  expect_identical(y$outer$a[], 1:10)
  expect_identical(sub$a[], 1:10)
  expect_identical(sub$b, letters)
})
