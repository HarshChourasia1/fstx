# 1.0 (WS4): all-dictionary string columns filled straight from the codes, and subsets of unfinished string columns
# that make no R string until used (views). Results must equal an eager read. Synthetic data only.

col_state <- function(x) fstx:::fstx_col_state_impl(x)
is_view <- function(v) any(grepl("fstx string subset", capture.output(.Internal(inspect(v)))))
unmade_view <- function(v) any(grepl("not made yet", capture.output(.Internal(inspect(v)))))

r4_df <- function(n, seed = 21) {
  set.seed(seed)
  data.frame(
    grp = sample(c(utf8_words, NA), n, TRUE),                                     # all dictionary pages
    key = ifelse(runif(n) < 0.05, NA, sprintf("k%06d_%s", sample.int(n, n, TRUE), sample(letters, n, TRUE))),
    num = seq_len(n),
    stringsAsFactors = FALSE
  )
}

r4_file <- function(x, parts = 1) {   # parts > 1: appended row groups, so dictionary columns have several dictionaries
  p <- tempfile(fileext = ".fstx")
  cut <- split(seq_len(nrow(x)), ceiling(seq_len(nrow(x)) / ceiling(nrow(x) / parts)))
  for (k in seq_along(cut)) write_fstx(x[cut[[k]], , drop = FALSE], p, append = k > 1)
  p
}

test_that("all-dictionary columns come back as plain vectors equal to an eager read, in every mode", {
  x <- r4_df(120000)
  p <- r4_file(x, parts = 3)
  for (s in c("auto", "eager")) {
    z <- read_fstx(p, strings = s)
    expect_null(col_state(z$grp), info = s)                       # a plain STRSXP
    expect_identical(z, x, ignore_attr = TRUE, info = s)
    expect_identical(read_fstx(p, strings = s, from = 39999, to = 80003), x[39999:80003, ], ignore_attr = TRUE, info = s)
    expect_identical(read_fstx(p, strings = s, columns = "grp", from = 5, to = 5)$grp, x$grp[5], info = s)
  }
  z <- read_fstx(p)
  expect_identical(table(z$grp, useNA = "ifany"), table(x$grp, useNA = "ifany"))
  expect_identical(unserialize(serialize(z, NULL)), z)
  z$grp[3] <- "changed"                                           # a plain vector: R copies or changes it as usual
  expect_identical(z$grp[3], "changed")
  expect_identical(read_fstx(p)$grp, x$grp)
  for (m in c("deferred", "lazy")) {                              # left as ALTREP: made whole in one pass on use
    z <- if (m == "lazy") read_fstx(p, lazy = TRUE) else read_fstx(p, strings = m)
    g <- z$grp
    g[7] <- "set"                                                 # R copies g first: Dataptr with nothing made yet
    expect_identical(g, replace(x$grp, 7, "set"), info = m)
    expect_identical(z$grp, x$grp, info = m)
    expect_true(col_state(z$grp)$materialised, info = m)
  }
})

test_that("a subset of up to half an unfinished column is a view; it equals the eager subset", {
  x <- r4_df(200000)
  p <- r4_file(x)
  set.seed(7)
  for (m in c("auto", "deferred", "lazy")) {
    z <- if (m == "lazy") read_fstx(p, lazy = TRUE) else read_fstx(p, strings = m)
    i <- sort(sample.int(200000, 70000))
    v <- z$key[i]
    expect_true(unmade_view(v), info = m)
    expect_identical(length(v), 70000L, info = m)
    w <- v[c(2, 1, 69999, 5)]                                      # a view of a view: still nothing made
    expect_true(unmade_view(w), info = m)
    expect_identical(w, x$key[i][c(2, 1, 69999, 5)], info = m)
    expect_identical(v, x$key[i], info = m)                         # made now
    expect_true(is_view(v) && !unmade_view(v), info = m)
    expect_identical(v[c(3, NA, 70001, 0)], x$key[i][c(3, NA, 70001, 0)], info = m)
    expect_false(col_state(z$key)$materialised, info = m)           # the column itself was not made whole
    d <- as.double(i[1:1000])
    expect_identical(z$key[d], x$key[d], info = m)                  # double indices
    expect_identical(z$key[c(1, 1, 2)], x$key[c(1, 1, 2)], info = m) # repeats
  }
})

test_that("views stay correct when the column is made, changed or dropped after the view was taken", {
  x <- r4_df(150000)
  p <- r4_file(x)
  i <- seq(1, 150000, by = 3)
  z <- read_fstx(p)
  v1 <- z$key[i]
  v2 <- z$key[i[1:10]]
  invisible(nchar(z$key))                                         # the column is finished (its bytes released)
  expect_true(col_state(z$key)$materialised)
  expect_identical(v1, x$key[i])
  expect_identical(v2[2:3], x$key[i[2:3]])                        # subset of an unmade view of a made column
  z <- read_fstx(p)
  k <- z$key
  v <- k[i]
  k[i[1]] <- "zz"                                                 # R copies the shared column first
  expect_identical(k[i[1]], "zz")
  expect_identical(v, x$key[i])
  v <- read_fstx(p)$key[i]                                        # only the view keeps the column alive
  invisible(gc())
  expect_identical(v, x$key[i])
  v <- read_fstx(p)$key[i]
  v[2] <- "set"                                                   # Set_elt makes the view first
  expect_identical(v, replace(x$key[i], 2, "set"))
  v <- read_fstx(p)$key[i]
  expect_identical(unserialize(serialize(v, NULL)), x$key[i])     # serialised as a plain vector
  expect_null(col_state(unserialize(serialize(v, NULL))))
  f <- tempfile(fileext = ".rds")
  saveRDS(read_fstx(p)[i, ], f)
  expect_identical(readRDS(f), x[i, ], ignore_attr = TRUE)
})

test_that("dplyr::filter leaves the columns it does not use as views and gives the eager result", {
  skip_if_not_installed("dplyr")
  x <- r4_df(200000)
  p <- r4_file(x)
  z <- read_fstx(p)
  g <- function(d) dplyr::filter(d, grp %in% c("café", "中文"))     # about 22% of the rows
  f <- g(z)                                                         # grp: plain; key sliced into a view
  expect_true(unmade_view(f$key))
  expect_identical(dplyr::count(f, grp), dplyr::count(g(x), grp))
  expect_true(unmade_view(f$key))                                   # count() did not need it
  expect_identical(f, g(x))
  expect_identical(dplyr::arrange(g(z), key), dplyr::arrange(g(x), key))
})

test_that("invalid UTF-8 in factor levels and dictionary strings reads without a crash, bytes unchanged", {
  bad <- c("ok", "\xff\xfe", "caf\xe9", "\xc3")
  Encoding(bad) <- "UTF-8"                                         # marked UTF-8 but not valid UTF-8
  set.seed(3)
  x <- data.frame(f = factor(sample(bad, 5000, TRUE), levels = bad), s = sample(bad, 5000, TRUE),
                  stringsAsFactors = FALSE)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  for (m in c("auto", "eager", "deferred", "lazy")) {
    z <- if (m == "lazy") read_fstx(p, lazy = TRUE) else read_fstx(p, strings = m)
    expect_identical(charToRaw(paste(levels(z$f), collapse = "|")), charToRaw(paste(bad, collapse = "|")), info = m)
    expect_identical(as.integer(z$f), as.integer(x$f), info = m)
    expect_identical(lapply(z$s, charToRaw), lapply(x$s, charToRaw), info = m)
    expect_identical(lapply(z$s[1:2000], charToRaw), lapply(x$s[1:2000], charToRaw), info = m)
  }
  z <- tryCatch(suppressWarnings(read_fstx(p, as_factor = TRUE)), error = function(e) e)   # an error is fine, a crash not
  expect_true(is.data.frame(z) || inherits(z, "error"))
})

test_that("the new paths are correct under gctorture", {
  skip_on_cran()
  x <- r4_df(30000)
  p <- r4_file(x, parts = 2)
  z <- read_fstx(p)
  i <- c(29000, 3, 15001, 3, 20000, 7)
  on.exit(gctorture(FALSE), add = TRUE)
  gctorture(TRUE)
  a <- read_fstx(p, columns = "grp", from = 14000, to = 16020)$grp   # dictionary fill
  d <- read_fstx(p, columns = "grp", strings = "deferred")$grp
  d2 <- d
  d2[1] <- "q"                                                      # duplicate: Dataptr, dictionary fill in one pass
  v <- z$key[i]                                                     # view
  w <- v[c(4, 1, 2)]                                                # view of a view
  vn <- nchar(w)                                                    # w made
  b <- z$key[c(10, 20)]
  invisible(nchar(z$key))
  bb <- b[2:1]                                                      # view of a made column
  gctorture(FALSE)
  expect_identical(a, x$grp[14000:16020])
  expect_identical(d, x$grp)
  expect_identical(d2, replace(x$grp, 1, "q"))
  expect_identical(w, x$key[i][c(4, 1, 2)])
  expect_identical(vn, nchar(x$key[i][c(4, 1, 2)]))
  expect_identical(v, x$key[i])
  expect_identical(bb, x$key[c(20, 10)])
})
