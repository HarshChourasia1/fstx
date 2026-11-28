# fstx 1.2: native int64 columns (physical type 4) from bit64::integer64 vectors. Synthetic data only. The tests build
# and cut integer64 values without bit64 (helper-fstx.R), so they run where bit64 is not installed. Tests marked
# skip_until_drop2() need the engine's int64 filters and Arrow types (drop 2).

i64_df <- function(n = 5000, seed = 3) {
  set.seed(seed)
  big <- i64_c(i64_min, i64_max, i64_parts(2^21, 1), i64_parts(-2^21 - 1, 2^32 - 1), i64(2^53), i64(-1), i64(0))
  v <- i64(sample(-1e6:1e6, n, TRUE))
  v[sample.int(n, 50)] <- i64_na
  w <- col_take(big, sample.int(length(big), n, TRUE))
  w[c(7, 77, 777)] <- i64_na
  new_df(id = seq_len(n), v = v, w = w, s = i64(1e12 + seq_len(n) * 3))
}

test_that("integer64 columns round-trip natively: NA, INT64_MIN + 1, INT64_MAX, past 2^53, negatives", {
  x <- i64_df()
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p, threads = test_threads(2))
  y <- read_fstx(p)
  expect_same(y, x)
  expect_identical(class(y$w), "integer64")
  expect_identical(typeof(y$w), "double")
  expect_identical(i64_hex(unclass(y$w)[c(7, 77, 777)]), rep("8000000000000000", 3))
  expect_identical(i64_hex(i64_c(i64_min, i64_max, i64_parts(2^21, 1))),
                   c("8000000000000001", "7fffffffffffffff", "0020000000000001"))
  m <- fstx_meta(p)
  expect_identical(m$columns$type, c("integer", "int64", "int64", "int64"))
  expect_identical(m$columns$class, c("integer", "integer64", "integer64", "integer64"))
  pg <- fstx_pages(p)
  expect_identical(unique(pg$kind[pg$name %in% c("v", "w", "s")]), "int64")
  # the feature bit is set and named (bit 16, "int64"); not "binary"
  b <- readBin(p, "raw", file.size(p))
  expect_true(length(grepRaw("int64", b, fixed = TRUE)) > 0)
  expect_false(length(grepRaw("binary", b, fixed = TRUE)) > 0)
  for (th in c(1L, test_threads(4))) expect_same(read_fstx(p, threads = th), x, label = th)
  # same bytes on every thread count
  q <- tempfile(fileext = ".fstx")
  write_fstx(x, q, threads = 1)
  expect_identical(readBin(q, "raw", file.size(q)), b)
  unlink(q)
})

test_that("every int64 encoding and codec round-trips, extremes included (FOR / DELTA width 64, DICT)", {
  x <- i64_df(4000)
  x$k <- i64_rep(i64_max, 4000)                   # constant
  x$e <- i64_rep(i64_c(i64_min, i64_max), 2000)   # deltas that wrap
  x$e[c(5, 2000)] <- i64_na
  x$na <- i64_rep(i64_na, 4000)                   # every row missing
  r <- c(4000, 1, 1024, 1025, 2000, 3, 3)
  for (codec in c("none", "lz4", "zstd")) {
    for (e in c("plain", "const", "for", "delta", "rle", "dict", "split")) {
      enc <- c(e, if (codec != "none") "keep_codec")
      p <- tempfile(fileext = ".fstx")
      write_fstx(x, p, compress = codec, .encoding = enc, threads = 2)
      lab <- paste(codec, e)
      expect_same(read_fstx(p), x, label = lab)
      expect_same(read_fstx(p, lazy = TRUE), x, label = paste(lab, "lazy"))
      expect_same(read_fstx(p, rows = r), df_take(x, r), label = paste(lab, "rows"))
      pg <- fstx_pages(p)
      expect_true(all(pg$kind[pg$name %in% c("v", "w", "s", "k", "e", "na")] == "int64"), label = lab)
      invisible(gc())
      unlink(p)
    }
  }
})

test_that("all-missing, zero-row and one-row int64 columns", {
  x <- new_df(a = i64_rep(i64_na, 3000), b = i64(c(rep(NA, 2999), 5)))
  expect_same(roundtrip(x), x)
  expect_same(roundtrip(df_take(x, 0)), df_take(x, 0))
  expect_same(roundtrip(df_take(x, 3000)), df_take(x, 3000))
  y <- roundtrip(new_df(a = i64(numeric(0))))
  expect_identical(class(y$a), "integer64")
  expect_identical(nrow(y), 0L)
})

test_that("lazy int64 columns decode page by page and keep the class", {
  x <- i64_df(300000)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p, threads = 2)
  y <- read_fstx(p, lazy = TRUE, threads = 2)
  st <- fstx:::fstx_col_state_impl(y$w)
  expect_true(st$lazy)
  expect_true(st$pages > 1)
  expect_identical(class(y$w), "integer64")
  k <- c(1, 150000, 300000, 299999)
  expect_identical(i64_hex(.subset(y$w, k)), i64_hex(.subset(x$w, k)))   # .subset: no copy of the column
  expect_false(fstx:::fstx_col_state_impl(y$w)$materialised)
  expect_same(y, x)
  rm(y)
  invisible(gc())
})

test_that("row lists read int64 values in the order given", {
  x <- i64_df()
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p)
  r <- c(5000, 1, 4999, 2, 2, 2500, 1024, 1025)
  expect_same(read_fstx(p, rows = r), df_take(x, r))
  expect_same(read_fstx(p, columns = "w", from = 7, to = 8)$w, col_take(x$w, 7:8))
})

test_that("int64 = \"double\" gives plain doubles, NA for missing, and warns only when a value loses precision", {
  x <- new_df(a = i64(c(1, -5, NA, 2^53, -2^53, 2^53 + 2)), b = i64_c(i64_parts(2^21, 1), i64(1:5)))
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x["a"], p)
  expect_silent(y <- read_fstx(p, int64 = "double"))
  expect_identical(y$a, c(1, -5, NA, 2^53, -2^53, 2^53 + 2))
  expect_null(attributes(y$a))
  write_fstx(x, p)
  expect_warning(y <- read_fstx(p, int64 = "double"), "column 'b' has values past 2\\^53 that lose precision")
  expect_identical(y$b, c(2^53, 1:5))
  expect_warning(z <- read_fstx(p, int64 = "double", lazy = TRUE), "column 'b'")
  expect_identical(z$b, c(2^53, 1:5))
  expect_identical(z$a, y$a)
  expect_warning(z <- read_fstx(p, int64 = "double", rows = c(6, 1)), "column 'b'")
  expect_identical(z$b, c(5, 2^53))
  # INT64_MAX rounds up to 2^63 (no int64 has that value): counted as lossy too
  write_fstx(new_df(m = i64_c(i64_max, i64_min)), p)
  expect_warning(y <- read_fstx(p, int64 = "double"), "column 'm'")
  expect_identical(y$m, c(2^63, -2^63))
  expect_error(read_fstx(p, int64 = "bits"), "'int64' should be one of \"integer64\", \"double\"")
})

test_that("a class besides integer64 and other attributes are kept (int64 = \"double\" drops integer64 only)", {
  x <- new_df(a = 1:3,
              v = structure(unclass(i64(c(1, NA, 3))), class = c("myid", "integer64"), note = "ids"),
              u = structure(unclass(i64(c(1, 2, 3))), class = "integer64", units = "count"))
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p)
  expect_same(read_fstx(p), x)
  expect_identical(fstx_meta(p)$columns$type, c("integer", "int64", "int64"))
  expect_identical(fstx_meta(p)$columns$class, c("integer", "myid/integer64", "integer64"))
  y <- read_fstx(p, int64 = "double")
  expect_identical(class(y$v), "myid")
  expect_identical(attr(y$v, "note"), "ids")
  expect_identical(unclass(y$v), structure(c(1, NA, 3), note = "ids"))
  expect_identical(y$u, structure(c(1, 2, 3), units = "count"))
})

test_that("int64 = \"integer64\" writes the fstx 1.0 layout, byte for byte as the 1.0 write path did", {
  x <- new_df(id = 1:3000, v = i64(rep(c(1, NA, 2^40), 1000)))
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p, q)))
  write_fstx(x, p, int64 = "integer64", threads = 1)
  m <- fstx_meta(p)
  expect_identical(m$columns$type, c("integer", "double"))
  expect_identical(m$columns$class, c("integer", "integer64"))
  expect_identical(unique(fstx_pages(p)$kind), c("int", "double"))
  b <- readBin(p, "raw", file.size(p))
  expect_false(length(grepRaw("int64", b, fixed = TRUE)) > 0)   # no feature bit, no feature name
  # what write_fstx() of fstx 1.0 passed to the writer: a double column with its class in the attribute blob
  fstx:::fstx_write_impl(
    cols = list(x$id, unclass(x$v)), path = q, names = c("id", "v"), phys = c(0L, 1L), logical = c(0L, 0L),
    ordered = c(FALSE, FALSE), levels = list(NULL, NULL), tzone = list(NULL, NULL),
    attrs = list(NULL, fstx:::.attr_encode(list(class = "integer64"))),
    table_attrs = fstx:::.attr_encode(list(class = "data.frame")), nrow = 3000, codec = 2L, level = 3L,
    threads = 1L, utf8_locale = TRUE, profile = 1L, keep_pct = 10L, force_encoding = 0L, append = FALSE,
    test_crash = 0L)
  expect_identical(readBin(q, "raw", file.size(q)), b)
  # read unchanged: the stored double vector with its class, whatever `int64` says
  for (mode in c("integer64", "double")) {
    expect_same(read_fstx(q, int64 = mode), x, label = mode)
    expect_same(read_fstx(q, int64 = mode, lazy = TRUE), x, label = mode)
    invisible(gc())
  }
  # and it cannot be filtered (its double statistics mean nothing)
  expect_error(read_fstx(q, where = v == 1), "columns of class 'integer64' cannot be filtered")
})

test_that("appends follow the file's int64 layout, whatever int64 says, and never change feature bits", {
  x <- i64_df(3000)
  a <- df_take(x, 1:1000)
  y <- df_rbind(x, a)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  # native file: rows appended with int64 = "integer64" stay native
  write_fstx(x, p)
  write_fstx(a, p, append = TRUE, int64 = "integer64")
  expect_identical(fstx_meta(p)$columns$type, c("integer", "int64", "int64", "int64"))
  expect_same(read_fstx(p), y)
  # 1.0-layout file: rows appended with the default stay in the 1.0 layout
  write_fstx(x, p, int64 = "integer64")
  write_fstx(a, p, append = TRUE)
  expect_identical(fstx_meta(p)$columns$type, c("integer", "double", "double", "double"))
  expect_false(length(grepRaw("int64", readBin(p, "raw", file.size(p)), fixed = TRUE)) > 0)
  expect_same(read_fstx(p), y)
  # a native column does not take plain doubles
  write_fstx(x, p)
  a$v <- as.double(seq_len(1000))
  expect_error(write_fstx(a, p, append = TRUE), "column 'v' is integer64/double in the file but double in the new rows")
})

test_that("merge compaction keeps each column's int64 layout", {
  x <- i64_df(3000)
  y <- df_rbind(x, x)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p, paste0(p, ".fstx-compact"))))
  write_fstx(x, p, int64 = "integer64")
  write_fstx(x, p, append = TRUE)
  fstx_compact(p, merge = TRUE)
  expect_identical(fstx_meta(p)$columns$type, c("integer", "double", "double", "double"))
  expect_identical(fstx_meta(p)$row_groups, 1)
  expect_same(read_fstx(p), y)
  write_fstx(x, p)
  write_fstx(x, p, append = TRUE)
  fstx_compact(p, merge = TRUE)
  expect_identical(fstx_meta(p)$columns$type, c("integer", "int64", "int64", "int64"))
  expect_identical(fstx_meta(p)$row_groups, 1)
  expect_same(read_fstx(p), y)
  fstx_compact(p)   # page copy keeps the bits
  expect_identical(fstx_meta(p)$columns$type, c("integer", "int64", "int64", "int64"))
  expect_same(read_fstx(p), y)
  # the layout hook is cleared after a merge: a fresh write is native again
  write_fstx(x, p, int64 = "integer64")
  fstx_compact(p, merge = TRUE)
  write_fstx(x, p)
  expect_identical(fstx_meta(p)$columns$type[2], "int64")
})

test_that("reads work without bit64: one message per column name and session", {
  x <- new_df(id = 1:5, cnt = i64(1:5))
  p <- tempfile(fileext = ".fstx")
  hooks <- fstx:::.test_hooks
  msgs <- fstx:::.i64_msg
  on.exit({
    unlink(p)
    hooks$no_bit64 <- NULL
  })
  write_fstx(x, p)
  hooks$no_bit64 <- TRUE
  rm(list = ls(msgs), envir = msgs)
  expect_message(y <- read_fstx(p), "install bit64 to print and compute with column 'cnt', or read it with int64 = \"double\"")
  expect_same(y, x)
  expect_silent(read_fstx(p))
  expect_silent(read_fstx(p, int64 = "double"))
  expect_silent(write_fstx(x, p, append = TRUE))
  rm(list = ls(msgs), envir = msgs)
  expect_silent(read_fstx(p, from = 1, to = 0))   # zero rows (the prototype appends and filters read): none
  expect_silent(read_fstx(p, int64 = "double"))
  expect_message(read_fstx(p, lazy = TRUE), "column 'cnt'")
  rm(list = ls(msgs), envir = msgs)
  write_fstx(new_df(a = i64(1), b = i64(2)), p)
  expect_message(read_fstx(p), "columns 'a', 'b', or read them with")
})

test_that("with bit64 installed the values are bit64's own", {
  skip_if_not_installed("bit64")
  v <- bit64::as.integer64(c("9007199254740993", "-9223372036854775807", "9223372036854775807", NA, "0", "-1"))
  expect_identical(i64_hex(v), i64_hex(i64_c(i64_parts(2^21, 1), i64_min, i64_max, i64_na, i64(0), i64(-1))))
  x <- data.frame(v = v)
  y <- roundtrip(x)
  expect_same(y, x)
  expect_identical(as.character(y$v), c("9007199254740993", "-9223372036854775807", "9223372036854775807", NA, "0", "-1"))
  expect_true(bit64::is.integer64(y$v))
  expect_identical(as.character(sum(y$v[-4])), "9007199254740992")
})

# ---------------------------------------------------------------- filters (engine drop 2)
filter_df <- function() {
  n <- 6000
  v <- i64(c(seq_len(n - 6) - 3000, NA, NA))   # -2999 .. 2994, then two NA
  v <- i64_c(v, i64(2^53), i64_parts(2^21, 1), i64_min, i64_max)   # rows n-3 .. n
  new_df(id = seq_len(n), v = v)
}
nw <- function(cond) as.numeric(which(cond & !is.na(cond)))   # expected rows (double, as fstx_which gives)

test_that("int64 filters: integer, double and integer64 values, compared exactly", {
  skip_until_drop2()
  x <- filter_df()
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p)
  n <- 6000
  d <- c(seq_len(n - 6) - 3000, rep(NA, 6))   # the small values as doubles; NA for the two NA and the 4 extremes
  # integer values (exact int64)
  expect_identical(fstx_which(p, v == 5L), nw(d == 5))
  expect_identical(fstx_which(p, v > 2990L), c(nw(d > 2990), n - 3, n - 2, n))
  expect_identical(fstx_which(p, v %in% c(1L, -7L, NA)), sort(c(nw(d %in% c(1, -7)), n - 5, n - 4)))
  expect_identical(fstx_which(p, v == NA_integer_), numeric(0))
  # doubles: exact real compare (2.5 matches nothing, < -2997.5 matches -2998 and INT64_MIN + 1)
  expect_identical(fstx_which(p, v == 2.5), numeric(0))
  expect_identical(fstx_which(p, v < -2997.5), c(nw(d < -2997.5), n - 1))
  expect_identical(fstx_which(p, v == 2^53), n - 3)          # not 2^53 + 1
  expect_identical(fstx_which(p, v > 2^53), c(n - 2, n))
  expect_identical(fstx_which(p, v >= 2^63), numeric(0))     # past INT64_MAX
  expect_identical(fstx_which(p, v > -Inf), c(nw(!is.na(d)), n - 3, n - 2, n - 1, n))
  expect_identical(fstx_which(p, v %in% c(0, 0.5, 2^53)), c(nw(d == 0), n - 3))
  expect_identical(fstx_which(p, v == NaN), numeric(0))
  # integer64 values: exact past 2^53
  expect_identical(fstx_which(p, v == i64_parts(2^21, 1)), n - 2)
  expect_identical(fstx_which(p, v != i64_parts(2^21, 1)), c(nw(!is.na(d)), n - 3, n - 1, n))
  expect_identical(fstx_which(p, v <= i64_min), n - 1)
  expect_identical(fstx_which(p, v >= i64_max), n)
  expect_identical(fstx_which(p, v %in% i64_c(i64(10), i64_max, i64_na)), c(nw(d == 10), n - 5, n - 4, n))
  expect_identical(fstx_which(p, v == i64_na), numeric(0))
  # between, is.na, !is.na, a value on the left
  expect_identical(fstx_which(p, between(v, -2L, 2L)), nw(d >= -2 & d <= 2))
  expect_identical(fstx_which(p, is.na(v)), c(n - 5, n - 4))
  expect_identical(length(fstx_which(p, !is.na(v))), as.integer(n - 2))
  expect_identical(fstx_which(p, 5L < v & v < 8L), nw(d > 5 & d < 8))
  expect_identical(fstx_which(p, v > 0 & id > 5994), c(n - 3, n - 2, n))
  # read_fstx(where) returns those rows; lazy and rows paths are the same reader
  expect_same(read_fstx(p, where = v > 2^53)$v, i64_c(i64_parts(2^21, 1), i64_max))
})

test_that("int64 filters skip pages by their rounded min / max", {
  skip_until_drop2()
  n <- 100000
  x <- new_df(s = i64(1e15 + seq_len(n)))
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p, profile = "fast")
  e <- fstx_explain(p, s <= i64(1e15 + 10))
  expect_true(e[["pages_total"]] > 1)
  expect_identical(e[["pages_skipped"]], e[["pages_total"]] - 1)
  expect_identical(e[["rows_matched"]], 0)   # plan only
  expect_identical(fstx_explain(p, s <= 1e15 + 10.5, exact = TRUE)[["rows_matched"]], 10)
  expect_identical(fstx_which(p, s > i64(1e15 + n - 3)), c(n - 2, n - 1, n))
})

test_that("int64 filters refuse strings and logical values (before the engine)", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(filter_df(), p)
  msg <- "'where' on column 'v': compare an int64 column with numbers or integer64 values"
  expect_error(fstx_which(p, v == "5"), msg)
  expect_error(fstx_which(p, v == TRUE), msg)
  expect_error(fstx_which(p, v), msg)
  expect_error(fstx_which(p, v == as.Date("2020-01-01")), msg)
})

test_that("1.0-layout integer64 columns stay unfilterable", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(filter_df(), p, int64 = "integer64")
  expect_error(fstx_which(p, v == 5L), "columns of class 'integer64' cannot be filtered")
  expect_error(fstx_which(p, v == i64(5)), "columns of class 'integer64' cannot be filtered")
})

test_that("the Arrow stream passes int64 columns as Arrow int64 (1.0 layout: float64)", {
  skip_until_drop2()
  skip_if_not_installed("nanoarrow")
  x <- new_df(id = 1:4, v = i64(c(1, NA, -3, 2^40)))
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p, q)))
  write_fstx(x, p)
  expect_identical(fstx_arrow_stream(p)$get_schema()$children$v$format, "l")
  df <- nanoarrow::convert_array_stream(fstx_arrow_stream(p))
  expect_identical(as.double(df$v), c(1, NA, -3, 2^40))
  write_fstx(x, q, int64 = "integer64")
  expect_identical(fstx_arrow_stream(q)$get_schema()$children$v$format, "g")
})
