make_file <- function(compress = "zstd", n = 20000) {
  x <- make_df(n, seed = 5)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p, compress = compress)
  list(p = p, x = x)
}

test_that("a flipped byte in a page is caught by the page checksum", {
  for (codec in c("zstd", "lz4", "none")) {
    f <- make_file(codec)
    flip_byte(f$p, 64 + 10)   # first page of the first column starts at byte 64
    expect_error(read_fstx(f$p), "checksum", info = codec)
    expect_error(read_fstx(f$p, columns = "int", from = 1, to = 5), "checksum", info = codec)
    # reading columns that do not touch the broken page still works
    expect_identical(read_fstx(f$p, columns = "chr"), f$x["chr"], info = codec)
    # verify = FALSE may give wrong data or a decoder error, but never a crash
    expect_true(read_outcome(f$p, verify = FALSE) %in% c("data", "error"), info = codec)
    unlink(f$p)
  }
})

test_that("a flipped byte in a string page with verify = FALSE never crashes", {
  for (codec in c("none", "lz4", "zstd")) {
    f <- make_file(codec)
    m <- fstx_meta(f$p)
    for (k in c(0, 1, 2, 3, 50, 4000, 5000)) {
      g <- tempfile(fileext = ".fstx")
      file.copy(f$p, g)
      # string column 'chr' is the 4th; its first page comes after the first three columns' pages
      off <- 64 + sum(ceiling(m$columns$compressed_bytes[1:3] / 64) * 64)
      flip_byte(g, off + k)
      expect_error(read_fstx(g, columns = "chr"), "checksum")
      expect_true(read_outcome(g, columns = "chr", verify = FALSE) %in% c("data", "error"))
      unlink(g)
    }
    unlink(f$p)
  }
})

test_that("a flipped byte in the footer, header or tail gives an error", {
  f <- make_file()
  on.exit(unlink(f$p), add = TRUE)
  orig <- readBin(f$p, "raw", file.size(f$p))
  fp <- footer_pos(f$p)
  for (off in c(fp[1], fp[1] + fp[2] %/% 2, fp[1] + fp[2] - 1)) {
    writeBin(orig, f$p)
    flip_byte(f$p, off)
    expect_error(read_fstx(f$p), "footer", info = off)
    expect_error(read_fstx(f$p, verify = FALSE), "footer", info = off)
    expect_error(fstx_meta(f$p), "footer", info = off)
  }
  for (off in c(0, 4, 8, 12, 20, 24, 31)) {   # magic, version, endian mark, reserved, checksum
    writeBin(orig, f$p)
    flip_byte(f$p, off)
    expect_error(read_fstx(f$p), "FSTX|header|endian|version", info = off)
  }
  sz <- length(orig)
  for (off in sz - c(32, 25, 24, 17, 16, 9, 1)) {   # footer offset, length, checksum, version, magic
    writeBin(orig, f$p)
    flip_byte(f$p, off)
    expect_error(read_fstx(f$p), info = off)
  }
})

test_that("truncated, empty and foreign files give errors", {
  f <- make_file()
  on.exit(unlink(f$p), add = TRUE)
  orig <- readBin(f$p, "raw", file.size(f$p))
  sz <- length(orig)
  for (cut in unique(c(0, 1, 10, 31, 32, 63, 64, 65, 95, 96, 100, 1000, sz %/% 2, sz - 33, sz - 32, sz - 31, sz - 1))) {
    writeBin(orig[seq_len(cut)], f$p)
    expect_error(read_fstx(f$p), info = cut)
    expect_error(fstx_meta(f$p), info = cut)
  }
  writeBin(raw(0), f$p)
  expect_error(read_fstx(f$p), "empty")
  writeLines(rep("this is a text file, not FSTX", 20), f$p)
  expect_error(read_fstx(f$p), "not an FSTX file")
  saveRDS(f$x, f$p)
  expect_error(read_fstx(f$p), "not an FSTX file")
  writeBin(c(charToRaw("FSTX"), as.raw(sample(0:255, 4000, TRUE))), f$p)
  expect_error(read_fstx(f$p))
})

test_that("random single-byte damage gives an error or the right data, never a crash", {
  f <- make_file("zstd", n = 5000)
  on.exit(unlink(f$p), add = TRUE)
  orig <- readBin(f$p, "raw", file.size(f$p))
  set.seed(11)
  offs <- unique(c(sample.int(length(orig), 400) - 1, 0:40, length(orig) - 0:40))
  bad <- 0
  for (off in offs) {
    b <- orig
    b[off + 1] <- xor(b[off + 1], as.raw(sample(1:255, 1)))
    writeBin(b, f$p)
    r <- tryCatch(read_fstx(f$p), error = function(e) NULL)
    if (!is.null(r) && !identical(r, f$x)) bad <- bad + 1   # undetected damage (should be padding only)
    expect_true(read_outcome(f$p, verify = FALSE) %in% c("data", "error"))
  }
  expect_identical(bad, 0)
})

test_that("random truncation never crashes", {
  f <- make_file("lz4", n = 5000)
  on.exit(unlink(f$p), add = TRUE)
  orig <- readBin(f$p, "raw", file.size(f$p))
  set.seed(12)
  for (cut in sample.int(length(orig) - 1, 60)) {
    writeBin(orig[seq_len(cut)], f$p)
    expect_error(read_fstx(f$p, verify = FALSE))
  }
})
