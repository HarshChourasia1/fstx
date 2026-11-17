# FST-X 1.0: Arrow stream straight from the file (WS2). The stream itself is C++ (src/r_arrow.cpp); this checks the
# arguments, runs a `where` filter up front (its matching rows are what the stream then reads) and hands the engine
# an empty nanoarrow stream to fill.
fstx_arrow_stream <- function(path, columns = NULL, from = 1, to = NULL, where = NULL, threads = fstx_hw_threads(),
                              batch_rows = NULL) {
  if (!requireNamespace("nanoarrow", quietly = TRUE))
    stop("fstx_arrow_stream: needs the 'nanoarrow' package: install.packages(\"nanoarrow\")", call. = FALSE)
  fn <- "fstx_arrow_stream"   # named in the errors of the shared argument helpers (WS9)
  path <- .path_arg(path, fn = fn)
  threads <- .whole(threads, "threads", 1, fn = fn)
  if (!is.null(batch_rows)) batch_rows <- as.double(.whole(batch_rows, "batch_rows", 1, fn = fn))
  w <- .window(from, to, fn = fn)
  m <- fstx_meta_impl(path)
  nm <- m$name
  if (is.null(columns)) {
    idx <- seq_along(nm)
  } else if (is.character(columns)) {
    if (anyNA(columns)) stop("fstx_arrow_stream: column names must not be NA", call. = FALSE)
    idx <- match(enc2utf8(columns), nm)
    if (anyNA(idx))
      stop(sprintf("fstx_arrow_stream: column '%s' is not in the file", columns[is.na(idx)][1L]), call. = FALSE)
  } else if (is.numeric(columns)) {
    if (anyNA(columns) || any(columns < 1 | columns > length(nm) | columns != trunc(columns)))
      stop(sprintf("fstx_arrow_stream: column positions must be whole numbers from 1 to %d", length(nm)), call. = FALSE)
    idx <- as.integer(columns)
  } else {
    stop("fstx_arrow_stream: 'columns' must be NULL, column names or column positions", call. = FALSE)
  }
  out_names <- nm[idx]
  if (anyDuplicated(out_names)) out_names <- make.unique(out_names)   # as x[, cols] does
  # rows as in read_fstx(): from / to 1-based and inclusive, to = NULL is the last row, to past the end is clamped
  nr <- m$nrow
  from <- w[1L]
  t <- if (is.na(w[2L])) nr else min(w[2L], nr)
  if (from < 1 || from != trunc(from)) stop("fstx_arrow_stream: 'from' must be a whole number >= 1", call. = FALSE)
  if (t != trunc(t)) stop("fstx_arrow_stream: 'to' must be a whole number", call. = FALSE)
  if (from > nr + 1 || (from > nr && nr > 0))
    stop(sprintf("fstx_arrow_stream: 'from' (%.0f) is beyond the last row (%.0f)", from, nr), call. = FALSE)
  if (t < from - 1) stop("fstx_arrow_stream: 'to' must not be smaller than 'from'", call. = FALSE)
  rows <- NULL
  wq <- substitute(where)
  if (!is.null(wq))
    rows <- fstx_filter_impl(path, .where_preds(path, wq, parent.frame(), fn = fn), from, t, threads, .verify_bits(TRUE), FALSE)$rows
  s <- nanoarrow::nanoarrow_allocate_array_stream()
  fstx_arrow_stream_impl(list(path = path, cols = idx - 1L, names = enc2utf8(out_names), from = from - 1, to = t,
                              rows = rows, threads = threads, batch_rows = batch_rows, verify = .verify_bits(TRUE),
                              stream = s))
  s
}
