# M3: lazy columns (decoded page by page from the memory-mapped file). Synthetic data only.

col_state <- function(x) fstx:::fstx_col_state_impl(x)

test_that("a lazy read decodes only the pages that are used", {
  n <- 400000
  x <- data.frame(a = seq_len(n), b = runif(n), c = sample(c("p", "q", NA), n, TRUE), d = rep(c(TRUE, NA), n / 2),
                  stringsAsFactors = FALSE)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p, profile = "fast")
  z <- read_fstx(p, lazy = TRUE)
  for (nm in names(z)) {
    s <- col_state(z[[nm]])
    expect_true(s$lazy)
    expect_true(s$file_open)
    expect_identical(s$pages_ready, 0)
  }
  expect_identical(z$a[123456], 123456L)
  expect_identical(col_state(z$a)$pages_ready, 1)
  expect_identical(col_state(z$b)$pages_ready, 0)       # untouched columns cost nothing
  expect_identical(head(z, 3), head(x, 3))              # subsetting goes element by element
  expect_false(col_state(z$b)$materialised)
  expect_identical(z$b[n], x$b[n])
  expect_identical(sum(z$a), sum(x$a))                  # region reads, no full copy
  expect_false(col_state(z$a)$materialised)
  expect_identical(mean(z$b), mean(x$b))
  expect_identical(z$c, x$c)
  expect_identical(z$d, x$d)
  out <- capture.output(.Internal(inspect(z$b)))
  expect_true(any(grepl("fstx lazy double column", out)))
})

test_that("the mapping stays alive while a column needs it, and the finaliser closes it", {
  x <- make_df(30000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  z <- read_fstx(p, lazy = TRUE)
  keep <- z$chr
  rm(z)
  invisible(gc())
  expect_identical(keep, x$chr)                         # still readable after the data frame is gone
  keep2 <- read_fstx(p, lazy = TRUE)$dbl
  invisible(gc())
  expect_error(write_fstx(x, p), "open in this R session")
  expect_identical(keep2[1:5], x$dbl[1:5])
  rm(keep)
  invisible(gc())
  expect_error(write_fstx(x, p), "open in this R session")   # keep2 still maps it
  rm(keep2)
  expect_silent(write_fstx(x, p))                       # every lazy column is gone (write_fstx runs gc())
  z <- read_fstx(p, lazy = TRUE)
  invisible(z$int[])                                    # [] keeps the ALTREP
  y <- z$int + 0L                                       # full use: materialised, the source is released
  expect_identical(y, x$int + 0L)
  s <- col_state(z$int)
  expect_true(s$materialised)
  expect_false(s$file_open)
  for (nm in names(z)) {                                # use every value of every column
    v <- z[[nm]]
    if (is.character(v)) invisible(v == v) else invisible(unclass(v) + 0L)
  }
  rm(v)
  expect_true(all(vapply(z, function(v) col_state(v)$materialised, NA)))
  expect_silent(write_fstx(x, p))                       # all columns fully used: the file is closed
  expect_identical(z, x)
})

test_that("writing to a lazily mapped path gives a clear error, other paths are fine", {
  x <- make_df(100)
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  z <- read_fstx(p, lazy = TRUE)
  expect_error(write_fstx(x, p), "memory-mapped by a lazy read_fstx\\(\\) result")
  expect_error(write_fstx(x, file.path(dirname(p), ".", basename(p))), "open in this R session")
  expect_silent(write_fstx(z, q))                       # writing a lazy table elsewhere works
  expect_identical(read_fstx(q), x)
  rm(z)
  invisible(gc())
  expect_silent(write_fstx(x, p))
})

test_that("the open-file check knows the file by its id, not its name (hard link, other spelling, copy)", {
  x <- make_df(100)
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".fstx")
  h <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p, q, h)), add = TRUE)
  write_fstx(x, p)
  expect_true(file.copy(p, q))
  z <- read_fstx(p, lazy = TRUE)
  if (.Platform$OS.type == "windows") {                 # other spellings of the same path
    if (grepl("/", p, fixed = TRUE))
      expect_error(write_fstx(x, gsub("/", "\\", p, fixed = TRUE)), "open in this R session")
    expect_error(write_fstx(x, file.path(dirname(p), toupper(basename(p)))), "open in this R session")
  }
  if (isTRUE(suppressWarnings(file.link(p, h))))        # same file through a second name
    expect_error(write_fstx(x, h), "open in this R session")
  expect_silent(write_fstx(x, q))                       # a different file with the same bytes is not refused
  rm(z)
  invisible(gc())
  expect_silent(write_fstx(x, p))
})

test_that("modifying lazy columns copies them and leaves the source alone", {
  x <- make_df(5000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  z <- read_fstx(p, lazy = TRUE)
  z2 <- z
  z2$int[1] <- -1L
  z2$chr[2] <- "new"
  z2$dbl[3] <- 0.5
  expect_identical(z2$int[1], -1L)
  expect_identical(z2$chr[2], "new")
  expect_identical(z2$dbl[3], 0.5)
  expect_identical(z, x)
  i <- z$int
  i[10] <- 99L
  expect_identical(i[10], 99L)
  expect_identical(z$int[10], x$int[10])
})

test_that("verify checks each page when it is first decoded", {
  n <- 100000
  x <- data.frame(a = seq_len(n) * 7L, b = sample(c("x", "yy", "zzz"), n, TRUE), c = runif(n),
                  stringsAsFactors = FALSE)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p, profile = "fast")
  pg <- fstx_pages(p)
  target <- pg[pg$name == "c" & !pg$dictionary, ][2, ]   # second page of column c
  flip_byte(p, target$offset + 40)
  z <- read_fstx(p, lazy = TRUE)                        # opening checks header and footer only
  expect_identical(z$a, x$a)
  expect_identical(z$b, x$b)
  expect_identical(z$c[1:10], x$c[1:10])                # first page is fine
  expect_error(z$c[target$first_row], "checksum")
  expect_error(sum(z$c), "checksum")
  rm(z)
  invisible(gc())
  z <- read_fstx(p, lazy = TRUE, verify = FALSE)
  r <- tryCatch({ sum(z$c); "data" }, error = function(e) "error")
  expect_true(r %in% c("data", "error"))
  rm(z)
  invisible(gc())
})

test_that("the fixed-page cache stays near 64 MB per column", {
  skip_on_cran()
  n <- 24e6
  x <- data.frame(a = sample.int(1e6, n, TRUE))
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p, profile = "fast")
  z <- read_fstx(p, lazy = TRUE)
  idx <- seq(1, n, by = 5000)
  expect_identical(z$a[idx], x$a[idx])                  # touches every page one element at a time
  s <- col_state(z$a)
  expect_false(s$materialised)
  expect_lte(s$cached_bytes, 64 * 2^20)
  expect_gt(s$cached_bytes, 0)
  rm(z, x)
  invisible(gc())
})
