# Makes the golden-1.2 file that fstx 1.0.2 itself writes: v10_integer64.fstx, a bit64 integer64 column in the
# fstx 1.0 layout (a double column holding the int64 bits, class "integer64" in the column's attribute blob, no
# feature bit). Synthetic data only; base R (no bit64 needed).
#
#   Rscript make_golden_v10.R <out dir> <library with fstx 1.0.2>
#
# Run once with <out dir> = this folder and fstx 1.0.2 (built from tag v1.0.2, or the R-universe 1.0.2 binary,
# whose package code is the same), before make_golden.R, which reads the file with fstx 1.2, appends to and
# compacts a copy of it, and lists it in files.csv. Commit the .fstx file.

args <- commandArgs(TRUE)
if (length(args) < 2L) stop("usage: Rscript make_golden_v10.R <out dir> <fstx 1.0.2 library>")
out <- normalizePath(args[1], winslash = "/", mustWork = FALSE)
lib <- normalizePath(args[2], winslash = "/")
me <- sub("^--file=", "", grep("^--file=", commandArgs(FALSE), value = TRUE)[1])
source(file.path(dirname(normalizePath(me)), "canon.R"))
suppressPackageStartupMessages(library(fstx, lib.loc = lib))
ver <- as.character(packageVersion("fstx", lib.loc = lib))
if (ver != "1.0.2") stop("needs fstx 1.0.2, found ", ver)
cat("fstx", ver, "from", lib, "\n")

dir.create(out, recursive = TRUE, showWarnings = FALSE)
f <- file.path(out, "v10_integer64.fstx")
unlink(f)
x <- golden_v10_table()
write_fstx(x, f, threads = 1L)
y <- read_fstx(f, threads = 1L)
if (!identical(y, x, num.eq = FALSE)) stop("v10_integer64.fstx does not read back as written in fstx 1.0.2")
m <- fstx_meta(f)
stopifnot(identical(m$columns$type, c("integer", "double", "double", "character")))
b <- readBin(f, "raw", file.size(f))
stopifnot(length(grepRaw("fstx 1.0.2", b, fixed = TRUE)) > 0)
cat(sprintf("V10 %s %d bytes md5 %s\n", basename(f), file.size(f), unname(tools::md5sum(f))))
