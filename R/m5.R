# FST-X milestone M5: appends, compaction, row lists and filters with page skipping.

# ---------------------------------------------------------------- append
# Checks x against the file and returns x with factor codes remapped to the merged levels, the file's table
# attributes (raw) and its profile.
.append_prepare <- function(x, path) {
  m <- fstx_meta_impl(path)
  ta <- .attr_decode(m$table_attrs, path, "table")
  # character row names cannot grow; integer ones (as x[i, ] leaves) are positions and become automatic
  if (is.character(ta[["row.names"]]))
    stop("write_fstx: cannot append to a file with stored row names", call. = FALSE)
  if (is.character(attr(x, "row.names")))
    stop("write_fstx: cannot append rows with row names (remove them with rownames(x) <- NULL)", call. = FALSE)
  tab <- m$table_attrs
  if (!is.null(ta[["row.names"]])) {
    ta[["row.names"]] <- NULL
    tab <- .attr_encode(ta)
  }
  proto <- read_fstx(path, from = 1, to = 0, threads = 1L)
  fn <- names(proto)
  xn <- names(x)
  if (is.null(xn)) xn <- rep("", length(x))
  if (length(xn) != length(fn) || !identical(enc2utf8(as.character(xn)), enc2utf8(fn)))
    stop(sprintf("write_fstx: the new rows have columns (%s) but the file has (%s)",
                 paste(xn, collapse = ", "), paste(fn, collapse = ", ")), call. = FALSE)
  for (i in seq_along(fn)) {
    a <- proto[[i]]
    b <- x[[i]]
    if (!identical(typeof(a), typeof(b)) || !identical(oldClass(a), oldClass(b)))
      stop(sprintf("write_fstx: column '%s' is %s in the file but %s in the new rows", fn[i],
                   paste(c(oldClass(a), typeof(a)), collapse = "/"), paste(c(oldClass(b), typeof(b)), collapse = "/")),
           call. = FALSE)
    if (is.factor(a)) {
      la <- levels(a)
      lb <- levels(b)
      if (is.ordered(a)) {
        if (!identical(la, lb))
          stop(sprintf("write_fstx: ordered factor '%s' must keep the file's levels", fn[i]), call. = FALSE)
      } else if (!identical(la, lb)) {
        lv <- c(la, setdiff(lb, la))
        codes <- match(lb, lv)[unclass(b)]
        at <- attributes(b)
        at$levels <- lv
        attributes(codes) <- at
        x[[i]] <- codes
      }
    }
    if (inherits(a, "POSIXct") && !identical(attr(a, "tzone"), attr(b, "tzone")))
      stop(sprintf("write_fstx: POSIXct column '%s' has time zone '%s' in the file but '%s' in the new rows", fn[i],
                   format(attr(a, "tzone")), format(attr(b, "tzone"))), call. = FALSE)
  }
  list(x = x, table_attrs = tab, profile = m$profile)
}

# ---------------------------------------------------------------- compact
#' Compact an fstx file
#'
#' Appends leave the old footers (and anything an interrupted append wrote) inside the file. fstx_compact()
#' copies the pages into a fresh file without re-encoding them, then replaces the original. With merge = TRUE
#' it reads the whole table and writes it again (same profile), so the small row groups that appends add are
#' merged into full ones. Page copy keeps the file's profile and feature fields; merge = TRUE writes a fresh
#' file and drops unknown optional features and a newer profile.
#'
#' @param path file path.
#' @param merge FALSE (default): copy the pages; TRUE: rewrite the table.
#' @param threads threads for merge = TRUE.
#' @return a named vector: file size before and after, invisibly.
fstx_compact <- function(path, merge = FALSE, threads = fstx_hw_threads()) {
  fn <- "fstx_compact"
  path <- .path_arg(path, fn = fn)
  .flag(merge, "merge", fn)
  threads <- .whole(threads, "threads", 1, fn)
  if (!file.exists(path)) .err(fn, "'%s' does not exist", path)
  before <- file.size(path)
  tmp <- paste0(path, ".fstx-compact")
  on.exit(if (file.exists(tmp)) unlink(tmp), add = TRUE)
  run <- function() {
    if (merge) {
      prof <- fstx_meta_impl(path)$profile
      if (prof == "unknown") prof <- "balanced"  # a newer fstx's profile: the rewrite is a fresh balanced file
      x <- read_fstx(path, threads = threads)
      write_fstx(x, tmp, profile = prof, threads = threads)
    } else {
      fstx_compact_impl(path, tmp)
    }
  }
  k <- tryCatch(run(), error = function(e) e)
  if (inherits(k, "error")) {
    if (!grepl("it is open in this R session", conditionMessage(k), fixed = TRUE)) stop(k)
    gc()
    run()
  }
  if (!file.rename(tmp, path)) .err(fn, "could not replace '%s'", path)
  invisible(c(before = before, after = file.size(path)))
}

# ---------------------------------------------------------------- filters
.op_ids <- c(none = 0L, "==" = 1L, "!=" = 2L, "<" = 3L, "<=" = 4L, ">" = 5L, ">=" = 6L, "%in%" = 7L,
             isna = 8L, notna = 9L)
.flip <- c("==" = "==", "!=" = "!=", "<" = ">", "<=" = ">=", ">" = "<", ">=" = "<=")

# A `where` argument as an expression: a captured call, a formula (its right side) or a quoted call held in
# a variable.
.where_expr <- function(wq, env, cols, fn = "read_fstx") {
  if (is.call(wq) && identical(wq[[1L]], as.name("~"))) return(wq[[length(wq)]])
  if (is.name(wq) && !(as.character(wq) %in% cols)) {
    v <- eval(wq, env)
    if (inherits(v, "formula")) return(v[[length(v)]])
    if (is.call(v) || is.name(v)) return(v)
    .err(fn, "'where' must be a condition such as x > 5 & y == \"a\"")
  }
  wq
}

.is_col <- function(e, cols) is.name(e) && as.character(e) %in% cols

# Splits a condition into simple terms joined by &: list(list(col, op, value)) with op one of .op_ids names.
.where_terms <- function(e, cols, env, fn = "read_fstx") {
  bad <- function(why) .err(fn, "'where' %s: %s", why, paste(deparse(e), collapse = " "))
  if (.is_col(e, cols)) return(list(list(col = as.character(e), op = "==", value = TRUE)))
  if (!is.call(e)) bad("is not a condition on the file's columns")
  f <- paste(deparse(e[[1L]]), collapse = "")
  if (f == "(") return(.where_terms(e[[2L]], cols, env, fn))
  if (f %in% c("&", "&&")) return(c(.where_terms(e[[2L]], cols, env, fn), .where_terms(e[[3L]], cols, env, fn)))
  if (f %in% c("|", "||", "xor")) bad("supports only & between conditions, not |")
  if (f == "is.na" && length(e) == 2L && .is_col(e[[2L]], cols))
    return(list(list(col = as.character(e[[2L]]), op = "isna", value = NULL)))
  if (f == "!" && length(e) == 2L) {
    a <- e[[2L]]
    while (is.call(a) && identical(a[[1L]], as.name("("))) a <- a[[2L]]
    if (.is_col(a, cols)) return(list(list(col = as.character(a), op = "==", value = FALSE)))
    if (is.call(a) && identical(a[[1L]], as.name("is.na")) && length(a) == 2L && .is_col(a[[2L]], cols))
      return(list(list(col = as.character(a[[2L]]), op = "notna", value = NULL)))
    bad("supports ! only as !is.na(column) or !column")
  }
  if (f %in% c("between", "dplyr::between", "data.table::between")) {
    if (length(e) < 4L || !.is_col(e[[2L]], cols)) bad("needs between(column, lower, upper)")
    c1 <- as.character(e[[2L]])
    return(list(list(col = c1, op = ">=", value = eval(e[[3L]], env)),
                list(col = c1, op = "<=", value = eval(e[[4L]], env))))
  }
  if (f == "%in%") {
    if (!.is_col(e[[2L]], cols)) bad("needs a column on the left of %in%")
    if (.is_col(e[[3L]], cols)) bad("cannot compare two columns")
    return(list(list(col = as.character(e[[2L]]), op = "%in%", value = eval(e[[3L]], env))))
  }
  if (f %in% names(.flip)) {
    l <- .is_col(e[[2L]], cols)
    r <- .is_col(e[[3L]], cols)
    if (l && r) bad("cannot compare two columns")
    if (!l && !r) bad("needs a column on one side of each comparison")
    if (l) return(list(list(col = as.character(e[[2L]]), op = f, value = eval(e[[3L]], env))))
    return(list(list(col = as.character(e[[3L]]), op = .flip[[f]], value = eval(e[[2L]], env))))
  }
  bad("uses an unsupported function or operator")
}

# One term -> one engine predicate, with the value converted to what the column stores.
.term_pred <- function(t, proto, fn = "read_fstx") {
  x <- proto[[t$col]]
  cl <- oldClass(x)
  op <- t$op
  v <- t$value
  bad <- function(why) .err(fn, "'where' on column '%s': %s", t$col, why)
  p <- list(col = enc2utf8(t$col), op = .op_ids[[op]])
  if (op %in% c("isna", "notna")) return(p)
  cmp <- op %in% c("<", "<=", ">", ">=")
  if (op != "%in%" && length(v) != 1L) bad(sprintf("the value of %s must be a single value", op))
  if (is.character(x)) {
    if (cmp) bad("character columns support ==, !=, %in% and is.na() only")
    if (is.factor(v)) v <- as.character(v)
    if (!is.character(v) && !(is.logical(v) && all(is.na(v)))) bad("compare a character column with strings")
    v <- enc2utf8(as.character(v))
    if (op == "%in%") return(c(p, list(strs = v[!is.na(v)], set_na = anyNA(v))))
    if (is.na(v)) return(list(col = p$col, op = 0L))
    return(c(p, list(strs = v)))
  }
  num <- NULL
  if (is.factor(x)) {
    lv <- levels(x)
    if (is.factor(v)) v <- as.character(v)
    if (!is.character(v) && !(is.logical(v) && all(is.na(v)))) bad("compare a factor with level names")
    v <- as.character(v)
    if (cmp) {
      if (!is.ordered(x)) bad("<, <=, > and >= need an ordered factor")
      k <- match(v, lv)
      if (!is.na(v) && is.na(k)) bad(sprintf("'%s' is not one of its levels", v))
      num <- as.double(k)
    } else if (op == "%in%") {
      k <- match(v[!is.na(v)], lv)
      return(c(p, list(set = as.double(k[!is.na(k)]), set_na = anyNA(v))))
    } else {
      k <- match(v, lv)
      if (is.na(v)) return(list(col = p$col, op = 0L))
      if (is.na(k)) return(if (op == "==") list(col = p$col, op = 0L) else list(col = p$col, op = .op_ids[["notna"]]))
      num <- as.double(k)
    }
  } else if (inherits(x, "Date")) {
    if (is.character(v)) v <- as.Date(v)
    if (!inherits(v, "Date") && !(is.logical(v) && all(is.na(v)))) bad("compare a Date column with Dates or date strings")
    num <- as.double(unclass(v))
  } else if (inherits(x, "POSIXct")) {
    tz <- attr(x, "tzone")
    if (is.character(v)) v <- as.POSIXct(v, tz = if (is.null(tz)) "" else tz)
    if (!inherits(v, "POSIXct") && !(is.logical(v) && all(is.na(v)))) bad("compare a POSIXct column with date-times")
    num <- as.double(unclass(v))
  } else if (is.null(cl) && (is.numeric(x) || is.logical(x))) {
    if (!(is.numeric(v) || is.logical(v)) || is.object(v)) bad("compare a numeric or logical column with numbers")
    num <- as.double(v)
  } else {
    bad(sprintf("columns of class '%s' cannot be filtered", paste(cl, collapse = "/")))
  }
  if (op == "%in%") {
    isn <- is.nan(num)
    return(c(p, list(set = num[!is.na(num)], set_na = any(is.na(num) & !isn), set_nan = any(isn))))
  }
  if (is.na(num)) return(list(col = p$col, op = 0L))
  c(p, list(value = num))
}

.where_preds <- function(path, wq, env, fn = "read_fstx") {
  cols <- fstx_meta_impl(path)$name
  e <- .where_expr(wq, env, cols, fn)
  terms <- .where_terms(e, cols, env, fn)
  proto <- read_fstx(path, columns = unique(vapply(terms, `[[`, "", "col")), from = 1, to = 0, threads = 1L)
  lapply(terms, .term_pred, proto = proto, fn = fn)
}

.window <- function(from, to, fn = NULL) {
  if (!is.numeric(from) || length(from) != 1L || is.na(from)) .err(fn, "'from' must be a single number")
  if (!is.null(to) && (!is.numeric(to) || length(to) != 1L || is.na(to))) .err(fn, "'to' must be NULL or a single number")
  c(as.double(from), if (is.null(to)) NA_real_ else as.double(to))
}

#' Show how a filter is served: pages skipped by their statistics and rows left
#'
#' @param path file path.
#' @param where a condition on the file's columns, as in read_fstx().
#' @param from,to row window (1-based, inclusive).
#' @param exact FALSE (default) only consults page statistics; TRUE also runs the filter and counts the rows.
#' @return a named numeric vector: pages_total (data pages of the filter columns in the window), pages_skipped
#'   (ruled out by min / max / NA count or the dictionary), pages_pruned (ruled out by other conditions),
#'   pages_all (every row matches), pages_read (decoded), rows_window, rows_candidate, rows_matched.
fstx_explain <- function(path, where, from = 1, to = NULL, exact = FALSE, threads = fstx_hw_threads()) {
  fn <- "fstx_explain"
  path <- .path_arg(path, fn = fn)
  w <- .window(from, to, fn)
  .flag(exact, "exact", fn)
  threads <- .whole(threads, "threads", 1, fn)
  preds <- .where_preds(path, substitute(where), parent.frame(), fn)
  r <- fstx_filter_impl(path, preds, w[1L], w[2L], threads, TRUE, !exact)
  r$stats
}

#' Rows of an fstx file that match a condition
#'
#' @inheritParams fstx_explain
#' @return the matching row numbers (1-based, ascending, double).
fstx_which <- function(path, where, from = 1, to = NULL, threads = fstx_hw_threads()) {
  fn <- "fstx_which"
  path <- .path_arg(path, fn = fn)
  w <- .window(from, to, fn)
  threads <- .whole(threads, "threads", 1, fn)
  preds <- .where_preds(path, substitute(where), parent.frame(), fn)
  fstx_filter_impl(path, preds, w[1L], w[2L], threads, TRUE, FALSE)$rows
}
