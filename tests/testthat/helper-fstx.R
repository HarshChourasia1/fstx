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
