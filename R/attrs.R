# FST-X 1.0: attribute blobs (Column.attrs, Footer.table_attrs; spec section 9.4).
#
# A named list of attributes whose values are plain logical, integer, double or character vectors (which may
# carry attributes of their own, two levels deep) is stored in the FXA1 encoding, which the reader decodes with
# bounds checks and never through unserialize(). Anything else falls back to serialize(x, NULL, xdr = FALSE),
# as fstx 1.0 first did. The FXA1 magic "FXA" can never start an R serialization ("A\n", "B\n" or "X\n").

.fxa_magic <- as.raw(c(0x46, 0x58, 0x41, 0x01))   # "FXA", version 1
.fxa_types <- c(logical = 1L, integer = 2L, double = 3L, character = 4L)

.fxa_i32 <- function(x) writeBin(as.integer(x), raw(), size = 4L, endian = "little")

# one attribute list (level 1: the blob; level 2: a value's own attributes; level 3: theirs) -> raw, or NULL
# if it does not fit. Level 3 is what a data.table index needs (its order vectors carry "starts", "anyna", ...).
.fxa_list <- function(a, level) {
  nm <- names(a)
  if (length(a) && (is.null(nm) || anyNA(nm) || !all(nzchar(nm)) || anyDuplicated(nm))) return(NULL)
  out <- vector("list", length(a) + 1L)
  out[[1L]] <- .fxa_i32(length(a))
  for (k in seq_along(a)) {
    v <- a[[k]]
    ty <- .fxa_types[typeof(v)]
    if (is.na(ty) || length(v) > .Machine$integer.max - 1) return(NULL)
    va <- attributes(v)
    if (!is.null(va) && level >= 3L) return(NULL)
    attributes(v) <- NULL
    body <- switch(typeof(v),
      double = writeBin(v, raw(), size = 8L, endian = "little"),
      character = {
        if (any(Encoding(v) == "bytes")) return(NULL)
        v <- enc2utf8(v)
        na <- is.na(v)
        len <- nchar(v, type = "bytes")
        len[na] <- -1L
        if (sum(as.double(len[!na])) > .Machine$integer.max - 1) return(NULL)
        b <- charToRaw(paste(v[!na], collapse = ""))
        if (length(b) != sum(len[!na])) return(NULL)
        c(.fxa_i32(len), b)
      },
      .fxa_i32(v))   # logical (0, 1, NA) and integer
    sub <- if (is.null(va)) .fxa_i32(0L) else .fxa_list(va, level + 1L)
    if (is.null(sub)) return(NULL)
    nb <- charToRaw(enc2utf8(nm[k]))
    out[[k + 1L]] <- c(.fxa_i32(length(nb)), nb, .fxa_i32(ty), .fxa_i32(length(v)), body, sub)
  }
  unlist(out, use.names = FALSE)
}

# attributes -> blob (NULL for none): FXA1 when they fit, else R serialization
.attr_encode <- function(a) {
  if (!length(a)) return(NULL)
  b <- tryCatch(.fxa_list(a, 1L), error = function(e) NULL)
  if (is.null(b)) serialize(a, NULL, xdr = FALSE) else c(.fxa_magic, b)
}

# FXA1 blob -> named list; any problem is an R error (turned into "corrupt" by .attr_decode)
.fxa_parse <- function(b) {
  n <- length(b)
  pos <- 4L                                          # bytes used so far (the magic)
  take <- function(k) {
    if (is.na(k) || k < 0 || k > n - pos) stop("truncated or wrong length", call. = FALSE)
    r <- b[pos + seq_len(k)]
    pos <<- pos + k
    r
  }
  i32 <- function(k = 1L) readBin(take(4 * k), "integer", k, size = 4L, endian = "little")
  left <- function() n - pos
  one_list <- function(level) {
    cnt <- i32()
    # an entry takes at least 17 bytes: name length, a 1-byte name, type, length, nested count
    if (is.na(cnt) || cnt < 0L || cnt > left() / 17) stop("attribute count", call. = FALSE)
    if (cnt > 0L && level > 3L) stop("attributes nested too deep", call. = FALSE)
    out <- vector("list", cnt)
    nms <- character(cnt)
    for (k in seq_len(cnt)) {
      nl <- i32()
      if (is.na(nl) || nl < 1L) stop("name length", call. = FALSE)
      nb <- take(nl)
      if (any(nb == as.raw(0))) stop("NUL in a name", call. = FALSE)
      nm <- rawToChar(nb)
      Encoding(nm) <- "UTF-8"
      if (!validUTF8(nm)) stop("a name is not valid UTF-8", call. = FALSE)
      ty <- i32()
      len <- i32()
      if (is.na(len) || len < 0L) stop("value count", call. = FALSE)
      if (is.na(ty) || ty < 1L || ty > 4L) stop(sprintf("value type %s", format(ty)), call. = FALSE)
      v <- if (ty == 3L) {
        if (len > left() / 8) stop("value count", call. = FALSE)
        readBin(take(8 * len), "double", len, size = 8L, endian = "little")
      } else {
        if (len > left() / 4) stop("value count", call. = FALSE)
        x <- i32(len)
        if (ty == 1L) {
          if (any(!is.na(x) & x != 0L & x != 1L)) stop("logical value other than 0, 1, NA", call. = FALSE)
          as.logical(x)
        } else if (ty == 2L) {
          x
        } else {
          if (anyNA(x) || any(x < -1L)) stop("string length", call. = FALSE)
          ok <- x >= 0L
          lens <- x[ok]
          tot <- sum(as.double(lens))
          if (tot > left()) stop("string bytes", call. = FALSE)
          s <- take(tot)
          if (any(s == as.raw(0))) stop("NUL in a string", call. = FALSE)
          v <- rep(NA_character_, len)
          if (length(lens)) {   # NUL-terminate each string, then split them in one step
            buf <- raw(tot + length(lens))
            buf[seq_len(tot) + rep.int(seq_along(lens) - 1L, lens)] <- s
            str <- readBin(buf, "character", length(lens))
            if (length(str) != length(lens) || !identical(nchar(str, type = "bytes"), lens))
              stop("string bytes", call. = FALSE)
            Encoding(str) <- "UTF-8"
            v[ok] <- str
          }
          v
        }
      }
      a <- one_list(level + 1L)
      if (length(a)) attributes(v) <- a
      out[k] <- list(v)
      nms[k] <- nm
    }
    if (anyDuplicated(nms)) stop("duplicate names", call. = FALSE)
    names(out) <- nms
    out
  }
  res <- one_list(1L)
  if (pos != n) stop("bytes after the end", call. = FALSE)
  res
}

.attr_corrupt <- function(path, what, why)
  stop(sprintf("fstx: '%s' is corrupt (%s attributes: %s)", path, what, why), call. = FALSE)

# blob -> named list (NULL for none). FXA1 is decoded safely; anything else goes through unserialize() when
# `unser` is TRUE and is skipped (NULL) when it is FALSE. `what` names the blob in the error ("table", "column 'x'").
.attr_decode <- function(b, path, what, unser = TRUE) {
  if (is.null(b)) return(NULL)
  if (length(b) >= 3L && identical(b[1:3], .fxa_magic[1:3])) {
    if (length(b) < 4L || b[4L] != .fxa_magic[4L])
      .attr_corrupt(path, what, sprintf("attribute encoding version %d", if (length(b) < 4L) 0L else as.integer(b[4L])))
    return(tryCatch(.fxa_parse(b), error = function(e) .attr_corrupt(path, what, conditionMessage(e))))
  }
  if (unser) unserialize(b) else NULL
}

