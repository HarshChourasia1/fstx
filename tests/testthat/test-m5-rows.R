# M5: scattered row reads. Synthetic data only.

same_rows <- function(y, x, rows) {
  e <- x[rows, , drop = FALSE]
  rownames(e) <- NULL
  rownames(y) <- NULL
  expect_identical(y, e)
}

test_that("rows= returns the rows asked for, in order, with repeats, for every column type", {
  x <- make_df(20000, seed = 11)
  for (prof in c("balanced", "fast", "small")) {
    p <- tempfile(fileext = ".fstx")
    write_fstx(x, p, profile = prof)
    set.seed(5)
    r <- sample.int(nrow(x), 1000, replace = TRUE)
    same_rows(read_fstx(p, rows = r), x, r)
    same_rows(read_fstx(p, rows = sort(r)), x, sort(r))         # ascending with repeats
    same_rows(read_fstx(p, rows = rev(seq_len(nrow(x)))), x, rev(seq_len(nrow(x))))
    same_rows(read_fstx(p, rows = c(1, nrow(x), 1)), x, c(1, nrow(x), 1))
    same_rows(read_fstx(p, rows = 101:200), x, 101:200)          # one run: the range read
    same_rows(read_fstx(p, rows = as.double(r)), x, r)
    y <- read_fstx(p, rows = r, columns = c("chr", "dbl", "chr"))
    expect_identical(names(y), c("chr", "dbl", "chr.1"))
    expect_identical(y$dbl, x$dbl[r])
    expect_identical(y$chr, x$chr[r])
    expect_identical(nrow(read_fstx(p, rows = integer(0))), 0L)
    unlink(p)
  }
})

test_that("rows= covers forced encodings, dictionary strings, small pages and several row groups", {
  set.seed(3)
  n <- 30000
  x <- data.frame(i = sample(c(1:50, NA), n, TRUE), d = round(runif(n) * 100, 2),
                  s = sample(c(sprintf("k%03d", 1:40), NA), n, TRUE),
                  u = sprintf("u%07d", sample.int(1e6, n)), stringsAsFactors = FALSE)
  for (enc in c("plain", "for", "delta", "rle", "dict", "alp", "alprd", "fsst", "split", "const")) {
    p <- tempfile(fileext = ".fstx")
    xx <- if (enc == "const") data.frame(i = rep(7L, n), d = rep(1.5, n)) else x
    w <- tryCatch(write_fstx(xx, p, .encoding = enc, compress = "none"), error = function(e) NULL)
    if (is.null(w)) next
    r <- sample.int(n, 500, TRUE)
    same_rows(read_fstx(p, rows = r), xx, r)
    unlink(p)
  }
  p <- tempfile(fileext = ".fstx")
  write_fstx(data.frame(a = 1L), p)
  expect_identical(read_fstx(p, rows = c(1, 1))$a, c(1L, 1L))
  unlink(p)
})

test_that("rows= errors clearly on NA, fractions and out-of-range rows", {
  x <- make_df(100)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  expect_error(read_fstx(p, rows = c(1, NA)), "NA")
  expect_error(read_fstx(p, rows = c(3, 1.5)), "whole")
  expect_error(read_fstx(p, rows = c(5, 101)), "out of range")
  expect_error(read_fstx(p, rows = c(0, 2)), "out of range")
  expect_error(read_fstx(p, rows = c(2, -1)), "out of range")
  expect_error(read_fstx(p, rows = "a"), "numeric")
  expect_error(read_fstx(p, rows = 1:3, from = 2), "not both")
  unlink(p)
})

test_that("rows= with lazy, deferred strings, as_factor, row names and data.table", {
  x <- make_df(5000, seed = 2)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  r <- c(4000, 3, 3, 2500)
  same_rows(read_fstx(p, rows = r, lazy = TRUE), x, r)
  same_rows(read_fstx(p, rows = r, strings = "deferred"), x, r)
  y <- read_fstx(p, rows = r, as_factor = "chr")
  expect_identical(y$chr, factor(x$chr[r]))
  rn <- data.frame(a = 1:5, row.names = c("v", "w", "x", "y", "z"))
  write_fstx(rn, p)
  y <- read_fstx(p, rows = c(5, 2, 2))
  expect_identical(rownames(y), c("z", "w", "w.1"))
  expect_identical(y$a, c(5L, 2L, 2L))
  if (requireNamespace("data.table", quietly = TRUE)) {
    dt <- data.table::as.data.table(x)
    write_fstx(dt, p)
    y <- read_fstx(p, rows = r)
    expect_s3_class(y, "data.table")
    expect_identical(as.data.frame(y), {e <- x[r, ]; rownames(e) <- NULL; e})
  }
  unlink(p)
})
