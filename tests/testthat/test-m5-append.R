# M5: appends and compaction. Synthetic data only.

bind <- function(a, b) {
  e <- rbind(a, b)
  rownames(e) <- NULL
  e
}

test_that("append adds rows, merges factor levels and keeps the file's class", {
  x <- make_df(7000, seed = 31)
  a <- x[1:3000, ]
  b <- x[3001:7000, ]
  rownames(b) <- NULL
  p <- tempfile(fileext = ".fstx")
  write_fstx(a, p)
  write_fstx(b, p, append = TRUE)
  y <- read_fstx(p)
  expect_identical(y, bind(a, b))
  b2 <- b
  b2$fct <- factor(as.character(b$fct), levels = c("new", "a", "cé", "b"))
  b2$fct[1:5] <- "new"
  write_fstx(b2, p, append = TRUE)
  y <- read_fstx(p)
  expect_identical(levels(y$fct), c("b", "a", "cé", "unused", "new"))
  expect_identical(as.character(y$fct), c(as.character(a$fct), as.character(b$fct), as.character(b2$fct)))
  expect_identical(nrow(y), 11000L)
  expect_identical(y$chr, c(a$chr, b$chr, b2$chr))
  # rows, where and lazy reads see the appended row groups
  expect_identical(read_fstx(p, rows = c(10999, 2, 5000))$dbl, y$dbl[c(10999, 2, 5000)])
  z <- read_fstx(p, where = int > 900)
  rownames(z) <- NULL
  e <- y[which(y$int > 900), ]
  rownames(e) <- NULL
  expect_identical(z, e)
  expect_identical(read_fstx(p, lazy = TRUE)$chr, y$chr)
  # a missing file is simply written; zero rows are fine
  q <- tempfile(fileext = ".fstx")
  write_fstx(a, q, append = TRUE)
  expect_identical(read_fstx(q), {r <- a; rownames(r) <- NULL; r})
  write_fstx(a[0, ], q, append = TRUE)
  expect_identical(nrow(read_fstx(q)), 3000L)
  if (requireNamespace("data.table", quietly = TRUE)) {
    write_fstx(data.table::as.data.table(a), q)
    write_fstx(b, q, append = TRUE)
    y <- read_fstx(q)
    expect_s3_class(y, "data.table")
    expect_identical(nrow(y), 7000L)
  }
  unlink(c(p, q))
})

test_that("append checks the schema", {
  x <- make_df(100)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  expect_error(write_fstx(x[, 1:3], p, append = TRUE), "columns")
  expect_error(write_fstx(x[, c(2, 1, 3:10)], p, append = TRUE), "columns")
  y <- x; y$int <- as.double(y$int)
  expect_error(write_fstx(y, p, append = TRUE), "'int'")
  y <- x; y$ord <- factor(as.character(y$ord), levels = c("hi", "mid", "lo"), ordered = TRUE)
  expect_error(write_fstx(y, p, append = TRUE), "ordered factor")
  y <- x; attr(y$ct, "tzone") <- "UTC"
  expect_error(write_fstx(y, p, append = TRUE), "time zone")
  y <- x; y$date <- as.character(y$date)
  expect_error(write_fstx(y, p, append = TRUE), "'date'")
  expect_identical(read_fstx(p), x)   # failed appends leave the file as it was
  rn <- data.frame(a = 1:3, row.names = c("u", "v", "w"))
  write_fstx(rn, p)
  expect_error(write_fstx(data.frame(a = 4L), p, append = TRUE), "row names")
  unlink(p)
})

test_that("an interrupted append leaves the old table readable, and the next append recovers", {
  x <- make_df(4000, seed = 9)
  a <- x[1:2500, ]
  b <- x[2501:4000, ]
  rownames(b) <- NULL
  p <- tempfile(fileext = ".fstx")
  write_fstx(a, p)
  size0 <- file.size(p)
  write_fstx(b, p, append = TRUE, .crash = 1L)   # stops after the new tail, header not reset
  s <- fstx:::fstx_sizes_impl(p)
  expect_identical(s[["committed"]], as.double(size0))
  expect_true(s[["physical"]] > size0)
  ra <- a; rownames(ra) <- NULL
  expect_identical(read_fstx(p), ra)
  expect_identical(fstx_meta(p)$nrow, 2500)
  # a crash in the middle: committed end set, half the pages written
  hb <- readBin(p, "raw", file.size(p))
  writeBin(hb[seq_len(size0 + (s[["physical"]] - size0) %/% 2)], p)
  expect_identical(read_fstx(p), ra)
  # the next append truncates the leftovers and works
  write_fstx(b, p, append = TRUE)
  s <- fstx:::fstx_sizes_impl(p)
  expect_identical(s[["valid_end"]], 0)
  expect_identical(s[["committed"]], s[["physical"]])
  expect_identical(read_fstx(p), bind(a, b))
  unlink(p)
})

test_that("compact keeps the data, drops dead space, and is byte-identical for files never appended", {
  x <- make_df(20000, seed = 41)
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".fstx")
  for (prof in c("balanced", "fast", "small")) {
    write_fstx(x, p, profile = prof)
    b0 <- readBin(p, "raw", file.size(p))
    fstx_compact(p)
    expect_identical(readBin(p, "raw", file.size(p)), b0)
  }
  write_fstx(x[1:5000, ], p)
  for (i in 1:3) write_fstx({y <- x[(5000 * i + 1):(5000 * (i + 1)), ]; rownames(y) <- NULL; y}, p, append = TRUE)
  before <- file.size(p)
  sz <- fstx_compact(p)
  expect_true(sz[["after"]] < before)
  expect_identical(read_fstx(p), x)
  expect_identical(nrow(fstx_pages(p)) > 0, TRUE)
  rg <- fstx_meta(p)$row_groups
  fstx_compact(p, merge = TRUE)
  expect_identical(read_fstx(p), x)
  expect_true(fstx_meta(p)$row_groups <= rg)
  # merge gives the same bytes as writing the table once
  write_fstx(x, q, profile = "balanced")
  expect_identical(readBin(p, "raw", file.size(p)), readBin(q, "raw", file.size(q)))
  unlink(c(p, q))
})

test_that("append and compact refuse a file held by a lazy result", {
  x <- make_df(3000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  held <- read_fstx(p, lazy = TRUE)
  expect_error(write_fstx(x, p, append = TRUE), "open in this R session")
  expect_error(fstx_compact(p), "open in this R session")
  expect_identical(held$int, x$int)
  rm(held)
  gc()
  write_fstx(x, p, append = TRUE)
  expect_identical(nrow(read_fstx(p)), 6000L)
  unlink(p)
})
