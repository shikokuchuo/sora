test_that("basic string vector round-trips via map_shared", {
  x <- c("hello", "world", "foo")
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_identical(x, y)
})

test_that("string vector with NA round-trips via map_shared", {
  x <- c("a", NA, "b", NA, "c")
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_identical(x, y)
  expect_true(is.na(y[2]))
  expect_true(is.na(y[4]))
})

test_that("empty strings round-trip via map_shared", {
  x <- c("", "", "notempty", "")
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_identical(x, y)
})

test_that("empty character vector round-trips via map_shared", {
  x <- character(0)
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_length(y, 0L)
  expect_identical(x, y)
})

test_that("UTF-8 strings round-trip via map_shared", {
  x <- c("\u00e9", "\u00fc", "\u2603")
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_identical(x, y)
})

test_that("large character vector round-trips via map_shared", {
  x <- paste0("str_", seq_len(10000))
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  expect_length(y, 10000L)
  expect_identical(x[1], y[1])
  expect_identical(x[10000], y[10000])
  expect_identical(x, y)
})

test_that("share returns a usable character vector", {
  x <- letters
  y <- share(x)
  expect_identical(x, y)
})

test_that("character column in data frame round-trips", {
  df <- data.frame(id = c("a", "b", "c"), val = 1:3, stringsAsFactors = FALSE)
  sdf <- share(df)
  df2 <- map_shared(shared_name(sdf))
  expect_s3_class(df2, "data.frame")
  expect_identical(df$id, df2$id)
  expect_identical(df$val, df2$val[])
})

test_that("character element in list round-trips", {
  lst <- list(x = letters, y = 1:10, z = c(NA, "test"))
  slst <- share(lst)
  lst2 <- map_shared(shared_name(slst))
  expect_identical(lst$x, lst2$x)
  expect_identical(lst$y, lst2$y[])
  expect_identical(lst$z, lst2$z)
})

test_that("named character vector inside a list preserves attrs", {
  lst <- list(x = c(a = "hello", b = "world"), y = 1:3)
  slst <- share(lst)
  expect_identical(slst$x, c(a = "hello", b = "world"))
  expect_identical(names(slst$x), c("a", "b"))
  expect_identical(slst$y[], 1:3)

  slst2 <- map_shared(shared_name(slst))
  expect_identical(slst2$x, c(a = "hello", b = "world"))
  expect_identical(names(slst2$x), c("a", "b"))

  elem <- slst[[1]]
  y <- unserialize(serialize(elem, NULL))
  expect_identical(y, c(a = "hello", b = "world"))
  expect_identical(names(y), c("a", "b"))
})

test_that("make.unique triggers Dataptr materialization on string ALTREP", {
  x <- share(c("a", "a", "b"))
  y <- map_shared(shared_name(x))
  z <- make.unique(y)
  expect_identical(z, c("a", "a.1", "b"))
  expect_length(y, 3L)
  expect_identical(y[1], "a")
  expect_identical(y[], c("a", "a", "b"))
})

test_that("duplicated works on string ALTREP", {
  x <- share(c("x", "y", "x"))
  y <- map_shared(shared_name(x))
  expect_identical(duplicated(y), c(FALSE, FALSE, TRUE))
})
