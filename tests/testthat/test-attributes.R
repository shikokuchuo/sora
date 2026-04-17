test_that("named double vector preserves names", {
  x <- c(a = 1, b = 2, c = 3)
  y <- share(x)
  expect_identical(x, y[])
  expect_identical(names(x), names(y))
})

test_that("named integer vector preserves names", {
  x <- c(a = 1L, b = 2L, c = 3L)
  y <- share(x)
  expect_identical(x, y[])
  expect_identical(names(x), names(y))
})

test_that("factor preserves class, levels, and codes", {
  x <- factor(c("b", "a", "c", "a", "b"))
  y <- share(x)
  expect_s3_class(y, "factor")
  expect_identical(levels(x), levels(y))
  expect_identical(as.integer(x), as.integer(y))
})

test_that("Date vector preserves class", {
  x <- as.Date("2024-01-01") + 0:9
  y <- share(x)
  expect_s3_class(y, "Date")
  expect_identical(x, y[])
})

test_that("POSIXct vector preserves class and tz", {
  x <- as.POSIXct("2024-01-01 12:00:00", tz = "UTC") + 0:4
  y <- share(x)
  expect_s3_class(y, "POSIXct")
  expect_identical(x, y[])
})

test_that("named character vector preserves names", {
  x <- c(a = "hello", b = "world")
  y <- share(x)
  expect_identical(x, y)
  expect_identical(names(x), names(y))
})

test_that("factor column in a data frame round-trips", {
  df <- data.frame(id = factor(c("x", "y", "z")), val = 1:3)
  df2 <- share(df)
  expect_s3_class(df2, "data.frame")
  expect_s3_class(df2$id, "factor")
  expect_identical(levels(df$id), levels(df2$id))
  expect_identical(as.integer(df$id), as.integer(df2$id))
  expect_identical(df$val, df2$val[])
})

test_that("Date column in a data frame round-trips", {
  df <- data.frame(date = as.Date("2024-01-01") + 0:4, val = 1:5)
  df2 <- share(df)
  expect_s3_class(df2, "data.frame")
  expect_s3_class(df2$date, "Date")
  expect_identical(df$date, df2$date[])
  expect_identical(df$val, df2$val[])
})

test_that("map_shared preserves names attribute", {
  x <- share(c(a = 1, b = 2, c = 3))
  y <- map_shared(shared_name(x))
  expect_identical(names(x), names(y))
  expect_identical(x[], y[])
})

test_that("map_shared preserves factor", {
  x <- share(factor(c("a", "b", "c")))
  y <- map_shared(shared_name(x))
  expect_s3_class(y, "factor")
  expect_identical(levels(x), levels(y))
})
