# M6s: string columns stay correct under garbage-collector pressure, after errors, and when the file changes.

col_state <- function(x) fstx:::fstx_col_state_impl(x)

gc_df <- function(n, seed = 11) {
  set.seed(seed)
  data.frame(i = seq_len(n), s = sprintf("s%06d", sample.int(n)), d = sample(c("a", "bb", NA), n, TRUE),
             stringsAsFactors = FALSE)
}

test_that("string modes are correct under gctorture", {
  skip_on_cran()
  x <- gc_df(140000)                                   # more than one page per column
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  expect_gt(fstx:::fstx_col_state_impl(read_fstx(p)$s)$pages, 1)
  on.exit(gctorture(FALSE), add = TRUE)
  for (m in c("auto", "deferred", "eager")) {
    gctorture(TRUE)
    z <- read_fstx(p, strings = m, threads = 2)
    a <- z$s[c(139000, 1, 70000)]
    b <- z$d[2]
    z$d[3] <- "new"
    r <- unserialize(serialize(read_fstx(p, strings = m, threads = 2), NULL))
    gctorture(FALSE)
    ref <- x
    ref$d[3] <- "new"
    expect_identical(a, x$s[c(139000, 1, 70000)], info = m)
    expect_identical(b, x$d[2], info = m)
    expect_identical(z, ref, info = m)
    expect_identical(r, x, info = m)
  }
})

test_that("a stress loop of reads, partial use, copies and collections stays correct", {
  skip_on_cran()
  x <- gc_df(300000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  set.seed(5)
  keep <- list()
  for (k in 1:60) {
    m <- c("auto", "deferred", "eager")[k %% 3 + 1]
    z <- read_fstx(p, strings = m, threads = 1 + k %% 4)
    idx <- sample.int(300000, 20)
    ok <- identical(z$s[idx], x$s[idx]) && identical(z$d[idx], x$d[idx])
    if (k %% 4 == 0) ok <- ok && identical(sum(nchar(z$s)), sum(nchar(x$s)))
    if (k %% 5 == 0) keep[[length(keep) + 1]] <- z            # some stay alive across later collections
    if (k %% 7 == 0) ok <- ok && identical(unserialize(serialize(z, NULL)), x)
    if (k %% 3 == 0) invisible(gc(full = k %% 2 == 0))
    junk <- lapply(1:50, function(j) paste0("j", sample.int(1e5, 2000)))   # churn R's string cache
    expect_true(ok, info = paste(k, m))
    rm(z, junk)
  }
  invisible(gc())
  for (z in keep) expect_identical(z, x)
})

test_that("auto and deferred columns do not depend on the file after the read", {
  x <- gc_df(200000)
  y <- gc_df(200000, seed = 99)
  p <- tempfile(fileext = ".fstx")
  for (m in c("auto", "deferred")) {
    write_fstx(x, p)
    z <- read_fstx(p, strings = m)
    invisible(z$s[150000])                             # one page made, the rest not
    expect_false(col_state(z$s)$file_open)
    write_fstx(y, p)                                   # overwrite while z is alive
    expect_identical(z, x, info = m)
    z <- read_fstx(p, strings = m)
    expect_true(file.remove(p))                        # delete while z is alive
    expect_identical(z$s[1], y$s[1], info = m)
    expect_identical(unserialize(serialize(z, NULL)), y, info = m)
    expect_identical(z, y, info = m)
  }
})

test_that("lazy string columns keep the file; overwriting it is refused while they are alive", {
  x <- gc_df(150000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  z <- read_fstx(p, lazy = TRUE)
  invisible(z$s[1])
  expect_error(write_fstx(gc_df(10), p), "memory-mapped by a lazy read_fstx\\(\\) result")
  expect_identical(z$s, x$s)
  rm(z)
  invisible(gc())
  expect_silent(write_fstx(gc_df(10), p))
})

test_that("an error inside a string column leaves garbage collection switched on", {
  n <- 150000
  x <- gc_df(n)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p, profile = "fast")
  pg <- fstx_pages(p)
  target <- pg[pg$name == "s" & !pg$dictionary, ][2, ]   # second page of column s
  flip_byte(p, target$offset + 40)
  z <- read_fstx(p, lazy = TRUE)
  expect_identical(z$s[1:10], x$s[1:10])
  expect_error(z$s[target$first_row], "checksum")       # Elt
  expect_error(nchar(z$s), "checksum")                  # Dataptr
  expect_error(read_fstx(p), "checksum")                # auto: pages are checked at read time
  expect_error(read_fstx(p, strings = "eager"), "checksum")
  ran <- FALSE
  local({
    e <- new.env()
    reg.finalizer(e, function(e) ran <<- TRUE)
  })
  for (k in 1:300) junk <- numeric(1e6)                # 2.4 GB of churn: collections must run
  expect_true(ran)
  rm(z)
  invisible(gc())
})

test_that("an RDS of every string mode written now reads back in a fresh R session", {
  skip_on_cran()
  x <- gc_df(100000)
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  fw <- function(f) normalizePath(f, winslash = "/", mustWork = FALSE)
  ref <- tempfile(fileext = ".rds")
  saveRDS(x, ref)
  qs <- character(0)
  for (m in c("auto", "deferred", "eager")) for (touch in c(FALSE, TRUE)) {
    z <- read_fstx(p, strings = m)
    if (touch) invisible(z$s[90000])
    q <- tempfile(fileext = ".rds")
    saveRDS(z, q)
    qs <- c(qs, q)
  }
  file.remove(p)                                       # the RDS files must not need the fstx file
  lib <- dirname(find.package("fstx"))
  script <- tempfile(fileext = ".R")
  writeLines(c(sprintf(".libPaths(c('%s', .libPaths()))", fw(lib)),
               sprintf("r <- readRDS('%s')", fw(ref)),
               sprintf("q <- c(%s)", paste0("'", fw(qs), "'", collapse = ", ")),
               "cat(vapply(q, function(f) identical(readRDS(f), r), NA), '\n')"), script)
  out <- system2(file.path(R.home("bin"), "Rscript"), shQuote(fw(script)), stdout = TRUE, stderr = TRUE)
  expect_identical(tail(out, 1), paste(c(rep("TRUE", 6), ""), collapse = " "))
})
