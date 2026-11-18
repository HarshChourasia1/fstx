# Speed check, R side: the R package on the file bench_py.py wrote (same machine, same data).
#   Rscript bench_r.R <out dir>
args <- commandArgs(TRUE)
out <- args[1]
suppressPackageStartupMessages(library(fstx))
p <- file.path(out, "bench.fstx")
reps <- 5L
timed <- function(f) { t <- vapply(seq_len(reps), function(i) system.time(f())[["elapsed"]], 0); c(median(t), min(t)) }
x <- read_fstx(p, strings = "eager")  # warm the page cache; plain R strings, as Python gets
cat(sprintf("R %s, fstx %s, %d rows, %d columns, %.0f MiB in R memory\n", R.version$version.string,
            packageVersion("fstx"), nrow(x), ncol(x), as.numeric(object.size(x)) / 2^20))
show <- function(what, r) cat(sprintf("%-15s %-38s median %7.3f s  min %7.3f s\n", what, "R fstx", r[1], r[2]))
show("read", timed(function() read_fstx(p)))  # default: strings made on first use (ALTREP)
show("read_eager", timed(function() read_fstx(p, strings = "eager")))  # every R string made during the read
show("read_1_thread", timed(function() read_fstx(p, threads = 1L, strings = "eager")))
show("read_2_columns", timed(function() read_fstx(p, columns = c("price", "city"), strings = "eager")))
q <- file.path(out, "bench_r.fstx")
show("write", timed(function() write_fstx(x, q)))
show("write_1_thread", timed(function() write_fstx(x, q, threads = 1L)))
cat(sprintf("size of R's file: %.1f MiB\n", file.size(q) / 2^20))
unlink(q)
