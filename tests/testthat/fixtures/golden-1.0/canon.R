# Golden files, fstx format 1.0 (WS8): what "the same content" means. Sourced by make_golden.R and by
# test-compat-golden.R, so both describe a table the same way. Base R only.
#
# A column is described by its class, row count, NA count and two MD5s:
#   data: int32 little-endian for integer, logical, factor codes and integer Dates (NA = int32 min);
#         the 8-byte little-endian bit patterns for doubles (NA_real_ and other NaNs keep their bits);
#         for strings, per value a u32 length then the UTF-8 bytes, or 0xFFFFFFFF for NA.
#   attributes: name=value lines (values as UTF-8 text), in the order R holds them.
# Text is converted to UTF-8 bytes first, so encoding marks never change a description.

golden_md5_raw <- function(r) {
  f <- tempfile()
  on.exit(unlink(f))
  writeBin(if (length(r)) r else raw(0), f)
  unname(tools::md5sum(f))
}

golden_canon <- function(x) {
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

golden_attr_text <- function(a) {
  if (!length(a)) return("")
  val <- vapply(a, function(v) paste(enc2utf8(as.character(v)), collapse = "\x1f"), "")
  paste(enc2utf8(names(a)), val, sep = "=", collapse = "\n")
}

golden_attr_md5 <- function(a) golden_md5_raw(charToRaw(golden_attr_text(a)))

# One row per column.
golden_describe <- function(df) {
  k <- ncol(df)
  data.frame(
    index = seq_len(k),
    column = enc2utf8(names(df)),
    class = vapply(df, function(x) paste(class(x), collapse = "/"), "", USE.NAMES = FALSE),
    rows = rep(nrow(df), k),
    nas = vapply(df, function(x) sum(is.na(x)), 0, USE.NAMES = FALSE),
    data_md5 = vapply(df, function(x) golden_md5_raw(golden_canon(x)), "", USE.NAMES = FALSE),
    attr_md5 = vapply(df, function(x) golden_attr_md5(attributes(x)), "", USE.NAMES = FALSE),
    stringsAsFactors = FALSE
  )
}

# Table-level attributes other than names (class, row names, extra attributes).
golden_table_md5 <- function(df) {
  a <- attributes(df)
  a[["names"]] <- NULL
  golden_attr_md5(a)
}
