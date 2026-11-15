expect_subset <- function(p, x, cols, from, to, ...) {
  got <- read_fstx(p, columns = cols, from = from, to = to, ...)
  last <- if (is.null(to)) nrow(x) else min(to, nrow(x))
  sel <- if (is.null(cols)) names(x) else if (is.numeric(cols)) names(x)[cols] else cols
  want <- x[seq.int(from, length.out = last - from + 1), sel, drop = FALSE]
  rownames(want) <- NULL
  expect_identical(got, want, info = sprintf("cols=%s from=%s to=%s", paste(sel, collapse = ","), from, format(to)))
}

test_that("columns= and from/to give exactly x[from:to, columns]", {
  x <- make_df(200000, seed = 3)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p), add = TRUE)
  write_fstx(x, p)
  expect_subset(p, x, NULL, 1, NULL)
  expect_subset(p, x, "dbl", 1, NULL)
  expect_subset(p, x, c("chr", "int"), 1, 10)
  expect_subset(p, x, c("ct", "fct", "chr"), 32760, 32780)   # across a double-page boundary
  expect_subset(p, x, NULL, 65530, 65545)                     # across an int-page boundary
  expect_subset(p, x, NULL, 777, 777)                         # one row
  expect_subset(p, x, NULL, 199999, NULL)
  expect_subset(p, x, c(4, 1, 4), 1000, 150000)               # positions, repeats, any order
  expect_subset(p, x, NULL, 150000, 1e9)                      # 'to' past the end is clamped
  expect_subset(p, x, NULL, 200000, 200000)
  expect_subset(p, x, c("chr", "ord"), 5, 150000, threads = 1)
})

test_that("row ranges across row groups give the right rows", {
  n <- 2.2e6
  x <- data.frame(i = seq_len(n), s = as.character(seq_len(n) %% 1013), d = seq_len(n) / 3)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p), add = TRUE)
  write_fstx(x, p)
  expect_subset(p, x, NULL, 1048570, 1048590)
  expect_subset(p, x, c("s", "d"), 1000000, 2100000)
  expect_subset(p, x, "s", 2097152, 2097153)
  expect_subset(p, x, NULL, 1, NULL)
})

test_that("bad column and row arguments give errors", {
  x <- make_df(100)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p), add = TRUE)
  write_fstx(x, p)
  expect_error(read_fstx(p, columns = "nope"), "not in the file")
  expect_error(read_fstx(p, columns = 99), "out of range")
  expect_error(read_fstx(p, from = 0), "from")
  expect_error(read_fstx(p, from = 101), "beyond")
  expect_error(read_fstx(p, from = 50, to = 10), "smaller")
  expect_error(read_fstx(p, from = 1.5), "whole")
  expect_error(read_fstx(file.path(tempdir(), "does-not-exist.fstx")), "cannot open")
  expect_identical(nrow(read_fstx(p, from = 100, to = 99)), 0L)
})

test_that("a zero-column table still reports its rows", {
  x <- make_df(50)[, 0]
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p), add = TRUE)
  write_fstx(x, p)
  expect_identical(dim(read_fstx(p, from = 11, to = 20)), c(10L, 0L))
})
