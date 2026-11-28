# R's view of a table, as plain JSON data: what an R user sees in each column. Sourced by make_rmatch.R and
# check_rmatch.R; tests/rview.py builds the same view from pyarrow, pandas and polars data.
#
# Per column: name, typeof, class (oldClass, [] for none), tzone (POSIXct only: the attribute, null = absent),
# levels (factor only; null = an NA level), values (one per row, null = NA):
#   logical   "TRUE" / "FALSE"
#   integer   decimal text
#   double    sprintf("%.17g"), or "NaN", "Inf", "-Inf"
#   integer64 as.character()
#   character the string; over 2,000 bytes "<bytes>:<md5 of the UTF-8 bytes>"
#   list      (fstx 1.2 binary columns: raw vectors) lowercase hex of the bytes ("" for raw(0)), NULL = NA; over
#             2,000 bytes "<bytes>:<md5 of the bytes>"
#   factor    the 1-based code (a number)
#   Date      "YYYY-MM-DD" of the whole day (floor)
#   POSIXct   "YYYY-MM-DD hh:mm:ss.uuuuuu +hh:mm": the clock time and UTC offset in the column's own time zone
#             (UTC when tzone is "" or absent: R would use the session's zone, Python has none), from the stored
#             seconds x: floor(x) for the second, round((x - floor(x)) * 1e6) for the microseconds

rv_pad <- function(v, w) formatC(v, width = w, flag = "0", format = "d")

rv_date <- function(x) {
  v <- as.numeric(unclass(x))
  out <- rep(NA_character_, length(v))
  ok <- is.finite(v)
  if (any(ok)) {
    lt <- as.POSIXlt(.Date(floor(v[ok])))
    out[ok] <- sprintf("%s-%s-%s", rv_pad(lt$year + 1900L, 4L), rv_pad(lt$mon + 1L, 2L), rv_pad(lt$mday, 2L))
  }
  out
}

rv_time <- function(x) {
  tz <- attr(x, "tzone")
  vtz <- if (is.null(tz) || !nzchar(tz[1L])) "UTC" else tz[1L]
  v <- as.numeric(unclass(x))
  out <- rep(NA_character_, length(v))
  ok <- is.finite(v)
  if (any(ok)) {
    wh <- floor(v[ok])
    fr <- round((v[ok] - wh) * 1e6)
    up <- fr >= 1e6
    wh[up] <- wh[up] + 1
    fr[up] <- 0
    lt <- as.POSIXlt(.POSIXct(wh, tz = vtz))
    off <- lt$gmtoff
    off[is.na(off)] <- 0L
    sg <- ifelse(off < 0, "-", "+")
    ao <- abs(off)
    out[ok] <- sprintf("%s-%s-%s %s:%s:%s.%s %s%s:%s", rv_pad(lt$year + 1900L, 4L), rv_pad(lt$mon + 1L, 2L),
                       rv_pad(lt$mday, 2L), rv_pad(lt$hour, 2L), rv_pad(lt$min, 2L), rv_pad(as.integer(lt$sec), 2L),
                       rv_pad(as.integer(fr), 6L), sg, rv_pad(ao %/% 3600L, 2L), rv_pad((ao %% 3600L) %/% 60L, 2L))
  }
  out
}

rv_dbl <- function(v) {
  v <- as.numeric(unclass(v))
  ifelse(is.nan(v), "NaN", ifelse(is.na(v), NA_character_, ifelse(v == Inf, "Inf", ifelse(v == -Inf, "-Inf",
         sprintf("%.17g", v)))))
}

rv_chr <- function(v) {
  v <- unclass(v)
  attributes(v) <- NULL
  vapply(v, function(e) {
    if (is.na(e)) return(NA_character_)
    e <- enc2utf8(e)
    n <- nchar(e, type = "bytes")
    if (n > 2000L) sprintf("%d:%s", n, unname(tools::md5sum(bytes = charToRaw(e)))) else e
  }, "", USE.NAMES = FALSE)
}

rv_raw <- function(v) {
  v <- unclass(v)
  attributes(v) <- NULL
  vapply(v, function(e) {
    if (is.null(e)) return(NA_character_)
    if (length(e) > 2000L) sprintf("%d:%s", length(e), unname(tools::md5sum(bytes = e)))
    else paste(as.character(e), collapse = "")
  }, "", USE.NAMES = FALSE)
}

rview_col <- function(x, name) {
  cls <- oldClass(x)
  out <- list(name = name, typeof = typeof(x), class = as.list(if (is.null(cls)) character(0) else cls),
              tzone = NULL, levels = NULL)
  if (inherits(x, "POSIXct")) {
    tz <- attr(x, "tzone")
    out["tzone"] <- list(if (is.null(tz)) NULL else tz[1L])
  }
  vals <- if (is.factor(x)) {
    out$levels <- as.list(levels(x))
    as.integer(unclass(x))
  } else if (inherits(x, "integer64")) as.character(x)
  else if (inherits(x, "Date")) rv_date(x)
  else if (inherits(x, "POSIXct")) rv_time(x)
  else switch(typeof(x),
    logical = ifelse(unclass(x), "TRUE", "FALSE"),
    integer = as.character(unclass(x)),
    double = rv_dbl(x),
    character = rv_chr(x),
    list = rv_raw(x),
    stop("rview: no view for a ", typeof(x), " column"))
  out$values <- as.list(unname(vals))
  out
}

# The view of a data frame, normalised through JSON so that two views compare with identical().
rview <- function(df) {
  v <- list(names = as.list(names(df)), nrow = nrow(df),
            columns = lapply(seq_along(df), function(j) rview_col(df[[j]], names(df)[j])))
  jsonlite::fromJSON(rview_json(v), simplifyVector = FALSE)
}

rview_json <- function(v, pretty = FALSE)
  jsonlite::toJSON(v, auto_unbox = TRUE, null = "null", na = "null", digits = NA, pretty = pretty)
