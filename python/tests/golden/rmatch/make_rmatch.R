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
