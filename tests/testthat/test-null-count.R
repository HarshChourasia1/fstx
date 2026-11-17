# Row 9 (C3): options(fstx.verify_nulls = TRUE) counts the missing values of every decoded page against the
# page's null_count. Crafted pages come from fstx:::.fstx_forge(nulls0 = ) (column 0's first page). Synthetic data.

forge <- function(src, dst, ...) fstx:::.fstx_forge(src, dst, ...)
with_nulls_check <- function(code) {
  old <- options(fstx.verify_nulls = TRUE)
  on.exit(options(old))
  code
}
# a double R treats as NA that the writer stores as a value: NA_real_ with the quiet bit set (0x7FF80000000007A2)
quiet_na <- readBin(as.raw(c(0xA2, 0x07, 0, 0, 0, 0, 0xF8, 0x7F)), "double", size = 8, endian = "little")

page0_nulls <- function(p) {   # null_count of column 0's first data page
  g <- fstx_pages(p)
  g$nulls[g$column == 1 & !g$dictionary][1]
}

crafted <- function(x, nulls0) {
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".fstx")
  write_fstx(x, p, threads = 1)
  forge(p, q, nulls0 = nulls0)
  unlink(p)
  q
}

test_that("ordinary files pass the check in every read path", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- make_df(70000)
  x$chr_many <- sprintf("row %d", seq_len(nrow(x)))   # plain (not dictionary-coded) strings
  x$chr_many[c(5, 69000)] <- NA
  x$qna <- c(quiet_na, NA, rnorm(nrow(x) - 2))        # counted in null_count, stored as a value
  write_fstx(x, p, threads = test_threads(2))
  expect_true(any(fstx_pages(p)$nulls > 0))
  with_nulls_check({
    for (s in c("eager", "auto", "deferred")) expect_identical(read_fstx(p, strings = s), x, info = s)
    expect_identical(read_fstx(p, threads = 1), x)
    expect_identical(as.data.frame(read_fstx(p, lazy = TRUE)), x)
    z <- read_fstx(p, lazy = TRUE)
    expect_identical(anyNA(z$int), anyNA(x$int))
    r <- c(69000, 3, 3, 40000)
    want <- x[r, ]
    rownames(want) <- NULL
    got <- read_fstx(p, rows = r)
    rownames(got) <- NULL
    expect_identical(got, want)
    expect_identical(read_fstx(p, from = 100, to = 66000), {
      w <- x[100:66000, ]
      rownames(w) <- NULL
      w
    })
    expect_identical(read_fstx(p, where = int > 500)$int, x$int[!is.na(x$int) & x$int > 500])
    expect_identical(read_fstx(p, as_factor = TRUE)$int, x$int)
  })
})

test_that("a page whose null_count is 0 but holds NA is refused under the check (int, lazy, rows, where)", {
  x <- data.frame(i = c(1L, NA, 3:3000), d = seq_len(3000) / 2)
  q <- crafted(x, nulls0 = 0L)
  on.exit(unlink(q))
  expect_identical(page0_nulls(q), 0)
  # by default the count is trusted: R still reads NA (the pattern), a lazy column says it has none
  expect_identical(read_fstx(q), x)
  expect_false(anyNA(read_fstx(q, lazy = TRUE)$i))
  with_nulls_check({
    expect_error(read_fstx(q), "column 'i' \\(rows 1 to 3000\\): .*null_count\\): the file is corrupt")
    expect_error(read_fstx(q, verify = FALSE), "null_count")
    expect_error(read_fstx(q, from = 2000, to = 2010), "corrupt")
    expect_error(read_fstx(q, rows = c(3000, 1000)), "corrupt")   # rows away from the NA: the whole page is counted
    expect_error(read_fstx(q, where = d > 1000), "corrupt")
    expect_error(anyNA(read_fstx(q, lazy = TRUE)$i), "corrupt")
    expect_identical(read_fstx(q, columns = "d")$d, x$d)          # other columns are fine
  })
})

test_that("a page whose null_count is above its NA count is refused under the check", {
  x <- data.frame(i = 1:3000, l = rep(c(TRUE, FALSE, NA), 1000))
  q <- crafted(x, nulls0 = 3000L)
  on.exit(unlink(q))
  expect_identical(read_fstx(q), x)
  with_nulls_check(expect_error(read_fstx(q), "null_count\\): the file is corrupt"))
})

test_that("doubles: the quiet NA counts as the writer counts it; a hidden NA is refused", {
  x <- data.frame(d = c(1.5, quiet_na, 2.5, seq_len(2997) / 3))
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p, threads = 1)
  expect_identical(page0_nulls(p), 1)
  expect_true(is.na(with_nulls_check(read_fstx(p))$d[2]))
  q <- crafted(x, nulls0 = 0L)   # null_count 0 and R's NA pattern on an unmarked row (fe54253)
  on.exit(unlink(q), add = TRUE)
  expect_true(is.na(read_fstx(q)$d[2]))
  with_nulls_check({
    expect_error(read_fstx(q), "null_count\\): the file is corrupt")
    expect_error(read_fstx(q, lazy = TRUE)$d[2], "corrupt")
  })
})

test_that("string pages, plain and dictionary codes, are checked", {
  plain <- data.frame(s = c(NA, sprintf("value %d", 2:3000)), stringsAsFactors = FALSE)
  dict <- data.frame(s = rep(c("a", NA, "b"), 1000), stringsAsFactors = FALSE)
  for (x in list(plain, dict)) {
    q <- crafted(x, nulls0 = 0L)
    expect_identical(read_fstx(q), x)
    with_nulls_check({
      for (s in c("eager", "auto", "deferred"))
        expect_error(as.data.frame(read_fstx(q, strings = s)), "null_count\\): the file is corrupt", info = s)
      expect_error(read_fstx(q, rows = c(3000, 1500)), "corrupt")
      expect_error(anyNA(read_fstx(q, lazy = TRUE)$s), "corrupt")   # lazy strings decode on first use
      expect_error(read_fstx(q, as_factor = TRUE), "corrupt")
    })
    unlink(q)
  }
})

test_that("the Arrow stream passes the pattern on by default and refuses the page under the check", {
  skip_if_not_installed("nanoarrow")
  x <- data.frame(i = c(1L, NA, 3:3000), s = rep(c("a", NA, "b"), 1000), stringsAsFactors = FALSE)
  q <- crafted(x, nulls0 = 0L)
  on.exit(unlink(q))
  s <- fstx_arrow_stream(q, threads = 1)
  b <- s$get_next()
  expect_identical(as.integer(b$children$i$null_count), 0L)   # the NA went out as a value
  rm(s, b)
  with_nulls_check({
    s <- fstx_arrow_stream(q, threads = 1)
    expect_error(nanoarrow::convert_array_stream(s), "null_count\\): the file is corrupt")
    rm(s)
    s <- fstx_arrow_stream(q, where = i > 10, threads = 1)
    expect_error(nanoarrow::convert_array_stream(s), "corrupt")
    rm(s)
    p <- tempfile(fileext = ".fstx")
    on.exit(unlink(p), add = TRUE)
    y <- make_df(5000)
    y$ord <- NULL
    write_fstx(y, p, threads = 1)
    got <- as.data.frame(nanoarrow::convert_array_stream(fstx_arrow_stream(p, threads = test_threads(2))))
    expect_identical(got$int, y$int)
    expect_identical(got$dbl, y$dbl)
    expect_identical(got$chr, y$chr)
  })
  invisible(gc())
})
