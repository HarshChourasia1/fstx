test_that("every codec round-trips", {
  x <- make_df(150000, seed = 9)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p), add = TRUE)
  sizes <- c()
  want <- x[40000:90000, c("chr", "dbl")]
  rownames(want) <- NULL
  for (cfg in list(list("none", 1), list("lz4", 1), list("zstd", 1), list("zstd", 19), list("zstd", -3))) {
    write_fstx(x, p, compress = cfg[[1]], level = cfg[[2]])
    expect_identical(read_fstx(p), x, info = paste(cfg, collapse = " "))
    expect_identical(read_fstx(p, columns = c("chr", "dbl"), from = 40000, to = 90000, verify = FALSE), want)
    m <- fstx_meta(p)
    sizes[paste(cfg, collapse = "")] <- m$file_size
    if (cfg[[1]] == "none") expect_identical(m$columns$compressed_bytes, m$columns$raw_bytes)
  }
  expect_lt(sizes[["zstd1"]], sizes[["none1"]])
  expect_lt(sizes[["lz41"]], sizes[["none1"]])
  expect_lte(sizes[["zstd19"]], sizes[["zstd1"]])
})

test_that("fstx_meta describes the file", {
  x <- make_df(70000)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p), add = TRUE)
  write_fstx(x, p)
  m <- fstx_meta(p)
  expect_s3_class(m, "fstx_meta")
  expect_equal(m$nrow, 70000)
  expect_equal(m$ncol, ncol(x))
  expect_equal(m$file_size, file.size(p))
  expect_identical(m$columns$name, names(x))
  expect_identical(m$columns$class, c("integer", "double", "logical", "character", "factor", "factor",
                                      "Date", "Date", "POSIXct", "POSIXct"))
  expect_true(all(m$columns$compressed_bytes > 0))
  expect_lt(sum(m$columns$compressed_bytes), m$file_size)
  expect_identical(m$columns$tzone[9:10], c("Asia/Kolkata", "UTC"))
  expect_output(print(m), "70000 rows")
})
