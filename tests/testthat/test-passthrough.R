test_that("formulas pass through unchanged", {
  x <- y ~ x + z
  expect_identical(share(x), x)
})

test_that("NULL passes through unchanged", {
  expect_null(share(NULL))
})

test_that("pass-through elements survive inside a shared list", {
  lst <- list(fn = sum, val = 1:5, env = globalenv())
  slst <- share(lst)
  expect_identical(slst$fn, sum)
  expect_identical(slst$val[], 1:5)
  expect_identical(slst$env, globalenv())
})

test_that("list with pass-through elements round-trips through serialize", {
  slst <- share(list(fn = sum, val = 1:5, env = globalenv()))
  y <- unserialize(serialize(slst, NULL))
  expect_identical(y$fn, sum)
  expect_identical(y$val[], 1:5)
})
