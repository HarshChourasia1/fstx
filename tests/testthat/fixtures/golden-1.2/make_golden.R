# Makes the fstx format 1.2 golden files (design 1.2 section 8). Synthetic data only; base R (no bit64, no blob).
#
#   Rscript make_golden_v10.R <out dir> <library with fstx 1.0.2>     (first, once: the file fstx 1.0.2 writes)
#   Rscript make_golden.R     <out dir> <library with fstx 1.2>
#
# Run with <out dir> = this folder, from the merged 1.2 code (engine drop 2 + R binding), threads 1 throughout so
# the bytes do not depend on the machine; commit the .fstx files, files.csv and expected.csv.
# test-compat-golden12.R (R) and python/tests/test_golden12.py then check that every later fstx reads them the
# same way. Compatibility is checked one way (Harsh, 28 Nov 2026): 1.2 reads what 1.0.x wrote; nothing here runs
# fstx 1.0.x on 1.2 files. A second run on another platform (Linux) into another folder must give the same md5s.
#
# The files:
#   mix_i64.fstx          int64 pages (kind 7): tags 1-7 x missing-value modes 0-2 x codecs 0-2 (63 mixes), one
#                         append per encoding and codec forced with the .encoding test flags, three columns (no NA,
#                         scattered NA = bitmap, trailing NA run = runs), values past 2^53, then page-copy compacted
#   mix_bin_str.fstx      binary columns on string pages: PLAIN / FSST x modes x codecs (18), NUL and empty values
#   mix_bin_codes.fstx    binary columns dictionary-coded: codes tags 1-7 x modes x codecs (63) and the dictionary
#                         page PLAIN / FSST x codecs (6). With the 1.0 golden mixes: 294 + 150 = 444.
#   int64_extremes.fstx   INT64_MAX, INT64_MIN + 1, +-(2^53 + 1), 2^53, 0, -1, NA; an all-missing column; a constant
#   binary_values.fstx    NUL bytes, empty values, NULL, all 256 byte values, a 71,680-byte value, an all-missing
#                         column; classes none, blob, arrow_binary, AsIs, vctrs list_of
#   both_12.fstx          every 1.0 type plus int64 and binary (both feature bits), row names, a table attribute
#   appended_12.fstx      int64 + binary + string, written then appended to twice
#   compacted_12.fstx     the same, page-copy compacted; merged_12.fstx the same, compact(merge = TRUE)
#   v10_integer64.fstx    written by fstx 1.0.2 (make_golden_v10.R): integer64 in the 1.0 layout
#   v10_appended12.fstx   that file appended to by fstx 1.2 (the append follows the file: 1.0 layout, no bit)
#   v10_merged12.fstx     that file compacted by fstx 1.2 with merge = TRUE (the 1.0 layout is kept)
#   reserved12_*.fstx     every number still reserved in 1.2, forged (fstx:::.fstx_forge) on 1.2 files that have
#                         int64 and binary columns: refused by name ("Update fstx"), or corrupt where 1.2 says so
#                         (logical types 1-3 on int64 / binary), or read (optional bit 50, profile 3)
# files.csv: per file its size, MD5, the writer ("fstx 1.2.0" / "fstx 1.0.2"), whether 1.2 reads, refuses or
# calls it corrupt (with the message), format version, profile, rows, columns, column types and table attributes.
# expected.csv: per column (canon.R).

args <- commandArgs(TRUE)
if (length(args) < 2L) stop("usage: Rscript make_golden.R <out dir> <fstx 1.2 library>")
out <- normalizePath(args[1], winslash = "/", mustWork = FALSE)
lib <- normalizePath(args[2], winslash = "/")
me <- sub("^--file=", "", grep("^--file=", commandArgs(FALSE), value = TRUE)[1])
source(file.path(dirname(normalizePath(me)), "canon.R"))
suppressPackageStartupMessages(library(fstx, lib.loc = lib))
ver <- as.character(packageVersion("fstx", lib.loc = lib))
if (package_version(ver) < "1.2.0") stop("needs fstx 1.2 or later, found ", ver)
ft <- fstx_features()
stopifnot(identical(ft$format_write, 3L), all(ft$types$write[ft$types$name %in% c("int64", "binary")]))
writer <- paste("fstx", ver)
cat(writer, "from", lib, "\n")

dir.create(out, recursive = TRUE, showWarnings = FALSE)
gpath <- function(f) file.path(out, f)
if (!file.exists(gpath("v10_integer64.fstx")))
  stop("v10_integer64.fstx is missing: run make_golden_v10.R with fstx 1.0.2 first")
keep <- "v10_integer64.fstx"
old <- setdiff(basename(list.files(out, "\\.fstx$")), keep)
unlink(c(gpath(old), gpath(c("files.csv", "expected.csv"))))
w12 <- function(x, f, ...) fstx:::.write_fstx(x, gpath(f), threads = 1L, ...)
rd <- function(f, ...) suppressMessages(read_fstx(gpath(f), threads = 1L, ...))   # no bit64: one message

files <- list()     # one row per file
expected <- list()  # per-column descriptions
add_row <- function(f, expect, message, wr, m = NULL, y = NULL) {
  files[[length(files) + 1L]] <<- data.frame(file = f, writer = wr, expect = expect, message = message,
    format_version = if (is.null(m)) NA else m$format_version, profile = if (is.null(m)) "" else m$profile,
    nrow = if (is.null(y)) NA else nrow(y), ncol = if (is.null(y)) NA else ncol(y),
    types = if (is.null(m)) "" else paste(m$columns$type, collapse = ";"),
    table_md5 = if (is.null(y)) "" else golden_table_md5(y), stringsAsFactors = FALSE)
}
add_read <- function(f, x, wr = writer) {  # x: the table the file must read as
  y <- rd(f)
  if (!identical(y, x, num.eq = FALSE)) stop(sprintf("%s does not read back as written", f))
  yl <- rd(f, lazy = TRUE)
  # against the eager read, not x: identical() ignores attribute order, the attribute md5 does not
  if (!identical(golden_describe(yl), golden_describe(y))) stop(sprintf("%s: lazy read differs", f))
  rm(yl); invisible(gc())
  m <- fstx_meta(gpath(f))
  add_row(f, "read", "", wr, m, y)
  d <- golden_describe(y)
  if (nrow(d)) expected[[length(expected) + 1L]] <<- cbind(file = f, d, stringsAsFactors = FALSE)
}
# fstx_meta() must give `msg`; read_fstx() must give it too when the file is corrupt, and "Update fstx" (as the
# golden-1.0 test checks) when it is refused as newer
add_refuse <- function(f, msg, corrupt = FALSE) {
  for (k in 1:2) {
    e <- tryCatch({ suppressMessages(if (k == 1L) fstx_meta(gpath(f)) else read_fstx(gpath(f), threads = 1L)); NULL },
                  error = function(e) conditionMessage(e))
    want <- if (k == 1L || corrupt) msg else "Update fstx"
    if (is.null(e) || !grepl(want, e)) stop(sprintf("%s: expected an error matching '%s', got: %s", f, want, format(e)))
    if (grepl("Update fstx", e, fixed = TRUE) == corrupt)
      stop(sprintf("%s: %s, got: %s", f, if (corrupt) "corrupt, not 'Update fstx'" else "'Update fstx'", e))
  }
  add_row(f, if (corrupt) "corrupt" else "refuse", msg, writer)
}
nas <- c("none", "bitmap", "runs")
codecs <- c("none", "lz4", "zstd")
int_encs <- c("plain", "const", "for", "delta", "rle", "dict", "split")
grid <- function(kind, encs, na = nas) as.vector(outer(outer(paste(kind, encs), na, paste), codecs, paste))
seen_mixes <- function(f) {
  pg <- fstx_pages(gpath(f))
  bin <- fstx_meta(gpath(f))$columns$type[pg$column] == "binary"   # binary pages are string kinds
  unique(paste0(ifelse(bin, "binary ", ""), paste(pg$kind, pg$encoding, pg$na, pg$codec)))
}

# ---------------------------------------------------------------- every new legal mix (150)
N <- 160L  # rows per append, as golden-1.0
set.seed(20261127)
sc <- sort(sample.int(N, 12L))
run <- 101:N
na_at <- function(v, at) {
  if (is.list(v)) { v[at] <- list(NULL); return(v) }
  golden_i64_set(v, at, golden_i64_na(length(at)))
}
i64_pool <- golden_i64_c(golden_i64(c(1, 2, 3, 13, -4, 2^40)), golden_i64_parts(c(2^21, -2^21 - 1), c(1, 2^32 - 3)))
bin_pool <- list(charToRaw("alpha"), as.raw(c(0, 1, 0)), raw(0), as.raw(c(255, 254, 0, 7)), charToRaw("café"),
                 as.raw(0:63))
vals <- function(kind, const) {
  switch(kind,
    i64 = if (const) golden_i64_rep(golden_i64_parts(2^21, 5), N) else
      golden_i64_take(i64_pool, sample.int(length(i64_pool), N, TRUE)),
    bin_str = ,
    bin_codes = if (const) rep(list(as.raw(c(7, 0))), N) else bin_pool[sample.int(length(bin_pool), N, TRUE)])
}
piece <- function(kind, const) {
  v <- vals(kind, const)
  x <- list(x_0 = v, x_b = na_at(v, sc), x_r = na_at(v, run))
  list2DF(x)
}
mixes <- list(
  i64 = list(file = "mix_i64.fstx", encs = int_encs, flag = NULL),
  bin_str = list(file = "mix_bin_str.fstx", encs = c("plain", "fsst"), flag = NULL),
  bin_codes = list(file = "mix_bin_codes.fstx", encs = c(int_encs, "fsst_dict"), flag = "dict_codes")
)
seen <- character(0)
for (kind in names(mixes)) {
  f <- mixes[[kind]]$file
  parts <- list()
  for (codec in codecs) {
    for (e in mixes[[kind]]$encs) {
      x <- piece(kind, e == "const")
      enc <- if (e == "fsst_dict") c("plain", "fsst_dict") else c(e, mixes[[kind]]$flag)
      if (codec != "none") enc <- c(enc, "keep_codec")
      w12(x, f, compress = codec, .encoding = enc, append = length(parts) > 0L)
      parts[[length(parts) + 1L]] <- x
    }
  }
  fstx_compact(gpath(f))
  add_read(f, golden_df_rbind(parts))
  seen <- union(seen, seen_mixes(f))
}
want <- c(grid("int64", int_encs), grid("binary codes", int_encs), grid("binary string", c("plain", "fsst")),
          grid("binary dictionary", c("plain", "fsst"), "none"))
stopifnot(length(want) == 150L)
miss <- setdiff(want, seen)
if (length(miss)) stop("mixes not in the golden files: ", paste(miss, collapse = "; "))
cat("new mixes", length(intersect(want, seen)), "of", length(want), "\n")

# ---------------------------------------------------------------- values at the edges
ext <- golden_i64_c(golden_i64_max(), golden_i64_min1(), golden_i64_parts(c(2^21, -2^21 - 1), c(1, 2^32 - 1)),
                    golden_i64(c(2^53, 0, -1, NA, 2^62, -(2^62))))
x <- list2DF(list(
  ext = ext,
  all_na = golden_i64_na(length(ext)),
  const = golden_i64_max(length(ext)),
  n = seq_along(ext)
))
w12(x, "int64_extremes.fstx")
add_read("int64_extremes.fstx", x)

big <- as.raw(rep(0:255, 280))
bv <- list(raw(0), as.raw(0), as.raw(c(0, 0, 0)), as.raw(0:255), charToRaw("text"), NULL, big, charToRaw("café"))
x <- list2DF(list(
  plain = golden_bin(bv),
  all_null = golden_bin(rep(list(NULL), length(bv))),
  blob = golden_bin(rev(bv), "blob"),
  arrow = golden_bin(bv[c(2, 1, 4, 3, 6, 5, 8, 7)], "arrow_binary"),
  asis = golden_bin(bv, "AsIs"),
  list_of = golden_bin(rev(bv), "list_of")
))
w12(x, "binary_values.fstx")
add_read("binary_values.fstx", x)

# ---------------------------------------------------------------- tables with every type
types_df <- function(n) {
  set.seed(7)
  d <- data.frame(
    int = sample(c(1:50, NA), n, TRUE),
    dbl = sample(c(round(rnorm(20), 3), NA, NaN, Inf, -Inf, -0), n, TRUE),
    lgl = sample(c(TRUE, FALSE, NA), n, TRUE),
    chr = sample(c("a", "bb", "", NA, "longer text value"), n, TRUE),
    fac = factor(sample(c("lo", "mid", "hi", NA), n, TRUE), levels = c("lo", "mid", "hi")),
    ord = factor(sample(c("s", "m", "l"), n, TRUE), levels = c("s", "m", "l"), ordered = TRUE),
    date = as.Date("2026-01-01") + sample(c(0:400, NA), n, TRUE),
    date_dbl = structure(as.double(sample(c(0:400, NA), n, TRUE)) + 0.5, class = "Date"),
    ct = as.POSIXct(1.7e9 + sample(c(0:100000, NA), n, TRUE), tz = "UTC", origin = "1970-01-01"),
    stringsAsFactors = FALSE
  )
  d$labelled <- structure(sample(1:5, n, TRUE), label = "a label", class = "golden_class")
  d$i64 <- golden_i64_take(golden_i64_c(i64_pool, golden_i64_na()), sample.int(length(i64_pool) + 1L, n, TRUE))
  d$bin <- golden_bin(c(bin_pool, list(NULL))[sample.int(length(bin_pool) + 1L, n, TRUE)])
  d$blob <- golden_bin(c(bin_pool, list(NULL))[sample.int(length(bin_pool) + 1L, n, TRUE)], "blob")
  attr(d, "note") <- "golden table attribute"
  d
}
x <- types_df(200L)
rownames(x) <- sprintf("r%03d", seq_len(nrow(x)))
w12(x, "both_12.fstx")
add_read("both_12.fstx", x)
stopifnot(identical(fstx_meta(gpath("both_12.fstx"))$columns$type[11:13], c("int64", "binary", "binary")))

# appended (old footers left inside), the same page-copy compacted, and merged
set.seed(3)
ap <- lapply(1:3, function(k) list2DF(list(
  id = golden_i64_c(golden_i64(k * 100 + 1:60), golden_i64_parts(2^21, k)),
  b = golden_bin(c(bin_pool, list(NULL))[sample.int(length(bin_pool) + 1L, 61, TRUE)]),
  s = sample(c("x", "y", "zz", NA), 61, TRUE))))
for (k in 1:3) w12(ap[[k]], "appended_12.fstx", append = k > 1L)
x <- golden_df_rbind(ap)
add_read("appended_12.fstx", x)
invisible(file.copy(gpath("appended_12.fstx"), gpath("compacted_12.fstx")))
fstx_compact(gpath("compacted_12.fstx"))
add_read("compacted_12.fstx", x)
stopifnot(file.size(gpath("compacted_12.fstx")) < file.size(gpath("appended_12.fstx")))
invisible(file.copy(gpath("appended_12.fstx"), gpath("merged_12.fstx")))
fstx_compact(gpath("merged_12.fstx"), merge = TRUE, threads = 1L)
add_read("merged_12.fstx", x)

# ---------------------------------------------------------------- the file fstx 1.0.2 wrote, and 1.2 changes to it
x10 <- golden_v10_table()
add_read("v10_integer64.fstx", x10, wr = "fstx 1.0.2")
stopifnot(identical(fstx_meta(gpath("v10_integer64.fstx"))$columns$type, c("integer", "double", "double", "character")))
invisible(file.copy(gpath("v10_integer64.fstx"), gpath("v10_appended12.fstx")))
add10 <- x10[1:5, ]  # plain vectors: rows 1-5 rebuilt with the column helpers (subsetting drops the class)
add10$big <- golden_i64_take(x10$big, 1:5)
add10$small <- golden_i64_take(x10$small, 1:5)
rownames(add10) <- NULL
w12(add10, "v10_appended12.fstx", append = TRUE)   # int64 = "native" by default: the append follows the file
add_read("v10_appended12.fstx", golden_df_rbind(list(x10, add10)))
stopifnot(identical(fstx_meta(gpath("v10_appended12.fstx"))$columns$type, c("integer", "double", "double", "character")))
invisible(file.copy(gpath("v10_integer64.fstx"), gpath("v10_merged12.fstx")))
fstx_compact(gpath("v10_merged12.fstx"), merge = TRUE, threads = 1L)
add_read("v10_merged12.fstx", x10)
stopifnot(identical(fstx_meta(gpath("v10_merged12.fstx"))$columns$type, c("integer", "double", "double", "character")))

# ---------------------------------------------------------------- reserved numbers in 1.2
# Bases: column 0 int64 (bits 16 and 17: the file also has a binary column), column 0 binary (bit 17 only).
b64 <- list2DF(list(id = golden_i64(1:3), b = golden_bin(list(as.raw(1), NULL, raw(0))), v = c(0.5, 1, 1.5)))
bbin <- list2DF(list(id = golden_bin(list(as.raw(c(0, 1)), NULL, raw(0))), v = c(0.5, 1, 1.5)))
f64 <- tempfile(fileext = ".fstx")
fbin <- tempfile(fileext = ".fstx")
fstx:::.write_fstx(b64, f64, compress = "none", threads = 1L)
fstx:::.write_fstx(bbin, fbin, compress = "none", threads = 1L)
both <- c(16L, 17L)
forge <- function(src, f, ...) fstx:::.fstx_forge(src, gpath(f), ...)
refuse <- list(   # name = list(base, forge fields, message)
  version4 = list(f64, list(version = 4L, required = both), "format version 4; this reader supports up to 3\\. Update fstx"),
  phys6 = list(f64, list(phys0 = 6L, required = both), "column type 6 \\(column 'id'\\)"),
  logical4 = list(f64, list(logical0 = 4L, required = both), "logical type 4 \\(column 'id'\\)"),
  kind8 = list(f64, list(kind0 = 8L, required = both), "page kind 8 \\(column 'id'\\)"),
  codec3 = list(f64, list(codec0 = 3L, required = both), "codec 'zstd with a dictionary' \\(codec 3"),
  codec4 = list(f64, list(codec0 = 4L, required = both), "codec 'Pcodec' \\(codec 4"),
  codec5 = list(f64, list(codec0 = 5L, required = both), "uses codec 5 \\(column 'id'\\)"),
  codec128 = list(f64, list(codec0 = 128L, required = both), "experimental codec 128"),
  na3 = list(f64, list(na0 = 3L, required = both), "missing-value mode 3 \\(column 'id'\\)"),
  tag11 = list(f64, list(tag0 = 11L, required = both), "encoding 11 \\(column 'id'\\)"),
  bin_kind8 = list(fbin, list(kind0 = 8L, required = 17L), "page kind 8 \\(column 'id'\\)"),
  hdrbit3 = list(f64, list(required = c(3L, both), hdr_flags = 8L), "feature bit 3, which this fstx .* cannot read\\. Update fstx"),
  zstd_dict = list(f64, list(required = c(both, 18L)), "'zstd_dict' \\(bit 18\\).*Update fstx"),
  pcodec = list(f64, list(required = c(both, 19L)), "'pcodec' \\(bit 19\\).*Update fstx"),
  experimental = list(f64, list(required = c(both, 63L)), "'experimental' \\(bit 63\\).*Update fstx"),
  named40 = list(f64, list(required = c(both, 40L), names = c("int64", "binary", "future_thing")),
                 "'future_thing' \\(bit 40\\).*Update fstx")
)
for (nm in names(refuse)) {
  f <- sprintf("reserved12_%s.fstx", nm)
  do.call(forge, c(list(refuse[[nm]][[1]], f), refuse[[nm]][[2]]))
  add_refuse(f, refuse[[nm]][[3]])
}
combo <- "column 0 has an invalid type combination"
for (lt in 1:3) {   # logical types 1-3 (factor, Date, POSIXct) on int64 and on binary: corrupt
  f <- sprintf("reserved12_logical%d_int64.fstx", lt)
  forge(f64, f, logical0 = lt, required = both)
  add_refuse(f, combo, corrupt = TRUE)
  f <- sprintf("reserved12_logical%d_binary.fstx", lt)
  forge(fbin, f, logical0 = lt, required = 17L)
  add_refuse(f, combo, corrupt = TRUE)
}
forge(f64, "reserved12_optional50.fstx", required = both, optional = 50L, names = c("int64", "binary", "later_index"))
add_read("reserved12_optional50.fstx", b64)                      # unknown optional bit: reads
forge(f64, "reserved12_profile3.fstx", required = both, profile = 3L)
add_read("reserved12_profile3.fstx", b64)                        # unknown profile: reads
stopifnot(identical(fstx_meta(gpath("reserved12_profile3.fstx"))$profile, "unknown"))
unlink(c(f64, fbin))

# ---------------------------------------------------------------- manifest
fl <- do.call(rbind, files)
fl$bytes <- file.size(gpath(fl$file))
fl$md5 <- unname(tools::md5sum(gpath(fl$file)))
fl <- fl[c("file", "bytes", "md5", setdiff(names(fl), c("file", "bytes", "md5")))]
for (i in seq_len(nrow(fl))) {   # every file names its writer
  b <- readBin(gpath(fl$file[i]), "raw", fl$bytes[i])
  if (!length(grepRaw(fl$writer[i], b, fixed = TRUE))) stop(fl$file[i], " does not hold '", fl$writer[i], "'")
}
stopifnot(setequal(fl$file, basename(list.files(out, "\\.fstx$"))))
write.csv(fl, gpath("files.csv"), row.names = FALSE, fileEncoding = "UTF-8")
write.csv(do.call(rbind, expected), gpath("expected.csv"), row.names = FALSE, fileEncoding = "UTF-8")
total <- sum(file.size(list.files(out, full.names = TRUE)))
cat(sprintf("GOLDEN12 files %d fstx bytes %d total bytes in folder %d\n", nrow(fl), sum(fl$bytes), total))
for (k in order(-fl$bytes)[1:8]) cat(sprintf("  %-34s %7d\n", fl$file[k], fl$bytes[k]))
if (total > 400 * 1024) stop("the golden-1.2 set is over 400 KB")
