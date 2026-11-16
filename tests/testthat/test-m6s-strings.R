# M6s: strings = "auto" (the default) and the other string modes give the eager result in everyday R.

col_state <- function(x) fstx:::fstx_col_state_impl(x)
m6s_modes <- c("auto", "deferred", "eager")

m6s_df <- function(n, seed = 7) {
  set.seed(seed)
  data.frame(
    id   = seq_len(n),
    grp  = sample(c(utf8_words, NA), n, TRUE),                                  # dictionary pages
    key  = ifelse(runif(n) < 0.05, NA, sprintf("k%06d_%s", sample.int(n, n, TRUE), sample(letters, n, TRUE))),
    val  = round(rnorm(n), 3),
    note = ifelse(runif(n) < 0.3, "", strrep("é", sample(0:12, n, TRUE))),     # empty strings and UTF-8
    stringsAsFactors = FALSE
  )
}

test_that("strings = 'auto' is the default and leaves character columns as unmade ALTREP vectors", {
  x <- m6s_df(200000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  z <- read_fstx(p)
  s <- col_state(z$key)
  expect_false(is.null(s))
  expect_false(s$lazy)
  expect_false(s$file_open)
  expect_false(s$materialised)
  expect_gt(s$pages, 1)
  expect_null(col_state(z$id))                                     # numeric columns are read eagerly
  expect_null(col_state(read_fstx(p, strings = "eager")$key))      # eager: plain character vectors
  expect_identical(z, x)
  expect_true(col_state(z$key)$materialised)                       # identical() made every string
})

test_that("every string mode gives the eager result for base R operations", {
  x <- m6s_df(150000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  for (m in m6s_modes) for (th in unique(c(1L, test_threads(4L)))) {
    i <- paste(m, th)
    rd <- function() read_fstx(p, strings = m, threads = th)
    z <- rd()
    expect_identical(z$key[c(1, 70000, 150000, 2)], x$key[c(1, 70000, 150000, 2)], info = i)   # pages out of order
    expect_identical(z, x, info = i)
    expect_identical(nchar(rd()$note), nchar(x$note), info = i)
    expect_identical(grepl("_a$", rd()$key), grepl("_a$", x$key), info = i)
    expect_identical(table(rd()$grp, useNA = "always"), table(x$grp, useNA = "always"), info = i)
    expect_identical(sort(rd()$key, na.last = TRUE), sort(x$key, na.last = TRUE), info = i)
    expect_identical(order(rd()$grp, rd()$key), order(x$grp, x$key), info = i)
    expect_identical(unique(rd()$grp), unique(x$grp), info = i)
    expect_identical(match(x$key[1:500], rd()$key), match(x$key[1:500], x$key), info = i)
    expect_identical(rd()$key %in% x$key[1:100], x$key %in% x$key[1:100], info = i)
    expect_identical(paste(rd()$grp, rd()$note), paste(x$grp, x$note), info = i)
    expect_identical(substr(rd()$key, 2, 4), substr(x$key, 2, 4), info = i)
    expect_identical(toupper(rd()$key), toupper(x$key), info = i)
    expect_identical(gsub("k", "K", rd()$key), gsub("k", "K", x$key), info = i)
    expect_identical(is.na(rd()$key), is.na(x$key), info = i)
    expect_identical(anyNA(rd()$key), anyNA(x$key), info = i)
    expect_identical(rev(rd()$key), rev(x$key), info = i)
    expect_identical(head(rd()$key, 3), head(x$key, 3), info = i)
    expect_identical(tail(rd()$key, 3), tail(x$key, 3), info = i)
    expect_identical(factor(rd()$grp), factor(x$grp), info = i)
    expect_identical(as.factor(rd()$key[1:1000]), as.factor(x$key[1:1000]), info = i)
    expect_identical(split(rd()$val, rd()$grp), split(x$val, x$grp), info = i)
    expect_identical(tapply(rd()$val, rd()$grp, sum), tapply(x$val, x$grp, sum), info = i)
    expect_identical(aggregate(val ~ grp, rd(), length), aggregate(val ~ grp, x, length), info = i)
    expect_identical(duplicated(rd()$key), duplicated(x$key), info = i)
    expect_identical(rd()$key == "k000001_a", x$key == "k000001_a", info = i)
    expect_identical(which(rd()$grp == "café"), which(x$grp == "café"), info = i)
    expect_identical(enc2native(rd()$note), enc2native(x$note), info = i)
    expect_identical(Encoding(rd()$note), Encoding(x$note), info = i)
    expect_identical(as.character(rd()$key), x$key, info = i)
    expect_identical(c(rd()$key, "end"), c(x$key, "end"), info = i)
    expect_identical(vapply(rd()$grp[1:50], nchar, 1L), vapply(x$grp[1:50], nchar, 1L), info = i)
    expect_identical(lapply(rd()[c("grp", "key")], function(v) sum(nchar(v), na.rm = TRUE)),
                     lapply(x[c("grp", "key")], function(v) sum(nchar(v), na.rm = TRUE)), info = i)
    expect_identical(format(head(rd(), 20)), format(head(x, 20)), info = i)
    expect_identical(capture.output(print(head(rd(), 5))), capture.output(print(head(x, 5))), info = i)
    expect_identical(summary(rd()), summary(x), info = i)
    expect_identical(capture.output(str(rd())), capture.output(str(x)), info = i)
    rm(z)
  }
  invisible(gc())
})

test_that("every string mode gives the eager result when the data frame is modified", {
  x <- m6s_df(120000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  for (m in m6s_modes) {
    rd <- function() read_fstx(p, strings = m)
    a <- rd(); b <- x
    a$key[5] <- "new"; b$key[5] <- "new"
    expect_identical(a, b, info = m)
    a <- rd(); b <- x
    a[100000, "grp"] <- NA; b[100000, "grp"] <- NA
    expect_identical(a, b, info = m)
    a <- rd(); b <- x
    a$key[a$val > 1] <- "big"; b$key[b$val > 1] <- "big"
    expect_identical(a, b, info = m)
    a <- rd(); b <- x
    a$key2 <- paste0(a$key, "!"); b$key2 <- paste0(b$key, "!")
    expect_identical(a, b, info = m)
    a <- rd(); b <- x
    a$grp <- NULL; b$grp <- NULL
    expect_identical(a, b, info = m)
    a <- rd(); b <- x
    a[["key"]][119999:120000] <- c("y", "z"); b[["key"]][119999:120000] <- c("y", "z")
    expect_identical(a, b, info = m)
    a <- rd(); v <- a$key; v[1] <- "first"; w <- x$key; w[1] <- "first"
    expect_identical(v, w, info = m)
    expect_identical(a$key, x$key, info = m)                            # the copy did not change a
    v <- rd()$key; length(v) <- 10; expect_identical(v, x$key[1:10], info = m)
    expect_identical(transform(rd(), up = toupper(grp)), transform(x, up = toupper(grp)), info = m)
    expect_identical(within(rd(), k <- nchar(key)), within(x, k <- nchar(key)), info = m)
    expect_identical(rd()[order(rd()$key), ], x[order(x$key), ], info = m)
    expect_identical(rd()[rd()$grp %in% c("plain", "中文"), c("id", "key")], x[x$grp %in% c("plain", "中文"), c("id", "key")], info = m)
    expect_identical(subset(rd(), !is.na(key) & val > 0, c(key, note)), subset(x, !is.na(key) & val > 0, c(key, note)), info = m)
    expect_identical(rbind(rd(), rd()[1:10, ]), rbind(x, x[1:10, ]), info = m)
    expect_identical(cbind(rd(), z = 1), cbind(x, z = 1), info = m)
    expect_identical(merge(rd()[1:5000, c("id", "key")], rd()[3000:9000, c("id", "grp")], by = "id"),
                     merge(x[1:5000, c("id", "key")], x[3000:9000, c("id", "grp")], by = "id"), info = m)
    expect_identical(merge(rd()[1:3000, c("grp", "key")], data.frame(grp = c("plain", "café"), w = 1:2), by = "grp", all.x = TRUE),
                     merge(x[1:3000, c("grp", "key")], data.frame(grp = c("plain", "café"), w = 1:2), by = "grp", all.x = TRUE),
                     info = m)
    expect_identical(do.call(rbind, split(rd()[1:2000, ], rd()$grp[1:2000])), do.call(rbind, split(x[1:2000, ], x$grp[1:2000])),
                     info = m)
    expect_identical(as.list(rd()), as.list(x), info = m)
    expect_identical(as.matrix(rd()[1:100, c("grp", "key")]), as.matrix(x[1:100, c("grp", "key")]), info = m)
  }
  invisible(gc())
})

test_that("dplyr gives the eager result on every string mode", {
  skip_if_not_installed("dplyr")
  x <- m6s_df(100000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  f1 <- function(d) dplyr::count(dplyr::filter(d, grepl("_[a-e]$", key)), grp)
  f2 <- function(d) dplyr::summarise(dplyr::group_by(d, grp), n = dplyr::n(), m = mean(val), k = dplyr::n_distinct(key))
  f3 <- function(d) dplyr::arrange(dplyr::mutate(d, kk = paste0(key, grp)), kk, id)
  f4 <- function(d) dplyr::left_join(d[1:3000, c("id", "key")], d[2000:5000, c("id", "grp")], by = "id")
  f5 <- function(d) dplyr::distinct(d, grp)
  f6 <- function(d) dplyr::bind_rows(d[1:10, ], d[11:20, ])
  for (m in m6s_modes) {
    for (f in list(f1, f2, f3, f4, f5, f6)) expect_identical(f(read_fstx(p, strings = m)), f(x), info = m)
  }
  t <- dplyr::as_tibble(x)
  write_fstx(t, p)
  expect_identical(f2(read_fstx(p)), f2(t))
  expect_identical(read_fstx(p), t)
  invisible(gc())
})

test_that("data.table gives the eager result on every string mode", {
  skip_if_not_installed("data.table")
  x <- data.table::as.data.table(m6s_df(100000))
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  for (m in m6s_modes) {
    # data.table's syntax needs a data.table-aware caller: evaluate from an environment under globalenv()
    e <- list2env(list(z = read_fstx(p, strings = m), y = data.table::copy(x), p = p, m = m), parent = globalenv())
    expect_identical(evalq(z[, .N, by = grp], e), evalq(y[, .N, by = grp], e), info = m)
    expect_identical(evalq(z[, .(s = sum(val)), keyby = .(grp, substr(key, 1, 3))], e),
                     evalq(y[, .(s = sum(val)), keyby = .(grp, substr(key, 1, 3))], e), info = m)
    expect_identical(evalq(z[grp == "café" & !is.na(key)], e), evalq(y[grp == "café" & !is.na(key)], e), info = m)
    expect_identical(evalq(data.table::rbindlist(list(z[1:100], z[200:300])), e), evalq(data.table::rbindlist(list(y[1:100], y[200:300])), e), info = m)
    expect_identical(evalq(merge(fstx::read_fstx(p, strings = m)[1:5000], y[3000:9000, .(id, k2 = key)], by = "id"), e),
                     evalq(merge(y[1:5000], y[3000:9000, .(id, k2 = key)], by = "id"), e), info = m)
    expect_identical(evalq(data.table::fsetdiff(fstx::read_fstx(p, strings = m)[, .(grp)], y[1:10, .(grp)]), e),
                     evalq(data.table::fsetdiff(y[, .(grp)], y[1:10, .(grp)]), e), info = m)
    expect_identical(data.table::uniqueN(read_fstx(p, strings = m)$key), data.table::uniqueN(x$key), info = m)
    evalq(z[id < 10, key := "set"], e)
    evalq(y[id < 10, key := "set"], e)
    expect_identical(e$z, e$y, info = m)
    evalq(z[, up := toupper(note)], e)
    evalq(y[, up := toupper(note)], e)
    expect_identical(e$z, e$y, info = m)
    data.table::setkey(e$z, key)
    data.table::setkey(e$y, key)
    expect_identical(e$z, e$y, info = m)
    rm(e)
  }
  invisible(gc())
})

test_that("serialize, saveRDS and write.csv give the eager result on every string mode", {
  x <- m6s_df(120000)
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".rds")
  write_fstx(x, p)
  c0 <- tempfile(fileext = ".csv")
  c1 <- tempfile(fileext = ".csv")
  write.csv(x, c0, row.names = FALSE, fileEncoding = "UTF-8")
  for (m in m6s_modes) {
    for (touch in c("none", "one", "two", "all")) {
      z <- read_fstx(p, strings = m)
      if (touch == "one") invisible(z$key[100000])
      if (touch == "two") invisible(z$key[c(100000, 1)])
      if (touch == "all") invisible(nchar(z$key))
      i <- paste(m, touch)
      expect_identical(unserialize(serialize(z, NULL)), x, info = i)
      expect_identical(unserialize(serialize(z, NULL, xdr = FALSE)), x, info = i)
      expect_identical(unserialize(serialize(z, NULL, version = 2)), x, info = i)
      saveRDS(z, q)
      expect_identical(readRDS(q), x, info = i)
      expect_identical(z, x, info = i)                                 # serialising changed nothing
    }
    a <- read_fstx(p, strings = m)
    if (m == "auto") {
      expect_null(col_state(unserialize(serialize(a, NULL))$key))      # R's standard format
    }
    write.csv(read_fstx(p, strings = m), c1, row.names = FALSE, fileEncoding = "UTF-8")
    expect_identical(unname(tools::md5sum(c1)), unname(tools::md5sum(c0)), info = m)
    tc <- textConnection("out", "w", local = TRUE)
    write.table(read_fstx(p, strings = m)[1:2000, ], tc)
    close(tc)
    tc <- textConnection("ref", "w", local = TRUE)
    write.table(x[1:2000, ], tc)
    close(tc)
    expect_identical(out, ref, info = m)
    expect_identical(vapply(read_fstx(p, strings = m)$grp[1:30], deparse, ""), vapply(x$grp[1:30], deparse, ""), info = m)
  }
  invisible(gc())
})

test_that("RDS of an auto read is readable in a new R session without fstx", {
  skip_on_cran()
  x <- m6s_df(50000)
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".rds")
  q2 <- tempfile(fileext = ".rds")
  ref <- tempfile(fileext = ".rds")
  write_fstx(x, p)
  z <- read_fstx(p)
  invisible(z$key[60000])                          # one page made
  saveRDS(z, q)
  saveRDS(read_fstx(p), q2)                        # nothing made
  saveRDS(x, ref)
  fw <- function(f) normalizePath(f, winslash = "/")
  script <- tempfile(fileext = ".R")
  writeLines(c(sprintf("r <- readRDS('%s'); a <- readRDS('%s'); b <- readRDS('%s')", fw(ref), fw(q), fw(q2)),
               "cat(identical(a, r), identical(b, r), 'fstx' %in% loadedNamespaces(), '\n')"), script)
  out <- system2(file.path(R.home("bin"), "Rscript"), shQuote(fw(script)), stdout = TRUE, stderr = TRUE)
  expect_identical(tail(out, 1), "TRUE TRUE FALSE ")
})

test_that("C code using STRING_PTR_RO, DATAPTR_RO, STRING_ELT and SET_STRING_ELT sees the eager strings", {
  skip_on_cran()
  rcmd <- file.path(R.home("bin"), "R")
  d <- tempfile("m6s_c")
  dir.create(d)
  src <- file.path(d, "m6sc.c")
  writeLines(c(
    "#include <R.h>",
    "#include <Rinternals.h>",
    "SEXP m6s_copy_ptr(SEXP x) {",
    "  R_xlen_t n = XLENGTH(x);",
    "  const SEXP* p = STRING_PTR_RO(x);",
    "  SEXP out = PROTECT(allocVector(STRSXP, n));",
    "  for (R_xlen_t i = 0; i < n; ++i) SET_STRING_ELT(out, i, p[i]);",
    "  UNPROTECT(1);",
    "  return out;",
    "}",
    "SEXP m6s_copy_dataptr(SEXP x) {",
    "  R_xlen_t n = XLENGTH(x);",
    "  const SEXP* p = (const SEXP*)DATAPTR_RO(x);",
    "  SEXP out = PROTECT(allocVector(STRSXP, n));",
    "  for (R_xlen_t i = 0; i < n; ++i) SET_STRING_ELT(out, i, p[i]);",
    "  UNPROTECT(1);",
    "  return out;",
    "}",
    "SEXP m6s_copy_elt(SEXP x) {",
    "  R_xlen_t n = XLENGTH(x);",
    "  SEXP out = PROTECT(allocVector(STRSXP, n));",
    "  for (R_xlen_t i = n - 1; i >= 0; --i) SET_STRING_ELT(out, i, STRING_ELT(x, i));",
    "  UNPROTECT(1);",
    "  return out;",
    "}",
    "SEXP m6s_set(SEXP x, SEXP i, SEXP v) {",
    "  SET_STRING_ELT(x, (R_xlen_t)asReal(i) - 1, STRING_ELT(v, 0));",
    "  return x;",
    "}",
    "SEXP m6s_nchar_sum(SEXP x) {",
    "  double s = 0;",
    "  R_xlen_t n = XLENGTH(x);",
    "  for (R_xlen_t i = 0; i < n; ++i) { SEXP c = STRING_ELT(x, i); if (c != NA_STRING) s += LENGTH(c); }",
    "  return ScalarReal(s);",
    "}"), src)
  old <- setwd(d)
  on.exit(setwd(old), add = TRUE)
  res <- suppressWarnings(system2(rcmd, c("CMD", "SHLIB", "m6sc.c"), stdout = TRUE, stderr = TRUE))
  dll <- file.path(d, paste0("m6sc", .Platform$dynlib.ext))
  skip_if_not(file.exists(dll), "no C compiler")
  dl <- dyn.load(dll)
  on.exit(dyn.unload(dll), add = TRUE)
  x <- m6s_df(150000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  for (m in m6s_modes) {
    rd <- function() read_fstx(p, strings = m)$key
    expect_identical(.Call(dl$m6s_copy_ptr, rd()), x$key, info = m)
    expect_identical(.Call(dl$m6s_copy_dataptr, rd()), x$key, info = m)
    expect_identical(.Call(dl$m6s_copy_elt, rd()), x$key, info = m)
    expect_identical(.Call(dl$m6s_nchar_sum, rd()), as.double(sum(nchar(x$key, "bytes"), na.rm = TRUE)), info = m)
    z <- rd()
    invisible(z[100000])                                               # a page made alone, then a set
    z2 <- .Call(dl$m6s_set, z, 3, "c-set")
    w <- x$key
    w[3] <- "c-set"
    expect_identical(z2, w, info = m)
  }
})

test_that("edge cases: zero rows, one row, all NA, empty strings, long strings, one page", {
  p <- tempfile(fileext = ".fstx")
  cases <- list(
    zero = data.frame(s = character(0), i = integer(0), stringsAsFactors = FALSE),
    one = data.frame(s = "ü", stringsAsFactors = FALSE),
    one_na = data.frame(s = NA_character_, stringsAsFactors = FALSE),
    all_na = data.frame(s = rep(NA_character_, 200000), stringsAsFactors = FALSE),
    all_empty = data.frame(s = rep("", 200000), stringsAsFactors = FALSE),
    long = data.frame(s = c(strrep("x", 2e6), NA, "", strrep("ñ", 5e5)), stringsAsFactors = FALSE),
    one_page = data.frame(s = as.character(1:1000), stringsAsFactors = FALSE),
    unique_many = data.frame(s = sprintf("u%07d", 1:300000), stringsAsFactors = FALSE),
    mixed_pages = data.frame(s = c(rep(c("a", "b"), 100000), sprintf("v%06d", 1:100000)), stringsAsFactors = FALSE),
    latin1 = data.frame(s = rep(iconv(c("caf\xe9", "na\xefve", NA), "latin1", "latin1"), 70000), stringsAsFactors = FALSE)
  )
  for (nm in names(cases)) {
    x <- cases[[nm]]
    write_fstx(x, p)
    ref <- x
    if (nm == "latin1") ref$s <- enc2utf8(ref$s)
    for (m in m6s_modes) {
      i <- paste(nm, m)
      n <- nrow(x)
      z <- read_fstx(p, strings = m)
      expect_identical(length(z$s), n, info = i)
      if (n > 0) {
        expect_identical(z$s[n], ref$s[n], info = i)                     # last page first
        expect_identical(z$s[1], ref$s[1], info = i)
      }
      expect_identical(z, ref, info = i)
      expect_identical(unserialize(serialize(read_fstx(p, strings = m), NULL)), ref, info = i)
      if (n >= 2) expect_identical(read_fstx(p, strings = m, from = 2)$s, ref$s[2:n], info = i)
      if (n > 0) expect_identical(anyNA(read_fstx(p, strings = m)$s), anyNA(ref$s), info = i)
    }
  }
  invisible(gc())
})

test_that("pages are made in any order: first alone, then into the full vector", {
  x <- data.frame(s = sprintf("r%07d", 1:400000), stringsAsFactors = FALSE)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  np <- col_state(read_fstx(p)$s)$pages
  expect_gt(np, 3)
  set.seed(3)
  for (m in c("auto", "deferred")) {
    for (k in 1:6) {
      z <- read_fstx(p, strings = m)
      idx <- sample.int(400000, 50)
      expect_identical(z$s[idx], x$s[idx], info = m)                     # random pages, in random order
      expect_identical(z$s[rev(idx)], x$s[rev(idx)], info = m)
      if (k %% 2 == 0) z$s[idx[1]] <- "set"                              # set after some pages are made
      ref <- x$s
      if (k %% 2 == 0) ref[idx[1]] <- "set"
      expect_identical(z$s, ref, info = m)
    }
    z <- read_fstx(p, strings = m)
    for (r in seq(400000, 1, by = -65536)) expect_identical(z$s[r], x$s[r], info = m)   # every page, backwards
    expect_identical(z$s, x$s, info = m)
    expect_true(col_state(z$s)$materialised, info = m)
    z <- read_fstx(p, strings = m)
    v <- vapply(seq(1, 400000, by = 997), function(r) z$s[[r]], "")      # element by element
    expect_identical(v, x$s[seq(1, 400000, by = 997)], info = m)
  }
})

test_that("small subsets make only the strings asked for (Extract_subset) and match the eager result", {
  x <- m6s_df(300000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  set.seed(9)
  for (m in c("auto", "deferred", "lazy")) {
    rd <- function() if (m == "lazy") read_fstx(p, lazy = TRUE) else read_fstx(p, strings = m)
    z <- rd()
    idx <- sample.int(300000, 5000)
    expect_identical(z$key[idx], x$key[idx], info = m)
    expect_identical(z$grp[idx], x$grp[idx], info = m)                     # dictionary pages
    expect_identical(z$note[sort(idx)], x$note[sort(idx)], info = m)       # empty strings, UTF-8
    expect_false(col_state(z$key)$materialised, info = m)                  # no page was made for the subset
    expect_null(col_state(z$key[idx]))                                     # the subset is a plain vector
    expect_identical(z$key[c(idx, idx)], x$key[c(idx, idx)], info = m)     # repeats
    expect_identical(z$key[as.double(idx)], x$key[as.double(idx)], info = m)
    expect_identical(z$key[c(1, NA, 5)], x$key[c(1, NA, 5)], info = m)     # NA index: R's default
    expect_identical(z$key[c(2, 300001)], x$key[c(2, 300001)], info = m)   # past the end: R's default
    expect_identical(z$key[-(1:299990)], x$key[-(1:299990)], info = m)     # negative
    expect_identical(z$key[x$val > 2.5], x$key[x$val > 2.5], info = m)     # logical
    expect_identical(z$key[integer(0)], character(0), info = m)
    invisible(z$key[250000])                                               # a page made alone
    invisible(z$key[c(1, 100000)])                                         # and two into the full vector
    expect_identical(z$key[idx], x$key[idx], info = m)                     # mixes made and unmade pages
    expect_identical(z[idx, ], x[idx, ], info = m)                         # rows of a data frame
    expect_identical(z, x, info = m)
    expect_identical(z$key[idx], x$key[idx], info = m)                     # complete: R's default
  }
  skip_if_not_installed("dplyr")
  z <- read_fstx(p)
  f <- function(d) dplyr::count(dplyr::filter(d, grepl("_q$", key)), grp)
  expect_identical(f(z), f(x))
  expect_null(col_state(z$note))                                           # M6.1: all-dictionary, plain at read
  expect_identical(dplyr::slice(z, c(5, 1, 299999)), dplyr::slice(x, c(5, 1, 299999)))
  expect_identical(z, x)
})

test_that("small subsets are correct under gctorture", {
  skip_on_cran()
  x <- m6s_df(140000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  z <- read_fstx(p)
  idx <- c(139000, 3, 70001, 3, 100000)
  on.exit(gctorture(FALSE), add = TRUE)
  gctorture(TRUE)
  a <- z$key[idx]
  b <- z$note[idx]
  gctorture(FALSE)
  expect_identical(a, x$key[idx])
  expect_identical(b, x$note[idx])
})
