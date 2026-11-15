# 1.0 WS3 write speed: the interleaved FSST encoder, thread-count independence of the written bytes, and the FSST
# lone-escape check in the reader. Synthetic data only.

ws3_strings <- function() {
  set.seed(31)
  w <- c(vapply(1:800, function(i) paste(sample(letters, sample(2:9, 1), TRUE), collapse = ""), ""),
         "café", "中文", "naïve")
  sent <- function(n, lo, hi) vapply(seq_len(n), function(i) paste(sample(w, sample(lo:hi, 1), TRUE), collapse = " "), "")
  list(short = { x <- sample(w, 40000, TRUE); x[sample(40000, 4000)] <- NA; x[sample(40000, 4000)] <- ""; x },
       long = sent(600, 80, 900),                                  # 500 to 8,000 bytes: 511-byte chunks
       one_big = c(sent(1, 6000, 6000), sent(500, 1, 3)),          # one string holds most of the bytes
       tiny = sent(40, 1, 5))                                      # under 16 KB: one lane
}

test_that("FSST pages round-trip for short, long, lopsided and tiny string sets", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  for (nm in names(ss <- ws3_strings())) {
    x <- data.frame(s = ss[[nm]], stringsAsFactors = FALSE)
    for (pr in c("fast", "balanced")) {
      write_fstx(x, p, profile = pr, threads = 1, .encoding = "fsst")
      expect_true(all(fstx_pages(p)$encoding == "fsst"), label = paste(nm, pr))
      expect_identical(read_fstx(p)$s, x$s, label = paste(nm, pr))
    }
  }
})

test_that("written bytes are the same at 1 and 14 threads", {
  p1 <- tempfile(fileext = ".fstx"); p14 <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p1, p14)))
  ss <- ws3_strings()
  set.seed(32)
  n <- 120000
  x <- data.frame(i = sample.int(1000L, n, TRUE), d = round(rnorm(n), 2), r = runif(n),
                  s = sample(c(ss$short, ss$long), n, TRUE), k = sample(c("a", "bb", "ccc", NA), n, TRUE),
                  stringsAsFactors = FALSE)
  for (pr in c("fast", "balanced", "small")) {
    write_fstx(x, p1, profile = pr, threads = 1)
    write_fstx(x, p14, profile = pr, threads = 14)
    expect_identical(unname(tools::md5sum(p1)), unname(tools::md5sum(p14)), label = pr)
  }
  expect_identical(read_fstx(p14), x)
})

test_that("an FSST string ending in a lone escape byte is refused as corrupt", {
  # The vendored decoder reads symbol[255] (past its 255-entry table) for a lone escape as the last code.
  # Patch one code byte of a raw (codec none) FSST page and read without page checksums.
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- data.frame(s = rep("a", 5000), stringsAsFactors = FALSE)      # every string is one code byte
  write_fstx(x, p, profile = "fast", threads = 1, .encoding = "fsst")
  pg <- fstx_pages(p)
  expect_identical(pg$encoding[1], "fsst")
  orig <- readBin(p, "raw", file.size(p))
  u32 <- function(b, z) sum(as.integer(b[z + 1 + 0:3]) * 256^(0:3))   # z: 0-based file offset
  o <- pg$offset[1]
  # page header (8, no NA block), FSST block header (8) and total (8), symbol table block, code bytes block
  expect_identical(orig[o + 9], as.raw(10))                           # T_FSST
  bb <- o + 32 + 8 * ceiling(u32(orig, o + 28) / 8)
  expect_identical(orig[bb + 1], as.raw(1))                           # T_PLAIN
  n1 <- u32(orig, o + 4)                                              # rows in the first page
  expect_identical(u32(orig, bb + 4), n1)                             # one code byte per string
  for (k in unique(c(1, n1 %/% 2, n1))) {
    b <- orig
    b[bb + 8 + k] <- as.raw(255)                                      # string k: a lone escape
    writeBin(b, p)
    expect_error(read_fstx(p, verify = FALSE), "FSST escape at the end", label = k)
  }
  b <- orig
  b[bb + 8 + 1:2] <- as.raw(255)                                      # escape + literal 255 over strings 1-2:
  writeBin(b, p)                                                      # string 1 ends in a lone escape
  expect_error(read_fstx(p, verify = FALSE), "FSST escape at the end")
  for (k in unique(c(1, n1))) {
    b <- orig
    b[bb + 8 + k] <- as.raw(200)                                      # a code the small table does not have
    writeBin(b, p)
    expect_error(read_fstx(p, verify = FALSE), "corrupt page .FSST code not in the symbol table", label = k)
  }
  writeBin(orig, p)
  expect_identical(read_fstx(p, verify = FALSE)$s, x$s)
})

test_that("forging a file with row groups but no columns does not crash", {
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p, q)))
  write_fstx(data.frame(row.names = 1:10), p)
  r <- tryCatch({ fstx:::.fstx_forge(p, q); "ok" }, error = function(e) conditionMessage(e))
  expect_true(r == "ok" || grepl("fstx", r), label = r)
  expect_error(fstx:::.fstx_forge(p, q, codec0 = 0), "no columns")
})
