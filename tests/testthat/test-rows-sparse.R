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

test_that("filtered and row-list streams with threads = 1 can be pulled from a thread that is not R's", {
  # The rows path once kept codec contexts and buffers in thread_local storage: in the MinGW build the thread that
  # pulled the batches crashed when it ended (threads = 1 decodes on the pulling thread itself).
  skip_if_not_installed("nanoarrow")
  set.seed(5)
  n <- 6000
  x <- data.frame(i = seq_len(n), d = runif(n), s = sprintf("text %d %s", seq_len(n), strrep("x", n %% 7)),
                  k = sample(c("red", "green", "blue"), n, TRUE), stringsAsFactors = FALSE)
  for (cmp in c("zstd", "none")) {
    p <- tempfile(fileext = ".fstx")
    write_fstx(x[1:2500, ], p, compress = cmp, threads = 1)
    write_fstx(x[2501:n, ], p, append = TRUE, compress = cmp, threads = 1)
    for (w in list(quote(i > 100), quote(k == "red"), quote(i > 5900), NULL)) {
      for (th in c(1, test_threads(2))) {
        s <- eval(bquote(fstx_arrow_stream(p, where = .(w), threads = .(th), batch_rows = 1000)))
        r <- fstx:::fstx_arrow_stream_impl(list(op = "pull_thread", stream = s))
        want <- if (is.null(w)) n else sum(eval(w, x))
        expect_true(r$other_thread)
        expect_identical(r$error, "")
        expect_identical(r$rows, as.double(want))
        expect_null(s$get_next())
      }
    }
    # the same rows as read_fstx after all that
    expect_identical(read_fstx(p, where = i > 100)$s, x$s[x$i > 100])
    unlink(p)
  }
})
