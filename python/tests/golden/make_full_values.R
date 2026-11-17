# Every value of the golden columns that the sidecars cut short (over 100 rows: 12 values + an md5).
#
#   Rscript make_full_values.R [golden dir]      (default: the folder this script is in)
#
# The write tests build a pyarrow.Table from a sidecar without the Python reader, so they need all the values.
# Reads the golden .fstx files (it does not make them again) and writes full/<name>.json: per column `values`
# in the sidecar's encoding, doubles also as `bits`. The 7 codec_ / profile_ cases hold one table, written once
# as full/cd.json; tests check each column against the md5 in its own sidecar.
args <- commandArgs(TRUE)
me <- normalizePath(sub("^--file=", "", grep("^--file=", commandArgs(FALSE), value = TRUE)[1]), winslash = "/")
gdir <- if (length(args)) normalizePath(args[1], winslash = "/") else dirname(me)
suppressPackageStartupMessages({
  library(fstx)
  library(jsonlite)
})
num <- function(s) structure(s, class = "json")
one <- function(v) {
  if (is.double(v)) {
    m <- matrix(as.character(writeBin(v, raw(), size = 8L, endian = "big")), nrow = 8L)
    list(values = lapply(v, function(e) if (is.nan(e)) "NaN" else if (is.na(e)) NULL else num(sprintf("%.17g", e))),
         bits = as.list(apply(m, 2L, paste, collapse = "")))
  } else {
    list(values = lapply(v, function(e) if (is.na(e)) NULL else e))
  }
}
dump <- function(case, name) {
  x <- read_fstx(file.path(gdir, paste0(case, ".fstx")), threads = 1L)
  stopifnot(all(vapply(x, function(v) is.null(attributes(v)), NA)))
  d <- list(from = paste0(case, ".fstx"), nrow = nrow(x),
            columns = lapply(names(x), function(n) c(list(name = n), one(x[[n]]))))
  dir.create(file.path(gdir, "full"), showWarnings = FALSE)
  write_json(d, file.path(gdir, "full", paste0(name, ".json")), auto_unbox = TRUE, null = "null", na = "null",
             digits = NA, json_verbatim = TRUE)
  cat(sprintf("full/%s.json  %d rows\n", name, nrow(x)))
}
dump("strings_many", "strings_many")
dump("codec_none", "cd")
