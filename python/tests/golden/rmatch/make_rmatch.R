# R-match golden files: tables written by R, with R's own view of every value, for test_rmatch.py.
#
#   Rscript make_rmatch.R [out dir]          (default: the folder this script is in)
#
# Needs fstx 1.0.0, bit64 and jsonlite. Writes <case>.fstx and <case>.view.json (rview.R: per column typeof,
# class, tzone, levels and every value as R shows it). test_rmatch.py reads each file with pyarrow, pandas and
# polars and compares what Python shows with this view, then writes the tables back for check_rmatch.R.
# Synthetic values only.

args <- commandArgs(TRUE)
me <- normalizePath(sub("^--file=", "", grep("^--file=", commandArgs(FALSE), value = TRUE)[1]), winslash = "/")
here <- dirname(me)
out <- if (length(args)) normalizePath(args[1], winslash = "/", mustWork = FALSE) else here
suppressPackageStartupMessages({
  library(fstx)
  library(bit64)
  library(jsonlite)
})
stopifnot(identical(as.character(packageVersion("fstx")), "1.0.0"))
source(file.path(here, "rview.R"))
dir.create(out, recursive = TRUE, showWarnings = FALSE)

case <- function(name, x, about) {
  p <- file.path(out, paste0(name, ".fstx"))
  write_fstx(x, p, threads = 1L)
  y <- read_fstx(p, threads = 1L)
  if (!identical(y, x)) stop(sprintf("%s does not read back identical() in R", name))
  v <- c(list(file = basename(p), about = about, made_with = paste("fstx 1.0.0,", R.version.string)), rview(y))
  writeLines(rview_json(v, pretty = TRUE), file.path(out, paste0(name, ".view.json")), useBytes = TRUE)
  cat(sprintf("%-24s %6.0f bytes\n", basename(p), file.size(p)))
}
ct <- function(v, tz) .POSIXct(v, tz = tz)

# ---------------------------------------------------------------- part A: dates, times, integer64
days <- as.Date(c("1969-12-31", "1970-01-01", "2024-02-29", "1900-03-01", "0001-01-01", "9999-12-31", NA, "2026-11-18"))
# 1969-12-31 23:59:58.5, the epoch, a quarter second, New York's spring-forward instant (07:00 UTC) and the
# second before it, 01:30 New York twice (EDT, then EST), 9999-12-31 23:59:59 UTC, NA
secs <- c(-1.5, 0, 1700000000.25, 1710054000, 1710053999, 1730611800, 1730615400, 253402300799, NA)
x <- data.frame(d_dbl = days, d_int = structure(as.integer(unclass(days)), class = "Date"))
case("rmatch_dates", x, "Date as double and as integer: before 1970, leap day, years 1 and 9999, NA")

x <- data.frame(utc = ct(secs, "UTC"), kolkata = ct(secs, "Asia/Kolkata"), new_york = ct(secs, "America/New_York"),
                kathmandu = ct(secs, "Asia/Kathmandu"), st_johns = ct(secs, "America/St_Johns"),
                tz_empty = ct(secs, ""), tz_none = ct(secs, "UTC"))
attr(x$tz_none, "tzone") <- NULL
case("rmatch_times", x,
     "POSIXct in UTC, +05:30, a DST zone (New York: spring forward, 01:30 twice), +05:45, -03:30 with DST, tzone \"\" and none; before 1970, fractional seconds, year 9999, NA")

x <- data.frame(us = ct(c(0.000001, -0.000001, 1.999999, 1700000000.123456, 1e9 + 0.5, -86400.25, NA), "Asia/Kolkata"))
case("rmatch_microseconds", x, "POSIXct with whole microseconds, either side of the epoch")

x <- data.frame(
  big = as.integer64(c("0", "1", "-1", "2147483647", "2147483648", "-2147483648", "9007199254740993",
                       "9223372036854775807", "-9223372036854775807", NA)),
  small = as.integer64(c(1:9, NA)),
  none = as.integer64(rep(NA, 10L)))
case("rmatch_int64", x, "bit64 integer64: past int32 and 2^53, the extremes, NA; a column of small values; all NA")

# ---------------------------------------------------------------- part B: factors, NA, strings
fac <- function(codes, levels, ordered = FALSE)
  structure(as.integer(codes), levels = levels, class = if (ordered) c("ordered", "factor") else "factor")
x <- data.frame(
  plain = factor(c("b", "a", NA, "c", "a", "b", NA, "c")),
  unused = factor(c("x", "x", "z", NA, "x", "z", "z", "x"), levels = c("w", "x", "y", "z", "v")),
  ordered = factor(c("lo", "hi", "mid", NA, "hi", "lo", "mid", "mid"), levels = c("lo", "mid", "hi"), ordered = TRUE),
  # NA as a level (exclude = NULL) and a real NA code in the same column
  na_level = fac(c(1, 3, NA, 2, 3, 1, NA, 3), c("a", "b", NA)),
  # the level "NA" (a string), the empty level, a level with a space, Indian scripts, unsorted levels
  odd_levels = factor(c("NA", "", " ", "हिन्दी", NA, "தமிழ்", "NA", "z"),
                      levels = c("z", "NA", "", " ", "हिन्दी", "தமிழ்")),
  all_na = factor(rep(NA_character_, 8L), levels = c("p", "q")),
  no_levels = factor(rep(NA_character_, 8L), levels = character(0)),
  ordered_unused = factor(rep(c("b", "c"), 4L), levels = c("c", "b", "a"), ordered = TRUE))
case("rmatch_factors", x,
     "factors: NA codes, unused levels, ordered, an NA level next to NA codes, the levels \"NA\" and \"\", Indian-script levels, all NA, no levels")

x <- data.frame(
  lgl = c(NA, TRUE, FALSE, NA, TRUE, NA),
  int = c(NA, 1L, -2147483647L, 2147483647L, NA, 0L),
  dbl = c(NA, NaN, -0, Inf, -Inf, 0.1),
  dbl_nan_only = c(NaN, 1, NaN, 2, NaN, 3),
  chr = c(NA, "", "NA", " ", "a", NA),
  date = as.Date(c(NA, "2024-01-01", NA, "1970-01-01", NA, NA)),
  time = .POSIXct(c(NA, 0, NA, 1, NA, 2), tz = "Asia/Kolkata"),
  i64 = as.integer64(c(NA, "5000000000", NA, "-1", NA, "0")),
  lgl_all = NA, int_all = NA_integer_, dbl_all = NA_real_, chr_all = NA_character_,
  date_all = as.Date(rep(NA, 6L)), time_all = .POSIXct(rep(NA_real_, 6L), tz = "UTC"),
  stringsAsFactors = FALSE)
case("rmatch_na", x, "NA in every type, first and last rows; NaN apart from NA; \"\" and \"NA\" apart from NA; columns that are all NA")

long <- strrep("abcdefghij", 10000L)                         # 100,000 bytes
long_hi <- strrep("हिन्दी ", 20000L)                           # 380,000 bytes of 3-byte characters (and spaces)
x <- data.frame(
  s = c("plain", "", NA, "NA", "हिन्दी", "தமிழ்", "😀 4-byte", "é combining", "日本語", "العربية",
        "tab\tline\nbreak", " lead and trail ", "quote\" back\\", "₹ 1,23,456.78", long, long_hi),
  stringsAsFactors = FALSE)
case("rmatch_strings", x,
     "strings: ASCII, empty and NA and \"NA\", Indian scripts, a 4-byte emoji, combining marks, CJK, right-to-left, control characters, 100 KB and 380 KB strings")
