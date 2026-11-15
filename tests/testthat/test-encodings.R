# M2: page encodings, the chooser, string dictionaries, FSST and the profiles. Synthetic data only.

encs <- c("plain", "const", "for", "delta", "rle", "dict", "split", "alp", "alprd", "fsst")
profiles <- c("balanced", "fast", "small")

# Edge vectors of length n for every column type the encodings see.
edge_df <- function(n, seed = 3) {
  set.seed(seed)
  imax <- .Machine$integer.max
  ints <- c(imax, -imax, 0L, 1L, -1L, NA)
  dbls <- c(NA, NaN, Inf, -Inf, 0, -0, 5e-324, -5e-324, .Machine$double.xmax, -.Machine$double.xmax,
            0.1, 1e-300, 123.45, -9.99, 2^53, 2^53 + 2)
  strs <- c("", NA, "a", "café", "\U0001F600", strrep("z", 300), "same")
  data.frame(
    i_rand  = sample(ints, n, TRUE),
    i_seq   = seq_len(n) * 7L - 3L,
    i_const = rep(42L, n),
    i_na    = rep(NA_integer_, n),
    i_runs  = rep_len(rep(c(5L, NA, -2L), each = 300), n),
    i_ext   = rep_len(c(imax, -imax), n),
    i_small = sample(0:3, n, TRUE),
    d_rand  = sample(dbls, n, TRUE),
    d_dec   = round(runif(n, -1000, 1000), 2),
    d_real  = rnorm(n),
    d_int   = as.double(sample.int(1e6, n, TRUE)),
    d_sort  = cumsum(runif(n)),
    d_const = rep(-0, n),
    d_na    = rep(NA_real_, n),
    d_nan   = rep(c(NaN, 1.5), length.out = n),
    l_rand  = sample(c(TRUE, FALSE, NA), n, TRUE),
    l_true  = rep(TRUE, n),
    s_rand  = sample(strs, n, TRUE),
    s_uniq  = if (n) sprintf("id-%08d-%s", sample.int(1e8, n), sample(c("x", "é"), n, TRUE)) else character(0),
    s_na    = rep(NA_character_, n),
    s_empty = rep("", n),
    f_fct   = factor(sample(c("lo", "hi", NA), n, TRUE), levels = c("lo", "hi", "unused")),
    stringsAsFactors = FALSE
  )
}

test_that("every forced encoding round-trips edge data at awkward sizes", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  for (n in c(0L, 1L, 2L, 1023L, 1024L, 1025L, 4097L, 70001L)) {
    x <- edge_df(n)
    for (e in c(list(NULL), as.list(encs))) {
      write_fstx(x, p, .encoding = e, threads = 2)
      y <- read_fstx(p)
      expect_identical(y, x, label = sprintf("n = %d, encoding %s", n, if (is.null(e)) "auto" else e))
    }
  }
})

test_that("forced encodings are really used where they apply", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- edge_df(5000)
  for (e in c("plain", "for", "delta", "rle", "dict", "split")) {
    write_fstx(x, p, .encoding = e)
    pg <- fstx_pages(p)
    # a forced dictionary falls back to FOR when it would hold more than half the page's values
    cols <- if (e == "dict") c("i_rand", "i_runs", "i_small") else c("i_rand", "i_seq", "i_runs", "i_small")
    expect_true(all(pg$encoding[pg$name %in% cols] == e), label = e)
  }
  for (e in c("alp", "alprd", "split", "dict", "plain")) {
    write_fstx(x, p, .encoding = e)
    pg <- fstx_pages(p)
    cols <- if (e == "dict") "d_rand" else c("d_dec", "d_real", "d_rand")
    expect_true(all(pg$encoding[pg$name %in% cols] == e), label = e)
  }
  write_fstx(x, p, .encoding = "fsst")
  pg <- fstx_pages(p)
  expect_true(all(pg$encoding[pg$name == "s_uniq"] == "fsst"))
  write_fstx(x, p, .encoding = "dict")
  pg <- fstx_pages(p)
  expect_true(any(pg$dictionary & pg$name == "s_uniq"))
  expect_true(all(pg$kind[pg$name == "s_uniq" & !pg$dictionary] == "codes"))
})

test_that("the chooser picks sensible encodings", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  n <- 200000L
  set.seed(5)
  x <- data.frame(
    id    = seq_len(n),
    small = sample.int(100L, n, TRUE),
    k     = rep(7L, n),
    price = round(runif(n, 50, 5000), 2),
    real  = rnorm(n),
    city  = sample(sprintf("city_%02d", 1:20), n, TRUE),
    uniq  = sprintf("https://example.com/item/%d/%s", sample.int(1e7, n), sample(c("red", "blue"), n, TRUE)),
    stringsAsFactors = FALSE
  )
  write_fstx(x, p, profile = "fast")
  pg <- fstx_pages(p)
  enc_of <- function(nm) unique(pg$encoding[pg$name == nm & !pg$dictionary])
  expect_identical(enc_of("id"), "delta")
  expect_true(all(enc_of("small") %in% c("for", "dict")))
  expect_identical(enc_of("k"), "const")
  expect_identical(enc_of("price"), "alp")
  expect_true(any(pg$dictionary & pg$name == "city"))
  expect_identical(enc_of("uniq"), "fsst")
  expect_identical(read_fstx(p), x)
  # the encoded file is far smaller than the plain one
  write_fstx(x, p, profile = "fast", .encoding = "plain")
  plain <- file.size(p)
  write_fstx(x, p, profile = "fast")
  expect_lt(file.size(p), plain * 0.6)
})

test_that("profiles round-trip, record themselves and order by size", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- make_df(50000)
  sz <- c()
  for (pr in profiles) {
    write_fstx(x, p, profile = pr)
    expect_identical(read_fstx(p), x, label = pr)
    m <- fstx_meta(p)
    expect_identical(m$profile, pr)
    expect_identical(m$format_version, 3)
    sz[pr] <- file.size(p)
    if (pr == "fast") expect_true(all(fstx_pages(p)$codec == "none"))
  }
  expect_lte(sz[["small"]], sz[["balanced"]])
  expect_lte(sz[["balanced"]], sz[["fast"]])
  write_fstx(x, p, profile = "fast", compress = "lz4")
  expect_identical(read_fstx(p), x)
  expect_error(write_fstx(x, p, profile = "tiny"))
  expect_error(write_fstx(x, p, .encoding = "zip"), "encoding")
})

test_that("string dictionaries work across row groups, row ranges and repeated columns", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  n <- 1048576L + 70000L   # two row groups
  set.seed(9)
  x <- data.frame(
    a = sample(c("north", "south", "east", "west", NA), n, TRUE),
    b = c(sample(sprintf("u%07d", 1:2e6), 1048576L), sample(c("x", "y"), 70000L, TRUE)),  # plain, then dictionary
    v = seq_len(n),
    stringsAsFactors = FALSE
  )
  write_fstx(x, p)
  pg <- fstx_pages(p)
  expect_identical(sum(pg$dictionary & pg$name == "a"), 2L)
  expect_identical(sum(pg$dictionary & pg$name == "b"), 1L)
  expect_identical(read_fstx(p), x)
  for (r in list(c(1, 1), c(1048570, 1048590), c(1048577, 1048577), c(500000, n), c(n, n))) {
    expect_identical(read_fstx(p, from = r[1], to = r[2]), x[r[1]:r[2], , drop = FALSE] |> `rownames<-`(NULL),
                     label = paste(r, collapse = "-"))
  }
  y <- read_fstx(p, columns = c("a", "b", "a"), from = 1048000, to = 1049000)
  expect_identical(y[[1]], x$a[1048000:1049000])
  expect_identical(y[[3]], x$a[1048000:1049000])
  expect_identical(y[[2]], x$b[1048000:1049000])
})

test_that("factor codes go through the integer chain and keep their checks", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  n <- 100000L
  x <- data.frame(f = factor(sample(c(letters, NA), n, TRUE), levels = letters),
                  o = factor(rep(c("lo", "mid", "hi"), length.out = n), levels = c("lo", "mid", "hi"), ordered = TRUE))
  for (pr in profiles) {
    write_fstx(x, p, profile = pr)
    expect_identical(read_fstx(p), x)
  }
  pg <- fstx_pages(p)
  expect_true(all(pg$kind[pg$name == "f"] == "int"))
})

test_that("files are byte-identical at 1 and many threads for every profile", {
  x <- make_df(300000)
  x$u <- sprintf("key-%d", sample.int(1e7, nrow(x)))
  p1 <- tempfile(fileext = ".fstx")
  pn <- tempfile(fileext = ".fstx")
  on.exit(unlink(c(p1, pn)))
  for (pr in profiles) {
    write_fstx(x, p1, profile = pr, threads = 1)
    write_fstx(x, pn, profile = pr, threads = test_threads(max(2L, parallel::detectCores())))
    expect_identical(unname(tools::md5sum(p1)), unname(tools::md5sum(pn)), label = pr)
  }
})

test_that("a file written by fstx 0.1 (format 1) gives a clear error", {
  m1 <- test_path("fixtures", "m1_format1.fstx")
  skip_if_not(file.exists(m1))
  expect_error(read_fstx(m1), "older FST-X version")
  expect_error(fstx_meta(m1), "older FST-X version")
})

# ---------------------------------------------------------------- decoder fuzz
fuzz_file <- function(x, p, ...) {
  write_fstx(x, p, ...)
  readBin(p, "raw", file.size(p))
}

test_that("damaged encoded pages give an error or data, never a crash (verify = FALSE)", {
  set.seed(11)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- edge_df(6000)
  for (cfg in list(list("fast"), list("balanced"), list("fast", "dict"), list("fast", "fsst"),
                   list("fast", "rle"), list("fast", "alprd"), list("fast", "delta"))) {
    orig <- fuzz_file(x, p, profile = cfg[[1]], .encoding = if (length(cfg) > 1) cfg[[2]])
    pg <- fstx_pages(p)
    outcomes <- character(0)
    for (k in 1:120) {
      b <- orig
      r <- pg[sample.int(nrow(pg), 1), ]
      nflip <- sample(1:3, 1)
      for (j in seq_len(nflip)) {
        off <- r$offset + sample.int(r$comp_bytes, 1) - 1
        b[off + 1] <- as.raw(sample.int(256, 1) - 1)
      }
      writeBin(b, p)
      outcomes <- c(outcomes, read_outcome(p, verify = FALSE))
      outcomes <- c(outcomes, read_outcome(p, verify = FALSE, from = r$first_row, to = r$first_row))
    }
    expect_true(all(outcomes %in% c("data", "error")), label = paste(unlist(cfg), collapse = "/"))
    # with verify = TRUE every one of these is caught as a checksum error
    writeBin(orig, p)
  }
})

test_that("damaged page bodies with fixed checksums never crash the decoders", {
  # Rewrite one page body with garbage of the same length, then patch nothing else: verify = FALSE skips the
  # checksum, so the decoders see arbitrary bytes behind a valid directory.
  set.seed(12)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- edge_df(3000)
  orig <- fuzz_file(x, p, profile = "fast")
  pg <- fstx_pages(p)
  outcomes <- character(0)
  for (i in seq_len(nrow(pg))) {
    for (mode in 1:3) {
      b <- orig
      idx <- pg$offset[i] + seq_len(pg$comp_bytes[i])
      if (mode == 1) b[idx] <- as.raw(sample.int(256, length(idx), TRUE) - 1)          # all random
      if (mode == 2) b[idx[-(1:8)]] <- as.raw(sample.int(256, length(idx) - 8, TRUE) - 1)  # keep page header
      if (mode == 3) b[idx[9:min(length(idx), 24)]] <- as.raw(0xff)                    # huge block lengths
      writeBin(b, p)
      outcomes <- c(outcomes, read_outcome(p, verify = FALSE))
    }
  }
  expect_true(all(outcomes %in% c("data", "error")))
  expect_true(any(outcomes == "error"))
})

test_that("truncated M2 files never crash", {
  set.seed(13)
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  for (pr in profiles) {
    orig <- fuzz_file(edge_df(5000), p, profile = pr)
    for (k in 1:25) {
      cut <- sample.int(length(orig) - 1, 1)
      writeBin(orig[seq_len(cut)], p)
      expect_identical(read_outcome(p), "error")
    }
  }
})
