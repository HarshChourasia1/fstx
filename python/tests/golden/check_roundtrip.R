# R -> Python -> R check for the Python port (python/DESIGN.md).
#
#   Rscript check_roundtrip.R [golden dir]       (default: the folder this script is in)
#
# The Python tests read every <case>.fstx of the golden dir and write it back into <golden dir>/from_python/:
#   from_python/<case>.fstx          read and written with pyarrow (read_fstx -> pyarrow.Table -> write_fstx)
#   from_python/<case>.pandas.fstx   the same through fstx.to_pandas() and the pandas DataFrame, optional
#   from_python/<case>.polars.fstx   the same through fstx.to_polars() and the polars DataFrame, optional
# This script reads each pair in R and compares the tables with identical(). manifest.json says, per case and path,
# whether identical() is expected ("identical ...") or not (anything else). Result per file:
#   PASS      identical, as expected
#   KNOWN     not identical, and the manifest says so (the difference is shown)
#   BETTER    identical although the manifest expects a difference (update DESIGN.md / manifest.json)
#   FAIL      not identical although the manifest expects identical
#   ERROR     the Python file could not be read
# Also shown, without changing the result: double columns whose bits differ where identical() does not look
# (NA payloads, NaN sign), and strings whose encoding mark differs. Exit status 1 when any FAIL or ERROR.

args <- commandArgs(TRUE)
me <- normalizePath(sub("^--file=", "", grep("^--file=", commandArgs(FALSE), value = TRUE)[1]), winslash = "/")
gdir <- if (length(args)) normalizePath(args[1], winslash = "/") else dirname(me)
pdir <- file.path(gdir, "from_python")
suppressPackageStartupMessages({
  library(fstx)
  library(bit64)
  library(jsonlite)
})
mf <- fromJSON(file.path(gdir, "manifest.json"), simplifyVector = FALSE)
if (!dir.exists(pdir)) stop("no folder ", pdir, ": run the Python tests first")

bits <- function(v) { v <- unclass(v); attributes(v) <- NULL; writeBin(as.double(v), raw(), size = 8L) }

# Where two tables differ: per column, then attributes; at most a few lines.
explain <- function(a, b) {
  out <- character(0)
  if (!identical(names(a), names(b))) out <- c(out, sprintf("names: %s vs %s", toString(names(a)), toString(names(b))))
  for (j in seq_len(min(length(a), length(b)))) {
    x <- a[[j]]
    y <- b[[j]]
    if (identical(x, y)) next
    what <- if (!identical(typeof(x), typeof(y))) sprintf("type %s vs %s", typeof(x), typeof(y))
            else if (!identical(oldClass(x), oldClass(y))) sprintf("class %s vs %s", toString(oldClass(x)), toString(oldClass(y)))
            else if (!identical(attributes(x), attributes(y))) paste("attributes:", paste(all.equal(attributes(x), attributes(y)), collapse = "; "))
            else {
              k <- which(!mapply(identical, as.list(unclass(x)), as.list(unclass(y))))
              show <- function(v) if (is.double(v)) sprintf("%.17g", unclass(v)) else format(v)
              sprintf("%d value(s) differ, first at row %d: %s vs %s", length(k), k[1L],
                      show(x[k[1L]]), show(y[k[1L]]))
            }
    out <- c(out, sprintf("column '%s': %s", names(a)[j], what))
  }
  ta <- attributes(a)[setdiff(names(attributes(a)), c("names", ".internal.selfref"))]
  tb <- attributes(b)[setdiff(names(attributes(b)), c("names", ".internal.selfref"))]
  if (!identical(ta, tb)) out <- c(out, paste("table attributes:", paste(all.equal(ta, tb), collapse = "; ")))
  if (!length(out)) out <- "differs (no column or attribute difference found)"
  head(out, 6L)
}

notes <- function(a, b) {
  out <- character(0)
  for (j in seq_len(min(length(a), length(b)))) {
    x <- a[[j]]
    y <- b[[j]]
    if (is.double(x) && is.double(y) && length(x) == length(y) && !identical(bits(x), bits(y)))
      out <- c(out, sprintf("column '%s': double bits differ (identical() ignores NA payloads / NaN sign)", names(a)[j]))
    if (is.character(x) && is.character(y) && length(x) == length(y) && !identical(Encoding(x), Encoding(y)))
      out <- c(out, sprintf("column '%s': encoding marks differ", names(a)[j]))
  }
  out
}

res <- list()
for (cs in mf$cases) {
  orig <- file.path(gdir, cs$file)
  for (path in c("pyarrow", "pandas", "polars")) {
    pf <- file.path(pdir, if (path == "pyarrow") cs$file else sub("\\.fstx$", paste0(".", path, ".fstx"), cs$file))
    if (!file.exists(pf)) next
    expect <- cs[[paste0("roundtrip_", path)]]
    want_same <- startsWith(expect, "identical")
    a <- read_fstx(orig, threads = 1L)
    b <- tryCatch(read_fstx(pf, threads = 1L), error = function(e) e)
    if (inherits(b, "error")) {
      st <- "ERROR"
      why <- conditionMessage(b)
      nt <- character(0)
    } else {
      same <- identical(a, b)
      st <- if (same && want_same) "PASS" else if (same) "BETTER" else if (want_same) "FAIL" else "KNOWN"
      why <- if (same) character(0) else explain(a, b)
      nt <- notes(a, b)
    }
    res[[length(res) + 1L]] <- data.frame(file = basename(pf), path = path, result = st, stringsAsFactors = FALSE)
    cat(sprintf("%-6s %-34s %s\n", st, basename(pf), if (st %in% c("KNOWN", "BETTER")) paste0("(manifest: ", expect, ")") else ""))
    for (w in c(why, nt)) cat("         ", w, "\n")
  }
}
if (!length(res)) stop("no Python files in ", pdir)
r <- do.call(rbind, res)
cases <- vapply(mf$cases, `[[`, "", "file")
miss <- setdiff(cases, r$file[r$path == "pyarrow"])
cat(sprintf("\n%d files: %s\n", nrow(r), paste(names(table(r$result)), table(r$result), sep = " ", collapse = ", ")))
if (length(miss)) cat("cases with no from_python file:", paste(miss, collapse = ", "), "\n")
if (any(r$result %in% c("FAIL", "ERROR"))) quit(status = 1L)
