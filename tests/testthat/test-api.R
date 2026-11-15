# API snapshot: fails when an exported function or one of its arguments (name, order or default) changes.
# A change here must be a decided API change (plan D10); update the snapshot in the same commit.

api_snapshot <- list(
  write_fstx = c(x = "", path = "", profile = 'c("balanced", "fast", "small")', compress = "NULL",
                 level = "NULL", threads = "fstx_hw_threads()", append = "FALSE", .encoding = "NULL",
                 .crash = "0L"),
  read_fstx = c(path = "", columns = "NULL", from = "1", to = "NULL", threads = "fstx_hw_threads()",
                verify = "TRUE", lazy = "FALSE", strings = 'c("auto", "eager", "deferred", "altrep")',
                as_factor = "FALSE", rows = "NULL", where = "NULL"),
  fstx_meta = c(path = ""),
  fstx_pages = c(path = ""),
  fstx_simd = c(target = "NULL"),
  fstx_compact = c(path = "", merge = "FALSE", threads = "fstx_hw_threads()"),
  fstx_explain = c(path = "", where = "", from = "1", to = "NULL", exact = "FALSE",
                   threads = "fstx_hw_threads()"),
  fstx_which = c(path = "", where = "", from = "1", to = "NULL", threads = "fstx_hw_threads()"),
  fstx_features = character(),
  fstx_upgrade = c(path = "", to = "NULL"),
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
