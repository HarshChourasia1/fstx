# fstx 1.2: binary columns (physical type 5) from list columns of raw vectors. Synthetic data only. Tests marked
# skip_until_drop2() need the engine's Arrow stream types (drop 2).

bin_pool <- function() list(raw(0), as.raw(0), as.raw(c(0, 0, 0)), charToRaw("abc"), as.raw(c(0x61, 0, 0x62)),
                            as.raw(0:255), raw_n(300, 2), as.raw(0xff), charToRaw("café"))
bin_low <- function(n, seed = 7) {    # few distinct values (dictionary-friendly), NULL scattered
  set.seed(seed)
  pool <- bin_pool()
  out <- pool[sample.int(length(pool), n, TRUE)]
  out[sample.int(n, n %/% 20)] <- list(NULL)
  out
}
bin_high <- function(n) {   # mostly distinct values of 0 to 60 bytes, NUL bytes included
  out <- lapply(seq_len(n), function(i) raw_n(i %% 61, i))
  out[seq(10, n, by = 97)] <- list(NULL)
  out
}
bin_df <- function(n = 5000) new_df(id = seq_len(n), lo = bin_low(n), hi = bin_high(n))

test_that("binary columns round-trip: NUL bytes, empty values, NULL for missing", {
  x <- bin_df()
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p, threads = test_threads(2))
  y <- read_fstx(p)
  expect_identical(y, x)
  expect_identical(typeof(y$lo), "list")
  expect_null(attributes(y$lo))
  expect_true(all(vapply(y$hi, function(e) is.null(e) || is.raw(e), NA)))
  expect_identical(vapply(y$lo, is.null, NA), vapply(x$lo, is.null, NA))   # empty values are not missing
  expect_true(any(lengths(y$lo) == 0L & !vapply(y$lo, is.null, NA)))
  m <- fstx_meta(p)
  expect_identical(m$columns$type, c("integer", "binary", "binary"))
  expect_identical(m$columns$class, c("integer", "list", "list"))
  pg <- fstx_pages(p)
  expect_true(all(pg$kind[pg$name %in% c("lo", "hi")] %in% c("string", "codes", "dictionary")))
  b <- readBin(p, "raw", file.size(p))
  expect_true(length(grepRaw("binary", b, fixed = TRUE)) > 0)   # feature bit 17 named
  expect_false(length(grepRaw("int64", b, fixed = TRUE)) > 0)
  for (th in c(1L, test_threads(4))) expect_identical(read_fstx(p, threads = th), x, label = th)
  q <- tempfile(fileext = ".fstx")
  write_fstx(x, q, threads = 1)
  expect_identical(readBin(q, "raw", file.size(q)), b)   # same bytes on every thread count
  unlink(q)
})

test_that("all-NULL, all-empty, zero-row and one-row binary columns", {
  x <- new_df(a = rep(list(NULL), 3000), e = rep(list(raw(0)), 3000), z = rep(list(as.raw(0)), 3000))
  expect_identical(roundtrip(x), x)
  expect_identical(roundtrip(df_take(x, 0)), df_take(x, 0))
  expect_identical(roundtrip(df_take(x, 2)), df_take(x, 2))
  y <- roundtrip(new_df(b = list()))
  expect_identical(y$b, list())
})

test_that("values over 64 KB, and a page of large values, round-trip (plain and FSST)", {
  x <- new_df(b = list(raw_n(70000, 1), NULL, raw_n(200000, 2), raw(0), raw_n(65536, 3), raw_n(1e6, 4),
                       rep(as.raw(0), 100000)))
  for (enc in list(NULL, "plain", "fsst")) {
    p <- tempfile(fileext = ".fstx")
    write_fstx(x, p, .encoding = enc)
    expect_identical(read_fstx(p), x, label = paste(enc))
    expect_identical(read_fstx(p, rows = c(6, 1, 3)), df_take(x, c(6, 1, 3)), label = paste(enc, "rows"))
    if (identical(enc, "fsst")) expect_true("fsst" %in% fstx_pages(p)$encoding)
    unlink(p)
  }
})

test_that("every binary page mix: plain, FSST, dictionaries, each codec, each missing-value shape", {
  n <- 5000
  set.seed(9)
  na_as <- function(v, at) { v[at] <- list(NULL); v }
  base <- list(lo = bin_low(n), hi = bin_high(n), k = rep(list(as.raw(c(1, 0, 2))), n))
  x <- list()
  for (nm in names(base)) {
    x[[paste0(nm, "_0")]] <- lapply(base[[nm]], function(e) if (is.null(e)) raw(0) else e)
    x[[paste0(nm, "_b")]] <- na_as(base[[nm]], sample.int(n, n %/% 10))
    x[[paste0(nm, "_r")]] <- na_as(base[[nm]], 1001:2500)
  }
  x <- list2DF(x)
  seen <- character(0)
  r <- c(1, 2, 1023, 1024, 1025, 2000, 4999, 5000, 3)
  for (codec in c("none", "lz4", "zstd")) {
    for (e in c("plain", "fsst")) {
      for (fl in list(NULL, "dict_codes", "fsst_dict")) {
        enc <- c(e, fl, if (codec != "none") "keep_codec")
        lab <- paste(codec, paste(enc, collapse = "+"))
        p <- tempfile(fileext = ".fstx")
        write_fstx(x, p, compress = codec, .encoding = enc, threads = 2)
        expect_identical(read_fstx(p), x, label = lab)
        expect_identical(read_fstx(p, rows = r), df_take(x, r), label = paste(lab, "rows"))
        expect_identical(read_fstx(p, rows = sort(r)), df_take(x, sort(r)), label = paste(lab, "sorted rows"))
        expect_identical(read_fstx(p, from = 1000, to = 2600), df_take(x, 1000:2600), label = paste(lab, "range"))
        pg <- fstx_pages(p)
        seen <- union(seen, unique(paste(pg$kind, pg$encoding, pg$na, pg$codec)))
        unlink(p)
      }
    }
  }
  nas <- c("none", "bitmap", "runs")
  codecs <- c("none", "lz4", "zstd")
  want <- c(
    as.vector(outer(outer(paste("string", c("plain", "fsst")), nas, paste), codecs, paste)),
    as.vector(outer(paste("dictionary", c("plain", "fsst"), "none"), codecs, paste))
  )
  expect_identical(setdiff(want, seen), character(0))
  expect_true(any(startsWith(seen, "codes ")))
})

test_that("blob, arrow_binary, AsIs and vctrs list_of classes come back (ptype restored)", {
  vals <- list(as.raw(1:3), NULL, raw(0), as.raw(c(0, 255)))
  ab <- structure(vals, ptype = raw(0), class = c("arrow_binary", "vctrs_list_of", "vctrs_vctr", "list"))
  lo <- structure(vals, ptype = raw(0), class = c("vctrs_list_of", "vctrs_vctr", "list"))
  x <- new_df(id = 1:4, ab = ab, lo = lo, as = I(vals))
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p)
  y <- read_fstx(p)
  expect_identical(y, x)
  expect_identical(attr(y$ab, "ptype"), raw(0))
  expect_identical(class(y$as), "AsIs")
  expect_identical(fstx_meta(p)$columns$class,
                   c("integer", "arrow_binary/vctrs_list_of/vctrs_vctr/list", "vctrs_list_of/vctrs_vctr/list", "AsIs"))
  # the attribute blobs are FXA1 (no R serialization needed): attributes = FALSE keeps them
  expect_identical(read_fstx(p, attributes = FALSE), x)
})

test_that("blob objects round-trip without blob being a dependency", {
  skip_if_not_installed("blob")
  x <- data.frame(id = 1:3)
  x$b <- blob::as_blob(list(as.raw(c(0, 1)), NULL, raw(0)))
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p)
  y <- read_fstx(p)
  expect_identical(y, x)
  expect_true(blob::is_blob(y$b))
  expect_identical(fstx_meta(p)$columns$type, c("integer", "binary"))
})

test_that("other list columns are refused, with no file left", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- new_df(a = 1:3, l = list(as.raw(1), 2L, NULL))
  expect_error(write_fstx(x, p), "column 'l' has type 'list' with an element of type 'integer' \\(row 2\\)")
  x$l <- I(list(as.raw(1), NULL, "a"))
  expect_error(write_fstx(x, p), "column 'l' has type 'list' with an element of type 'character' \\(row 3\\)")
  x$l <- structure(list(as.raw(1), NULL, raw(0)), class = "foo")
  expect_error(write_fstx(x, p), "column 'l' has type 'list' \\(class 'foo'\\), which fstx cannot store yet")
  x$l <- structure(list(1L, NULL, 2L), ptype = integer(0), class = c("vctrs_list_of", "vctrs_vctr", "list"))
  expect_error(write_fstx(x, p), "column 'l' has type 'list' \\(class 'vctrs_list_of/vctrs_vctr/list'\\)")
  x$l <- NULL
  x$d <- new_df(u = 1:3)
  expect_error(write_fstx(x, p), "column 'd' has type 'list' \\(class 'data.frame'\\)")
  expect_false(file.exists(p))
})

test_that("lazy, deferred strings and as_factor leave binary columns as eager lists", {
  x <- bin_df()
  x$s <- sprintf("s%03d", x$id %% 7)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p)
  for (args in list(list(lazy = TRUE), list(strings = "deferred"), list(strings = "eager"), list(as_factor = TRUE))) {
    y <- do.call(read_fstx, c(list(p), args))
    lab <- paste(names(args), unlist(args))
    expect_identical(y$lo, x$lo, label = lab)
    expect_identical(y$hi, x$hi, label = lab)
  }
  expect_error(read_fstx(p, as_factor = "lo"), "names column 'lo', which is not a character column")
  invisible(gc())
})

test_that("binary appends and compaction keep the column", {
  x <- bin_df(3000)
  a <- df_take(x, 1:1000)
  y <- df_rbind(x, a)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p, paste0(p, ".fstx-compact"))))
  write_fstx(x, p)
  write_fstx(a, p, append = TRUE)
  expect_identical(read_fstx(p), y)
  expect_identical(fstx_meta(p)$row_groups, 2)
  fstx_compact(p)
  expect_identical(read_fstx(p), y)
  fstx_compact(p, merge = TRUE)
  expect_identical(fstx_meta(p)$row_groups, 1)
  expect_identical(fstx_meta(p)$columns$type, c("integer", "binary", "binary"))
  expect_identical(read_fstx(p), y)
  b <- x
  b$lo <- as.character(seq_len(3000))
  expect_error(write_fstx(b, p, append = TRUE), "column 'lo' is list in the file but character in the new rows")
})

test_that("int64 and binary columns together, with every other type", {
  n <- 4000
  x <- make_df(n)
  x$i64 <- i64(sample(-5:5, n, TRUE))
  x$bin <- bin_low(n)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p)
  b <- readBin(p, "raw", file.size(p))
  expect_true(length(grepRaw("int64", b, fixed = TRUE)) > 0 && length(grepRaw("binary", b, fixed = TRUE)) > 0)
  expect_same(read_fstx(p), x)
  expect_same(read_fstx(p, lazy = TRUE), x)
  r <- c(n, 1, 77, 77, 2048)
  expect_same(read_fstx(p, rows = r), df_take(x, r))
  expect_same(read_fstx(p, columns = c("bin", "i64", "chr")), x[c("bin", "i64", "chr")])
  invisible(gc())
})

# ---------------------------------------------------------------- filters
test_that("binary filters: is.na() and !is.na() only", {
  x <- bin_df()
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p)
  msg <- "'where' on column 'lo': binary columns support is.na\\(\\) and !is.na\\(\\) only"
  expect_error(fstx_which(p, lo == "abc"), msg)
  expect_error(fstx_which(p, lo == as.raw(1)), msg)
  expect_error(fstx_which(p, lo %in% list(raw(0))), msg)
  expect_error(fstx_which(p, lo), msg)
  nul <- vapply(x$lo, is.null, NA)
  expect_identical(fstx_which(p, is.na(lo)), as.numeric(which(nul)))
  expect_identical(fstx_which(p, !is.na(lo) & id < 100), as.numeric(which(!nul & x$id < 100)))
  y <- read_fstx(p, where = is.na(hi))
  expect_true(all(vapply(y$hi, is.null, NA)))
  expect_identical(nrow(y), sum(vapply(x$hi, is.null, NA)))
})

test_that("the Arrow stream passes binary columns as Arrow binary", {
  skip_until_drop2()
  skip_if_not_installed("nanoarrow")
  x <- new_df(id = 1:4, b = list(as.raw(c(0, 1)), NULL, raw(0), charToRaw("abc")))
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(x, p, .encoding = "plain")
  expect_identical(fstx_arrow_stream(p)$get_schema()$children$b$format, "z")
  df <- nanoarrow::convert_array_stream(fstx_arrow_stream(p))
  expect_identical(lapply(unclass(df$b), function(e) if (is.null(e)) NULL else as.raw(e)), unclass(x$b))
})
