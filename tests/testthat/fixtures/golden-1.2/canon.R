# Golden files, fstx format 1.2: what "the same content" means. Sourced by make_golden.R, make_golden_v10.R and
# test-compat-golden12.R, so all describe a table the same way. Base R only: no bit64, no blob
# (an integer64 vector is a double vector of int64 bit patterns with class "integer64"; a blob is a list of raw
# vectors with a class).
#
# As golden-1.0/canon.R, plus the two 1.2 types:
#   integer64 (an int64 column, or the fstx 1.0 layout): the 8-byte little-endian bit patterns, NA = INT64_MIN;
#             its NA count counts INT64_MIN patterns (is.na() needs bit64 for that)
#   binary (a list of raw vectors, NULL = missing): per value a u32 length then the bytes, or 0xFFFFFFFF for NULL
# A column is described by its class, row count, NA count and two MD5s (data, attributes).

golden_md5_raw <- function(r) {
  f <- tempfile()
  on.exit(unlink(f))
  writeBin(if (length(r)) r else raw(0), f)
  unname(tools::md5sum(f))
}

golden_canon <- function(x) {
  if (is.list(x)) {  # binary
    na4 <- as.raw(c(255, 255, 255, 255))
    parts <- lapply(unclass(x), function(b) {
      if (is.null(b)) return(na4)
      c(writeBin(length(b), raw(), size = 4, endian = "little"), as.raw(b))
    })
    r <- unlist(parts, use.names = FALSE)
    return(if (is.null(r)) raw(0) else r)
  }
  v <- unclass(x)
  attributes(v) <- NULL
  if (is.character(v)) {
    na4 <- as.raw(c(255, 255, 255, 255))
    parts <- lapply(v, function(s) {
      if (is.na(s)) return(na4)
      b <- charToRaw(enc2utf8(s))
      c(writeBin(length(b), raw(), size = 4, endian = "little"), b)
    })
    r <- unlist(parts, use.names = FALSE)
    if (is.null(r)) raw(0) else r
  } else if (is.double(v)) {
    writeBin(v, raw(), size = 8, endian = "little")
  } else {
    writeBin(as.integer(v), raw(), size = 4, endian = "little")
  }
}

# NA count without bit64: INT64_MIN patterns for integer64, NULL elements for binary, is.na() otherwise.
golden_nas <- function(x) {
  if (is.list(x)) return(sum(vapply(unclass(x), is.null, NA)))
  if (inherits(x, "integer64")) {
    if (!length(x)) return(0)
    m <- matrix(golden_canon(x), nrow = 8L)
    return(sum(colSums(m == as.raw(c(0, 0, 0, 0, 0, 0, 0, 0x80))) == 8L))
  }
  sum(is.na(x))
}

golden_attr_text <- function(a) {
  if (!length(a)) return("")
  val <- vapply(a, function(v) paste(enc2utf8(as.character(v)), collapse = "\x1f"), "")
  paste(enc2utf8(names(a)), val, sep = "=", collapse = "\n")
}

golden_attr_md5 <- function(a) golden_md5_raw(charToRaw(golden_attr_text(a)))

# One row per column.
golden_describe <- function(df) {
  k <- ncol(df)
  cols <- unclass(df)
  data.frame(
    index = seq_len(k),
    column = enc2utf8(names(df)),
    class = vapply(cols, function(x) paste(class(x), collapse = "/"), "", USE.NAMES = FALSE),
    rows = rep(nrow(df), k),
    nas = vapply(cols, golden_nas, 0, USE.NAMES = FALSE),
    data_md5 = vapply(cols, function(x) golden_md5_raw(golden_canon(x)), "", USE.NAMES = FALSE),
    attr_md5 = vapply(cols, function(x) golden_attr_md5(attributes(x)), "", USE.NAMES = FALSE),
    stringsAsFactors = FALSE
  )
}

# Table-level attributes other than names (class, row names, extra attributes).
golden_table_md5 <- function(df) {
  a <- attributes(df)
  a[["names"]] <- NULL
  golden_attr_md5(a)
}

# ---------------------------------------------------------------- building the 1.2 types without bit64
# integer64 from the high 32 bits (signed, -2^31 .. 2^31-1) and the low 32 bits (0 .. 2^32-1), as doubles.
golden_i64_parts <- function(hi, lo) {
  n <- max(length(hi), length(lo))
  hi <- rep_len(as.double(hi), n)
  lo <- rep_len(as.double(lo), n)
  lo <- ifelse(lo >= 2^31, lo - 2^32, lo)
  w <- suppressWarnings(as.integer(rbind(lo, hi)))  # -2^31 gives NA_integer_, whose bits are -2^31
  r <- writeBin(w, raw(), size = 4, endian = "little")
  structure(readBin(r, "double", n, size = 8, endian = "little"), class = "integer64")
}
# integer64 from whole numbers with |v| <= 2^53 (NA gives the missing value).
golden_i64 <- function(v) {
  v <- as.double(v)
  na <- is.na(v)
  v[na] <- 0
  hi <- floor(v / 2^32)
  x <- golden_i64_parts(hi, v - hi * 2^32)
  if (any(na)) x <- golden_i64_set(x, which(na), golden_i64_na(sum(na)))
  x
}
golden_i64_na <- function(n = 1L) golden_i64_parts(rep(-2^31, n), rep(0, n))
golden_i64_max <- function(n = 1L) golden_i64_parts(rep(2^31 - 1, n), rep(2^32 - 1, n))
golden_i64_min1 <- function(n = 1L) golden_i64_parts(rep(-2^31, n), rep(1, n))   # INT64_MIN + 1
# c(), subsetting and replacement that keep the bits and the class whether or not bit64 is loaded
golden_i64_c <- function(...) structure(unlist(lapply(list(...), unclass), use.names = FALSE), class = "integer64")
golden_i64_take <- function(x, i) structure(unclass(x)[i], class = "integer64")
golden_i64_set <- function(x, i, value) {
  v <- unclass(x)
  v[i] <- unclass(value)
  structure(v, class = "integer64")
}
golden_i64_rep <- function(x, n) structure(rep_len(unclass(x), n), class = "integer64")

# Binary columns: a list of raw vectors with a class (none, blob, arrow_binary, AsIs, vctrs list_of).
golden_bin <- function(x, cls = c("none", "blob", "arrow_binary", "AsIs", "list_of")) {
  x <- unname(x)
  switch(match.arg(cls),
    none = x,
    blob = structure(x, ptype = raw(0), class = c("blob", "vctrs_list_of", "vctrs_vctr", "list")),
    arrow_binary = structure(x, class = c("arrow_binary", "vctrs_vctr", "list")),
    AsIs = structure(x, class = "AsIs"),
    list_of = structure(x, ptype = raw(0), class = c("vctrs_list_of", "vctrs_vctr", "list")))
}

# Columns of the same type joined end to end (integer64 and binary keep their class and attributes).
golden_col_c <- function(parts) {
  a <- attributes(parts[[1]])
  if (is.list(parts[[1]])) {
    v <- do.call(c, lapply(parts, unclass))
    attributes(v) <- a[setdiff(names(a), "names")]
    return(v)
  }
  if (inherits(parts[[1]], "integer64")) return(do.call(golden_i64_c, parts))
  do.call(c, parts)
}
# Data frames with the same columns stacked (rbind() would lose the class of integer64 without bit64).
golden_df_rbind <- function(dfs) {
  x <- lapply(names(dfs[[1]]), function(nm) golden_col_c(lapply(dfs, `[[`, nm)))
  names(x) <- names(dfs[[1]])
  list2DF(x)
}

# The table make_golden_v10.R writes with fstx 1.0.2 (an integer64 column in the 1.0 layout) and make_golden.R
# checks with fstx 1.2. Values avoid R's NA_real_ bit pattern (low word 1954), which a 1.0 double page marks as
# missing.
golden_v10_table <- function() {
  n <- 300L
  set.seed(102)
  big <- golden_i64_c(golden_i64(c(0, 1, -1, 123456789, -987654321, 2^40, -(2^40), 2^53, NA)),
                      golden_i64_parts(c(2^21, -2^21 - 1), c(1, 2^32 - 1)),     # 2^53 + 1 and -(2^53 + 1)
                      golden_i64_max(), golden_i64_min1())
  idx <- sample.int(length(big), n, TRUE)
  idx[1:length(big)] <- seq_along(big)
  list2DF(list(
    id = seq_len(n),
    big = golden_i64_take(big, idx),
    small = golden_i64(sample(c(-3:40, NA), n, TRUE)),
    s = sample(c("a", "bb", NA, "text"), n, TRUE)
  ))
}
