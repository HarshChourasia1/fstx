test_that("every supported type round-trips with NAs, empty and non-ASCII strings", {
  x <- make_df(5000)
  y <- roundtrip(x)
  expect_identical(y, x)
  expect_identical(lapply(y, class), lapply(x, class))
  expect_identical(attr(y$ct, "tzone"), "Asia/Kolkata")
  expect_identical(typeof(y$idt), "integer")
  expect_identical(levels(y$fct), levels(x$fct))
  expect_true(is.ordered(y$ord))
})

test_that("doubles keep NA vs NaN, infinities and signed zero", {
  x <- data.frame(d = c(NA, NaN, Inf, -Inf, 0, -0, 1e-310, .Machine$double.xmax))
  y <- roundtrip(x)
  expect_identical(y, x)
  expect_true(is.na(y$d[1]) && !is.nan(y$d[1]))
  expect_true(is.nan(y$d[2]))
  expect_identical(1 / y$d[6], -Inf)
})

test_that("POSIXct with no tzone and columns with other attributes round-trip", {
  x <- data.frame(a = structure(c(1.5, NA, 3e9), class = c("POSIXct", "POSIXt")),
                  b = as.POSIXct(c("2020-01-01 00:00:00", NA, "2021-06-01 12:00:00"), tz = ""),
                  dt = as.difftime(c(1, 2, NA), units = "hours"),
                  i64 = structure(c(1, 2, 3), class = "integer64"))
  x$lab <- structure(1:3, label = "a label", myattr = list(k = 1))
  expect_null(attr(x$a, "tzone"))
  y <- roundtrip(x)
  expect_identical(y, x)
  expect_null(attr(y$a, "tzone"))
  expect_identical(attr(y$lab, "label"), "a label")
  expect_identical(attr(y$dt, "units"), "hours")
})

test_that("factor with an NA level and odd classes take the generic path", {
  x <- data.frame(f = addNA(factor(c("a", NA, "b"))))
  x$g <- structure(1:3, levels = c("x", "y", "z"), class = c("myfactor", "factor"))
  expect_identical(roundtrip(x), x)
})

test_that("zero-row, zero-column, one-row and one-column tables round-trip", {
  x <- make_df(10)
  expect_identical(roundtrip(x[0, ]), x[0, ])
  expect_identical(nrow(roundtrip(x[0, ])), 0L)
  z <- x[, 0]
  expect_identical(roundtrip(z), z)
  expect_identical(dim(roundtrip(z)), c(10L, 0L))
  e <- data.frame()
  expect_identical(roundtrip(e), e)
  one <- x[1, ]
  rownames(one) <- NULL
  expect_identical(roundtrip(one), one)
  expect_identical(roundtrip(x["chr"]), x["chr"])
  expect_identical(roundtrip(x["dbl"]), x["dbl"])
})

test_that("row names that are not automatic are kept", {
  x <- data.frame(a = 1:3, b = c("x", "y", "z"), row.names = c("r1", "r2", "ré"))
  expect_identical(roundtrip(x), x)
  y <- make_df(20)[5:12, ]
  expect_identical(roundtrip(y), y)
})

test_that("sizes at page boundaries round-trip", {
  for (n in c(1023, 1024, 1025, 32767, 32768, 32769, 65535, 65536, 65537)) {
    x <- make_df(n, seed = n)
    expect_identical(roundtrip(x), x, info = paste("n =", n))
  }
})

test_that("tables at the row-group size round-trip", {
  for (n in c(1048575, 1048576, 1048577)) {
    x <- data.frame(i = seq_len(n), d = as.numeric(seq_len(n)) / 7, s = as.character(n - seq_len(n) %% 977))
    expect_identical(roundtrip(x), x, info = paste("n =", n))
  }
})

test_that("a table spanning several row groups round-trips", {
  n <- 2.6e6
  x <- make_df(n, seed = 42)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p), add = TRUE)
  write_fstx(x, p)
  expect_identical(read_fstx(p), x)
  m <- fstx_meta(p)
  expect_equal(m$row_groups, 3)
  expect_equal(m$nrow, n)
})

test_that("long strings are split into byte-capped pages and round-trip", {
  big <- strrep("x", 700000)   # two of these in one 1024-row vector pass the 1 MB split point
  x <- data.frame(s = c(big, "a", NA, paste0(big, "é"), rep("short", 3000)), stringsAsFactors = FALSE)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p), add = TRUE)
  write_fstx(x, p)   # few distinct values: a dictionary page and one code page
  expect_identical(read_fstx(p), x)
  write_fstx(x, p, .encoding = "plain")   # plain string pages are byte-capped (M2: forced, else dictionary)
  expect_identical(read_fstx(p), x)
  expect_identical(read_fstx(p, from = 4, to = 5), data.frame(s = c(paste0(big, "é"), "short")))
  expect_gte(fstx_meta(p)$columns$pages, 4)
})

test_that("data.frame and data.table keep their class", {
  x <- make_df(3000)
  expect_identical(class(roundtrip(x)), "data.frame")
  skip_if_not_installed("data.table")
  dt <- data.table::as.data.table(x)
  y <- roundtrip(dt)
  expect_identical(class(y), class(dt))
  expect_equal(y, dt)
  expect_identical(as.data.frame(y), as.data.frame(dt))
  data.table::set(y, j = "extra", value = 1L)   # adding a column in place needs the over-allocation
  expect_identical(ncol(y), ncol(dt) + 1L)
  data.table::setkeyv(dt, "int")
  yk <- roundtrip(dt)
  expect_identical(data.table::key(yk), "int")
  expect_equal(yk, dt)
})

test_that("tibble keeps its class", {
  skip_if_not_installed("tibble")
  x <- tibble::as_tibble(make_df(2000))
  y <- roundtrip(x)
  expect_identical(class(y), class(x))
  expect_identical(y, x)
})

test_that("latin1 strings are converted to UTF-8; bytes-encoded strings are refused", {
  s <- c("caf\xe9", "plain", NA)
  Encoding(s) <- "latin1"
  x <- data.frame(s = s, stringsAsFactors = FALSE)
  y <- roundtrip(x)
  expect_identical(y$s, enc2utf8(s))
  expect_identical(Encoding(y$s[1]), "UTF-8")
  b <- "ab\xff"
  Encoding(b) <- "bytes"
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p), add = TRUE)
  expect_error(write_fstx(data.frame(b = b), p), "bytes")
  expect_false(file.exists(p))
})

test_that("unsupported column types give a clear error and leave no file", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p), add = TRUE)
  x <- data.frame(a = 1:2)
  x$l <- list(1, "a")
  expect_error(write_fstx(x, p), "column 'l' has type 'list'")
  expect_error(write_fstx(data.frame(z = complex(real = 1:2, imaginary = 0)), p), "type 'complex'")
  expect_error(write_fstx(data.frame(r = as.raw(1:2)), p), "type 'raw'")
  m <- data.frame(a = 1:2)
  m$m <- matrix(1:4, 2)
  expect_error(write_fstx(m, p), "matrix")
  expect_error(write_fstx(list(a = 1), p), "data frame")
  expect_error(write_fstx(data.frame(a = 1), p, compress = "gzip"))
  expect_error(write_fstx(data.frame(a = 1), p, level = 99), "level")
  expect_false(file.exists(p))
})
