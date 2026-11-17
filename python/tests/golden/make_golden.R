# Golden files for the Python port of fstx (python/DESIGN.md). Synthetic values only.
#
#   Rscript make_golden.R [out dir]          (default: the folder this script is in)
#
# Needs fstx 1.0.0, bit64, data.table, tibble, hms and jsonlite. Writes, into the out dir:
#   <case>.fstx         small files, one per type or edge case (list: `cases` below and manifest.json)
#   <case>.json         what the file holds, so a Python test can check a read without R (format below)
#   manifest.json       every case: what it covers and whether R -> Python -> R should give an identical() table
#   format-1.0.json     the same per-column expectations for the package's own format golden files
#                       (tests/testthat/fixtures/golden-1.0, every encoding x missing-value mode x codec mix and the
#                       refusals of reserved numbers); those files are read from there, not copied
#
# <case>.json, per column:
#   name, r_typeof, r_class, stored (phys, logical, ordered, levels, tzone / tzone_present, blob = none | FXA1 |
#   R-serialization), arrow_type (what DESIGN.md says Python returns), n, nulls (Python null count under that
#   mapping), md5 (canonical bytes, below), values (by kind, below), attrs (other R attributes), and pages.
# Values: logical true/false/null; integer numbers/null; double numbers or "NaN" "Inf" "-Inf" "-0", null = NA,
# plus `bits` (16 hex digits, big-endian, the exact IEEE pattern R holds); integer64 decimal strings plus bits;
# character strings/null (strings over 2,000 bytes as {bytes, md5}; `hex` added when a value is not valid UTF-8,
# and the value itself is then null);
# factor `codes` (1-based, null = NA) and `levels` (null = an NA level); Date `days` (numbers as for double) and
# `date32` (whole days, floor, null when not finite); POSIXct `seconds` and `us` (round half even of seconds x 1e6,
# decimal string, null when not finite) and `iso_utc`. Columns over 100 rows give only their first 12 values
# (values_truncated = true); the md5 covers every value.
# Canonical md5 of a column, as R holds it after the read:
#   logical / integer (incl. Date stored as int): int32 LE per value, NA = 0x80000000
#   double (incl. Date, POSIXct, difftime): float64 LE bits per value, every NA (NaN with low word 1954) written as
#     0x7FF00000000007A2, other NaN bits as they are
#   integer64: the 8 stored bytes LE per value (NA = 0x8000000000000000)
#   character: per value int32 LE byte length (-1 = NA) then the bytes
#   factor: int32 codes as integer, then the levels as character
# The md5 of no bytes is d41d8cd98f00b204e9800998ecf8427e.

args <- commandArgs(TRUE)
me <- normalizePath(sub("^--file=", "", grep("^--file=", commandArgs(FALSE), value = TRUE)[1]), winslash = "/")
here <- dirname(me)
out <- if (length(args)) normalizePath(args[1], winslash = "/", mustWork = FALSE) else here
repo <- normalizePath(file.path(here, "..", "..", ".."), winslash = "/")
suppressPackageStartupMessages({
  library(fstx)
  library(bit64)
  library(jsonlite)
})
stopifnot(identical(as.character(packageVersion("fstx")), "1.0.0"),
          requireNamespace("data.table", quietly = TRUE), requireNamespace("tibble", quietly = TRUE),
          requireNamespace("hms", quietly = TRUE))
dir.create(out, recursive = TRUE, showWarnings = FALSE)
unlink(list.files(out, "\\.(fstx|json)$", full.names = TRUE))
gp <- function(f) file.path(out, f)

# ---------------------------------------------------------------- helpers
i32 <- function(v) writeBin(as.integer(v), raw(), size = 4L, endian = "little")
bare <- function(v) { v <- unclass(v); attributes(v) <- NULL; v }
hex <- function(r) paste(as.character(r), collapse = "")
md5 <- function(r) unname(tools::md5sum(bytes = r))
dbl_hex <- function(v) {
  v <- bare(v)
  if (!length(v)) return(list())
  m <- matrix(as.character(writeBin(as.double(v), raw(), size = 8L, endian = "big")), nrow = 8L)
  as.list(apply(m, 2L, paste, collapse = ""))
}
num <- function(s) structure(s, class = "json")   # written as a bare JSON number
dbl_json <- function(v) lapply(bare(v), function(e) {
  if (is.nan(e)) "NaN" else if (is.na(e)) NULL else if (e == Inf) "Inf" else if (e == -Inf) "-Inf"
  else if (e == 0 && 1 / e < 0) "-0" else num(sprintf("%.17g", e))
})
int_json <- function(v) lapply(as.integer(bare(v)), function(e) if (is.na(e)) NULL else e)
str_raw <- function(s) {
  if (!length(s)) return(raw(0))
  unlist(lapply(s, function(e) if (is.na(e)) i32(-1L) else { b <- charToRaw(e); c(i32(length(b)), b) }),
         use.names = FALSE)
}
canon_raw <- function(x) {
  if (is.factor(x)) return(c(i32(bare(x)), str_raw(levels(x))))
  if (inherits(x, "integer64")) return(writeBin(bare(x), raw(), size = 8L, endian = "little"))
  switch(typeof(x),
    logical = , integer = i32(bare(x)),
    double = { v <- bare(x); v[is.na(v) & !is.nan(v)] <- NA_real_; writeBin(v, raw(), size = 8L, endian = "little") },
    character = str_raw(bare(x)),
    stop("no canonical form for ", typeof(x)))
}
blob_kind <- function(b) if (is.null(b)) "none" else if (length(b) >= 3L && identical(b[1:3], as.raw(c(0x46, 0x58, 0x41)))) "FXA1" else "R-serialization"

# An attribute value as JSON: type, values, its own attributes. Anything that is not a plain vector: opaque.
attr_json <- function(v) {
  if (!is.atomic(v) || is.complex(v) || is.raw(v) || is.null(v)) return(list(type = typeof(v), opaque = TRUE))
  a <- attributes(v)
  vals <- switch(typeof(v), logical = lapply(bare(v), function(e) if (is.na(e)) NULL else e),
                 integer = int_json(v), double = dbl_json(v), character = lapply(bare(v), function(e) if (is.na(e)) NULL else e))
  r <- list(type = typeof(v), values = vals)
  if (length(a)) r$attrs <- lapply(a, attr_json)
  r
}

py_type <- function(x) {
  if (inherits(x, "integer64")) return("int64")
  if (is.factor(x)) return(sprintf("dictionary<values=string, indices=int32, ordered=%d>", as.integer(is.ordered(x))))
  if (inherits(x, "Date")) return("date32[day]")
  if (inherits(x, "POSIXct")) {
    tz <- attr(x, "tzone")
    return(if (is.null(tz) || !nzchar(tz[1L])) "timestamp[us]" else sprintf("timestamp[us, tz=%s]", tz[1L]))
  }
  switch(typeof(x), logical = "bool", integer = "int32", double = "double", character = "string")
}
py_nulls <- function(x) {
  if (inherits(x, "integer64")) return(sum(is.na(x)))
  if (is.factor(x)) return(sum(is.na(bare(x))))
  if (inherits(x, c("Date", "POSIXct")) && is.double(x)) return(sum(!is.finite(bare(x))))
  if (is.double(x)) { v <- bare(x); return(sum(is.na(v) & !is.nan(v))) }
  sum(is.na(bare(x)))
}

col_desc <- function(x, name, m, i, pg) {
  stored <- list(phys = m$phys[i], logical = if (nzchar(m$logical[i])) m$logical[i] else "none",
                 tzone_present = !is.na(m$tzone[i]), blob = blob_kind(m$col_attrs[[i]]))
  if (stored$logical == "factor") {
    stored$ordered <- is.ordered(x)
    stored$levels <- as.list(levels(x))
  }
  if (stored$tzone_present) stored$tzone <- m$tzone[i]
  b <- m$col_attrs[[i]]
  if (!is.null(b) && length(b) <= 2048L) stored$blob_hex <- hex(b)
  d <- list(name = name, r_typeof = typeof(x), r_class = as.list(oldClass(x)), stored = stored,
            arrow_type = py_type(x), n = length(x), nulls = py_nulls(x), md5 = md5(canon_raw(x)))
  if (inherits(x, "integer64")) {
    d$values <- lapply(as.character(x), function(e) if (is.na(e)) NULL else e)
    d$bits <- dbl_hex(x)
  } else if (is.factor(x)) {
    d$codes <- int_json(x)
    d$levels <- lapply(levels(x), function(e) if (is.na(e)) NULL else e)
  } else if (inherits(x, "Date")) {
    d$days <- if (is.double(x)) dbl_json(x) else int_json(x)
    if (is.double(x)) d$bits <- dbl_hex(x)
    d$date32 <- lapply(bare(x), function(e) if (!is.finite(e)) NULL else num(sprintf("%.0f", floor(e))))
  } else if (inherits(x, "POSIXct")) {
    d$seconds <- if (is.double(x)) dbl_json(x) else int_json(x)
    if (is.double(x)) d$bits <- dbl_hex(x)
    d$us <- lapply(bare(x), function(e) if (!is.finite(e)) NULL else sprintf("%.0f", round(e * 1e6)))
    d$iso_utc <- lapply(bare(x), function(e) if (!is.finite(e)) NULL else
      format(as.POSIXct(e, tz = "UTC", origin = "1970-01-01"), "%Y-%m-%dT%H:%M:%OS6Z"))
  } else if (is.character(x)) {
    v <- bare(x)
    d$values <- lapply(v, function(e) {
      if (is.na(e)) NULL
      else if (nchar(e, type = "bytes") > 2000L) list(bytes = nchar(e, type = "bytes"), md5 = md5(charToRaw(e)))
      else if (!validUTF8(e)) NULL   # given in `hex`
      else e
    })
    if (!all(validUTF8(v[!is.na(v)]))) d$hex <- lapply(v, function(e) if (is.na(e)) NULL else hex(charToRaw(e)))
  } else if (is.double(x)) {
    d$values <- dbl_json(x)
    d$bits <- dbl_hex(x)
  } else if (is.integer(x)) {
    d$values <- int_json(x)
  } else if (is.logical(x)) {
    d$values <- lapply(bare(x), function(e) if (is.na(e)) NULL else e)
  }
  a <- attributes(x)
  a[c("class", "levels", if (stored$tzone_present || stored$logical == "POSIXct") "tzone")] <- NULL
  if (length(a)) d$attrs <- lapply(a, attr_json)
  if (!is.null(pg) && nrow(pg)) {
    p <- pg[pg$column == i, c("row_group", "dictionary", "rows", "nulls", "codec", "kind", "na", "encoding"), drop = FALSE]
    rownames(p) <- NULL
    d$pages <- p
  }
  d
}

# Columns of more than 100 rows: the first 12 values only (values_truncated = true); md5, n and nulls cover the rest.
shorten <- function(cdesc) {
  cut <- cdesc$n > 100
  if (cut) for (k in c("values", "bits", "codes", "days", "date32", "seconds", "us", "iso_utc", "hex"))
    if (!is.null(cdesc[[k]])) cdesc[[k]] <- cdesc[[k]][1:12]
  cdesc$values_truncated <- cut
  cdesc
}

table_desc <- function(y, path) {
  m <- fstx:::fstx_meta_impl(path)
  pg <- if (m$nrow > 0 && m$ncol > 0) fstx_pages(path) else NULL
  ta <- attributes(y)
  auto <- .row_names_info(y) < 0L
  tb <- list(class = as.list(class(y)), blob = blob_kind(m$table_attrs),
             row_names = if (auto) "automatic" else attr_json(attr(y, "row.names")))
  if (!is.null(m$table_attrs) && length(m$table_attrs) <= 2048L) tb$blob_hex <- hex(m$table_attrs)
  ta[c("names", "row.names", "class", ".internal.selfref")] <- NULL
  if (length(ta)) tb$attrs <- lapply(ta, attr_json)
  cols <- lapply(seq_along(y), function(i) shorten(col_desc(y[[i]], names(y)[i], m, i, pg)))
  list(nrow = m$nrow, ncol = m$ncol, profile = m$profile, row_groups = m$row_groups, file_bytes = m$file_size,
       table = tb, columns = cols)
}

manifest <- list()
# R -> Python -> R through fstx.to_polars() and a polars DataFrame, which carries no fstx metadata: the cases
# that do not come back identical(), and why. Every other case is expected identical.
polars_notes <- c(
  zero_rows = "not identical: an empty polars Categorical has no levels and no order, and an empty Int64 comes back integer (int64 = 'auto')",
  all_na = "not identical: an all-null polars Categorical has no levels, and an all-null Int64 comes back integer (int64 = 'auto')",
  strings_invalid_utf8 = "not identical (as pyarrow)",
  datetimes = "not identical: polars carries no metadata, so tzone \"\" and no tzone both come back as UTC",
  datetimes_subus = "not identical (as pyarrow)",
  datetimes_odd = "not identical (as pyarrow); the integer POSIXct comes back double",
  dates = "not identical: the integer Date comes back double (polars carries no metadata)",
  dates_odd = "not identical (as pyarrow)",
  factors = "not identical: a polars Categorical keeps only the levels in use, in order of first appearance, and no order",
  factor_na_level = "not identical: to_polars turns the column into strings",
  classes_blob = "not identical: classes kept in the attribute blob are lost (polars carries no metadata)",
  difftime = "not identical: class and units are lost, the values come back as plain doubles",
  attrs_fxa1 = "not identical: attributes are lost (polars carries no metadata)",
  attrs_serialized = "not identical: attributes are lost (polars carries no metadata)",
  rownames_char = "not identical: row names are lost (polars has no index)",
  rownames_int = "not identical: row names are lost (polars has no index)",
  datatable = "not identical: comes back a plain data.frame (class, key and index are lost)",
  tibble = "not identical: comes back a plain data.frame",
  names = "not written: polars refuses repeated column names (to_polars raises ValueError)")
# Writes x (with write_fstx, or `writer` for the codec cases), checks it reads back identical() in R, writes the
# sidecar and records the case. pyarrow / pandas: the expected R -> Python -> R result ("identical" or why not);
# the polars one comes from polars_notes.
case <- function(name, x, about, pyarrow = "identical", pandas = "identical", writer = NULL, args = list()) {
  f <- paste0(name, ".fstx")
  p <- gp(f)
  if (is.null(writer)) do.call(write_fstx, c(list(x, p, threads = 1L), args)) else writer(x, p)
  y <- read_fstx(p, threads = 1L)
  if (!identical(y, x)) stop(sprintf("%s does not read back identical() in R", f))
  d <- c(list(file = f, about = about, fstx = "1.0.0", write_args = args), table_desc(y, p))
  write_json(d, gp(paste0(name, ".json")), auto_unbox = TRUE, null = "null", na = "null", pretty = TRUE,
             digits = NA, json_verbatim = TRUE)
  manifest[[length(manifest) + 1L]] <<- list(file = f, about = about, bytes = file.size(p),
                                             roundtrip_pyarrow = pyarrow, roundtrip_pandas = pandas,
                                             roundtrip_polars = if (name %in% names(polars_notes))
                                               polars_notes[[name]] else "identical")
  cat(sprintf("%-28s %6.0f bytes\n", f, file.size(p)))
}

# ---------------------------------------------------------------- data
s <- list(  # synthetic text in Indian scripts (greetings, place words), written as escapes
  hi = "नमस्ते दुनिया",
  mr = "महाराष्ट्र",
  bn = "নমস্কার",
  ta = "வணக்கம்",
  te = "నమస్కారం",
  kn = "ನಮಸ್ಕಾರ",
  ml = "നമസ്കാരം",
  gu = "નમસ્તે",
  pa = "ਸਤ ਸ੍ਰੀ ਅਕਾਲ",
  or = "ନମସ୍କାର",
  ur = "السلام علیکم",
  zwj = "क्‍ष",
  rupee = "₹ 1,23,456.78",
  astral = "\U00020b9f")
lv <- c("उत्तर", "தெற்கு", "পূর্ব")
col_hi <- "शहर"
stopifnot(all(validUTF8(unlist(s))), all(Encoding(unlist(s)) == "UTF-8"))

# ---------------------------------------------------------------- cases
case("basic", data.frame(
  lgl = c(TRUE, FALSE, NA, TRUE, FALSE, NA, TRUE, TRUE),
  int = c(0L, 1L, -1L, .Machine$integer.max, -.Machine$integer.max, NA, 42L, 7L),
  dbl = c(0.1, NA_real_, NaN, NA_real_ + 1, -0, Inf, -Inf, 5e-324),
  dbl2 = c(1.7976931348623157e308, -2.5, 1e-300, 3, 1 / 3, 123456.789, -1e15, 2^53 + 2),
  chr = c("a", "", NA, "tab\there", "line\nbreak", "quote\"s", "back\\slash", "spaces  "),
  stringsAsFactors = FALSE),
  "logical, integer (INT_MAX, -INT_MAX, NA), double (NA, NaN, quiet NA = NA_real_ + 1, -0, Inf, -Inf, subnormal, max), character (\"\", NA, control characters)",
  pyarrow = "identical (the quiet NA 0x7FF80000000007A2 comes back as NA_real_ 0x7FF00000000007A2; identical() does not see payloads)",
  pandas = "not identical: to_pandas uses pandas' nullable dtypes, and a Float64 column cannot tell NaN from NA (pandas 3), so R's NaN comes back as NA")

case("empty_0x0", data.frame(), "no columns, no rows")

z <- data.frame(lgl = logical(0), int = integer(0), dbl = double(0), chr = character(0),
                fac = factor(character(0), levels = c("a", "b")),
                ord = factor(character(0), levels = c("lo", "hi"), ordered = TRUE),
                date = as.Date(character(0)), ct = as.POSIXct(character(0), tz = "Asia/Kolkata"),
                i64 = integer64(0), stringsAsFactors = FALSE)
case("zero_rows", z, "every type with 0 rows (the schema alone: levels, ordered, time zone, integer64 class)")

x <- data.frame(row.names = 1:5)
case("zero_cols_5rows", x, "no columns but 5 rows (Arrow: a table of 5 rows and no columns)")

case("all_na", data.frame(
  lgl = NA, int = NA_integer_, dbl = NA_real_, chr = NA_character_,
  fac = factor(c(NA, NA, NA), levels = c("x", "y")), fac0 = factor(c(NA, NA, NA), levels = character(0)),
  date = as.Date(c(NA, NA, NA)), ct = as.POSIXct(c(NA, NA, NA), tz = "UTC"),
  i64 = as.integer64(c(NA, NA, NA)), stringsAsFactors = FALSE),
  "every type all NA, 3 rows (a factor with levels and one with none)")

long_ascii <- strrep("0123456789", 7000L)              # 70,000 bytes, over 64 KB
long_deva <- strrep(paste0(s$hi, " "), 300L)            # about 11 KB of 3-byte characters
case("strings_long", data.frame(s = c(long_ascii, long_deva, "", NA, "short"), stringsAsFactors = FALSE),
     "strings over 64 KB and over 2,000 bytes (given in the sidecar as byte count and md5)")

set.seed(20261117)
n <- 600L
many <- sprintf("id-%05d-%s-%s", sample.int(99999L, n), sample(c("north", "south", "east", "west"), n, TRUE),
                vapply(seq_len(n), function(i) paste(sample(letters, 6L, TRUE), collapse = ""), ""))
many[c(5L, 77L, 300L)] <- NA
few <- sample(c("red", "green", "blue", NA), n, TRUE)
case("strings_many", data.frame(many = many, few = few, stringsAsFactors = FALSE),
     "600 distinct strings (FSST pages) and a 3-value column (dictionary pages: codes + dictionary)")

ind <- c(unlist(s, use.names = FALSE), NA, "")
x <- data.frame(text = ind, stringsAsFactors = FALSE)
x[[col_hi]] <- factor(rep_len(c(lv, NA), nrow(x)), levels = lv)
case("strings_indic", x,
     "Hindi, Marathi, Bengali, Tamil, Telugu, Kannada, Malayalam, Gujarati, Punjabi, Odia, Urdu, a ZWJ conjunct, the rupee sign, a 4-byte character; factor levels and a column name in Indian scripts")

l1 <- c("caf\xe9", "na\xefve", "\xc5ngstr\xf6m", "\xa35", "plain", NA)
Encoding(l1) <- "latin1"
x <- data.frame(s = l1, stringsAsFactors = FALSE)
case("strings_latin1", x,
     "strings marked latin1 in R: stored as UTF-8 (R reads them back marked UTF-8; identical() still TRUE)")

bad <- c("ok", "bad\xffbyte", "\xe0\xa4", "caf\xc3\xa9", NA)   # native strings in a UTF-8 session, not valid UTF-8
stopifnot(isTRUE(l10n_info()[["UTF-8"]]))
case("strings_invalid_utf8", data.frame(s = bad, stringsAsFactors = FALSE),
     "strings that are not valid UTF-8 (allowed by spec 9 rule 7; sidecar gives their bytes in hex)",
     pyarrow = "not identical by default: bad bytes become U+FFFD (identical only with invalid_utf8 = 'binary')",
     pandas = "not identical (as pyarrow)")

case("integer64", data.frame(i64 = as.integer64(c(
  "0", "-1", "1", "9007199254740993", "-9007199254740993", "9223372036854775807", "-9223372036854775807", NA,
  "9218868437227407266",   # bits 0x7FF00000000007A2 = R's NA_real_: the writer marks it missing (null_count)
  "9221120237041092514",   # bits 0x7FF80000000007A2 = quiet NA_real_: counted in null_count, not marked
  "9218868437227405312",   # bits of Inf
  "9221120237041090560"))),  # bits of NaN
  "bit64 integer64 (a double column with class integer64): big values, NA, and values whose bits are NA_real_, Inf or NaN (they must come back as numbers)")

ct <- function(v, tz) .POSIXct(v, tz = tz)
ts_ok <- c(0, 1700000000, 1700000000.5, -86400.25, 1711929600, 1699999999.123456, NA, 253402300799)
x <- data.frame(utc = ct(ts_ok, "UTC"), kolkata = ct(ts_ok, "Asia/Kolkata"), new_york = ct(ts_ok, "America/New_York"),
                tz_empty = ct(ts_ok, ""), tz_none = ct(ts_ok, "UTC"))
attr(x$tz_none, "tzone") <- NULL
case("datetimes", x,
     "POSIXct in UTC, Asia/Kolkata (+05:30), America/New_York (DST), tzone \"\" and no tzone; whole, half and microsecond seconds, before 1970, 9999-12-31 23:59:59, NA",
     pyarrow = "identical if the writer restores tzone \"\" vs none from the field metadata; both read as naive timestamp[us]",
     pandas = "identical through to_pandas() with the metadata kept; tz \"\" vs none is lost without it")

case("datetimes_subus", data.frame(t = ct(c(1700000000.1234567, 1700000000 + 1 / 3, 0.0000004, 1.5e-7), "UTC")),
     "POSIXct with sub-microsecond parts (as Sys.time() gives)",
     pyarrow = "not identical: timestamp[us] rounds to the microsecond (exact only with datetimes = 'float')",
     pandas = "not identical (as pyarrow)")

case("datetimes_odd", data.frame(t = ct(c(Inf, -Inf, NaN, NA, 0), "UTC"),
                                 i = .POSIXct(c(1L, NA, 1700000000L, 0L, -1L), tz = "UTC")),
     "POSIXct Inf, -Inf, NaN, NA (Arrow has only null); a POSIXct stored as integer (kept as an attribute blob, not natively)",
     pyarrow = "not identical: Inf, -Inf and NaN become null and come back NA (exact only with datetimes = 'float'); the integer POSIXct comes back as int32 storage only if the writer keeps phys from the metadata",
     pandas = "not identical (as pyarrow)")

case("dates", data.frame(
  dbl = as.Date(c("1970-01-01", "2026-11-17", "1947-08-15", "0001-01-01", "9999-12-31", NA)),
  int = structure(c(0L, 20774L, -8175L, -719162L, 2932896L, NA), class = "Date")),
  "Date stored as double (as.Date) and as integer, from year 1 to 9999, NA",
  pyarrow = "identical if the writer restores double vs integer storage from the field metadata (date32 alone cannot tell)",
  pandas = "identical if dates stay datetime.date objects (pyarrow's default) and the metadata is kept; datetime64[ns] cannot hold years 1 and 9999")

case("dates_odd", data.frame(d = structure(c(1.5, -0.5, Inf, -Inf, NaN, NA, 3), class = "Date")),
     "fractional Dates, Inf, -Inf, NaN, NA",
     pyarrow = "not identical: date32 floors fractions and makes Inf / NaN null (exact only with datetimes = 'float')",
     pandas = "not identical (as pyarrow)")

case("factors", data.frame(
  plain = factor(c("b", "a", NA, "c", "a", "b"), levels = c("a", "b", "c")),
  unused = factor(c("x", "x", "z", NA, "x", "z"), levels = c("w", "x", "y", "z")),
  ord = factor(c("lo", "hi", "mid", NA, "hi", "lo"), levels = c("lo", "mid", "hi"), ordered = TRUE),
  one = factor(rep("only", 6)),
  indic = factor(rep_len(lv, 6L), levels = rev(lv))),
  "factor with NA codes, with unused levels (kept), ordered factor, one level, Indian-script levels in a non-sorted order")

case("factor_na_level", data.frame(f = factor(c("a", NA, "b", "a"), exclude = NULL)),
     "factor with NA as a level (exclude = NULL): not stored natively (levels in the FXA1 blob); the NA-level code is a value, not a missing value",
     pyarrow = "identical if the writer turns a null dictionary entry back into an NA level",
     pandas = "not identical: pandas categories cannot hold NA, so to_pandas turns the column into strings (with a warning)")

fs <- factor(c("a", "b", "a"))
class(fs) <- c("myfac", "factor")
x <- data.frame(f = fs)
x$d <- structure(c(1, 2, NA), class = c("mydate", "Date"))
x$t <- .POSIXct(c(0, 1, NA), tz = "UTC")
attr(x$t, "tzone") <- c("UTC", "UTC")
case("classes_blob", x,
     "factor, Date and POSIXct with an extra class or a 2-value tzone: R stores them as plain int / double with the class in the attribute blob",
     pyarrow = "identical if the class vector and blob travel in the field metadata",
     pandas = "identical only if to_pandas keeps the metadata")

case("difftime", data.frame(
  secs = as.difftime(c(1.5, NA, -3), units = "secs"), mins = as.difftime(c(1, 2.5, NA), units = "mins"),
  hours = as.difftime(c(0, 24, 1e6), units = "hours"), days = as.difftime(c(1, NA, 0.25), units = "days"),
  weeks = as.difftime(c(52, 1, NaN), units = "weeks"), hms = hms::hms(c(0, 3661, NA))),
  "difftime in every unit and hms (double + class + units in the attribute blob)",
  pyarrow = "identical (double with class and units in the metadata)",
  pandas = "not identical: the pandas Float64 column cannot tell NaN from NA (as basic), so the NaN in 'weeks' comes back NA; classes and units are kept")

x <- data.frame(v = c(1.5, 2.5, NA), code = c(1L, 2L, 3L), txt = c("p", "q", "r"), stringsAsFactors = FALSE)
attr(x$v, "label") <- "Weight in kg"
attr(x$v, "units") <- "kg"
attr(x$code, "labels") <- c(low = 1L, mid = 2L, high = 3L)    # a named vector: FXA1 level 2
comment(x$txt) <- "a comment"
attr(x, "note") <- "table note"
attr(x, "version") <- 3L
case("attrs_fxa1", x, "column labels, units, a named-vector attribute, comment; table attributes (all in FXA1)",
     pyarrow = "identical if the blobs travel in the metadata", pandas = "identical only if the metadata is kept")

x <- data.frame(v = 1:3)
attr(x$v, "meta") <- list(source = "synthetic", n = 3L)    # a list: needs R serialization
attr(x, "info") <- list(a = 1, b = "two")
case("attrs_serialized", x,
     "list-valued column and table attributes: stored with R serialize() (Python cannot decode them; it keeps the bytes)",
     pyarrow = "identical only if the opaque blobs are written back unchanged",
     pandas = "identical only if the metadata is kept")

case("rownames_char", data.frame(v = c(10L, 20L, 30L), row.names = c("r1", "r2", "rन")),
     "character row names (pandas: the index)",
     pyarrow = "identical (row names in the table metadata)",
     pandas = "identical if to_pandas makes them the index and from_pandas stores a string index as row names")

x <- data.frame(v = 1:5, w = letters[1:5])[c(4L, 2L), ]
case("rownames_int", x, "integer row names that are not automatic (x[c(4, 2), ] keeps 4, 2)",
     pyarrow = "identical (row names in the table metadata)",
     pandas = "identical if an integer index other than 0..n-1 becomes integer row names")

dt <- data.table::data.table(k = c(3L, 1L, 2L, 2L), v = c("c", "a", "b", "b"), w = c(0.5, 1, 2, 3))
data.table::setkey(dt, k)
data.table::setindex(dt, v)
case("datatable", dt, "data.table with a key (sorted) and a secondary index (index attribute with order vectors)",
     pyarrow = "not identical: the Python writer drops the index (it cannot tell whether rows were reordered); the key is kept because k is still sorted",
     pandas = "not identical (as pyarrow)")

case("tibble", tibble::tibble(a = 1:3, b = c("x", "y", NA)), "tibble class (tbl_df, tbl, data.frame)",
     pyarrow = "identical (class in the table blob)", pandas = "identical only if the metadata is kept")

x <- data.frame(1:2, 3:4, 5:6, 7:8, 9:10, 11:12)
names(x) <- c("a", "a", "", "with space & punct.", col_hi, "NA")
case("names", x, "duplicate names, an empty name, spaces and punctuation, a Hindi name, the name \"NA\"",
     pyarrow = "identical (Arrow allows duplicate and empty names)",
     pandas = "identical (pandas allows duplicate names; selecting by a duplicate name gives both)")

# the same table under every codec and profile; codec cases keep the codec on every page (test flag keep_codec)
set.seed(3)
n <- 2000L
cd <- data.frame(i = cumsum(sample(0:3, n, TRUE)), v = round(runif(n) * 100, 2),
                 s = sample(c("alpha", "beta", "gamma", "delta"), n, TRUE),
                 w = sprintf("item-%05d-%s", sample.int(99999L, n), sample(letters, n, TRUE)),
                 b = sample(c(TRUE, FALSE, NA), n, TRUE), stringsAsFactors = FALSE)
cd$v[sample.int(n, 50L)] <- NA
keep <- function(codec, level = NULL) function(x, p)
  fstx:::.write_fstx(x, p, compress = codec, level = level, .encoding = "keep_codec", threads = 1L)
case("codec_none", cd, "compress none on every page", writer = keep("none"), args = list(compress = "none"))
case("codec_lz4", cd, "LZ4 block on every page", writer = keep("lz4"), args = list(compress = "lz4"))
case("codec_zstd1", cd, "Zstandard level 1 on every page", writer = keep("zstd", 1L), args = list(compress = "zstd", level = 1L))
case("codec_zstd19", cd, "Zstandard level 19 on every page", writer = keep("zstd", 19L), args = list(compress = "zstd", level = 19L))
case("profile_fast", cd, "profile fast (no codec)", args = list(profile = "fast"))
case("profile_balanced", cd, "profile balanced (zstd 3 where it saves 10%)", args = list(profile = "balanced"))
case("profile_small", cd, "profile small (zstd 9)", args = list(profile = "small"))

# appends: 3 row groups, factor levels grown by the appends, old footers left inside the file
a1 <- data.frame(id = 1:3, f = factor(c("a", "b", "a")), t = .POSIXct(c(0, 1, 2), tz = "Asia/Kolkata"))
a2 <- data.frame(id = 4:5, f = factor(c("c", "a"), levels = c("a", "c")), t = .POSIXct(c(3, NA), tz = "Asia/Kolkata"))
a3 <- data.frame(id = 6L, f = factor("d"), t = .POSIXct(5, tz = "Asia/Kolkata"))
app <- rbind(a1, a2, a3)
app$f <- factor(as.character(app$f), levels = c("a", "b", "c", "d"))
rownames(app) <- NULL
case("appended", app, "written then appended twice: 3 row groups, factor levels a b + c + d, old footers inside the file",
     writer = function(x, p) {
       write_fstx(a1, p, threads = 1L)
       write_fstx(a2, p, append = TRUE, threads = 1L)
       write_fstx(a3, p, append = TRUE, threads = 1L)
     })

# ---------------------------------------------------------------- the package's format golden files
gdir <- file.path(repo, "tests", "testthat", "fixtures", "golden-1.0")
fl <- read.csv(file.path(gdir, "files.csv"), stringsAsFactors = FALSE, fileEncoding = "UTF-8")
fmt <- lapply(seq_len(nrow(fl)), function(i) {
  f <- file.path(gdir, fl$file[i])
  stopifnot(identical(unname(tools::md5sum(f)), fl$md5[i]))
  r <- list(file = fl$file[i], md5_file = fl$md5[i], expect = fl$expect[i])
  if (fl$expect[i] == "refuse") return(c(r, list(message_regex = fl$message[i])))
  y <- read_fstx(f, threads = 1L)
  c(r, table_desc(y, f))
})
write_json(list(source = "tests/testthat/fixtures/golden-1.0 (read from there; not copied)", fstx = "1.0.0",
                files = fmt),
           gp("format-1.0.json"), auto_unbox = TRUE, null = "null", na = "null", pretty = TRUE, digits = NA,
           json_verbatim = TRUE)

write_json(list(fstx = "1.0.0", r = R.version.string, made = format(Sys.Date()),
                note = paste("pcodec (codec 4) is reserved and not written by fstx 1.0: no file can hold it; the",
                             "refusal is covered by reserved_codec4.fstx and reserved_pcodec.fstx in format-1.0.json.",
                             "roundtrip_pyarrow: read_fstx() -> pyarrow.Table -> write_fstx(); roundtrip_pandas: the",
                             "same through fstx.to_pandas() and a pandas DataFrame; roundtrip_polars: the same",
                             "through fstx.to_polars() and a polars DataFrame, which carries no fstx metadata",
                             "(DESIGN.md sections 2 and 4)."),
                cases = manifest),
           gp("manifest.json"), auto_unbox = TRUE, null = "null", na = "null", pretty = TRUE)
tot <- sum(file.size(list.files(out, "\\.(fstx|json)$", full.names = TRUE)))
cat(sprintf("%d cases, %.0f KB in all (.fstx + .json)\n", length(manifest), tot / 1024))
