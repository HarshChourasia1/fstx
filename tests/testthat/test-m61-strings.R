# M6.1: speed fixes for strings = "auto" give the same results as an eager read. Synthetic data only.

col_state <- function(x) fstx:::fstx_col_state_impl(x)

m61_df <- function(n, seed = 11) {
  set.seed(seed)
  data.frame(
    grp  = sample(c(utf8_words, NA), n, TRUE),                                  # all dictionary pages
    key  = ifelse(runif(n) < 0.05, NA, sprintf("k%06d_%s", sample.int(n, n, TRUE), sample(letters, n, TRUE))),
    note = ifelse(runif(n) < 0.3, "", strrep("é", sample(0:12, n, TRUE))),     # empty strings and UTF-8
    stringsAsFactors = FALSE
  )
}

test_that("auto returns all-dictionary columns as plain vectors, other string columns as ALTREP", {
  x <- m61_df(200000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  z <- read_fstx(p)
  expect_null(col_state(z$grp))                                    # low cardinality: made at read
  expect_false(col_state(z$key)$materialised)
  expect_false(is.null(col_state(read_fstx(p, strings = "deferred")$grp)))   # deferred keeps it deferred
  expect_false(is.null(col_state(read_fstx(p, lazy = TRUE)$grp)))
  expect_identical(z, x)
  expect_identical(read_fstx(p, from = 70001, to = 140000), x[70001:140000, , drop = FALSE], ignore_attr = TRUE)
  expect_identical(read_fstx(p, rows = c(5, 199999, 3)), x[c(5, 199999, 3), ], ignore_attr = TRUE)
})

test_that("an element loop gives the eager values and makes the pages it touches", {
  x <- m61_df(300000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  for (m in c("auto", "deferred", "lazy")) {
    z <- if (m == "lazy") read_fstx(p, lazy = TRUE) else read_fstx(p, strings = m)
    v <- z$key
    r <- c(1:3000, 299990:300000, 140000:137000)
    got <- character(length(r))
    for (k in seq_along(r)) got[k] <- v[r[k]]                      # x[i]: one element through R's default
    expect_identical(got, x$key[r], info = m)
    for (k in seq_along(r)) got[k] <- v[r[k]]                      # again, on made pages
    expect_identical(got, x$key[r], info = m)
    expect_identical(v[NA_integer_], NA_character_, info = m)
    expect_identical(v[300001], NA_character_, info = m)
    expect_identical(z, x, info = m)
  }
})

test_that("subsets of a finished column follow R's rules for every index kind", {
  x <- m61_df(150000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  z <- read_fstx(p)
  invisible(nchar(z$key))                                           # every page made: the column is finished
  expect_true(col_state(z$key)$materialised)
  set.seed(4)
  idx <- sample.int(150000, 150000)
  ix <- list(idx, as.double(idx), c(3L, NA, 150000L, 150001L, 1L), c(1.5, 0.5, 2.9, NA, NaN, Inf, -Inf, 150000.7, 150001),
             c(-1, -150000), c(TRUE, FALSE, NA), integer(0), double(0), rep(7L, 5), 0, c(0, 4), 1e10, -0.5,
             x$note == "")
  for (i in ix) expect_identical(z$key[i], x$key[i], info = paste(head(i), collapse = " "))
  expect_identical(z[idx, ], x[idx, ], ignore_attr = TRUE)
  expect_identical(rbind(z, z[1:1000, ]), rbind(x, x[1:1000, ]))
  skip_if_not_installed("dplyr")
  expect_identical(dplyr::arrange(z, key), dplyr::arrange(x, key))
  expect_identical(dplyr::slice(z, c(150000, NA, 1)), dplyr::slice(x, c(150000, NA, 1)))
})

test_that("subsets of up to half the rows come from the decoded bytes; offsets survive page making", {
  x <- m61_df(300000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  set.seed(5)
  for (m in c("auto", "deferred", "lazy")) {
    z <- if (m == "lazy") read_fstx(p, lazy = TRUE) else read_fstx(p, strings = m)
    i40 <- sort(sample.int(300000, 120000))                         # 40%: from the bytes
    expect_identical(z$key[i40], x$key[i40], info = m)
    expect_identical(z$note[i40], x$note[i40], info = m)
    expect_false(col_state(z$key)$materialised, info = m)
    expect_identical(z$key[i40], x$key[i40], info = m)              # kept offsets, second call
    invisible(z$key[200000])                                        # page made alone (its offsets dropped)
    invisible(z$key[c(5, 100000)])
    invisible(z$key[70000])                                         # pages into the full vector
    expect_identical(z$key[i40], x$key[i40], info = m)              # made and unmade pages mixed
    i60 <- sample.int(300000, 180000)                               # 60%: R's default
    expect_identical(z$key[i60], x$key[i60], info = m)
    expect_identical(z, x, info = m)
  }
  skip_if_not_installed("dplyr")
  z <- read_fstx(p)
  f <- function(d) dplyr::count(dplyr::filter(d, !is.na(grp) & grp != "plain" & nchar(note) > 3), grp)  # ~37% of rows
  expect_identical(f(z), f(x))
  expect_false(col_state(z$key)$materialised)                       # the sliced column was not made whole
  expect_identical(dplyr::filter(z, !is.na(key) & grepl("[a-l]$", key)), dplyr::filter(x, !is.na(key) & grepl("[a-l]$", key)))
})

test_that("the new subset paths are correct under gctorture", {
  skip_on_cran()
  x <- m61_df(140000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  z <- read_fstx(p)
  w <- read_fstx(p)
  invisible(nchar(w$key))                                           # finished column
  idx <- c(139000, 3, 70001, 3, 100000, NA, 140001)
  on.exit(gctorture(FALSE), add = TRUE)
  gctorture(TRUE)
  a <- z$key[idx[1:5]]                                              # from the bytes, offsets kept
  b <- z$key[100000]                                                # one element: page made
  d <- w$key[idx]                                                   # finished column
  e <- z$note[idx[1:5]]
  gctorture(FALSE)
  expect_identical(a, x$key[idx[1:5]])
  expect_identical(b, x$key[100000])
  expect_identical(d, x$key[idx])
  expect_identical(e, x$note[idx[1:5]])
})
