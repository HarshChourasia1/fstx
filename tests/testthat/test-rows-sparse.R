# Rows plans decode only the strings asked for (enc::decode_str_page_rows). Synthetic data only.

test_that("rows= and where on FSST and plain string pages: NA, empty and long strings, sparse and dense rows", {
  set.seed(17)
  n <- 50000
  words <- c("", "a", "lorem", "ipsum", strrep("long", 40), "über", "z")
  s <- vapply(seq_len(n), function(i) paste(sample(words, sample.int(4, 1), TRUE), collapse = " "), "")
  s[sample.int(n, 4000)] <- NA
  s[sample.int(n, 2000)] <- ""
  x <- data.frame(k = sample.int(1000L, n, TRUE), s = s, stringsAsFactors = FALSE)
  for (enc in c("fsst", "plain")) for (cmp in c("none", "zstd")) {
    p <- tempfile(fileext = ".fstx")
    write_fstx(x, p, .encoding = enc, compress = cmp)
    for (r in list(sort(sample.int(n, 30)), sample.int(n, 3000, TRUE), seq(1, n, by = 997), c(n, 1, n))) {
      y <- read_fstx(p, rows = r)
      expect_identical(y$s, x$s[r])
      expect_identical(y$k, x$k[r])
    }
    y <- read_fstx(p, where = k == 7L)
    expect_identical(y$s, x$s[x$k == 7L])
    old <- options(fstx.verify_nulls = TRUE)
    expect_identical(read_fstx(p, rows = c(5, 4, 40000))$s, x$s[c(5, 4, 40000)])
    options(old)
    unlink(p)
  }
})
