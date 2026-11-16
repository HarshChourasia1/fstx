# M3: eager, deferred-string and lazy reads give the same data frames. Synthetic data only.

col_state <- function(x) fstx:::fstx_col_state_impl(x)

m3_tables <- function() {
  set.seed(7)
  n2 <- 50000
  strs <- data.frame(
    low   = sample(c("north", "south", "east", "west", "", NA), n2, TRUE),
    high  = sprintf("item-%07d-%s", sample.int(1e7, n2), sample(utf8_words, n2, TRUE)),
    sparse = ifelse(runif(n2) < 0.95, NA_character_, sample(utf8_words, n2, TRUE)),
    long  = ifelse(seq_len(n2) %% 5000 == 0, strrep("x", 70000), "short"),
    stringsAsFactors = FALSE)
  n3 <- 20000
  odd <- data.frame(na_i = rep(NA_integer_, n3), na_d = rep(NA_real_, n3), na_s = rep(NA_character_, n3),
                    na_l = rep(NA, n3), k_i = rep(7L, n3), k_s = rep("same", n3), stringsAsFactors = FALSE)
  n4 <- 1200000  # two row groups (1,048,576 rows each by default)
  big <- data.frame(id = seq_len(n4),
                    grp = ifelse(seq_len(n4) <= 1048576, sample(c("a", "b", "c"), n4, TRUE),
                                 sample(c("c", "d", "e", NA), n4, TRUE)),
                    v = round(runif(n4), 3), stringsAsFactors = FALSE)
  t5 <- make_df(3000, seed = 5)
  if (requireNamespace("data.table", quietly = TRUE)) t5 <- data.table::as.data.table(t5)
  list(mixed = list(make_df(5000), "balanced"), strings = list(strs, "fast"), odd = list(odd, "small"),
       rowgroups = list(big, "balanced"), dt = list(t5, "small"))
}

test_that("eager, deferred and lazy reads of five tables are identical to the source", {
  for (nm in names(tabs <- m3_tables())) {
    x <- tabs[[nm]][[1]]
    p <- tempfile(fileext = ".fstx")
    write_fstx(x, p, profile = tabs[[nm]][[2]], threads = test_threads(4))
    for (th in unique(c(1, test_threads(4)))) {
      expect_identical(read_fstx(p, threads = th), x, info = paste(nm, "eager", th))
      d <- read_fstx(p, strings = "deferred", threads = th)
      expect_identical(d, x, info = paste(nm, "deferred", th))
      z <- read_fstx(p, lazy = TRUE, threads = th)
      expect_identical(z, x, info = paste(nm, "lazy", th))
      rm(z)
    }
    # partial reads cross page and row-group borders
    nr <- nrow(x)
    fr <- max(1, nr %/% 3 - 777)
    to <- min(nr, fr + nr %/% 2)
    cols <- rev(names(x))[1:min(3, ncol(x))]
    ref <- read_fstx(p, columns = cols, from = fr, to = to)
    expect_identical(read_fstx(p, columns = cols, from = fr, to = to, strings = "deferred"), ref, info = nm)
    expect_identical(read_fstx(p, columns = cols, from = fr, to = to, lazy = TRUE), ref, info = nm)
    expect_identical(as.data.frame(ref), as.data.frame(x)[fr:to, cols, drop = FALSE], ignore_attr = "row.names",
                     info = nm)
    invisible(gc())
    unlink(p)
  }
})

test_that("deferred strings leave the file closed and are ALTREP until used", {
  x <- make_df(20000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  d <- read_fstx(p, strings = "deferred")
  s <- col_state(d$chr)
  expect_false(s$lazy)
  expect_false(s$file_open)
  expect_false(s$materialised)
  expect_null(col_state(d$int))            # only strings are deferred
  expect_silent(write_fstx(x, p))          # nothing keeps the file open
  expect_identical(d$chr[c(1, 20000)], x$chr[c(1, 20000)])
  y <- d$chr
  y[2] <- "changed"                        # copy on modify: d is untouched
  expect_identical(y[2], "changed")
  expect_identical(d$chr, x$chr)
  expect_identical(nchar(d$chr), nchar(x$chr))
})

test_that("edge cases: zero rows, zero columns, one row, repeated columns, all NA", {
  x <- make_df(10)
  p <- tempfile(fileext = ".fstx")
  for (m in list(list(), list(strings = "deferred"), list(lazy = TRUE))) {
    write_fstx(x[0, ], p)
    expect_identical(do.call(read_fstx, c(list(p), m)), x[0, ])
    write_fstx(x[, 0], p)
    expect_identical(do.call(read_fstx, c(list(p), m)), x[, 0])
    write_fstx(x[1, ], p)
    expect_identical(do.call(read_fstx, c(list(p), m)), x[1, ])
    write_fstx(x, p)
    y <- do.call(read_fstx, c(list(p, columns = c("chr", "chr", "int")), m))
    expect_identical(unname(as.list(y)), unname(as.list(x[, c("chr", "chr", "int")])))
    y <- do.call(read_fstx, c(list(p, from = 4, to = 4), m))
    expect_identical(y$chr, x$chr[4])
    rm(y)
    na <- data.frame(s = rep(NA_character_, 3000), i = rep(NA_integer_, 3000))
    write_fstx(na, p)
    expect_identical(do.call(read_fstx, c(list(p), m)), na)
  }
})

test_that("strings argument is checked", {
  p <- tempfile(fileext = ".fstx")
  write_fstx(make_df(5), p)
  expect_error(read_fstx(p, strings = "nope"), "should be one of")
  expect_error(read_fstx(p, lazy = NA), "'lazy'")
})
