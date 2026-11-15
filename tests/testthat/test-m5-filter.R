# M5: filters with page skipping. Synthetic data only. Every result is checked against subset().

ref <- function(x, keep) {
  e <- x[which(keep), , drop = FALSE]
  rownames(e) <- NULL
  e
}
chk <- function(y, e) {
  rownames(y) <- NULL
  expect_identical(y, e)
}

test_that("where= matches subset() on every column type and operator", {
  x <- make_df(30000, seed = 21)
  x$s_sorted <- sort(x$int, na.last = TRUE)
  for (prof in c("balanced", "fast")) {
    p <- tempfile(fileext = ".fstx")
    write_fstx(x, p, profile = prof)
    v <- 500L
    chk(read_fstx(p, where = int > v), ref(x, x$int > v))
    chk(read_fstx(p, where = v < int), ref(x, v < x$int))
    chk(read_fstx(p, where = int == 17), ref(x, x$int == 17))
    chk(read_fstx(p, where = int != 17), ref(x, x$int != 17))
    chk(read_fstx(p, where = int <= 3.5), ref(x, x$int <= 3.5))
    chk(read_fstx(p, where = int %in% c(1, 5, NA)), ref(x, x$int %in% c(1, 5, NA)))
    chk(read_fstx(p, where = is.na(int)), ref(x, is.na(x$int)))
    chk(read_fstx(p, where = !is.na(int) & int < 10), ref(x, !is.na(x$int) & x$int < 10))
    chk(read_fstx(p, where = dbl >= 1.5), ref(x, x$dbl >= 1.5))
    chk(read_fstx(p, where = dbl < -2 & int > 100), ref(x, x$dbl < -2 & x$int > 100))
    chk(read_fstx(p, where = is.na(dbl)), ref(x, is.na(x$dbl)))
    chk(read_fstx(p, where = !is.na(dbl)), ref(x, !is.na(x$dbl)))
    chk(read_fstx(p, where = dbl %in% c(NaN, Inf)), ref(x, x$dbl %in% c(NaN, Inf)))
    chk(read_fstx(p, where = dbl %in% NA), ref(x, x$dbl %in% NA))
    chk(read_fstx(p, where = lgl), ref(x, x$lgl))
    chk(read_fstx(p, where = !lgl), ref(x, !x$lgl))
    chk(read_fstx(p, where = lgl == FALSE), ref(x, x$lgl == FALSE))
    chk(read_fstx(p, where = chr == "café"), ref(x, x$chr == "café"))
    chk(read_fstx(p, where = chr != "café"), ref(x, x$chr != "café"))
    chk(read_fstx(p, where = chr %in% c("中文", "", NA)), ref(x, x$chr %in% c("中文", "", NA)))
    chk(read_fstx(p, where = is.na(chr)), ref(x, is.na(x$chr)))
    chk(read_fstx(p, where = chr == "nothing"), ref(x, x$chr == "nothing"))
    chk(read_fstx(p, where = fct == "cé"), ref(x, x$fct == "cé"))
    chk(read_fstx(p, where = fct != "a"), ref(x, x$fct != "a"))
    chk(read_fstx(p, where = fct != "zz"), ref(x, x$fct != "zz"))
    chk(read_fstx(p, where = fct %in% c("a", "unused", "zz")), ref(x, x$fct %in% c("a", "unused", "zz")))
    chk(read_fstx(p, where = ord >= "mid"), ref(x, x$ord >= "mid"))
    chk(read_fstx(p, where = ord < "hi" & ord > "lo"), ref(x, x$ord < "hi" & x$ord > "lo"))
    d0 <- as.Date("2022-01-01")
    chk(read_fstx(p, where = date > d0), ref(x, x$date > d0))
    chk(read_fstx(p, where = date <= "2021-06-30"), ref(x, x$date <= as.Date("2021-06-30")))
    chk(read_fstx(p, where = idt == as.Date(18500)), ref(x, x$idt == as.Date(18500)))
    t0 <- as.POSIXct("2024-03-06 12:00:00", tz = "Asia/Kolkata")
    chk(read_fstx(p, where = ct < t0), ref(x, x$ct < t0))
    chk(read_fstx(p, where = between(ctu, as.POSIXct("2024-03-01 01:00:00", tz = "UTC"),
                                     as.POSIXct("2024-03-01 02:00:00", tz = "UTC"))),
        ref(x, x$ctu >= as.POSIXct("2024-03-01 01:00:00", tz = "UTC") & x$ctu <= as.POSIXct("2024-03-01 02:00:00", tz = "UTC")))
    chk(read_fstx(p, where = s_sorted >= 990), ref(x, x$s_sorted >= 990))
    chk(read_fstx(p, where = int == NA), ref(x, rep(FALSE, nrow(x))))
    chk(read_fstx(p, where = (int > 10) & (chr == "plain") & lgl), ref(x, x$int > 10 & x$chr == "plain" & x$lgl))
    chk(read_fstx(p, columns = c("dbl", "chr"), where = int > 900),
        ref(x[c("dbl", "chr")], x$int > 900))
    chk(read_fstx(p, where = int > 900, from = 1000, to = 20000),
        ref(x[1000:20000, ], x$int[1000:20000] > 900))
    cond <- quote(int > 900 & lgl)
    chk(read_fstx(p, where = cond), ref(x, x$int > 900 & x$lgl))
    chk(read_fstx(p, where = ~ int < 5), ref(x, x$int < 5))
    expect_identical(fstx_which(p, int > 990), as.double(which(x$int > 990)))
    unlink(p)
  }
})

test_that("where= rejects what it cannot do exactly", {
  x <- make_df(100)
  x$big <- structure(as.double(1:100), class = "myclass")
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  expect_error(read_fstx(p, where = int > 1 | lgl), "only &")
  expect_error(read_fstx(p, where = int > dbl), "two columns")
  expect_error(read_fstx(p, where = chr > "a"), "character columns")
  expect_error(read_fstx(p, where = fct > "a"), "ordered")
  expect_error(read_fstx(p, where = ord > "nope"), "not one of its levels")
  expect_error(read_fstx(p, where = big > 3), "cannot be filtered")
  expect_error(read_fstx(p, where = nosuch > 3), "column")
  expect_error(read_fstx(p, where = int > 1, rows = 1:3), "not both")
  expect_error(read_fstx(p, where = int == c(1, 2)), "single value")
  unlink(p)
})

test_that("pages are skipped on sorted columns and the counts add up", {
  n <- 2e6
  set.seed(4)
  x <- data.frame(k = seq_len(n), v = runif(n), s = sample(c("a", "b", "c"), n, TRUE),
                  t = as.POSIXct("2025-01-01", tz = "UTC") + seq_len(n), stringsAsFactors = FALSE)
  x$s[1:200000] <- "only-here"
  p <- tempfile(fileext = ".fstx")
  write_fstx(x, p)
  e <- fstx_explain(p, k > 1999000)
  expect_true(e[["pages_skipped"]] >= e[["pages_total"]] - 2)
  e <- fstx_explain(p, k > 1999000, exact = TRUE)
  expect_identical(e[["rows_matched"]], 1000)
  expect_identical(e[["pages_total"]], e[["pages_skipped"]] + e[["pages_pruned"]] + e[["pages_all"]] + e[["pages_read"]])
  e <- fstx_explain(p, k >= 1000000 & v < 0.5, exact = TRUE)
  expect_true(e[["pages_skipped"]] > 0 && e[["pages_pruned"]] > 0)
  expect_identical(e[["rows_matched"]], as.double(sum(x$k >= 1e6 & x$v < 0.5)))
  e <- fstx_explain(p, s == "only-here", exact = TRUE)   # dictionary chunks without the value are skipped
  expect_true(e[["pages_skipped"]] > 0)
  expect_identical(e[["rows_matched"]], 2e5)
  chk(read_fstx(p, where = t > as.POSIXct("2025-01-23 13:00:00", tz = "UTC")),
      ref(x, x$t > as.POSIXct("2025-01-23 13:00:00", tz = "UTC")))
  e <- fstx_explain(p, v > 2)
  expect_identical(e[["pages_skipped"]], e[["pages_total"]])
  expect_identical(nrow(read_fstx(p, where = v > 2)), 0L)
  unlink(p)
})
