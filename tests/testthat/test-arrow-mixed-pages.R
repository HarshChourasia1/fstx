# The Arrow stream on a string column whose row groups are dictionary-coded except a short plain last one (1.0.2
# raised "dictionary and plain pages mixed in column 's'"). Skipped when nanoarrow is not installed.

test_that("a dictionary row group then a short plain one: range batches and row lists", {
  skip_if_not_installed("nanoarrow")
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  n <- 1048581
  x <- data.frame(s = sprintf("s%d", (seq_len(n) - 1L) %% 5000L), i = seq_len(n), stringsAsFactors = FALSE)
  write_fstx(x, p, threads = test_threads(4))
  pg <- fstx_pages(p)
  expect_identical(pg$kind[pg$name == "s" & pg$row_group == max(pg$row_group)], "string")  # the case under test
  for (th in c(1L, test_threads(4))) {
    got <- nanoarrow::convert_array_stream(fstx_arrow_stream(p, threads = th))
    expect_identical(as.character(got$s), x$s)
  }
  w <- nanoarrow::convert_array_stream(fstx_arrow_stream(p, from = n - 8, columns = "s"))
  expect_identical(as.character(w$s), x$s[(n - 8):n])
  f <- nanoarrow::convert_array_stream(fstx_arrow_stream(p, where = i >= n - 6))
  expect_identical(as.character(f$s), x$s[(n - 6):n])
})
