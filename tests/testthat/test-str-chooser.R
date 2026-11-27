# Synthetic test data only.
# The plain / FSST chooser under zstd (1.2 fix): it compresses its samples as pages of the size they stand for. Before,
# a column of short sentences (as the benchmark's mixed.comment) kept plain + zstd pages although FSST + zstd ones
# are smaller: its FSST sample was under 128 KB, where zstd takes smaller tables than for the real pages. With the
# data below (seeds 2 and 3 at 100,000 rows) fstx 1.0.2 / beat took plain, 4.6% and 6.6% bigger than FSST.

comment_like <- function(n, seed) {
  set.seed(seed)
  vocab <- unique(vapply(1:6000, function(i) paste(sample(letters, sample(3:9, 1), TRUE), collapse = ""), ""))[1:5000]
  w <- matrix(sample(vocab, n * 8, TRUE, 1 / seq_along(vocab)), n)
  len <- sample(2:8, n, TRUE)
  s <- character(n)
  for (L in 2:8) {
    i <- which(len == L)
    if (length(i)) s[i] <- do.call(paste, lapply(seq_len(L), function(j) w[i, j]))
  }
  s[sample.int(n, n * 0.3)] <- NA
  s
}

str_bytes <- function(p, col) {
  pg <- fstx_pages(p)
  sum(pg$comp_bytes[pg$name == col])
}

test_that("the string chooser takes the smaller of plain + zstd and FSST + zstd (short sentences)", {
  for (seed in 2:3) {
    x <- data.frame(comment = comment_like(100000, seed), stringsAsFactors = FALSE)
    p <- c(auto = tempfile(fileext = ".fstx"), plain = tempfile(fileext = ".fstx"), fsst = tempfile(fileext = ".fstx"))
    write_fstx(x, p[["auto"]], threads = test_threads(4))
    write_fstx(x, p[["plain"]], .encoding = "plain", threads = test_threads(4))
    write_fstx(x, p[["fsst"]], .encoding = "fsst", threads = test_threads(4))
    b <- vapply(p, str_bytes, 0, col = "comment")
    expect_lt(b[["fsst"]], b[["plain"]], label = paste("seed", seed))  # the data: FSST + zstd is the smaller
    expect_lte(b[["auto"]], min(b[["plain"]], b[["fsst"]]), label = paste("seed", seed))
    pg <- fstx_pages(p[["auto"]])
    expect_true(all(pg$encoding[pg$name == "comment"] == "fsst"), label = paste("seed", seed))
    expect_identical(read_fstx(p[["auto"]])$comment, x$comment)
    unlink(p)
  }
})

test_that("the string chooser keeps random text plain and the fast profile's rule", {
  set.seed(5)
  r <- vapply(1:30000, function(i) rawToChar(as.raw(sample(33:126, 24, TRUE))), "")
  x <- data.frame(r = r, s = comment_like(30000, 2), stringsAsFactors = FALSE)
  p <- tempfile(fileext = ".fstx")
  q <- c(plain = tempfile(fileext = ".fstx"), fsst = tempfile(fileext = ".fstx"))
  on.exit(unlink(c(p, q)))
  write_fstx(x, p, threads = test_threads(2))
  for (e in names(q)) write_fstx(x["r"], q[[e]], .encoding = e, threads = test_threads(2))
  pg <- fstx_pages(p)
  # random printable text: FSST + zstd is 2.6% bigger than plain + zstd (beat took FSST)
  expect_lt(str_bytes(q[["plain"]], "r"), str_bytes(q[["fsst"]], "r"))
  expect_true(all(pg$encoding[pg$name == "r"] == "plain"))
  write_fstx(x, p, profile = "fast", threads = test_threads(2))
  pg <- fstx_pages(p)
  expect_true(all(pg$encoding[pg$name == "s"] == "fsst"))   # no codec: FSST alone halves the text
  expect_identical(read_fstx(p), x)
})

test_that("the string chooser's pick does not depend on the thread count or the SIMD target", {
  x <- data.frame(comment = comment_like(120000, seed = 7), n = seq_len(120000), stringsAsFactors = FALSE)
  s0 <- fstx_simd()
  s0 <- if (s0$simd) tolower(s0$target) else "scalar"  # the run's setting (FSTX_SIMD may force one): put back
  on.exit(fstx_simd(s0), add = TRUE)
  h <- character()
  for (simd in unique(c("scalar", s0, "auto"))) {
    fstx_simd(simd)
    for (th in unique(c(1L, test_threads(2L), test_threads(8L)))) {
      q <- tempfile(fileext = ".fstx")
      write_fstx(x, q, threads = th)
      h[paste(simd, th)] <- unname(tools::md5sum(q))
      unlink(q)
    }
  }
  expect_length(unique(h), 1L)
})
