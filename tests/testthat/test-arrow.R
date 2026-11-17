# WS2: fstx_arrow_stream(). Synthetic data only. Skipped when nanoarrow is not installed.

arrow_df <- function(s) nanoarrow::convert_array_stream(s)
# nanoarrow 0.9.0 cannot convert an ordered dictionary to R (its own as_nanoarrow_array() of an ordered factor
# fails the same way), so the R round trips below leave ordered factors out; their Arrow form is checked directly.
df_no_ord <- function(n) {
  x <- make_df(n)
  x$ord <- NULL
  x
}
child_formats <- function(s) {
  sc <- s$get_schema()
  vapply(sc$children, function(k) if (is.null(k$dictionary)) k$format else paste0("dict:", k$dictionary$format), "")
}

# what read_fstx() gives, in the R types nanoarrow converts the Arrow types back to
as_arrow_r <- function(x) {
  for (j in seq_along(x)) if (is.factor(x[[j]])) x[[j]] <- as.character(x[[j]])
  x <- as.data.frame(x, stringsAsFactors = FALSE)
  rownames(x) <- NULL
  x
}
expect_same_df <- function(got, want) {
  want <- as_arrow_r(want)
  got <- as.data.frame(got, stringsAsFactors = FALSE)
  expect_identical(names(got), names(want))
  expect_identical(nrow(got), nrow(want))
  for (j in seq_along(want)) {
    g <- got[[j]]
    w <- want[[j]]
    if (inherits(w, "POSIXct")) {   # microseconds on the way out
      expect_s3_class(g, "POSIXct")
      expect_equal(as.numeric(g), as.numeric(w), tolerance = 1e-12, info = names(want)[j])
      expect_identical(attr(g, "tzone"), attr(w, "tzone"), info = names(want)[j])
      expect_identical(is.na(g), is.na(w))
    } else if (inherits(w, "Date")) {
      expect_identical(as.numeric(g), as.numeric(floor(unclass(w))), info = names(want)[j])
    } else {
      expect_identical(g, w, info = names(want)[j])
    }
  }
}

test_that("every column type goes through the stream and back like read_fstx", {
  skip_if_not_installed("nanoarrow")
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- make_df(5000)
  x$chr_many <- sprintf("row %d %s", seq_len(nrow(x)), x$chr)   # plain (not dictionary-coded) strings
  x$chr_many[c(3, 77)] <- NA
  x$lgl_full <- rep(c(TRUE, FALSE), length.out = nrow(x))      # no NA
  x$int_full <- seq_len(nrow(x))
  x$dbl_full <- seq_len(nrow(x)) / 4
  write_fstx(x, p, threads = test_threads(2))
  s <- fstx_arrow_stream(p, threads = test_threads(2))
  f <- child_formats(s)
  expect_identical(unname(f[c("int", "dbl", "lgl", "date", "idt")]), c("i", "g", "b", "tdD", "tdD"))
  expect_identical(unname(f[c("fct", "ord")]), c("dict:u", "dict:u"))
  expect_identical(unname(f["ct"]), "tsu:Asia/Kolkata")
  expect_identical(unname(f["ctu"]), "tsu:UTC")
  expect_identical(unname(f["chr"]), "dict:u")                 # few distinct values: dictionary-coded pages
  expect_identical(unname(f["chr_many"]), "u")
  sc <- s$get_schema()
  expect_identical(bitwAnd(sc$children$ord$flags, 1L), 1L)      # ordered factor -> ordered dictionary
  expect_identical(bitwAnd(sc$children$fct$flags, 1L), 0L)
  cols <- setdiff(names(x), "ord")
  expect_same_df(arrow_df(fstx_arrow_stream(p, columns = cols, threads = test_threads(2))), read_fstx(p, columns = cols))
  b <- fstx_arrow_stream(p, columns = "ord", threads = 1)$get_next()$children$ord
  expect_identical(nanoarrow::convert_array(b$dictionary), levels(x$ord))
  idx <- nanoarrow::convert_buffer(b$buffers[[2]])
  expect_identical(as.integer(idx) + 1L, as.integer(x$ord))
})

test_that("NA, NaN and Inf in doubles: NA becomes null, NaN and Inf stay", {
  skip_if_not_installed("nanoarrow")
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- data.frame(d = c(1.5, NA, NaN, Inf, -Inf, 0))
  write_fstx(x, p, threads = 1)
  b <- fstx_arrow_stream(p, threads = 1)$get_next()
  expect_equal(b$children$d$null_count, 1)
  expect_identical(nanoarrow::convert_array(b)$d, x$d)
})

test_that("fractional Dates round down to the day; times round to the microsecond", {
  skip_if_not_installed("nanoarrow")
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- data.frame(d = as.Date(c(1.5, -0.5, NA, 10)),
                  t = as.POSIXct(c(0.0000004, 1.0000006, NA, -1.5), tz = "UTC"))
  write_fstx(x, p, threads = 1)
  y <- arrow_df(fstx_arrow_stream(p, threads = 1))
  expect_identical(as.numeric(y$d), c(1, -1, NA, 10))
  expect_equal(as.numeric(y$t), c(0, 1.000001, NA, -1.5), tolerance = 1e-12)
})

test_that("empty table, zero columns, columns, from / to and where", {
  skip_if_not_installed("nanoarrow")
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- df_no_ord(3000)
  write_fstx(x, p, threads = 1)
  y <- read_fstx(p)
  # columns by name, in any order, repeats made unique as x[, cols] does
  expect_same_df(arrow_df(fstx_arrow_stream(p, columns = c("chr", "int", "chr"), threads = 1)),
                 read_fstx(p, columns = c("chr", "int", "chr")))
  expect_same_df(arrow_df(fstx_arrow_stream(p, columns = c(4, 1), threads = 1)), read_fstx(p, columns = c(4, 1)))
  expect_same_df(arrow_df(fstx_arrow_stream(p, from = 101, to = 2050, threads = 1)),
                 read_fstx(p, from = 101, to = 2050))
  expect_same_df(arrow_df(fstx_arrow_stream(p, where = int > 500 & chr == "plain", threads = 1)),
                 read_fstx(p, where = int > 500 & chr == "plain"))
  expect_same_df(arrow_df(fstx_arrow_stream(p, where = int > 500, from = 10, to = 2000, threads = 1)),
                 read_fstx(p, where = int > 500, from = 10, to = 2000))
  # a filter that matches nothing, an empty window, and no columns
  s <- fstx_arrow_stream(p, where = int > 5000, threads = 1)
  expect_null(s$get_next())
  expect_identical(nrow(arrow_df(fstx_arrow_stream(p, from = 5, to = 4, threads = 1))), 0L)
  z <- arrow_df(fstx_arrow_stream(p, columns = character(0), threads = 1))
  expect_identical(dim(z), c(3000L, 0L))
  # a file with no rows
  q <- tempfile(fileext = ".fstx")
  on.exit(unlink(q), add = TRUE)
  write_fstx(x[0, ], q, threads = 1)
  s <- fstx_arrow_stream(q, threads = 1)
  expect_identical(length(s$get_schema()$children), ncol(x))
  e <- arrow_df(fstx_arrow_stream(q, threads = 1))
  expect_identical(dim(e), c(0L, ncol(x)))
  # argument checks
  expect_error(fstx_arrow_stream(p, columns = "nope"), "not in the file")
  expect_error(fstx_arrow_stream(p, columns = 99), "column positions")
  expect_error(fstx_arrow_stream(p, from = 4000), "beyond the last row")
  expect_error(fstx_arrow_stream(p, batch_rows = 0), "batch_rows")
  expect_error(fstx_arrow_stream(p, where = nope > 1), "^fstx_arrow_stream: ")   # not "read_fstx:"
  # a huge thread count is capped at the hardware threads (a batch starts its own threads)
  expect_same_df(arrow_df(fstx_arrow_stream(p, threads = 5000)), y)
})

test_that("batches follow row groups and batch_rows; dictionaries come per row group", {
  skip_if_not_installed("nanoarrow")
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- df_no_ord(4000)
  write_fstx(x[1:1500, ], p, threads = 1)
  write_fstx(x[1501:2600, ], p, append = TRUE, threads = 1)   # each append is a new row group
  write_fstx(x[2601:4000, ], p, append = TRUE, threads = 1)
  lens <- function(s) {
    n <- integer(0)
    while (!is.null(b <- s$get_next())) n <- c(n, b$length)
    n
  }
  expect_identical(lens(fstx_arrow_stream(p, threads = 1)), c(1500L, 1100L, 1400L))
  expect_identical(lens(fstx_arrow_stream(p, batch_rows = 1000, threads = 1)), c(1000L, 500L, 1000L, 100L, 1000L, 400L))
  expect_identical(lens(fstx_arrow_stream(p, from = 1400, to = 1600, threads = 1)), c(101L, 100L))
  y <- read_fstx(p)
  expect_same_df(arrow_df(fstx_arrow_stream(p, batch_rows = 333, threads = test_threads(2))), y)
  expect_same_df(arrow_df(fstx_arrow_stream(p, where = int < 300, batch_rows = 50, threads = 1)),
                 read_fstx(p, where = int < 300))
})

test_that("row parts of fixed-width columns: same batches at 1 and many threads, same as read_fstx", {
  skip_if_not_installed("nanoarrow")
  # Default pages hold 32,768 doubles or 65,536 ints; each write below is its own row group. With fewer columns
  # than threads a fixed-width column is filled in row parts cut at page edges; the batches must not change.
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  n <- 300000
  x <- make_df(n, seed = 7)
  x$chr <- NULL
  x$ord <- NULL
  x$dna <- seq_len(n) / 8                  # NA only from row 200,001: pages before it say null_count 0
  x$dna[seq(200001, n, by = 997)] <- NA
  x$dfull <- seq_len(n) / 4                # no NA: no validity bitmap
  write_fstx(x[1:130001, ], p, threads = 1)
  write_fstx(x[130002:230004, ], p, append = TRUE, threads = 1)
  write_fstx(x[230005:n, ], p, append = TRUE, threads = 1)
  hw <- fstx:::fstx_hw_threads()
  all_batches <- function(s) {
    out <- list()
    while (!is.null(b <- s$get_next())) out[[length(out) + 1L]] <- b
    out
  }
  same_batches <- function(...) {
    b1 <- all_batches(fstx_arrow_stream(p, threads = 1, ...))
    bn <- all_batches(fstx_arrow_stream(p, threads = hw, ...))
    expect_identical(length(bn), length(b1))
    for (i in seq_along(b1)) {
      for (nm in names(b1[[i]]$children)) {
        g <- bn[[i]]$children[[nm]]
        w <- b1[[i]]$children[[nm]]
        expect_identical(c(g$length, g$null_count), c(w$length, w$null_count), info = nm)
        expect_identical(lapply(g$buffers, as.raw), lapply(w$buffers, as.raw), info = nm)
      }
    }
  }
  same_batches()
  same_batches(columns = "dbl")                                      # one column: as many parts as threads
  same_batches(columns = c("dna", "lgl"), from = 32761, to = 262150, batch_rows = 50001)
  same_batches(columns = c("int", "dna", "ct"), where = dbl > 0.5)   # selected rows
  y <- read_fstx(p)
  expect_same_df(arrow_df(fstx_arrow_stream(p, threads = 1)), y)
  expect_same_df(arrow_df(fstx_arrow_stream(p, threads = hw)), y)
  expect_same_df(arrow_df(fstx_arrow_stream(p, columns = "dbl", threads = hw)), read_fstx(p, columns = "dbl"))
  expect_same_df(arrow_df(fstx_arrow_stream(p, from = 32761, to = 262150, batch_rows = 50001, threads = hw)),
                 read_fstx(p, from = 32761, to = 262150))
  expect_same_df(arrow_df(fstx_arrow_stream(p, where = dbl > 0.5, threads = hw)), read_fstx(p, where = dbl > 0.5))
  b <- fstx_arrow_stream(p, columns = c("dfull", "dna"), threads = hw)$get_next()
  expect_identical(length(as.raw(b$children$dfull$buffers[[1]])), 0L)
  expect_equal(b$children$dna$null_count, 0)                         # first row group: rows 1 to 130,001
})

test_that("the stream releases everything once, also when dropped half-read", {
  skip_if_not_installed("nanoarrow")
  # in a fresh R process, so the count of live stream / schema / array holders starts at 0
  fw <- function(x) normalizePath(x, winslash = "/", mustWork = FALSE)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- df_no_ord(3000)
  write_fstx(x[1:1000, ], p, threads = 1)
  write_fstx(x[1001:3000, ], p, append = TRUE, threads = 1)
  script <- tempfile(fileext = ".R")
  on.exit(unlink(script), add = TRUE)
  writeLines(c(
    sprintf(".libPaths(%s)   # the child must load this fstx, not another installed copy",
            paste(deparse(fw(.libPaths())), collapse = "")),
    "suppressPackageStartupMessages(library(fstx))",
    "live <- function() fstx:::fstx_arrow_stream_impl(list(op = 'live'))",
    "settle <- function() for (i in 1:3) invisible(gc())",
    sprintf("p <- '%s'", fw(p)),
    "n0 <- live()",
    "s <- fstx_arrow_stream(p, threads = 1)",
    "b <- s$get_next()                                   # one of two batches read",
    "n1 <- live()",
    "nanoarrow::nanoarrow_pointer_release(s)",
    "v1 <- nanoarrow::nanoarrow_pointer_is_valid(s)",
    "nanoarrow::nanoarrow_pointer_release(s)             # a second release is a no-op",
    "ok_b <- identical(nanoarrow::convert_array(b$children$int), read_fstx(p, columns = 'int', to = 1000)$int)",
    "rm(b, s); settle()",
    "n2 <- live()",
    "s <- fstx_arrow_stream(p, threads = 1); invisible(s$get_next()); rm(s); settle()   # dropped, not released",
    "n3 <- live()",
    "cat('RESULT', n0, n1 > n0, v1, ok_b, n2, n3, file.remove(p), '\n')"), script)
  rs <- file.path(R.home("bin"), "Rscript")
  out <- system2(rs, shQuote(fw(script)), stdout = TRUE, stderr = TRUE)
  expect_identical(grep("^RESULT", out, value = TRUE), "RESULT 0 TRUE FALSE TRUE 0 0 TRUE ")
})

test_that("batches can be pulled from a thread that is not R's", {
  skip_if_not_installed("nanoarrow")
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- df_no_ord(6000)
  write_fstx(x[1:2500, ], p, threads = 1)
  write_fstx(x[2501:6000, ], p, append = TRUE, threads = 1)
  s <- fstx_arrow_stream(p, threads = test_threads(2), batch_rows = 1000)
  r <- fstx:::fstx_arrow_stream_impl(list(op = "pull_thread", stream = s))
  expect_true(r$other_thread)
  expect_identical(r$error, "")
  expect_identical(r$batches, 7)
  expect_identical(r$rows, 6000)
  expect_null(s$get_next())             # the thread read it to the end
})

test_that("a damaged page comes back as a stream error, not a crash", {
  skip_if_not_installed("nanoarrow")
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(data.frame(a = seq_len(5000), b = sprintf("s%d", seq_len(5000))), p, threads = 1)
  pg <- fstx_pages(p)
  flip_byte(p, pg$offset[pg$name == "b"][1] + 5)
  s <- fstx_arrow_stream(p, threads = 1)
  expect_error(s$get_next(), "checksum|corrupt")
})

test_that("arrow, DuckDB and polars take the stream", {
  skip_if_not_installed("nanoarrow")
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- df_no_ord(2000)
  write_fstx(x, p, threads = 1)
  want <- as_arrow_r(read_fstx(p, columns = c("int", "dbl", "chr", "lgl")))
  cols <- c("int", "dbl", "chr", "lgl")
  # optional consumers, not in Suggests: package names go through variables so R CMD check does not list them
  has <- function(pkg) requireNamespace(pkg, quietly = TRUE)
  fun <- function(pkg, f) getExportedValue(pkg, f)
  if (has("arrow")) {
    tb <- as.data.frame(fun("arrow", "as_arrow_table")(fstx_arrow_stream(p, columns = cols, threads = 1)))
    expect_same_df(as_arrow_r(tb), want)   # arrow makes dictionaries into factors
  }
  if (has("duckdb") && has("DBI")) {
    con <- fun("DBI", "dbConnect")(fun("duckdb", "duckdb")())
    on.exit(fun("DBI", "dbDisconnect")(con, shutdown = TRUE), add = TRUE)
    fun("duckdb", "duckdb_register_arrow")(con, "t", fstx_arrow_stream(p, columns = cols, threads = 1))
    expect_equal(fun("DBI", "dbGetQuery")(con, "SELECT count(*) AS n FROM t")$n, 2000)
  }
  if (has("polars")) {
    pl <- as.data.frame(fun("polars", "as_polars_df")(fstx_arrow_stream(p, columns = cols, threads = 1)))
    expect_identical(nrow(pl), 2000L)
  }
  skip_if(!any(vapply(c("arrow", "duckdb", "polars"), requireNamespace, NA, quietly = TRUE)),
          "none of arrow, duckdb, polars is installed")
})
