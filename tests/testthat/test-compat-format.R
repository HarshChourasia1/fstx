# 1.0 format lock (WS1): format 3, feature bits, reserved numbers, limits, and every legal mix of page kind,
# encoding, missing-value mode and codec. Synthetic data only.

forge <- function(src, dst, ...) fstx:::.fstx_forge(src, dst, ...)

small_df <- function(n = 3000) {
  set.seed(11)
  data.frame(id = seq_len(n), v = round(runif(n), 3), s = sample(c("a", "b", "café"), n, TRUE),
             stringsAsFactors = FALSE)
}

test_that("new files are format 3 and record the real package version", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  write_fstx(small_df(), p)
  expect_identical(fstx_meta(p)$format_version, 3)
  b <- readBin(p, "raw", file.size(p))
  expect_true(length(grepRaw(paste0("fstx ", utils::packageVersion("fstx")), b, fixed = TRUE)) > 0)
  expect_false(length(grepRaw("fstx unknown", b, fixed = TRUE)) > 0)
})

test_that("fstx_features() reports what this build reads and writes", {
  f <- fstx_features()
  expect_identical(f$version, as.character(utils::packageVersion("fstx")))
  expect_identical(f$format_read, 3L)
  expect_identical(f$format_write, 3L)
  expect_identical(f$features$bit, c(16:19, 63L))
  expect_identical(f$features$name, c("int64", "binary", "zstd_dict", "pcodec", "experimental"))
  expect_false(any(f$features$read))
  expect_identical(f$codecs$read, c(TRUE, TRUE, TRUE, FALSE, FALSE))
  expect_identical(f$codecs$write, c(TRUE, TRUE, TRUE, FALSE, FALSE))
  expect_identical(f$types$read, c(TRUE, TRUE, TRUE, TRUE, FALSE, FALSE))
})

test_that("feature bits: unknown required bits are refused by name, optional ones ignored", {
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p, q)))
  x <- small_df()
  write_fstx(x, p)
  # a header bit (0-15) is checked before the footer is read
  forge(p, q, required = 3L, hdr_flags = 8L)
  expect_error(read_fstx(q), "feature bit 3, which this fstx .* cannot read. Update fstx")
  expect_error(fstx_meta(q), "Update fstx")
  # a footer bit is named from the file's own list
  forge(p, q, required = 40L, names = "future_thing")
  expect_error(read_fstx(q), "file feature 'future_thing' \\(bit 40\\).*Update fstx")
  forge(p, q, required = c(20L, 41L), optional = 50L, names = c("first", "second", "opt"))
  expect_error(read_fstx(q), "'first' \\(bit 20\\)")
  # reserved bits are named even without a list
  for (b in 16:19) {
    forge(p, q, required = b)
    nm <- c("int64", "binary", "zstd_dict", "pcodec")[b - 15L]
    expect_error(read_fstx(q), sprintf("'%s' \\(bit %d\\).*Update fstx", nm, b), label = nm)
  }
  forge(p, q, required = 63L)
  expect_error(read_fstx(q), "'experimental' \\(bit 63\\).*Update fstx")
  # unknown optional bits do not stop a read
  forge(p, q, optional = c(5L, 50L), names = c("nice", "nicer"))
  expect_identical(read_fstx(q), x)
  # header and footer must agree on bits 0-15
  forge(p, q, required = 2L, hdr_flags = 0L)
  expect_error(read_fstx(q), "header and footer feature bits differ")
})

test_that("unknown optional bits: reading works, appends and page-copy compaction are refused", {
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p, q, paste0(q, ".fstx-compact"))))
  x <- small_df()
  write_fstx(x, p)
  forge(p, q, optional = 50L, names = "later_index")
  before <- readBin(q, "raw", file.size(q))
  msg <- "uses file features this fstx .* does not know. Update fstx to change this file; reading works"
  expect_error(write_fstx(x, q, append = TRUE), msg)
  expect_error(fstx_compact(q), msg)
  expect_identical(readBin(q, "raw", file.size(q)), before)
  expect_identical(read_fstx(q), x)
  # merge = TRUE reads the table and writes a fresh file (no feature bits)
  fstx_compact(q, merge = TRUE)
  expect_identical(read_fstx(q), x)
  write_fstx(x, q, append = TRUE)
  expect_identical(nrow(read_fstx(q)), 2L * nrow(x))
})

test_that("a profile from a newer fstx reads as 'unknown' and appends and compaction keep it", {
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p, q)))
  x <- small_df()
  write_fstx(x, p)
  for (pr in c(3L, 255L)) {
    forge(p, q, profile = pr)
    expect_identical(fstx_meta(q)$profile, "unknown", label = pr)
    expect_output(print(fstx_meta(q)), "profile unknown")
    expect_identical(read_fstx(q), x)
  }
  write_fstx(x, q, append = TRUE)
  expect_identical(fstx_meta(q)$profile, "unknown")
  y <- rbind(x, x)
  rownames(y) <- NULL
  expect_identical(read_fstx(q), y)
  fstx_compact(q)
  expect_identical(fstx_meta(q)$profile, "unknown")
  expect_identical(read_fstx(q), y)
  fstx_compact(q, merge = TRUE)
  expect_identical(fstx_meta(q)$profile, "balanced")
  expect_identical(read_fstx(q), y)
})

test_that("newer format versions and every reserved number get their own message", {
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p, q)))
  write_fstx(small_df(), p, compress = "none")
  cases <- list(
    list(list(version = 4L), "format version 4; this reader supports up to 3. Update fstx"),
    list(list(phys0 = 4L), "64-bit integer columns \\(column 'id', type 4\\)"),
    list(list(phys0 = 5L), "binary columns \\(column 'id', type 5\\)"),
    list(list(phys0 = 9L), "column type 9 \\(column 'id'\\)"),
    list(list(logical0 = 4L), "logical type 4 \\(column 'id'\\)"),
    list(list(codec0 = 3L), "codec 'zstd with a dictionary' \\(codec 3"),
    list(list(codec0 = 4L), "codec 'Pcodec' \\(codec 4"),
    list(list(codec0 = 77L), "uses codec 77 \\(column 'id'\\)"),
    list(list(codec0 = 200L), "experimental codec 200"),
    list(list(kind0 = 7L), "page kind 7 \\(column 'id'\\)"),
    list(list(kind0 = 15L), "page kind 15"),
    list(list(na0 = 3L), "missing-value mode 3 \\(column 'id'\\)"),
    list(list(tag0 = 11L), "encoding 11 \\(column 'id'\\)"),
    list(list(tag0 = 255L), "encoding 255")
  )
  for (k in cases) {
    do.call(forge, c(list(p, q), k[[1]]))
    lab <- paste(names(k[[1]]), unlist(k[[1]]))
    expect_error(fstx_meta(q), k[[2]], label = lab)
    expect_error(read_fstx(q), "Update fstx", label = lab)
  }
  # a forged copy with nothing changed reads the same
  forge(p, q)
  expect_identical(read_fstx(q), read_fstx(p))
})

test_that("files from fstx 0.x (format versions 1 and 2) are refused by every entry point", {
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p, q, paste0(q, ".fstx-compact"))))
  x <- small_df()
  write_fstx(x, p)
  msg <- "written by fstx 0\\.x \\(file format 2\\); not supported by fstx 1\\.0 or later"
  # a plain format 2 file, and the format 2 forms the fuzz seeds use (newer profile, feature fields, header bits)
  for (k in list(list(), list(profile = 3L), list(required = 40L), list(hdr_flags = 1L))) {
    do.call(forge, c(list(p, q, version = 2L), k))
    lab <- paste(c("version 2", names(k)), collapse = " ")
    before <- readBin(q, "raw", file.size(q))
    expect_error(fstx_meta(q), msg, label = lab)
    expect_error(fstx_pages(q), msg, label = lab)
    expect_error(read_fstx(q), msg, label = lab)
    expect_error(read_fstx(q, lazy = TRUE), msg, label = lab)
    expect_error(fstx_which(q, id > 5), msg, label = lab)
    expect_error(fstx_explain(q, id > 5), msg, label = lab)
    expect_error(write_fstx(x, q, append = TRUE), msg, label = lab)
    expect_error(fstx_compact(q), msg, label = lab)
    expect_error(fstx_compact(q, merge = TRUE), msg, label = lab)
    expect_identical(readBin(q, "raw", file.size(q)), before, label = lab)
  }
  m1 <- test_path("fixtures", "m1_format1.fstx")
  expect_error(read_fstx(m1), "written by fstx 0\\.x \\(file format 1\\); not supported by fstx 1\\.0 or later")
})

test_that("fstx_arrow_stream refuses a file from fstx 0.x", {
  skip_if_not_installed("nanoarrow")
  p <- tempfile(fileext = ".fstx")
  q <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p, q)))
  write_fstx(small_df(), p)
  forge(p, q, version = 2L)
  expect_error(fstx_arrow_stream(q), "written by fstx 0\\.x \\(file format 2\\); not supported by fstx 1\\.0 or later")
})

test_that("the writer refuses column names and time zones that are not UTF-8 text", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- data.frame(a = 1:3)
  bad <- rawToChar(as.raw(c(0x61, 0xff, 0x62)))
  Encoding(bad) <- "UTF-8"
  names(x) <- bad
  expect_error(write_fstx(x, p), "not valid UTF-8")
})

# ---------------------------------------------------------------- every legal mix
# Columns for each page kind in three missing-value shapes (none, scattered = bitmap, one long run = runs);
# the *_k columns are constant apart from missing values, so a forced "const" applies.
mix_df <- function(n = 5000) {
  set.seed(5)
  sc <- sample.int(n, n %/% 10)
  run <- 1001:2500
  na_as <- function(v, at) { v[at] <- NA; v }
  base <- list(
    i = sample(1:40, n, TRUE), ik = rep(7L, n),
    d = sample(c(0.5, 1.25, 3, 100.75, -2), n, TRUE), dk = rep(2.5, n),
    l = sample(c(TRUE, FALSE), n, TRUE), lk = rep(TRUE, n),
    s = sample(c("alpha", "beta", "café", "delta-a-bit-longer", ""), n, TRUE), sk = rep("same", n)
  )
  out <- list()
  for (nm in names(base)) {
    out[[paste0(nm, "_0")]] <- base[[nm]]
    out[[paste0(nm, "_b")]] <- na_as(base[[nm]], sc)
    out[[paste0(nm, "_r")]] <- na_as(base[[nm]], run)
  }
  out$f <- factor(sample(c("x", "y", NA), n, TRUE))
  as.data.frame(out, stringsAsFactors = FALSE)
}

test_that("every legal mix of kind, encoding, missing-value mode and codec round-trips", {
  x <- mix_df()
  files <- character(0)
  on.exit(unlink(files))
  int_encs <- c("plain", "const", "for", "delta", "rle", "dict", "split")
  seen <- character(0)
  for (codec in c("none", "lz4", "zstd")) {
    for (e in c(int_encs, "alp", "alprd", "fsst")) {
      for (fl in list(NULL, "dict_codes", "fsst_dict")) {
        enc <- c(e, fl, if (codec != "none") "keep_codec")
        lab <- paste(codec, paste(enc, collapse = "+"))
        p <- tempfile(fileext = ".fstx")  # a new file each time: a lazy result keeps its file mapped
        files <- c(files, p)
        write_fstx(x, p, compress = codec, .encoding = enc, threads = 2)
        expect_identical(read_fstx(p), x, label = lab)
        expect_identical(read_fstx(p, lazy = TRUE), x, label = paste(lab, "lazy"))
        expect_identical(read_fstx(p, strings = "deferred"), x, label = paste(lab, "deferred"))
        r <- c(1, 2, 1023, 1024, 1025, 2000, 4999, 5000)
        y <- read_fstx(p, rows = r)
        rownames(y) <- NULL
        z <- x[r, ]
        rownames(z) <- NULL
        expect_identical(y, z, label = paste(lab, "rows"))
        w <- read_fstx(p, where = i_b > 20)
        rownames(w) <- NULL
        z <- x[!is.na(x$i_b) & x$i_b > 20, ]
        rownames(z) <- NULL
        expect_identical(w, z, label = paste(lab, "where int"))
        w <- read_fstx(p, where = s_b == "café")
        rownames(w) <- NULL
        z <- x[!is.na(x$s_b) & x$s_b == "café", ]
        rownames(z) <- NULL
        expect_identical(w, z, label = paste(lab, "where string"))
        pg <- fstx_pages(p)
        seen <- union(seen, unique(paste(pg$kind, pg$encoding, pg$na, pg$codec)))
        invisible(gc())
      }
    }
  }
  codecs <- c("none", "lz4", "zstd")
  nas <- c("none", "bitmap", "runs")
  want <- c(
    as.vector(outer(outer(paste("int", int_encs), nas, paste), codecs, paste)),
    as.vector(outer(outer(paste("logical", int_encs), nas, paste), codecs, paste)),
    as.vector(outer(outer(paste("codes", int_encs), nas, paste), codecs, paste)),
    as.vector(outer(outer(paste("double", c(int_encs, "alp", "alprd")), nas, paste), codecs, paste)),
    as.vector(outer(outer(paste("string", c("plain", "fsst")), nas, paste), codecs, paste)),
    as.vector(outer(paste("dictionary", c("plain", "fsst"), "none"), codecs, paste))
  )
  missing <- setdiff(want, seen)
  expect_identical(missing, character(0))
})
