# fstx format 1.0 golden files (WS8): files written by fstx 1.0 that every later fstx must read the
# same way (fixtures/golden-1.0, made by make_golden.R there), and the spec's worked bit-packing example.
# Synthetic data only.

gdir <- test_path("fixtures", "golden-1.0")
source(file.path(gdir, "canon.R"), local = TRUE)
have_golden <- file.exists(file.path(gdir, "files.csv"))

page_bytes <- function(p, pg) {
  con <- file(p, "rb")
  on.exit(close(con))
  seek(con, pg$offset)
  readBin(con, "raw", pg$raw_bytes)
}
u32 <- function(...) writeBin(as.integer(c(...)), raw(), size = 4, endian = "little")

test_that("the spec's worked bit-packing example (section 6.4) is what fstx writes", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(data.frame(x = c(100L, 103L, 101L, 107L, 100L)), p, compress = "none", .encoding = "for", threads = 1L)
  pg <- fstx_pages(p)
  expect_identical(nrow(pg), 1L)
  expect_identical(c(pg$kind, pg$encoding, pg$na, pg$codec), c("int", "for", "none", "none"))
  expect_equal(pg$raw_bytes, 416)
  want <- c(u32(1, 5),                 # page header: kind 1, mode 0, 5 rows
            u32(3, 400),               # block header: tag 3 (FOR), payload 400 bytes
            u32(3, 0),                 # width 3, pad
            u32(100, 0),               # base 100, pad
            u32(0, 3, 1, 7, 0, rep(0, 91)))  # 96 packed words
  expect_identical(page_bytes(p, pg), want)

  # values 37 (lane 5, row 1) and 330 (lane 10, row 10, split across words 10 and 42), width 3
  x <- integer(400)
  x[38] <- 7L
  x[331] <- 5L
  write_fstx(data.frame(x = x), p, compress = "none", .encoding = "for", threads = 1L)
  pg <- fstx_pages(p)
  b <- page_bytes(p, pg)
  words <- readBin(b[33:416], "integer", 96, size = 4, endian = "little")
  w <- integer(96)
  w[5 + 1] <- 7L * 8L           # bits 3-5 of word 5
  w[10 + 1] <- 1073741824L      # low 2 bits of 5 (binary 01) at bits 30-31 of word 10
  w[42 + 1] <- 1L               # its top bit at bit 0 of word 42
  expect_identical(words, w)
  expect_identical(read_fstx(p)$x, x)
})

test_that("a fresh file starts with the header and checksum of spec section 9.2", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(data.frame(x = 1:3), p, threads = 1L)
  hdr <- as.raw(c(0x46, 0x53, 0x54, 0x58, 3, 0, 0, 0, 4, 3, 2, 1, rep(0, 12)))
  expect_identical(readBin(p, "raw", 32), c(hdr, as.raw(c(0x1E, 0xFC, 0xB4, 0x0C, 0x57, 0x43, 0x5A, 0x43))))
})

test_that("golden files are unchanged and read as recorded", {
  skip_if_not(have_golden, "golden files not generated yet (fixtures/golden-1.0/make_golden.R)")
  fl <- read.csv(file.path(gdir, "files.csv"), stringsAsFactors = FALSE, fileEncoding = "UTF-8")
  ex <- read.csv(file.path(gdir, "expected.csv"), stringsAsFactors = FALSE, fileEncoding = "UTF-8")
  cols <- c("index", "column", "class", "rows", "nas", "data_md5", "attr_md5")
  expect_lt(sum(fl$bytes), 300 * 1024)
  for (i in seq_len(nrow(fl))) {
    f <- file.path(gdir, fl$file[i])
    lab <- fl$file[i]
    expect_identical(unname(tools::md5sum(f)), fl$md5[i], label = lab)
    if (fl$expect[i] == "refuse") {
      expect_error(fstx_meta(f), fl$message[i], label = lab)
      expect_error(read_fstx(f), "Update fstx", label = lab)
      next
    }
    m <- fstx_meta(f)
    expect_equal(c(m$format_version, m$nrow, m$ncol), c(fl$format_version[i], fl$nrow[i], fl$ncol[i]), label = lab)
    expect_identical(m$profile, fl$profile[i], label = lab)
    e <- ex[ex$file == fl$file[i], cols]
    rownames(e) <- NULL
    for (lazy in c(FALSE, TRUE)) {
      x <- read_fstx(f, lazy = lazy)
      expect_identical(golden_table_md5(x), fl$table_md5[i], label = paste(lab, "table", lazy))
      d <- golden_describe(x)
      if (!nrow(e)) {
        expect_identical(nrow(d), 0L, label = lab)
        next
      }
      expect_identical(d$column, enc2utf8(e$column), label = paste(lab, "names", lazy))
      for (k in setdiff(cols, "column")) expect_equal(d[[k]], e[[k]], label = paste(lab, k, lazy))
      rm(x)
    }
  }
  invisible(gc())
})

test_that("the golden mix files hold all 294 legal mixes", {
  skip_if_not(have_golden, "golden files not generated yet (fixtures/golden-1.0/make_golden.R)")
  f <- list.files(gdir, "^mix_.*\\.fstx$", full.names = TRUE)
  seen <- unique(unlist(lapply(f, function(p) {
    pg <- fstx_pages(p)
    paste(pg$kind, pg$encoding, pg$na, pg$codec)
  })))
  nas <- c("none", "bitmap", "runs")
  codecs <- c("none", "lz4", "zstd")
  ie <- c("plain", "const", "for", "delta", "rle", "dict", "split")
  grid <- function(kind, encs, na = nas) as.vector(outer(outer(paste(kind, encs), na, paste), codecs, paste))
  want <- c(grid("int", ie), grid("logical", ie), grid("codes", ie), grid("double", c(ie, "alp", "alprd")),
            grid("string", c("plain", "fsst")), grid("dictionary", c("plain", "fsst"), "none"))
  expect_identical(length(want), 294L)
  expect_identical(setdiff(want, seen), character(0))
})
