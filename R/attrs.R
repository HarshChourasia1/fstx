# FST-X 1.0: attribute blobs (Column.attrs, Footer.table_attrs; spec section 9.4).
#
# A named list of attributes whose values are plain logical, integer, double or character vectors (which may
# carry attributes of their own, two levels deep) is stored in the FXA1 encoding, which the reader decodes with
# bounds checks and never through unserialize(). Anything else falls back to serialize(x, NULL, xdr = FALSE),
# as fstx 1.0 first did. The FXA1 magic "FXA" can never start an R serialization ("A\n", "B\n" or "X\n").

.fxa_magic <- as.raw(c(0x46, 0x58, 0x41, 0x01))   # "FXA", version 1
.fxa_types <- c(logical = 1L, integer = 2L, double = 3L, character = 4L)

.fxa_i32 <- function(x) writeBin(as.integer(x), raw(), size = 4L, endian = "little")

# one attribute list (level 1: the blob; level 2: a value's own attributes; level 3: theirs) -> a list of raw
# pieces (joined once by .attr_encode), or NULL if it does not fit. Level 3 is what a data.table index needs (its
# order vectors carry "starts", "anyna", ...).
.fxa_list <- function(a, level) {
  if (!length(a)) return(list(.fxa_i32(0L)))
  nm <- names(a)
  if ((is.null(nm) || anyNA(nm) || !all(nzchar(nm)) || anyDuplicated(nm))) return(NULL)
  nm <- enc2utf8(nm)
  if (!all(validUTF8(nm))) return(NULL)
  out <- list(.fxa_i32(length(a)))
  for (k in seq_along(a)) {
    v <- a[[k]]
    ty <- .fxa_types[typeof(v)]
    if (is.na(ty) || length(v) > .Machine$integer.max - 1) return(NULL)
    va <- attributes(v)
    if (!is.null(va) && level >= 3L) return(NULL)
    attributes(v) <- NULL
    body <- switch(typeof(v),
      double = list(writeBin(v, raw(), size = 8L, endian = "little")),
      character = {
        if (any(Encoding(v) == "bytes")) return(NULL)
        v <- enc2utf8(v)
        na <- is.na(v)
        if (any(na)) v[na] <- ""
        if (!all(validUTF8(v))) return(NULL)
        len <- nchar(v, type = "bytes")
        tot <- sum(len, 0)
        if (tot > .Machine$integer.max - 1) return(NULL)
        b <- charToRaw(paste(v, collapse = ""))
        if (length(b) != tot) return(NULL)
        len[na] <- -1L
        list(.fxa_i32(len), b)
      },
      list(.fxa_i32(v)))   # logical (0, 1, NA) and integer
    sub <- if (is.null(va)) list(.fxa_i32(0L)) else .fxa_list(va, level + 1L)
    if (is.null(sub)) return(NULL)
    nb <- charToRaw(nm[k])
    out <- c(out, list(.fxa_i32(length(nb)), nb, .fxa_i32(c(ty, length(v)))), body, sub)
  }
  out
}

# attributes -> blob (NULL for none): FXA1 when they fit, else R serialization
.attr_encode <- function(a) {
  if (!length(a)) return(NULL)
  b <- tryCatch(.fxa_list(a, 1L), error = function(e) NULL)
  if (is.null(b)) serialize(a, NULL, xdr = FALSE) else unlist(c(list(.fxa_magic), b), use.names = FALSE)
}

# FXA1 blob -> named list; any problem is an R error (turned into "corrupt" by .attr_decode). The bytes are
# read in order from a connection, so no index vectors are built (b[pos + seq_len(k)] made one of 8 bytes per byte).
.fxa_parse <- function(b) {
  n <- length(b)
  con <- rawConnection(b)
  on.exit(close(con))
  readBin(con, "raw", 4L)                            # the magic, checked by the caller
  pos <- 4L                                          # bytes read so far
  need <- function(k) {
    if (is.na(k) || k < 0 || k > n - pos) stop("truncated or wrong length", call. = FALSE)
    pos <<- pos + as.integer(k)
  }
  i32 <- function(k = 1L) {
    need(4 * k)
    readBin(con, "integer", k, size = 4L, endian = "little")
  }
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
      need(nl)
      nb <- readBin(con, "raw", nl)
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
        need(8 * len)
        readBin(con, "double", len, size = 8L, endian = "little")
      } else {
        if (len > left() / 4) stop("value count", call. = FALSE)
        x <- i32(len)
        if (ty == 1L) {
          if (any(!is.na(x) & x != 0L & x != 1L)) stop("logical value other than 0, 1, NA", call. = FALSE)
          as.logical(x)
        } else if (ty == 2L) {
          x
        } else {
          if (len && (anyNA(x) || min(x) < -1L)) stop("string length", call. = FALSE)
          has_na <- len && min(x) < 0L
          if (has_na) {
            ok <- x >= 0L
            x <- x[ok]
          }
          tot <- sum(x, 0)
          need(tot)
          s <- readBin(con, "raw", tot)
          # readChar splits the bytes by the lengths in one step; it warns (or fails) at a NUL. (On the
          # connection itself it needs ten times the memory.)
          nul <- function(e) stop("NUL in a string", call. = FALSE)
          str <- if (length(x)) tryCatch(readChar(s, x, useBytes = TRUE), warning = nul, error = nul) else character(0)
          rm(s)
          if (length(str) != length(x)) stop("string bytes", call. = FALSE)
          Encoding(str) <- "UTF-8"
          if (has_na) {
            v <- rep(NA_character_, len)
            v[ok] <- str
            v
          } else str
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

# Table attributes from a blob, checked before they go on a data frame of `nrow` rows with columns `cols`. The
# writer always stores a class with "data.frame", never names or dim, and row names (if any) one per row; anything
# else is corrupt. Automatic row names in the compact c(NA, -n) form are dropped (rebuilt on read). A data.table
# key (`sorted`) naming columns that are not there is dropped, and so are index order vectors that are not empty
# or a permutation of 1..nrow (data.table rebuilds indices when it needs them). Not checked: whether a key or
# index matches the data.
.table_attrs_check <- function(ta, nrow, cols, path) {
  bad <- function(why) .attr_corrupt(path, "table", why)
  if (!is.list(ta) || (length(ta) && is.null(names(ta)))) bad("not a named list")
  if (any(c("names", "dim", "dimnames") %in% names(ta))) bad("names, dim or dimnames")
  cl <- ta[["class"]]
  if (!is.character(cl) || !"data.frame" %in% cl) bad("no data.frame class")
  rn <- ta[["row.names"]]
  if (!is.null(rn)) {
    if (is.integer(rn) && length(rn) == 2L && is.na(rn[1L])) {
      if (is.na(rn[2L]) || abs(rn[2L]) != nrow) bad("row names do not match the row count")
      ta[["row.names"]] <- NULL
    } else if (!(is.integer(rn) || is.character(rn)) || length(rn) != nrow) {
      bad("row names do not match the row count")
    }
  }
  s <- ta[["sorted"]]
  if (!is.null(s) && (!is.character(s) || !all(s %in% cols))) ta[["sorted"]] <- NULL
  ix <- ta[["index"]]
  if (!is.null(ix)) ta[["index"]] <- .dt_index_check(ix, nrow)   # NULL removes it
  ta
}

.dt_index_check <- function(ix, n) {
  if (!is.integer(ix) || length(ix) != 0L) return(NULL)
  perm <- function(o) is.integer(o) && (length(o) == 0L ||
    (length(o) == n && !anyNA(o) && min(o) >= 1L && max(o) <= n && !anyDuplicated(o)))
  starts <- function(s) is.null(s) || (is.integer(s) && !anyNA(s) && all(s >= 1L & s <= max(n, 1L)) &&
    !is.unsorted(s, strictly = TRUE))
  a <- attributes(ix)
  keep <- a[vapply(a, function(o) perm(o) && starts(attr(o, "starts")), NA)]
  if (!length(keep)) return(NULL)
  attributes(ix) <- keep
  ix
}
