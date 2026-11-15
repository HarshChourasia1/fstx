# M4: SIMD kernels (Highway) give the same bytes and values as the plain loops on every target this CPU runs.
# Synthetic data only.

simd_targets <- function() c("scalar", tolower(fstx_simd()$cpu))
# The SIMD setting the run started with (FSTX_SIMD may force one); every test puts it back.
simd_now <- function() { s <- fstx_simd(); if (s$simd) tolower(s$target) else "scalar" }

m4_df <- function(n, seed = 4) {
  set.seed(seed)
  data.frame(
    i_seq  = seq_len(n) * 7L,                                        # delta
    i_for  = sample(c(1000:1300, NA), n, TRUE),                      # FOR + NA bitmap
    i_wide = as.integer(sample(c(-2e9, 2e9, 0, NA), n, TRUE) + sample.int(1e5, n, TRUE)),
    i_dict = sample(c(-7L, 123456789L, 42L), n, TRUE),               # dictionary
    d_dec  = round(runif(n, -900, 900), 2),                          # ALP
    d_exc  = ifelse(runif(n) < 0.02, c(NaN, Inf, -0, 1e300)[1 + (seq_len(n) %% 4)], round(rnorm(n), 3)),
    d_rnd  = rnorm(n),                                               # byte split
    d_na   = ifelse(runif(n) < 0.3, NA, round(runif(n), 1)),
    d_big  = sample(c(1e15, 2^53, -2^52, 3.5), n, TRUE),
    lgl    = sample(c(TRUE, FALSE, NA), n, TRUE),
    lgl2   = rep(c(TRUE, FALSE), length.out = n),
    chr    = sample(c("a", "bb", NA, "café"), n, TRUE),
    fct    = factor(sample(c("x", "y", "z", NA), n, TRUE)),
    date   = as.Date("2020-01-01") + sample(c(0:4000, NA), n, TRUE),
    ct     = as.POSIXct("2024-01-01", tz = "UTC") + sample.int(1e7, n, TRUE) * 1.5,
    stringsAsFactors = FALSE
  )
}

test_that("fstx_simd reports and switches targets", {
  s0 <- simd_now()
  on.exit(fstx_simd(s0))
  s <- fstx_simd()
  expect_named(s, c("simd", "target", "dispatch", "cpu", "compiled"))
  if (R.version$arch %in% c("x86_64", "amd64")) expect_true(all(c("SSE2") %in% s$compiled))   # x86-64 only
  expect_true(all(s$cpu %in% s$compiled))
  for (t in simd_targets()) {
    r <- fstx_simd(t)
    if (t == "scalar") {
      expect_false(r$simd)
    } else {
      expect_true(r$simd)
      expect_identical(tolower(r$target), t)
      expect_identical(r$dispatch, r$target)   # the dispatcher really calls this target's code
    }
  }
  expect_error(fstx_simd("avx3"), "not compiled")   # AVX-512 is never compiled in; not an ARM target either
  bad <- setdiff(tolower(s$compiled), tolower(s$cpu))
  for (t in bad) expect_error(fstx_simd(t), "cannot run")
  expect_identical(fstx_simd("auto")$target, s$cpu[1])   # auto = the best target the CPU runs
})

test_that("every SIMD kernel matches its plain loop bit for bit on each target", {
  s0 <- simd_now()
  on.exit(fstx_simd(s0))
  for (t in tolower(fstx_simd()$cpu)) {
    fstx_simd(t)
    for (seed in 1:2) {
      r <- as.data.frame(fstx:::fstx_kernel_selftest_impl(seed, 6L))
      expect_gt(sum(r$cases), 1000)
      expect_identical(sum(r$mismatches), 0L, info = paste(t, seed))
    }
  }
})

test_that("files are byte-identical and read back the same on every target", {
  s0 <- simd_now()
  on.exit(fstx_simd(s0))
  x <- m4_df(70001)
  for (prof in c("fast", "balanced", "small")) {
    md5 <- character()
    for (t in simd_targets()) {
      fstx_simd(t)
      p <- tempfile(fileext = ".fstx")
      write_fstx(x, p, profile = prof)
      md5[t] <- unname(tools::md5sum(p))
      for (tr in simd_targets()) {                 # write with one target, read with each
        fstx_simd(tr)
        expect_identical(read_fstx(p), x, info = paste(prof, t, tr))
      }
      fstx_simd(t)
      y <- x[33333:41234, ]
      rownames(y) <- NULL
      expect_identical(read_fstx(p, from = 33333, to = 41234), y)
      z <- read_fstx(p, lazy = TRUE)
      expect_identical(z$d_dec[], x$d_dec)
      expect_identical(z$i_for[], x$i_for)
      unlink(p)
    }
    expect_length(unique(md5), 1)
  }
})

test_that("odd page tails and widths decode the same with and without SIMD", {
  s0 <- simd_now()
  on.exit(fstx_simd(s0))
  for (n in c(1, 7, 31, 63, 64, 65, 255, 257, 1023, 4097)) {
    set.seed(n)
    x <- data.frame(a = ifelse(runif(n) < 0.1, NA, as.integer(floor(runif(n) * 2^min(30, n)))), b = round(runif(n) * 1000, 2),
                    c = sample(c(TRUE, NA), n, TRUE), d = as.numeric(sample(c(2^40, NA, -1), n, TRUE)))
    md5 <- character()
    for (t in simd_targets()) {
      fstx_simd(t)
      p <- tempfile(fileext = ".fstx")
      write_fstx(x, p, profile = "fast")
      md5[t] <- unname(tools::md5sum(p))
      expect_identical(read_fstx(p), x, info = paste(n, t))
      unlink(p)
    }
    expect_length(unique(md5), 1)
  }
})
