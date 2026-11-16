# Makes the fstx format 1.0 golden files (WS8). Synthetic data only.
#
#   Rscript make_golden.R <out dir> <library with fstx 1.0>
#
# Run once, from the released 1.0 code, with <out dir> = this folder; commit the .fstx files, files.csv and
# expected.csv. test-compat-golden.R then checks that every later fstx reads them the same way. The files:
#   mix_<kind>.fstx     every legal mix of page kind x encoding x missing-value mode x codec (spec section 7):
#                       one append per encoding and codec (forced with the .encoding test flags), three columns
#                       (no NA, scattered NA = bitmap, trailing NA run = runs), then page-copy compacted
#   appended.fstx       written, then appended to twice (old footers and tails left inside the file)
#   compacted.fstx      the same table, page-copy compacted
#   types.fstx          every column type and attribute fstx stores, with stored row names
#   empty.fstx, zero_cols.fstx, zero_cols_rows.fstx   no rows; no columns; no columns but rows
#   nonascii.fstx       non-ASCII column names, strings and factor levels
#   reserved_*.fstx     one file per reserved number or unknown feature, made with fstx:::.fstx_forge
# files.csv: per file its size, MD5, whether it reads or is refused (with the message), format version,
# profile, rows, columns and table attributes. expected.csv: per column (canon.R).
# The whole set must stay under 300 KB.

args <- commandArgs(TRUE)
if (length(args) < 2L) stop("usage: Rscript make_golden.R <out dir> <fstx 1.0 library>")
out <- normalizePath(args[1], winslash = "/", mustWork = FALSE)
lib1 <- normalizePath(args[2], winslash = "/")
me <- sub("^--file=", "", grep("^--file=", commandArgs(FALSE), value = TRUE)[1])
source(file.path(dirname(normalizePath(me)), "canon.R"))
suppressPackageStartupMessages(library(fstx, lib.loc = lib1))
stopifnot(identical(fstx_features()$format_write, 3L))
cat("fstx", as.character(packageVersion("fstx", lib.loc = lib1)), "\n")

dir.create(out, recursive = TRUE, showWarnings = FALSE)
unlink(c(list.files(out, "\\.fstx$", full.names = TRUE), file.path(out, c("files.csv", "expected.csv"))))
gpath <- function(f) file.path(out, f)

files <- list()     # one row per file
expected <- list()  # per-column descriptions
add_read <- function(f, x) {  # x: the table the file must read as
  y <- read_fstx(gpath(f))
  if (!identical(y, x)) stop(sprintf("%s does not read back as written", f))
  m <- fstx_meta(gpath(f))
  files[[length(files) + 1L]] <<- data.frame(file = f, expect = "read", message = "",
    format_version = m$format_version, profile = m$profile, nrow = nrow(y), ncol = ncol(y),
    table_md5 = golden_table_md5(y), stringsAsFactors = FALSE)
  d <- golden_describe(y)
  if (nrow(d)) expected[[length(expected) + 1L]] <<- cbind(file = f, d, stringsAsFactors = FALSE)
}
add_refuse <- function(f, msg) {
  e <- tryCatch({ read_fstx(gpath(f)); NULL }, error = function(e) conditionMessage(e))
  if (is.null(e) || !grepl(msg, e)) stop(sprintf("%s: expected refusal matching '%s', got: %s", f, msg, format(e)))
  files[[length(files) + 1L]] <<- data.frame(file = f, expect = "refuse", message = msg, format_version = NA,
    profile = "", nrow = NA, ncol = NA, table_md5 = "", stringsAsFactors = FALSE)
}

# ---------------------------------------------------------------- every legal mix
N <- 160L  # rows per append: 3 x 8 bytes of bitmap, so one trailing NA run is smaller as runs
set.seed(20261115)
sc <- sort(sample.int(N, 12L))
run <- 101:N
na_at <- function(v, at) { v[at] <- NA; v }
vals <- function(kind, const) {
  switch(kind,
    int = if (const) rep(7L, N) else sample(c(1L, 2L, 3L, 5L, 8L, 13L, -4L), N, TRUE),
    lgl = if (const) rep(TRUE, N) else sample(c(TRUE, FALSE), N, TRUE),
    dbl = if (const) rep(2.5, N) else sample(c(0.5, 1.25, 3, 100.75, -2, 7.1), N, TRUE),
    str = ,
    codes = if (const) rep("same", N) else
      sample(c("alpha", "beta", "café", "delta-a-bit-longer", "", "日本語"), N, TRUE))
}
piece <- function(kind, const) {
  v <- vals(kind, const)
  x <- list(v, na_at(v, sc), na_at(v, run))
  names(x) <- paste0("x_", c("0", "b", "r"))
  as.data.frame(x, stringsAsFactors = FALSE)
}
int_encs <- c("plain", "const", "for", "delta", "rle", "dict", "split")
mixes <- list(
  int = list(encs = int_encs, flag = NULL),
  lgl = list(encs = int_encs, flag = NULL),
  dbl = list(encs = c(int_encs, "alp", "alprd"), flag = NULL),
  str = list(encs = c("plain", "fsst"), flag = NULL),
  codes = list(encs = c(int_encs, "fsst_dict"), flag = "dict_codes")  # fsst_dict: plain codes, FSST dictionary
)
seen <- character(0)
for (kind in names(mixes)) {
  f <- sprintf("mix_%s.fstx", kind)
  parts <- list()
  for (codec in c("none", "lz4", "zstd")) {
    for (e in mixes[[kind]]$encs) {
      x <- piece(kind, e == "const")
      enc <- if (e == "fsst_dict") c("plain", "fsst_dict") else c(e, mixes[[kind]]$flag)
      if (codec != "none") enc <- c(enc, "keep_codec")
      fstx:::.write_fstx(x, gpath(f), compress = codec, .encoding = enc, threads = 1L, append = length(parts) > 0L)
      parts[[length(parts) + 1L]] <- x
    }
  }
  fstx_compact(gpath(f))
  x <- do.call(rbind, parts)
  rownames(x) <- NULL
  add_read(f, x)
  pg <- fstx_pages(gpath(f))
  seen <- union(seen, unique(paste(pg$kind, pg$encoding, pg$na, pg$codec)))
}
nas <- c("none", "bitmap", "runs")
codecs <- c("none", "lz4", "zstd")
grid <- function(kind, encs, na = nas) as.vector(outer(outer(paste(kind, encs), na, paste), codecs, paste))
want <- c(grid("int", int_encs), grid("logical", int_encs), grid("codes", int_encs),
          grid("double", c(int_encs, "alp", "alprd")), grid("string", c("plain", "fsst")),
          grid("dictionary", c("plain", "fsst"), "none"))
stopifnot(length(want) == 294L)
miss <- setdiff(want, seen)
if (length(miss)) stop("mixes not in the golden files: ", paste(miss, collapse = "; "))
cat("mixes", length(intersect(want, seen)), "of", length(want), "\n")

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
    ct_notz = structure(1.7e9 + as.double(sample(0:100000, n, TRUE)), class = c("POSIXct", "POSIXt")),
    stringsAsFactors = FALSE
  )
  d$labelled <- structure(sample(1:5, n, TRUE), label = "a label", class = "golden_class")
  attr(d, "note") <- "golden table attribute"
  d
}
x <- types_df(200L)
rownames(x) <- sprintf("r%03d", seq_len(nrow(x)))
write_fstx(x, gpath("types.fstx"), threads = 1L)
add_read("types.fstx", x)

x <- types_df(0L)
attr(x, "note") <- NULL
write_fstx(x, gpath("empty.fstx"), threads = 1L)
add_read("empty.fstx", x)

x <- data.frame()
write_fstx(x, gpath("zero_cols.fstx"), threads = 1L)
add_read("zero_cols.fstx", x)
x <- data.frame()
attr(x, "row.names") <- .set_row_names(5L)
write_fstx(x, gpath("zero_cols_rows.fstx"), threads = 1L)
add_read("zero_cols_rows.fstx", x)

x <- data.frame(a = c("café", "日本", NA, "Ωmega", "plain"),
                b = factor(c("über", "naïve", "über", NA, "naïve")),
                c = 1:5, stringsAsFactors = FALSE)
names(x) <- c("café", "日本", "Ω")
write_fstx(x, gpath("nonascii.fstx"), threads = 1L)
add_read("nonascii.fstx", x)

# appended (old footers left inside) and the same compacted
set.seed(3)
ap <- lapply(1:3, function(k) data.frame(id = (k * 100L) + 1:60, v = round(runif(60), 2),
                                        s = sample(c("x", "y", "zz", NA), 60, TRUE), stringsAsFactors = FALSE))
for (k in 1:3) write_fstx(ap[[k]], gpath("appended.fstx"), threads = 1L, append = k > 1L)
x <- do.call(rbind, ap)
rownames(x) <- NULL
add_read("appended.fstx", x)
invisible(file.copy(gpath("appended.fstx"), gpath("compacted.fstx")))
fstx_compact(gpath("compacted.fstx"))
add_read("compacted.fstx", x)
stopifnot(file.size(gpath("compacted.fstx")) < file.size(gpath("appended.fstx")))

# ---------------------------------------------------------------- reserved numbers and unknown features
base <- data.frame(id = 1:3, v = c(0.5, 1, 1.5), s = c("a", "b", "c"), stringsAsFactors = FALSE)
bf <- tempfile(fileext = ".fstx")
write_fstx(base, bf, compress = "none", threads = 1L)
forge <- function(f, ...) fstx:::.fstx_forge(bf, gpath(f), ...)
refuse <- list(
  version4 = list(list(version = 4L), "format version 4; this reader supports up to 3\\. Update fstx"),
  phys4 = list(list(phys0 = 4L), "64-bit integer columns \\(column 'id', type 4\\)"),
  phys5 = list(list(phys0 = 5L), "binary columns \\(column 'id', type 5\\)"),
  phys6 = list(list(phys0 = 6L), "column type 6 \\(column 'id'\\)"),
  logical4 = list(list(logical0 = 4L), "logical type 4 \\(column 'id'\\)"),
  codec3 = list(list(codec0 = 3L), "codec 'zstd with a dictionary' \\(codec 3"),
  codec4 = list(list(codec0 = 4L), "codec 'Pcodec' \\(codec 4"),
  codec5 = list(list(codec0 = 5L), "uses codec 5 \\(column 'id'\\)"),
  codec128 = list(list(codec0 = 128L), "experimental codec 128"),
  kind7 = list(list(kind0 = 7L), "page kind 7 \\(column 'id'\\)"),
  na3 = list(list(na0 = 3L), "missing-value mode 3 \\(column 'id'\\)"),
  tag11 = list(list(tag0 = 11L), "encoding 11 \\(column 'id'\\)"),
  hdrbit3 = list(list(required = 3L, hdr_flags = 8L), "feature bit 3, which this fstx .* cannot read\\. Update fstx"),
  int64 = list(list(required = 16L), "'int64' \\(bit 16\\).*Update fstx"),
  binary = list(list(required = 17L), "'binary' \\(bit 17\\).*Update fstx"),
  zstd_dict = list(list(required = 18L), "'zstd_dict' \\(bit 18\\).*Update fstx"),
  pcodec = list(list(required = 19L), "'pcodec' \\(bit 19\\).*Update fstx"),
  experimental = list(list(required = 63L), "'experimental' \\(bit 63\\).*Update fstx"),
  named40 = list(list(required = 40L, names = "future_thing"), "'future_thing' \\(bit 40\\).*Update fstx")
)
for (nm in names(refuse)) {
  f <- sprintf("reserved_%s.fstx", nm)
  do.call(forge, c(list(f), refuse[[nm]][[1]]))
  add_refuse(f, refuse[[nm]][[2]])
}
forge("reserved_optional50.fstx", optional = 50L, names = "later_index")  # unknown optional bit: reads
add_read("reserved_optional50.fstx", base)
forge("reserved_profile3.fstx", profile = 3L)                              # unknown profile: reads
add_read("reserved_profile3.fstx", base)
stopifnot(identical(fstx_meta(gpath("reserved_profile3.fstx"))$profile, "unknown"))
unlink(bf)

# ---------------------------------------------------------------- manifest
fl <- do.call(rbind, files)
fl$bytes <- file.size(gpath(fl$file))
fl$md5 <- unname(tools::md5sum(gpath(fl$file)))
fl <- fl[c("file", "bytes", "md5", setdiff(names(fl), c("file", "bytes", "md5")))]
write.csv(fl, gpath("files.csv"), row.names = FALSE, fileEncoding = "UTF-8")
write.csv(do.call(rbind, expected), gpath("expected.csv"), row.names = FALSE, fileEncoding = "UTF-8")
total <- sum(file.size(list.files(out, full.names = TRUE)))
cat(sprintf("GOLDEN files %d fstx bytes %d total bytes in folder %d\n", nrow(fl), sum(fl$bytes), total))
for (k in order(-fl$bytes)[1:8]) cat(sprintf("  %-28s %7d\n", fl$file[k], fl$bytes[k]))
if (total > 300 * 1024) stop("the golden set is over 300 KB")
