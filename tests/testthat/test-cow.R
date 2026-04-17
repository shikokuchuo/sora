test_that("double vector write triggers materialization without mutating SHM", {
  x <- as.double(1:100)
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  original <- y[1]

  y2 <- y
  y2[1] <- 999
  expect_equal(y2[1], 999)

  z <- map_shared(shared_name(sx))
  expect_equal(z[1], original)
})

test_that("string vector write triggers materialization without mutating SHM", {
  x <- c("original", "data")
  sx <- share(x)
  y <- map_shared(shared_name(sx))
  y2 <- y
  y2[1] <- "modified"
  expect_identical(y2[1], "modified")

  z <- map_shared(shared_name(sx))
  expect_identical(z[1], "original")
})

test_that("sum on materialized vector uses the private copy", {
  x <- share(as.double(1:100))
  x[1] <- 999
  expect_equal(sum(x), 999 + sum(2:100))
})
