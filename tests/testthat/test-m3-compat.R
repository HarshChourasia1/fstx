# M3: ALTREP columns work with base R, data.table, dplyr, saveRDS/readRDS, identical() and across sessions.

col_state <- function(x) fstx:::fstx_col_state_impl(x)

test_that("base R functions give the same answers on deferred and lazy columns", {
  x <- make_df(30000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  for (m in list(list(strings = "deferred"), list(lazy = TRUE))) {
    z <- do.call(read_fstx, c(list(p), m))
    expect_identical(table(z$chr, useNA = "always"), table(x$chr, useNA = "always"))
    expect_identical(order(z$chr, z$int), order(x$chr, x$int))
    expect_identical(z[z$int > 500 & !is.na(z$int), ], x[x$int > 500 & !is.na(x$int), ])
    expect_identical(split(z$dbl, z$lgl), split(x$dbl, x$lgl))
    expect_identical(paste0(z$chr, "!"), paste0(x$chr, "!"))
    expect_identical(unique(z$chr), unique(x$chr))
    expect_identical(rev(z$date), rev(x$date))
    expect_identical(format(z$ct[1:20]), format(x$ct[1:20]))
    expect_identical(merge(z[1:200, c("int", "chr")], x[1:200, c("int", "fct")], by = "int"),
                     merge(x[1:200, c("int", "chr")], x[1:200, c("int", "fct")], by = "int"))
    expect_identical(anyNA(z$chr), anyNA(x$chr))
    expect_true(identical(z, x))
    rm(z)
    invisible(gc())
  }
})

test_that("data.table works on lazy and deferred tables", {
  skip_if_not_installed("data.table")
  x <- data.table::as.data.table(make_df(20000))
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  for (m in list(list(strings = "deferred"), list(lazy = TRUE))) {
    z <- do.call(read_fstx, c(list(p), m))
    expect_true(data.table::is.data.table(z))
    # data.table's syntax needs a data.table-aware caller: evaluate from an environment under globalenv()
    e <- list2env(list(z = z, x = x), parent = globalenv())
    expect_identical(evalq(z[int > 500, .N], e), evalq(x[int > 500, .N], e))
    expect_identical(evalq(z[, .(s = sum(dbl, na.rm = TRUE)), keyby = chr], e),
                     evalq(x[, .(s = sum(dbl, na.rm = TRUE)), keyby = chr], e))
    evalq(z[, new := int * 2L], e)
    expect_identical(e$z$new, x$int * 2L)
    data.table::setkey(e$z, chr)
    expect_identical(e$z$chr, sort(x$chr, na.last = FALSE, method = "radix"))
    rm(z, e)
    invisible(gc())
  }
})

test_that("dplyr works on lazy tables", {
  skip_if_not_installed("dplyr")
  x <- make_df(5000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  z <- read_fstx(p, lazy = TRUE)
  f <- function(d) dplyr::summarise(dplyr::group_by(dplyr::filter(d, !is.na(int)), chr), n = dplyr::n(), m = mean(dbl))
  expect_identical(f(z), f(x))
  rm(z)
  invisible(gc())
})

test_that("saveRDS / readRDS round-trip deferred and lazy columns, compact and portable", {
  x <- make_df(300000)
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".rds")
  write_fstx(x, p)
  d <- read_fstx(p, strings = "deferred")
  invisible(d$chr[1])                                  # one page already made into R strings
  expect_gt(col_state(d$chr)$pages, 1)
  expect_false(col_state(d$chr)$materialised)
  saveRDS(d, q)
  y <- readRDS(q)
  expect_identical(y, x)
  expect_false(is.null(col_state(y$chr)))              # compact state: still a deferred column
  z <- read_fstx(p, lazy = TRUE)
  saveRDS(z, q)
  rm(z)
  invisible(gc())
  expect_silent(write_fstx(x, p))                      # serialising released nothing it should keep
  expect_identical(readRDS(q), x)
  old <- options(fstx.portable_serialize = TRUE)
  on.exit(options(old))
  saveRDS(read_fstx(p, strings = "deferred"), q)
  y <- readRDS(q)
  expect_identical(y, x)
  expect_null(col_state(y$chr))                        # plain R vectors: readable without fstx
  expect_identical(unserialize(serialize(read_fstx(p, lazy = TRUE), NULL)), x)
  invisible(gc())
})

test_that("RDS files written from deferred and lazy columns read back in a new R session", {
  skip_on_cran()
  x <- make_df(10000)
  p <- tempfile(fileext = ".fstx")
  q1 <- tempfile(fileext = ".rds")
  q2 <- tempfile(fileext = ".rds")
  ref <- tempfile(fileext = ".rds")
  write_fstx(x, p)
  saveRDS(read_fstx(p, strings = "deferred"), q1)
  z <- read_fstx(p, lazy = TRUE)
  saveRDS(z, q2)
  saveRDS(x, ref)
  rm(z)
  invisible(gc())
  fw <- function(f) normalizePath(f, winslash = "/")
  script <- tempfile(fileext = ".R")
  writeLines(c(sprintf("r <- readRDS('%s'); a <- readRDS('%s'); b <- readRDS('%s')", fw(ref), fw(q1), fw(q2)),
               "cat(identical(a, r), identical(b, r), 'fstx' %in% loadedNamespaces(), '\n')"), script)
  rs <- file.path(R.home("bin"), "Rscript")
  out <- system2(rs, shQuote(fw(script)), stdout = TRUE, stderr = TRUE)
  expect_identical(tail(out, 1), "TRUE TRUE TRUE ")
})
