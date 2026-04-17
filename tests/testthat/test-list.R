test_that("data frame round-trips via map_shared", {
  df <- data.frame(a = as.double(1:100), b = 1:100, c = rep(TRUE, 100))
  sdf <- share(df)
  df2 <- map_shared(shared_name(sdf))
  expect_s3_class(df2, "data.frame")
  expect_equal(nrow(df2), 100L)
  expect_identical(df$a, df2$a[])
  expect_identical(df$b, df2$b[])
  expect_identical(df$c, df2$c[])
  expect_identical(names(df), names(df2))
})

test_that("mixed list with atomic and character elements round-trips", {
  lst <- list(x = as.double(1:50), y = letters, z = 1:10)
  slst <- share(lst)
  lst2 <- map_shared(shared_name(slst))
  expect_identical(lst$x, lst2$x[])
  expect_identical(lst$y, lst2$y)
  expect_identical(lst$z, lst2$z[])
})

test_that("list with NULL elements round-trips and caches NULL reads", {
  lst <- list(NULL, 1:3, NULL)
  slst <- share(lst)
  lst2 <- map_shared(shared_name(slst))
  expect_null(lst2[[1]])
  expect_identical(lst[[2]], lst2[[2]][])
  expect_null(lst2[[3]])
  # cached access still returns NULL
  expect_null(lst2[[1]])
})

test_that("ALTLIST duplicate triggers column materialization", {
  df <- data.frame(a = as.double(1:10), b = 1:10)
  sdf <- share(df)
  df2 <- map_shared(shared_name(sdf))
  df3 <- df2
  df3$a <- df3$a * 2
  expect_s3_class(df3, "data.frame")
  expect_equal(df3$a, df$a * 2)
})

test_that("share of a bare data frame returns a usable data frame", {
  df <- data.frame(a = as.double(1:50), b = 1:50)
  df2 <- share(df)
  expect_s3_class(df2, "data.frame")
  expect_identical(df$a, df2$a[])
  expect_identical(df$b, df2$b[])
})

test_that("pairlist input is coerced and accessible by index", {
  pl <- pairlist(a = 1, b = 2L, c = "hello")
  spl <- share(pl)
  expect_length(spl, 3L)
  expect_identical(spl[[1]][], 1)
  expect_identical(spl[[2]][], 2L)
  expect_identical(spl[[3]], "hello")
})

test_that("pairlist round-trips via map_shared", {
  pl <- pairlist(a = 1, b = 2L, c = "hello")
  spl <- share(pl)
  spl2 <- map_shared(shared_name(spl))
  expect_identical(spl[[1]][], spl2[[1]][])
  expect_identical(spl[[2]][], spl2[[2]][])
  expect_identical(spl[[3]], spl2[[3]])
})
