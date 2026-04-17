test_that("shared_name + map_shared round-trip", {
  x <- share(as.double(1:50))
  nm <- shared_name(x)
  expect_true(is.character(nm))
  expect_gt(nchar(nm), 0L)
  y <- map_shared(nm)
  expect_identical(x[], y[])
})

test_that("is_shared distinguishes shared and non-shared objects", {
  expect_true(is_shared(share(1:10)))
  expect_false(is_shared(1:10))

  df <- share(data.frame(a = 1:3))
  expect_true(is_shared(df))

  s <- share(letters)
  expect_true(is_shared(s))

  expect_false(is_shared(NULL))
  expect_false(is_shared("hello"))
  expect_false(is_shared(data.frame(a = 1)))
  expect_false(is_shared(list(1, 2)))
})

test_that("shared_name returns a non-empty string for shared objects", {
  df <- share(data.frame(a = 1:3))
  nm <- shared_name(df)
  expect_true(is.character(nm))
  expect_gt(nchar(nm), 0L)

  s <- share(letters)
  expect_true(is.character(shared_name(s)))
})

test_that("shared_name returns '' for non-shared inputs", {
  expect_identical(shared_name(1:10), "")
  expect_identical(shared_name(NULL), "")
  expect_identical(shared_name(list(a = 1)), "")
  expect_identical(shared_name("a"), "")

  s <- share(letters)
  nm <- shared_name(s)
  expect_identical(shared_name(nm), "")
})

test_that("map_shared returns NULL on malformed input", {
  expect_null(map_shared(""))
  expect_null(map_shared("junk"))
  expect_null(map_shared(NULL))
  expect_null(map_shared(NA_character_))
  expect_null(map_shared(character(0)))
  expect_null(map_shared(c("a", "b")))
  expect_null(map_shared(1L))
  expect_null(map_shared(list("a")))
})

test_that("map_shared composes with shared_name sentinel without tryCatch", {
  expect_null(map_shared(shared_name(1:10)))
})

test_that("well-formed name for absent region errors", {
  bogus <- if (.Platform$OS.type == "windows") {
    "Local\\mori_nonexistent_xyz"
  } else {
    "/mori_nonexistent_xyz"
  }
  expect_error(map_shared(bogus), "not found")
})

test_that("is_shared is TRUE across all shared ALTREP kinds (host-side)", {
  expect_true(is_shared(share(1:10)))
  expect_true(is_shared(share(letters)))
  expect_true(is_shared(share(list(a = 1, b = "x"))))

  x <- share(list(list(1)))
  expect_true(is_shared(x[[1]]))

  y <- share(list(list(a = 1:3)))
  expect_true(is_shared(y[[1]][["a"]]))

  z <- share(list(list(s = letters)))
  expect_true(is_shared(z[[1]][["s"]]))
})

test_that("is_shared is TRUE across all shared ALTREP kinds (consumer-side)", {
  a <- share(1:10)
  expect_true(is_shared(map_shared(shared_name(a))))

  s <- share(letters)
  expect_true(is_shared(map_shared(shared_name(s))))

  L <- share(list(a = 1, b = "x"))
  expect_true(is_shared(map_shared(shared_name(L))))

  x <- share(list(list(1)))
  xx <- map_shared(shared_name(x))
  expect_true(is_shared(xx[[1]]))

  y <- share(list(list(a = 1:3)))
  yy <- map_shared(shared_name(y))
  expect_true(is_shared(yy[[1]][["a"]]))

  z <- share(list(list(s = letters)))
  zz <- map_shared(shared_name(z))
  expect_true(is_shared(zz[[1]][["s"]]))
})

test_that("shared_name returns '' for a sub-list", {
  x <- share(list(list(1)))
  sub <- x[[1]]
  expect_true(is_shared(sub))
  expect_identical(shared_name(sub), "")
})

test_that("is_shared/shared_name preserved after COW materialization", {
  x <- share(as.double(1:10))
  nm <- shared_name(x)
  expect_gt(nchar(nm), 0L)
  x[1] <- 99
  expect_equal(x[1], 99)
  expect_true(is_shared(x))
  expect_identical(shared_name(x), nm)
})
