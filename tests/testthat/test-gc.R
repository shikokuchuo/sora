test_that("GC cleans up a shared vector", {
  x <- share(1:100)
  nm <- shared_name(x)
  rm(x)
  gc()
  expect_error(map_shared(nm), "not found")
})

test_that("GC cleans up a shared list/data frame", {
  x <- share(data.frame(a = 1:10, b = as.double(1:10)))
  nm <- shared_name(x)
  rm(x)
  gc()
  expect_error(map_shared(nm), "not found")
})

test_that("GC cleans up a shared string vector", {
  x <- share(letters)
  nm <- shared_name(x)
  rm(x)
  gc()
  expect_error(map_shared(nm), "not found")
})

test_that("element reference keeps parent SHM alive through GC", {
  x <- share(data.frame(a = 1:10, b = as.double(1:10)))
  nm <- shared_name(x)
  col <- x$a
  rm(x)
  gc()

  y <- map_shared(nm)
  expect_s3_class(y, "data.frame")

  rm(col, y)
  gc()
  expect_error(map_shared(nm), "not found")
})
