# fstx format 1.2 golden files (design 1.2 section 8): files written by fstx 1.2, and one by fstx 1.0.2 with an
# integer64 column in the 1.0 layout, that every later fstx must read the same way (fixtures/golden-1.2, made by
# make_golden_v10.R and make_golden.R there). Skipped until those files are generated. Synthetic data only; needs
# neither bit64 nor blob.

g12dir <- test_path("fixtures", "golden-1.2")
source(file.path(g12dir, "canon.R"), local = TRUE)
have_g12 <- file.exists(file.path(g12dir, "files.csv"))
skip_g12 <- function() skip_if_not(have_g12, "golden-1.2 files not generated yet (fixtures/golden-1.2/make_golden.R)")

test_that("golden-1.2 files are unchanged and read as recorded", {
  skip_g12()
  fl <- read.csv(file.path(g12dir, "files.csv"), stringsAsFactors = FALSE, fileEncoding = "UTF-8")
  ex <- read.csv(file.path(g12dir, "expected.csv"), stringsAsFactors = FALSE, fileEncoding = "UTF-8")
  cols <- c("index", "column", "class", "rows", "nas", "data_md5", "attr_md5")
  expect_lt(sum(fl$bytes), 400 * 1024)
  expect_setequal(fl$file, list.files(g12dir, "\\.fstx$"))
  for (i in seq_len(nrow(fl))) {
    f <- file.path(g12dir, fl$file[i])
    lab <- fl$file[i]
    expect_identical(unname(tools::md5sum(f)), fl$md5[i], label = lab)
    expect_true(length(grepRaw(fl$writer[i], readBin(f, "raw", fl$bytes[i]), fixed = TRUE)) > 0, label = lab)
    if (fl$expect[i] != "read") {
      e1 <- tryCatch({ fstx_meta(f); "no error" }, error = conditionMessage)
      e2 <- tryCatch({ suppressMessages(read_fstx(f)); "no error" }, error = conditionMessage)
      expect_match(e1, fl$message[i], label = lab)
      if (fl$expect[i] == "corrupt") {
        expect_match(e2, fl$message[i], label = lab)
        expect_no_match(e1, "Update fstx", label = lab)
        expect_no_match(e2, "Update fstx", label = lab)
      } else {
        expect_match(e2, "Update fstx", label = lab)
      }
      next
    }
    m <- fstx_meta(f)
    expect_equal(c(m$format_version, m$nrow, m$ncol), c(fl$format_version[i], fl$nrow[i], fl$ncol[i]), label = lab)
    expect_identical(m$profile, fl$profile[i], label = lab)
    expect_identical(paste(m$columns$type, collapse = ";"), fl$types[i], label = lab)
    e <- ex[ex$file == fl$file[i], cols]
    rownames(e) <- NULL
    for (lazy in c(FALSE, TRUE)) {
      x <- suppressMessages(read_fstx(f, lazy = lazy))
      expect_identical(golden_table_md5(x), fl$table_md5[i], label = paste(lab, "table", lazy))
      d <- golden_describe(x)
      if (!nrow(e)) {
        expect_identical(nrow(d), 0L, label = lab)
        next
      }
      expect_identical(d$column, enc2utf8(e$column), label = paste(lab, "names", lazy))
      for (k in setdiff(cols, "column")) expect_equal(d[[k]], e[[k]], label = paste(lab, k, lazy))
      rm(x)
    }
  }
  invisible(gc())
})

test_that("golden-1.2 int64 columns read as doubles with int64 = \"double\"", {
  skip_g12()
  f <- file.path(g12dir, "int64_extremes.fstx")
  x <- suppressWarnings(read_fstx(f, int64 = "double"))
  expect_true(is.double(x$ext) && is.null(attr(x$ext, "class")))
  expect_identical(x$ext[3:9], c(2^53 + 1, -(2^53 + 1), 2^53, 0, -1, NA, 2^62))   # 2^53 + 1 rounds to 2^53
  expect_true(all(is.na(x$all_na)))
  # the 1.0 layout is not converted: the column stays as stored
  y <- suppressMessages(read_fstx(file.path(g12dir, "v10_integer64.fstx"), int64 = "double"))
  expect_s3_class(y$big, "integer64")
})

test_that("the golden-1.2 mix files hold all 150 new mixes (444 with golden-1.0)", {
  skip_g12()
  seen <- unique(unlist(lapply(list.files(g12dir, "^mix_.*\\.fstx$", full.names = TRUE), function(p) {
    pg <- fstx_pages(p)
    bin <- fstx_meta(p)$columns$type[pg$column] == "binary"
    paste0(ifelse(bin, "binary ", ""), paste(pg$kind, pg$encoding, pg$na, pg$codec))
  })))
  nas <- c("none", "bitmap", "runs")
  codecs <- c("none", "lz4", "zstd")
  ie <- c("plain", "const", "for", "delta", "rle", "dict", "split")
  grid <- function(kind, encs, na = nas) as.vector(outer(outer(paste(kind, encs), na, paste), codecs, paste))
  want <- c(grid("int64", ie), grid("binary codes", ie), grid("binary string", c("plain", "fsst")),
            grid("binary dictionary", c("plain", "fsst"), "none"))
  expect_identical(length(want), 150L)
  expect_identical(setdiff(want, seen), character(0))
})
