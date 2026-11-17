# Python -> R check of the files test_write.py makes from data that never came from R (python/DESIGN.md).
#
#   Rscript check_native.R [golden dir]       (default: the folder this script is in)
#
# Reads every file of <golden dir>/from_python/native/ with the R package and compares it with the table the
# Python test built, written out again here in R. PASS when every column has the same type, class, attributes
# and values (all.equal with tolerance 0, plus NA vs NaN) and the table attributes match. Exit status 1 on FAIL.

args <- commandArgs(TRUE)
me <- normalizePath(sub("^--file=", "", grep("^--file=", commandArgs(FALSE), value = TRUE)[1]), winslash = "/")
gdir <- if (length(args)) normalizePath(args[1], winslash = "/") else dirname(me)
ndir <- file.path(gdir, "from_python", "native")
suppressPackageStartupMessages({
  library(fstx)
  library(bit64)
})
if (!dir.exists(ndir)) stop("no folder ", ndir, ": run the Python tests first")

hindi <- "हिन्दी"
utc <- function(x, tz = "UTC") .POSIXct(x, tz = tz)
hms <- function(x) structure(x, units = "secs", class = c("hms", "difftime"))
secs <- function(x) structure(x, class = "difftime", units = "secs")
df <- function(...) data.frame(..., stringsAsFactors = FALSE, check.names = FALSE)
n <- 150000L
big <- function() df(i = 0:(n - 1L), f = (0:(n - 1L)) * 0.5, s = paste0("row", 0:(n - 1L)),
                     g = factor(paste0("g", (0:(n - 1L)) %% 7L), levels = paste0("g", 0:6)))
small <- df(x = 1:3, s = c("a", NA, "c"))
stamp <- c(1704103200, NA, 1719791999.5)  # 2024-01-01 10:00:00 and 2024-06-30 23:59:59.5 UTC
ist <- stamp - 19800

want <- list(
  native_types = function() df(
    b = c(TRUE, FALSE, NA), i8 = c(1L, -128L, NA), i16 = c(1L, -32768L, NA), u8 = c(0L, 255L, NA),
    u16 = c(0L, 65535L, NA), i32 = c(2147483647L, -2147483647L, NA), u32_small = c(0L, 7L, NA),
    u32_big = as.integer64(c("0", "4294967295", NA)), i64_small = c(5L, -5L, NA),
    i64_big = as.integer64(c("4611686018427387904", "-9223372036854775807", NA)),
    u64 = as.integer64(c("0", "9223372036854775807", NA)),
    f32 = c(1.5, -0.25, NA), f64 = c(0.1, NaN, NA), s = c("a", hindi, NA),
    ls = c("", strrep("x", 300), NA), d32 = as.Date(c("2024-02-29", "1969-12-31", NA)),
    d64 = structure(c(0, 2, NA), class = "Date"), ts_s = utc(c(0, 1700000000, NA)),
    ts_ms = utc(c(1500, -1, NA) / 1e3), ts_us = utc(c(1700000000123456, 0, NA) / 1e6),
    ts_ns = utc(c(1000000001, 0, NA) / 1e9), ts_tz = utc(c(0, 1700000000, NA), "Asia/Kolkata"),
    t32 = hms(c(3661, 0, NA)), t64 = hms(c(1.5, 0, NA)), dur = secs(c(90, -1, NA)), dur_ms = secs(c(1.5, 0, NA)),
    fac = factor(c("lo", "hi", NA), levels = c("lo", "hi")),
    ord = factor(c("M", "S", NA), levels = c("S", "M"), ordered = TRUE),
    dint = c(10L, 20L, NA), nul = c(NA, NA, NA)),
  chunked = function() df(
    i = c(1:5, NA, 7:11), f = c(1, 2, NA, 4:11) / 4, s = c(paste0("s", 1:6), NA, paste0("s", 8:11)),
    ls = c(paste0("s", 1:6), NA, paste0("s", 8:11)),
    d = factor(c("a", "b", NA, "b", "c", "b", "b", "c", NA, "b", "c"), levels = c("a", "b", "c"))),
  big.default = big, big.lz4 = big, big.none = big, big.small_1thread = big, big.fast = big, big.zstd1 = big,
  from_table = function() small, from_batch = function() small, from_reader = function() small,
  from_stream = function() small,
  empty = function() data.frame(),
  zero_rows = function() df(x = integer(0), s = character(0), f = factor(character(0))),
  int64_auto_small = function() df(a = c(1L, 2L, NA)),
  int64_auto_big = function() df(a = as.integer64(c("1", "1099511627776", NA))),
  int64_integer64 = function() df(a = as.integer64(c(1, 2, NA))),
  int64_double = function() df(a = c(1, 2^40, NA)),
  appended_py = function() df(
    x = 1:3, f = factor(c("a", "b", "c")), s = c("p", NA, "q"), t = utc(c(0, 1, 2), "Asia/Kolkata"),
    d = as.Date(c("2024-01-01", NA, "2024-01-02")), n = 7:9),
  pandas_native = function() df(
    i = 1:3, x = c(1.5, NA, 2.5), s = c("a", NA, "c"), c = factor(c("u", "v", "u"), levels = c("v", "u", "w")),
    o = factor(c("lo", "hi", NA), levels = c("lo", "hi"), ordered = TRUE), t = utc(stamp),
    tz = utc(ist, "Asia/Kolkata"), I = c(1L, NA, 3L), bo = c(TRUE, NA, FALSE),
    d = as.Date(c("2024-01-01", NA, "2020-02-29")), row.names = c("r1", "r2", "r3")),
  polars_native = function() df(
    i = c(1L, 2L, NA), f = c(1.5, NA, 2.5), s = c("a", NA, "c"), b = c(TRUE, NA, FALSE),
    d = as.Date(c("2024-01-01", NA, "2020-02-29")), t = utc(stamp), c = factor(c("x", "y", "x")),
    i8 = c(1L, -1L, NA), u64 = as.integer64(c("1", "1099511627776", NA))),
  strings_invalid_utf8.binary = function() read_fstx(file.path(gdir, "strings_invalid_utf8.fstx")),
  classes_blob.nometa = NULL  # R's own classes are gone on purpose: only has to read
)

differ <- function(a, b) {
  out <- character(0)
  if (!identical(names(a), names(b))) return(sprintf("names: %s vs %s", toString(names(a)), toString(names(b))))
  if (nrow(a) != nrow(b)) return(sprintf("rows: %d vs %d", nrow(a), nrow(b)))
  for (j in seq_along(a)) {
    x <- a[[j]]
    y <- b[[j]]
    why <- if (!identical(typeof(x), typeof(y))) sprintf("type %s vs %s", typeof(x), typeof(y))
           else if (!identical(oldClass(x), oldClass(y))) sprintf("class %s vs %s", toString(oldClass(x)), toString(oldClass(y)))
           else if (!isTRUE(e <- all.equal(x, y, tolerance = 0))) paste(e, collapse = "; ")
           else if (is.double(x) && !identical(is.nan(unclass(x)), is.nan(unclass(y)))) "NA vs NaN"
           else if (inherits(x, "integer64") && !identical(writeBin(unclass(x), raw()), writeBin(unclass(y), raw()))) "integer64 bits"
           else if (is.character(x) && !identical(charToRaw(paste(x, collapse = "\001")), charToRaw(paste(y, collapse = "\001")))) "string bytes"
           else NULL
    if (!is.null(why)) out <- c(out, sprintf("column '%s': %s", names(a)[j], why))
  }
  ta <- attributes(a)[setdiff(names(attributes(a)), "names")]
  tb <- attributes(b)[setdiff(names(attributes(b)), "names")]
  if (!isTRUE(e <- all.equal(ta[order(names(ta))], tb[order(names(tb))]))) out <- c(out, paste("table attributes:", paste(e, collapse = "; ")))
  head(out, 6L)
}

files <- sort(list.files(ndir, pattern = "\\.fstx$"))
res <- character(0)
for (f in files) {
  key <- sub("\\.fstx$", "", f)
  got <- tryCatch(read_fstx(file.path(ndir, f), threads = 1L), error = function(e) e)
  why <- if (inherits(got, "error")) paste("could not be read:", conditionMessage(got))
         else if (!key %in% names(want)) "no expectation in check_native.R"
         else if (is.null(want[[key]])) character(0)
         else differ(got, want[[key]]())
  st <- if (length(why)) "FAIL" else "PASS"
  res <- c(res, st)
  cat(sprintf("%-6s %s\n", st, f))
  for (w in why) cat("         ", w, "\n")
}
miss <- setdiff(names(want), sub("\\.fstx$", "", files))
cat(sprintf("\n%d files: %s\n", length(res), paste(names(table(res)), table(res), sep = " ", collapse = ", ")))
if (length(miss)) cat("expected files that are missing:", paste(miss, collapse = ", "), "\n")
if (any(res == "FAIL") || length(miss)) quit(status = 1L)
