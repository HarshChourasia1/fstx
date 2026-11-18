# Python -> R check of the R-match files (test_rmatch.py, python/DEV.md).
#
#   Rscript check_rmatch.R [golden dir]       (default: the folder above this script)
#
# For every <golden dir>/from_python/rmatch/<name>.fstx, test_rmatch.py wrote <name>.view.json: what R is to show
# after reading the file (rview.R's format), the R golden case it came from (source, or null for plain Python
# data), the columns that may differ from that source (known, DESIGN.md section 8) and differences still open
# (open). This script reads the file with the R package and compares R's view with that JSON. Result per file:
#   PASS   R shows what Python said, and every column equals the source's (or differs as `known` says: KNOWN
#          lines show those)
#   OPEN   R does not show what Python said, only in columns listed under `open` (shown)
#   FAIL   any other difference; ERROR: the file could not be read
# Exit status 1 on FAIL or ERROR.

args <- commandArgs(TRUE)
me <- normalizePath(sub("^--file=", "", grep("^--file=", commandArgs(FALSE), value = TRUE)[1]), winslash = "/")
gdir <- if (length(args)) normalizePath(args[1], winslash = "/") else dirname(dirname(me))
pdir <- file.path(gdir, "from_python", "rmatch")
suppressPackageStartupMessages({
  library(fstx)
  library(bit64)
  library(jsonlite)
})
source(file.path(gdir, "rmatch", "rview.R"))
if (!dir.exists(pdir)) stop("no folder ", pdir, ": run test_rmatch.py first")

show <- function(v) if (is.null(v)) "NULL" else paste(vapply(v, function(e) if (is.null(e)) "NA" else as.character(e), ""), collapse = ", ")

# {column: first difference} between two views
vdiff <- function(a, b) {
  out <- list()
  if (!identical(a$names, b$names)) return(list(`(names)` = paste(show(a$names), "vs", show(b$names))))
  if (!identical(a$nrow, b$nrow)) return(list(`(rows)` = paste(a$nrow, "vs", b$nrow)))
  for (j in seq_along(a$columns)) {
    x <- a$columns[[j]]
    y <- b$columns[[j]]
    for (k in c("typeof", "class", "tzone", "levels", "values")) {
      if (identical(x[[k]], y[[k]])) next
      out[[x$name]] <- if (k == "values") {
        i <- which(!mapply(identical, x[[k]], y[[k]]))[1L]
        sprintf("row %d: %s vs %s", i, show(x[[k]][i]), show(y[[k]][i]))
      } else sprintf("%s: %s vs %s", k, show(x[[k]]), show(y[[k]]))
      break
    }
  }
  out
}

files <- sort(list.files(pdir, pattern = "\\.fstx$"))
res <- character(0)
for (f in files) {
  js <- fromJSON(file.path(pdir, sub("\\.fstx$", ".view.json", f)), simplifyVector = FALSE)
  want <- js[c("names", "nrow", "columns")]
  lines <- character(0)
  got <- tryCatch(withCallingHandlers(read_fstx(file.path(pdir, f), threads = 1L),
                                      warning = function(w) invokeRestart("muffleWarning")),
                  error = function(e) e)
  if (inherits(got, "error")) {
    st <- "ERROR"
    lines <- conditionMessage(got)
  } else {
    rv <- suppressWarnings(rview(got))  # (an unknown time zone warns)
    d <- vdiff(rv, want)
    open <- names(d) %in% names(js$open)
    st <- if (!length(d)) "PASS" else if (all(open)) "OPEN" else "FAIL"
    for (k in names(d)) lines <- c(lines, sprintf("column '%s' %s: R %s",
                                                  k, if (k %in% names(js$open)) paste0("(open: ", js$open[[k]], ")") else "",
                                                  d[[k]]))
    if (!is.null(js$source)) {
      src <- rview(read_fstx(file.path(gdir, "rmatch", js$source), threads = 1L))
      ds <- vdiff(rv, src)
      for (k in names(ds)) {
        if (k %in% names(js$known)) lines <- c(lines, sprintf("KNOWN column '%s' differs from %s: %s (%s)", k, js$source, ds[[k]], js$known[[k]]))
        else {
          st <- "FAIL"
          lines <- c(lines, sprintf("column '%s' differs from %s: %s", k, js$source, ds[[k]]))
        }
      }
      for (k in setdiff(names(js$known), names(ds))) lines <- c(lines, sprintf("column '%s' is listed as known but equals %s", k, js$source))
    }
  }
  res <- c(res, st)
  cat(sprintf("%-6s %s\n", st, f))
  for (w in lines) cat("         ", w, "\n")
}
if (!length(res)) stop("no files in ", pdir)
cat(sprintf("\n%d files: %s\n", length(res), paste(names(table(res)), table(res), sep = " ", collapse = ", ")))
if (any(res %in% c("FAIL", "ERROR"))) quit(status = 1L)
