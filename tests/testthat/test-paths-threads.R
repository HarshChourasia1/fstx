test_that("paths with non-English characters work", {
  d <- file.path(tempdir(), "डेटा café üß")
  dir.create(d, showWarnings = FALSE)
  on.exit(unlink(d, recursive = TRUE), add = TRUE)
  p <- file.path(d, "täble नम.fstx")
  x <- make_df(3000)
  write_fstx(x, p)
  expect_true(file.exists(p))
  expect_identical(read_fstx(p), x)
  expect_equal(fstx_meta(p)$nrow, 3000)
})

test_that("1 thread and all threads write byte-identical files", {
  n <- 1.2e6   # two row groups, every type
  x <- make_df(n, seed = 21)
  p1 <- tempfile(fileext = ".fstx")
  pn <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p1, pn)), add = TRUE)
  for (codec in c("zstd", "lz4", "none")) {
    write_fstx(x, p1, compress = codec, threads = 1)
    write_fstx(x, pn, compress = codec, threads = test_threads(parallel::detectCores()))
    expect_identical(unname(tools::md5sum(p1)), unname(tools::md5sum(pn)), info = codec)
  }
  expect_identical(read_fstx(pn, threads = 1), x)
  expect_identical(read_fstx(pn, threads = test_threads(parallel::detectCores())), x)
  expect_identical(read_fstx(pn, threads = test_threads(3), columns = c("chr", "ctu"), from = 1000000, to = 1100000),
                   { e <- x[1000000:1100000, c("chr", "ctu")]; rownames(e) <- NULL; e })
})

test_that("writing over a file that was just read works (the map is released)", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p), add = TRUE)
  write_fstx(make_df(100), p)
  y <- read_fstx(p)
  write_fstx(make_df(200), p)
  expect_identical(nrow(read_fstx(p)), 200L)
  expect_true(file.remove(p))
})
