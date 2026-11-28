# Synthetic test data only.

# The test hooks .encoding and .crash are not arguments of the exported write_fstx(); fstx:::.write_fstx() is
# write_fstx() with them. Every test calls write_fstx() through it, so write_fstx(x, p, .encoding = "for")
# works in any test file. test-api.R checks the exported function itself.
write_fstx <- fstx:::.write_fstx

# Thread counts for tests: n, but never more than the package default allows (at most 2 under R CMD check).
test_threads <- function(n) as.integer(max(1L, min(n, fstx:::fstx_hw_threads())))
roundtrip <- function(x, ...) {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p, ...)
  read_fstx(p)
}

utf8_words <- c("plain", "", "café", "naïve", "नमस्ते",
                "中文", "\U0001F600 smile", "über straße")

make_df <- function(n, seed = 1) {
  set.seed(seed)
  s <- sample(c(utf8_words, NA), n, TRUE)
  d <- rnorm(n)
  d[sample.int(n, min(n, 3))] <- c(NA, NaN, Inf)[seq_len(min(n, 3))]
  i <- sample.int(1000L, n, TRUE)
  i[sample.int(n, min(n, 2))] <- NA
  data.frame(
    int  = i,
    dbl  = d,
    lgl  = sample(c(TRUE, FALSE, NA), n, TRUE),
    chr  = s,
    fct  = factor(sample(c("b", "a", "cé", NA), n, TRUE), levels = c("b", "a", "cé", "unused")),
    ord  = factor(sample(c("lo", "mid", "hi"), n, TRUE), levels = c("lo", "mid", "hi"), ordered = TRUE),
    date = as.Date("2020-01-01") + sample(c(0:3000, NA), n, TRUE),
    idt  = structure(sample(18000:19000, n, TRUE), class = "Date"),
    ct   = as.POSIXct("2024-03-01 10:00:00", tz = "Asia/Kolkata") + sample(c(1:1e6, NA), n, TRUE),
    ctu  = as.POSIXct("2024-03-01", tz = "UTC") + seq_len(n) * 0.5,
    stringsAsFactors = FALSE
  )
}

flip_byte <- function(path, offset) {   # offset counted from 0
  b <- readBin(path, "raw", file.size(path))
  b[offset + 1] <- xor(b[offset + 1], as.raw(0xff))
  writeBin(b, path)
}

truncate_file <- function(path, size) {
  b <- readBin(path, "raw", file.size(path))
  writeBin(b[seq_len(size)], path)
}

footer_pos <- function(path) {   # c(offset, length) from the 32-byte tail
  sz <- file.size(path)
  con <- file(path, "rb")
  on.exit(close(con))
  seek(con, sz - 32)
  v <- readBin(con, "integer", 4, size = 4, endian = "little")
  c(v[1] + v[2] * 2^32, v[3] + v[4] * 2^32)
}

# Reading must give an R error or a data frame; anything else (a crash) ends the test run.
read_outcome <- function(p, ...) {
  tryCatch({
    y <- read_fstx(p, ...)
    if (is.data.frame(y)) "data" else "other"
  }, error = function(e) "error")
}

# ---------------------------------------------------------------- fstx 1.2: int64 and binary columns
# Tests that need the engine's drop 2 (exact int64 filters, Arrow stream types) are skipped until
# this is TRUE (switch it on here once drop 2 is merged, or for one run with the environment variable
# FSTX_TEST_DROP2=true); the rest of the 1.2 tests need drop 1 (types, pages, read / write).
fstx_drop2 <- TRUE || identical(Sys.getenv("FSTX_TEST_DROP2"), "true")
skip_until_drop2 <- function() skip_if_not(fstx_drop2, "needs the fstx 1.2 engine drop 2 (int64 filters, Arrow types)")

# integer64 vectors without bit64: from the high 32 bits (signed) and the low 32 bits (unsigned), as doubles
to_i32 <- function(v) as.integer(ifelse(v == -2^31, NA, v))   # NA_integer_ has the bits of -2^31
i64_parts <- function(hi, lo) {
  lo <- ifelse(lo >= 2^31, lo - 2^32, lo)
  b <- writeBin(to_i32(as.vector(rbind(lo, hi))), raw(), size = 4L, endian = "little")
  structure(readBin(b, "double", length(hi), size = 8L, endian = "little"), class = "integer64")
}
# whole numbers (exact doubles) as integer64; NA gives integer64's NA (INT64_MIN)
i64 <- function(d) {
  na <- is.na(d)
  d[na] <- 0
  hi <- floor(d / 2^32)
  lo <- d - hi * 2^32
  hi[na] <- -2^31
  lo[na] <- 0
  i64_parts(hi, lo)
}
i64_na <- i64_parts(-2^31, 0)
i64_min <- i64_parts(-2^31, 1)              # INT64_MIN + 1, the smallest value a file can hold
i64_max <- i64_parts(2^31 - 1, 2^32 - 1)    # INT64_MAX
i64_c <- function(...) structure(unlist(lapply(list(...), unclass)), class = "integer64")
# the values' bits as hex strings (integer64 holds them in doubles, so identical() cannot tell NaN patterns apart)
i64_hex <- function(v) {
  if (!length(v)) return(character(0))
  m <- matrix(as.character(writeBin(as.vector(unclass(v)), raw(), size = 8L, endian = "big")), nrow = 8L)
  apply(m, 2L, paste, collapse = "")
}
# integer64 columns (or a vector) with their values as hex strings, every attribute kept
unbit <- function(x) {
  f <- function(v) {
    if (!inherits(v, "integer64")) return(v)
    a <- attributes(v)
    h <- i64_hex(v)
    a$class <- c("i64hex", a$class)
    attributes(h) <- a
    h
  }
  if (is.data.frame(x)) {
    x[] <- lapply(x, f)
    x
  } else f(x)
}
expect_same <- function(object, expected, ...) expect_identical(unbit(object), unbit(expected), ...)

# a binary value of n bytes (all 256 byte values, NUL included)
raw_n <- function(n, seed = 1) {
  set.seed(seed)
  as.raw(sample.int(256L, n, TRUE) - 1L)
}

# Data frames with integer64 or binary columns, built and cut without data.frame() / `[` (which, without bit64,
# cannot take integer64 columns or drop their class)
new_df <- function(...) list2DF(list(...))
i64_rep <- function(x, ...) structure(rep(as.vector(unclass(x)), ...), class = "integer64")
col_take <- function(v, r) {
  a <- attributes(v)
  out <- unclass(v)[r]
  if (!is.null(a)) attributes(out) <- a
  out
}
df_take <- function(x, r) list2DF(lapply(x, col_take, r))   # rows r, automatic row names
df_rbind <- function(x, y) list2DF(Map(function(u, v) {
  a <- attributes(u)
  out <- c(unclass(u), unclass(v))
  if (!is.null(a)) attributes(out) <- a
  out
}, x, y))
