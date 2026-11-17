# API snapshot: fails when an exported function or one of its arguments (name, order or default) changes.
# A change here must be a decided API change (plan D10); update the snapshot in the same commit.

api_snapshot <- list(
  write_fstx = c(x = "", path = "", profile = 'c("balanced", "fast", "small")', compress = "NULL",
                 level = "NULL", threads = "fstx_hw_threads()", append = "FALSE"),
  read_fstx = c(path = "", columns = "NULL", from = "1", to = "NULL", threads = "fstx_hw_threads()",
                verify = "TRUE", lazy = "FALSE", strings = 'c("auto", "eager", "deferred")',
                as_factor = "FALSE", rows = "NULL", where = "NULL", attributes = "TRUE"),
  fstx_meta = c(path = ""),
  fstx_pages = c(path = ""),
  fstx_simd = c(target = "NULL"),
  fstx_compact = c(path = "", merge = "FALSE", threads = "fstx_hw_threads()"),
  fstx_explain = c(path = "", where = "", from = "1", to = "NULL", exact = "FALSE",
                   threads = "fstx_hw_threads()"),
  fstx_which = c(path = "", where = "", from = "1", to = "NULL", threads = "fstx_hw_threads()"),
  fstx_features = character(),
  fstx_arrow_stream = c(path = "", columns = "NULL", from = "1", to = "NULL", where = "NULL",
                        threads = "fstx_hw_threads()", batch_rows = "NULL")
)

args_of <- function(f) {
  a <- formals(f)
  vapply(a, function(d) paste(deparse(d), collapse = " "), "", USE.NAMES = TRUE)
}

test_that("the exported functions are exactly the snapshot", {
  expect_setequal(getNamespaceExports("fstx"), names(api_snapshot))
})

test_that("every exported function has the snapshot's arguments, order and defaults", {
  for (fn in names(api_snapshot)) {
    got <- args_of(getExportedValue("fstx", fn))
    if (!length(got)) got <- character()
    expect_identical(got, api_snapshot[[fn]], label = fn)
  }
})

test_that("the print method for fstx_meta is registered", {
  expect_true(is.function(getS3method("print", "fstx_meta", envir = asNamespace("fstx"))))
})

# Behaviour of the 1.0 API decisions (D10, items 1, 2, 4, 5, 6 of ws-9/api-proposal.md)
test_that("test hooks reach the writer only through .write_fstx", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- data.frame(a = 1:5000)
  expect_error(fstx::write_fstx(x, p, .encoding = "plain"), "unused argument")
  fstx:::.write_fstx(x, p, .encoding = "plain", threads = 1)
  expect_true(all(fstx_pages(p)$encoding == "plain"))
  fstx::write_fstx(x, p, threads = 1)               # hooks are cleared after each call
  expect_false(all(fstx_pages(p)$encoding == "plain"))
})

test_that('strings = "altrep" still works, with a warning naming "deferred"', {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  x <- data.frame(s = sprintf("k%05d", 1:3000), stringsAsFactors = FALSE)
  fstx::write_fstx(x, p, threads = 1)
  expect_warning(y <- read_fstx(p, strings = "altrep", threads = 1), '"deferred"')
  expect_identical(y, x)
  expect_error(read_fstx(p, strings = "nope"), "read_fstx: 'strings' should be one of")
})

test_that("TRUE/FALSE arguments refuse other values and errors name the function", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  fstx::write_fstx(data.frame(a = 1:10), p, threads = 1)
  expect_error(read_fstx(p, verify = 1), "read_fstx: 'verify' must be TRUE or FALSE")
  expect_error(read_fstx(p, verify = NA), "'verify' must be TRUE or FALSE")
  expect_error(fstx_explain(p, a > 1, exact = "yes"), "fstx_explain: 'exact' must be TRUE or FALSE")
  expect_error(fstx_compact(p, merge = 1), "fstx_compact: 'merge' must be TRUE or FALSE")
  expect_error(read_fstx(p, lazy = NA), "read_fstx: 'lazy' must be TRUE or FALSE")
  expect_error(fstx::write_fstx(data.frame(a = 1), p, append = NA), "write_fstx: 'append' must be TRUE or FALSE")
  expect_error(read_fstx(c(p, p)), "read_fstx: 'path' must be")
  expect_error(fstx_meta(NA_character_), "fstx_meta: 'path' must be")
  expect_error(fstx_pages(1), "fstx_pages: 'path' must be")
  expect_error(read_fstx(p, threads = 0), "read_fstx: 'threads' must be")
  expect_error(read_fstx(p, from = "a"), "read_fstx: 'from' must be")
  expect_error(fstx_which(p, a > 1, to = "x"), "fstx_which: 'to' must be")
  expect_error(fstx_which(p, a | a), "fstx_which: 'where' supports only &")
  expect_error(fstx::write_fstx(data.frame(a = 1), p, profile = "tiny"), "write_fstx: 'profile' should be one of")
  expect_error(fstx::write_fstx(data.frame(a = 1), p, compress = "gzip"), "write_fstx: 'compress' should be one of")
  expect_error(fstx_simd(1), "fstx_simd: 'target' must be one string")
  expect_identical(nrow(read_fstx(p, verify = FALSE, threads = 1)), 10L)
  expect_identical(read_fstx(p, strings = NULL, threads = 1), fstx::write_fstx(data.frame(a = 1:10), p, profile = NULL, threads = 1))  # NULL = default, as match.arg
  expect_identical(fstx::write_fstx(data.frame(a = 1), p, profile = "sm", threads = 1), data.frame(a = 1))  # partial match kept
})

test_that("an append returns x as given, even when it adds factor levels", {
  p <- tempfile(fileext = ".fstx")
  on.exit(unlink(p))
  fstx::write_fstx(data.frame(f = factor(c("a", "b"))), p, threads = 1)
  y <- data.frame(f = factor(c("c", "a")))
  expect_identical(fstx::write_fstx(y, p, append = TRUE, threads = 1), y)
  expect_identical(levels(read_fstx(p, threads = 1)$f), c("a", "b", "c"))
})
