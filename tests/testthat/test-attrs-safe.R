# Attribute blobs in the FXA1 encoding (spec 9.4): round trips, the serialize() fallback, damaged blobs and
# read_fstx(attributes = FALSE). Synthetic data only.

magic <- as.raw(c(0x46, 0x58, 0x41, 0x01))
blob_kind <- function(b) {
  if (is.null(b)) "none" else if (identical(b[1:4], magic)) "fxa" else if (identical(b[1:2], charToRaw("B\n"))) "rds" else "?"
}
blobs_of <- function(p) {
  m <- fstx:::fstx_meta_impl(p)
  list(table = blob_kind(m$table_attrs), cols = setNames(vapply(m$col_attrs, blob_kind, ""), m$name))
}
rt <- function(x, ...) {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p, threads = 1L)
  list(y = read_fstx(p, ...), blobs = blobs_of(p))
}
# fails the block if unserialize() is called inside it
no_unserialize <- function(code) {
  suppressMessages(trace("unserialize", quote(stop("unserialize() was called")), print = FALSE, where = baseenv()))
  on.exit(suppressMessages(untrace("unserialize", where = baseenv())))
  force(code)
}
i32 <- function(...) writeBin(as.integer(c(...)), raw(), size = 4L, endian = "little")
dec <- function(b) fstx:::.attr_decode(b, "f.fstx", "table")

test_that("a plain data.frame's table blob is the spec's 43-byte example", {
  b <- fstx:::.attr_encode(list(class = "data.frame"))
  want <- c(magic, i32(1, 5), charToRaw("class"), i32(4, 1, 10), charToRaw("data.frame"), i32(0))
  expect_identical(b, want)
  expect_length(b, 43L)
  expect_identical(dec(b), list(class = "data.frame"))
  expect_lt(length(b), length(serialize(list(class = "data.frame"), NULL, xdr = FALSE)))
})

test_that("data.frame, row names and column attributes round trip through FXA1 without unserialize()", {
  set.seed(1)
  x <- data.frame(i = 1:6, d = c(1.5, NA, NaN, -0, Inf, 2), s = c("a", NA, "", "café", "日本", "z"),
                  f = factor(c("u", "v", "u", NA, "v", "u")), stringsAsFactors = FALSE)
  x$lab <- structure(c(1, 2, NA, 1, 2, 1), label = "Q1 caption", labels = c(Yes = 1, No = 2),
                     class = c("haven_labelled", "vctrs_vctr", "double"))
  x$named <- structure(1:6, names = letters[1:6])
  x$odd <- structure(6:1, flag = c(TRUE, NA, FALSE), empty_i = integer(0), empty_s = character(0),
                     na_s = NA_character_, dbl = c(NaN, -0, 1e300), when = as.Date("2026-11-17"),
                     lev = factor("b", levels = c("a", "b")), "naïve name" = "ü")
  attr(x, "note") <- "table note"
  attr(x, "long") <- strrep("x", 100000)
  rownames(x) <- sprintf("row%d", 1:6)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p, threads = 1L)
  b <- blobs_of(p)
  expect_identical(b$table, "fxa")
  expect_identical(unname(b$cols[c("lab", "odd")]), c("fxa", "fxa"))
  no_unserialize({
    expect_identical(read_fstx(p), x)
    expect_identical(read_fstx(p, lazy = TRUE), x)
    expect_identical(read_fstx(p, attributes = FALSE), x)
    expect_identical(fstx_meta(p)$columns$class[5], "haven_labelled/vctrs_vctr/double")
  })
  y <- x[c(4, 2, 6), ]                                     # integer row names
  rownames(y) <- NULL
  y <- y[c(3, 1), ]
  expect_identical(rt(y)$y, y)
  expect_identical(rt(y)$blobs$table, "fxa")
})

test_that("data.table (key, index) and tibble-like tables round trip through FXA1", {
  tb <- structure(data.frame(a = 1:3, b = c("x", "y", "z"), stringsAsFactors = FALSE),
                  class = c("tbl_df", "tbl", "data.frame"))
  r <- rt(tb)
  expect_identical(r$y, tb)
  expect_identical(r$blobs$table, "fxa")
  skip_if_not_installed("data.table")
  dt <- data.table::data.table(k = c(3L, 1L, 2L, 1L), g = c("b", "a", "b", "c"), v = c(0.5, 1, 2, 3))
  data.table::setkeyv(dt, "k")
  data.table::setindexv(dt, "g")
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(dt, p, threads = 1L)
  expect_identical(blobs_of(p)$table, "fxa")
  y <- no_unserialize(read_fstx(p))
  expect_identical(data.table::key(y), "k")
  expect_identical(data.table::indices(y), "g")
  expect_equal(y, dt)
  expect_identical(attributes(y)[c("sorted", "index")], attributes(dt)[c("sorted", "index")])
  data.table::set(y, j = "extra", value = 1L)   # over-allocated as before
})

test_that("attributes FXA1 cannot hold fall back to serialize() and attributes = FALSE skips them", {
  x <- data.frame(a = 1:3, b = c(0.5, 1, 2))
  x$lab <- structure(1:3, label = "a label")
  x$lst <- structure(1:3, label = "kept only with attributes = TRUE", myattr = list(k = 1))
  x$deep <- structure(1:3, outer = structure(1L, inner = structure(2L, inner2 = structure(3L, innermost = 4L))))
  x$cplx <- structure(1:3, z = 1i)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p, threads = 1L)
  b <- blobs_of(p)
  expect_identical(b$table, "fxa")
  expect_identical(unname(b$cols), c("none", "none", "fxa", "rds", "rds", "rds"))
  expect_identical(read_fstx(p), x)
  y <- no_unserialize(read_fstx(p, attributes = FALSE))
  expect_identical(attributes(y$lab), list(label = "a label"))
  for (k in c("lst", "deep", "cplx")) expect_identical(y[[k]], 1:3, label = k)
  expect_identical(y[c("a", "b", "lab")], x[c("a", "b", "lab")])
  expect_error(read_fstx(p, attributes = NA), "'attributes' must be TRUE or FALSE")
})

test_that("files with R-serialized blobs (fstx 1.0 before FXA1) still read; attributes = FALSE skips them", {
  old <- test_path("fixtures", "attrs_serialized.fstx")
  new <- test_path("fixtures", "golden-1.0", "types.fstx")
  m <- fstx:::fstx_meta_impl(old)
  expect_identical(blob_kind(m$table_attrs), "rds")
  expect_identical(blobs_of(new)$table, "fxa")
  x <- read_fstx(old)
  expect_identical(x, read_fstx(new))
  expect_identical(fstx_meta(old)$columns, fstx_meta(new)$columns)
  y <- no_unserialize(read_fstx(old, attributes = FALSE))
  expect_identical(class(y), "data.frame")
  expect_identical(.row_names_info(y), -nrow(x))            # automatic row names
  expect_null(attr(y, "note"))
  expect_identical(attributes(y$labelled), NULL)
  keep <- setdiff(names(x), "labelled")
  z <- x[keep]
  rownames(z) <- NULL
  attr(z, "note") <- NULL
  expect_identical(y[keep], z)                             # factor, Date, POSIXct are native types
})

test_that("damaged FXA1 blobs give the corrupt error, never a crash", {
  good <- fstx:::.attr_encode(list(class = "data.frame", row.names = c("a", NA, "ccc"), n = 1:3,
                                   l = c(TRUE, NA), d = 2.5, f = factor("q")))
  expect_identical(dec(good)$f, factor("q"))
  for (k in 3:(length(good) - 1L)) expect_error(dec(good[1:k]), "is corrupt \\(table attributes", info = k)
  bad <- list(
    trailing = c(good, as.raw(0)),
    version2 = c(magic[1:3], as.raw(2), good[-(1:4)]),
    count_huge = c(magic, i32(.Machine$integer.max)),
    count_neg = c(magic, i32(-1)),
    count_na = c(magic, i32(NA)),
    name_len0 = c(magic, i32(1, 0, 4, 0, 0)),
    name_len_huge = c(magic, i32(1, 1e9), charToRaw("a"), i32(4, 0, 0)),
    type0 = c(magic, i32(1, 1), charToRaw("a"), i32(0, 0, 0)),
    type5 = c(magic, i32(1, 1), charToRaw("a"), i32(5, 0, 0)),
    len_huge = c(magic, i32(1, 1), charToRaw("a"), i32(2, .Machine$integer.max), i32(1, 0)),
    len_neg = c(magic, i32(1, 1), charToRaw("a"), i32(3, -5, 0)),
    dbl_short = c(magic, i32(1, 1), charToRaw("a"), i32(3, 2), writeBin(1, raw(), size = 8), i32(0)),
    logical2 = c(magic, i32(1, 1), charToRaw("a"), i32(1, 1, 2, 0)),
    str_len_huge = c(magic, i32(1, 1), charToRaw("a"), i32(4, 1, 1e9, 0)),
    str_len_m2 = c(magic, i32(1, 1), charToRaw("a"), i32(4, 1, -2, 0)),
    str_len_na = c(magic, i32(1, 1), charToRaw("a"), i32(4, 1, NA, 0)),
    str_nul = c(magic, i32(1, 1), charToRaw("a"), i32(4, 1, 3), charToRaw("a"), as.raw(0), charToRaw("b"), i32(0)),
    name_nul = c(magic, i32(1, 2), as.raw(c(0x61, 0)), i32(2, 0, 0)),
    name_utf8 = c(magic, i32(1, 2), as.raw(c(0xff, 0xfe)), i32(2, 0, 0)),
    dup_names = c(magic, i32(2, 1), charToRaw("a"), i32(2, 0, 0, 1), charToRaw("a"), i32(2, 0, 0)),
    nested4 = c(magic, i32(1, 1), charToRaw("a"), i32(2, 0, 1, 1), charToRaw("b"), i32(2, 0, 1, 1),
                charToRaw("c"), i32(2, 0, 1, 1), charToRaw("d"), i32(2, 0, 0)),
    nested_dim = c(magic, i32(1, 1), charToRaw("a"), i32(2, 3, 1, 2, 3, 1, 3), charToRaw("dim"), i32(2, 2, 2, 2, 0))
  )
  for (k in names(bad)) expect_error(dec(bad[[k]]), "is corrupt \\(table attributes", info = k)
  # random damage: a list or the corrupt error, nothing else
  set.seed(42)
  for (r in 1:400) {
    b <- good
    at <- sample(5:length(b), sample(1:3, 1))
    b[at] <- as.raw(sample(0:255, length(at), TRUE))
    if (r %% 4 == 0) b <- b[seq_len(sample(4:length(b), 1))]
    e <- tryCatch({ dec(b); "" }, error = function(e) conditionMessage(e))
    expect_true(e == "" || grepl("is corrupt (table attributes", e, fixed = TRUE), info = paste(r, e))
  }
})

test_that("damaged FXA1 blobs in a file: read_fstx, fstx_meta and append say corrupt", {
  x <- data.frame(a = 1:3, b = c("x", "y", "z"), stringsAsFactors = FALSE)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  cut <- fstx:::.attr_encode(list(class = "data.frame"))[1:20]
  write_fstx(x, p, threads = 1L, .blobs = list(table = cut))
  expect_error(read_fstx(p), "is corrupt \\(table attributes")
  expect_error(read_fstx(p, attributes = FALSE), "is corrupt \\(table attributes")   # FXA1 is always decoded
  expect_error(fstx_meta(p), "is corrupt \\(table attributes")
  expect_error(write_fstx(x, p, append = TRUE), "is corrupt \\(table attributes")
  dim_blob <- fstx:::.attr_encode(list(dim = c(2L, 2L)))                    # a matrix the writer never makes
  write_fstx(x, p, threads = 1L, .blobs = list(cols = list(dim_blob, NULL)))
  expect_error(read_fstx(p), "is corrupt \\(column 'a' attributes")
  expect_error(read_fstx(p, lazy = TRUE), "is corrupt \\(column 'a' attributes")
  write_fstx(x, p, threads = 1L, .blobs = list(cols = list(NULL, c(magic, i32(1)))))
  expect_error(read_fstx(p), "is corrupt \\(column 'b' attributes")
  expect_error(fstx_meta(p), "is corrupt \\(column 'b' attributes")
})

test_that("append keeps an R-serialized table blob, and re-encodes one it changes as FXA1", {
  x <- data.frame(a = 1:3, s = c("x", "y", "z"), stringsAsFactors = FALSE)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p, threads = 1L, .blobs = list(table = serialize(list(class = "data.frame"), NULL, xdr = FALSE)))
  expect_identical(blobs_of(p)$table, "rds")
  write_fstx(x, p, threads = 1L, append = TRUE)
  expect_identical(blobs_of(p)$table, "rds")
  z <- rbind(x, x)
  expect_identical(read_fstx(p), z)
  y <- x[c(3, 1), ]                                        # stored integer row names: dropped on append
  write_fstx(y, p, threads = 1L, .blobs = list(table = serialize(attributes(y)[c("row.names", "class")], NULL,
                                                                 xdr = FALSE)))
  write_fstx(x, p, threads = 1L, append = TRUE)
  expect_identical(blobs_of(p)$table, "fxa")
  z <- rbind(y, x)
  rownames(z) <- NULL
  expect_identical(read_fstx(p), z)
})
