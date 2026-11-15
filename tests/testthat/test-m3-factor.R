# M3: as_factor builds factors from the string dictionaries. Synthetic data only.

test_that("as_factor = TRUE converts dictionary-coded columns and matches factor()", {
  set.seed(3)
  n <- 60000
  x <- data.frame(low = sample(c("b", "a", "cé", "", NA), n, TRUE),
                  uniq = sprintf("u%06d", sample.int(1e6, n)),
                  i = seq_len(n), stringsAsFactors = FALSE)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  pg <- fstx_pages(p)
  expect_true(any(pg$name == "low" & pg$dictionary))
  for (m in list(list(), list(strings = "deferred"), list(lazy = TRUE))) {
    y <- do.call(read_fstx, c(list(p, as_factor = TRUE), m))
    expect_identical(y$low, factor(x$low))
    expect_true(is.character(y$uniq))      # not dictionary-coded: left alone
    expect_identical(y$i, x$i)
    y2 <- do.call(read_fstx, c(list(p, as_factor = c("low", "uniq")), m))
    expect_identical(y2$uniq, factor(x$uniq))   # named and not dictionary-coded: factor() in R
    expect_identical(y2$low, factor(x$low))
    rm(y, y2)
    invisible(gc())
  }
  expect_error(read_fstx(p, as_factor = "i"), "not a character column")
  expect_error(read_fstx(p, as_factor = "zz"), "not among the columns read")
  expect_error(read_fstx(p, columns = "i", as_factor = "low"), "not among the columns read")
  expect_error(read_fstx(p, as_factor = NA), "'as_factor'")
  expect_identical(read_fstx(p, as_factor = FALSE), x)
})

test_that("levels are the union of the dictionaries of the row groups read", {
  set.seed(4)
  n <- 1200000
  g <- ifelse(seq_len(n) <= 1048576, sample(c("m", "a", "z"), n, TRUE), sample(c("z", "q", NA), n, TRUE))
  x <- data.frame(g = g, stringsAsFactors = FALSE)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  expect_identical(sum(fstx_pages(p)$dictionary), 2L)
  y <- read_fstx(p, as_factor = TRUE)
  expect_identical(y$g, factor(g))
  expect_identical(levels(y$g), c("a", "m", "q", "z"))
  tail_rows <- read_fstx(p, from = 1100000, as_factor = TRUE)$g
  expect_identical(levels(tail_rows), c("q", "z"))      # only the second row group's dictionary
  expect_identical(as.character(tail_rows), g[1100000:n])
  z <- read_fstx(p, as_factor = TRUE, lazy = TRUE)
  expect_identical(z$g[c(1, 1048577, n)], factor(g)[c(1, 1048577, n)])
  expect_identical(z$g, factor(g))
  rm(z)
  invisible(gc())
})

test_that("as_factor on empty selections and stored factors", {
  x <- make_df(50)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  y <- read_fstx(p, from = 3, to = 2, as_factor = TRUE)
  expect_identical(y$chr, factor(character(0)))
  expect_identical(read_fstx(p, as_factor = TRUE)$fct, x$fct)   # stored factors keep their levels
})
