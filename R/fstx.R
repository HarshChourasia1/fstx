# FST-X: a column-store file format for R data frames (milestone M3, file format 2).

#' @useDynLib fstx, .registration = TRUE
NULL

.onUnload <- function(libpath) library.dynam.unload("fstx", libpath)

.codec_ids <- c(none = 0L, lz4 = 1L, zstd = 2L)
.profile_ids <- c(fast = 0L, balanced = 1L, small = 2L)
.balanced_level <- 3L   # M2 timing: level 1 leaves text 15% above qdata; 3 costs ~10% more write time
.small_level <- 9L      # M2 timing: 2-4% smaller than level 3 for ~1.8x the write time; 12+ gains < 1%
.encoding_ids <- c(plain = 1L, const = 2L, "for" = 3L, delta = 4L, rle = 5L, dict = 6L, split = 7L,
                   alp = 8L, alprd = 9L, fsst = 10L)

.path_arg <- function(path, what = "path") {
  if (!is.character(path) || length(path) != 1L || is.na(path) || !nzchar(path))
    stop(sprintf("'%s' must be a single non-empty string", what), call. = FALSE)
  enc2utf8(normalizePath(path.expand(path), winslash = "/", mustWork = FALSE))
}

.whole <- function(x, what, min = -Inf) {
  if (!is.numeric(x) || length(x) != 1L || is.na(x) || x != round(x) || x < min)
    stop(sprintf("'%s' must be a single whole number%s", what,
                 if (is.finite(min)) sprintf(" >= %s", min) else ""), call. = FALSE)
  as.integer(x)
}

#' Write a data frame to an FST-X file
#'
#' Every page is encoded with the lightweight encoding that fits it best (bit-packing, frame-of-reference,
#' delta, run-length, dictionary, ALP for doubles, FSST for strings); the profile decides what goes on top.
#'
#' @param x a data.frame (data.table and tibble keep their class).
#' @param path file path.
#' @param profile "balanced" (default: encodings plus ZSTD level 3 where it saves at least 10%),
#'   "fast" (encodings only, no codec: fastest reads) or "small" (encodings plus ZSTD level 9).
#' @param compress outer codec per page, overriding the profile: "zstd", "lz4" or "none".
#' @param level ZSTD level (ignored for lz4 and none); default 3, or 9 for profile "small".
#' @param threads number of threads (default: all cores; at most 2 under R CMD check core limits).
#' @param append FALSE (default) replaces the file. TRUE adds the rows of x at the end of an existing file
#'   (a missing file is simply written). Columns must match the file's in names, order, type and class;
#'   factor levels may grow (the file's levels first, new ones after; ordered factors need the same levels);
#'   POSIXct columns need the same time zone; files with stored row names cannot be appended to. The
#'   profile defaults to the file's. An interrupted append leaves the old table readable.
#' @param .encoding for tests only: force one encoding on every page where it applies.
#' @param .crash for tests only.
#' @return x, invisibly.
write_fstx <- function(x, path, profile = c("balanced", "fast", "small"), compress = NULL, level = NULL,
                       threads = fstx_hw_threads(), append = FALSE, .encoding = NULL, .crash = 0L) {
  if (!is.data.frame(x)) stop("write_fstx: 'x' must be a data frame", call. = FALSE)
  if (!is.logical(append) || length(append) != 1L || is.na(append)) stop("write_fstx: 'append' must be TRUE or FALSE", call. = FALSE)
  file_ta <- NULL
  if (append) {
    path <- .path_arg(path)
    if (!file.exists(path)) {
      append <- FALSE
    } else {
      chk <- .append_prepare(x, path)
      x <- chk$x
      file_ta <- chk$table_attrs
      if (missing(profile)) profile <- chk$profile
    }
  }
  profile <- match.arg(profile)
  keep_pct <- 0L
  if (is.null(compress)) {
    compress <- if (profile == "fast") "none" else "zstd"
    if (profile == "balanced") keep_pct <- 10L
  } else {
    compress <- match.arg(compress, c("zstd", "lz4", "none"))
  }
  if (is.null(level)) level <- if (profile == "small") .small_level else .balanced_level
  level <- .whole(level, "level")
  force <- 0L
  if (!is.null(.encoding)) {
    if (!is.character(.encoding) || length(.encoding) != 1L || !(.encoding %in% names(.encoding_ids)))
      stop(sprintf("write_fstx: '.encoding' must be one of %s", paste(names(.encoding_ids), collapse = ", ")),
           call. = FALSE)
    force <- .encoding_ids[[.encoding]]
  }
  threads <- .whole(threads, "threads", 1)
  path <- .path_arg(path)
  nr <- .row_names_info(x, 2L)

  cols <- unclass(x)
  attributes(cols) <- NULL
  nc <- length(cols)
  nm <- names(x)
  if (is.null(nm)) nm <- rep("", nc)
  nm[is.na(nm)] <- "NA"
  nm <- enc2utf8(nm)

  types <- vapply(cols, typeof, "")
  ok <- types %in% c("integer", "double", "logical", "character")
  if (!all(ok)) {
    i <- which(!ok)[1L]
    stop(sprintf("write_fstx: column '%s' has type '%s' (class '%s'), which FST-X cannot store yet",
                 nm[i], types[i], paste(class(cols[[i]]), collapse = "/")), call. = FALSE)
  }
  lens <- lengths(cols)
  if (any(lens != nr)) {
    i <- which(lens != nr)[1L]
    stop(sprintf("write_fstx: column '%s' has %.0f values but the table has %.0f rows (matrix columns are not supported)",
                 nm[i], lens[i], nr), call. = FALSE)
  }
  phys <- match(types, c("integer", "double", "logical", "character")) - 1L
  logical <- integer(nc)
  ordered <- logical(nc)
  levels <- vector("list", nc)
  tzone <- vector("list", nc)
  blobs <- vector("list", nc)

  ats <- lapply(cols, attributes)
  for (i in which(!vapply(ats, is.null, NA))) {
    a <- ats[[i]]
    cl <- a[["class"]]
    if (!is.null(a[["dim"]]))
      stop(sprintf("write_fstx: column '%s' is a matrix or array, which FST-X cannot store yet", nm[i]), call. = FALSE)
    if (types[i] == "integer" && (identical(cl, "factor") || identical(cl, c("ordered", "factor"))) &&
        is.character(a[["levels"]]) && !anyNA(a[["levels"]])) {
      logical[i] <- 1L
      ordered[i] <- length(cl) == 2L
      levels[[i]] <- enc2utf8(a[["levels"]])
      a[["class"]] <- NULL
      a[["levels"]] <- NULL
    } else if (types[i] %in% c("integer", "double") && identical(cl, "Date")) {
      logical[i] <- 2L
      a[["class"]] <- NULL
    } else if (types[i] == "double" && identical(cl, c("POSIXct", "POSIXt")) &&
               (is.null(a[["tzone"]]) || (is.character(a[["tzone"]]) && length(a[["tzone"]]) == 1L && !is.na(a[["tzone"]])))) {
      logical[i] <- 3L
      if (!is.null(a[["tzone"]])) tzone[[i]] <- enc2utf8(a[["tzone"]])
      a[["class"]] <- NULL
      a[["tzone"]] <- NULL
    }
    # anything else (other classes, units, labels, ...) travels as an R-serialized blob
    if (length(a)) blobs[[i]] <- serialize(a, NULL, xdr = FALSE)
  }

  ta <- attributes(x)
  ta[["names"]] <- NULL
  if (.row_names_info(x) < 0L) ta[["row.names"]] <- NULL   # automatic row names are rebuilt on read
  ta[[".internal.selfref"]] <- NULL                        # data.table pointer, rebuilt on read
  table_blob <- if (length(ta)) serialize(ta, NULL, xdr = FALSE) else NULL
  if (append) table_blob <- file_ta   # the file keeps its own class and table attributes

  utf8_locale <- isTRUE(l10n_info()[["UTF-8"]])
  for (attempt in seq_len(nc + 1L)) {
    w <- function() fstx_write_impl(cols, path, nm, phys, logical, ordered, levels, tzone, blobs, table_blob,
                                    as.double(nr), .codec_ids[[compress]], level, threads, utf8_locale,
                                    .profile_ids[[profile]], keep_pct, force, append, as.integer(.crash))
    k <- tryCatch(w(), error = function(e) e)
    if (inherits(k, "error")) {
      if (!grepl("it is open in this R session", conditionMessage(k), fixed = TRUE)) stop(k)
      gc()   # lazy results nobody can reach any more close the file in their finalisers
      k <- w()
    }
    if (k == 0L) return(invisible(x))
    cols[[k]] <- enc2utf8(cols[[k]])   # non-UTF-8 strings: convert this column and try again
  }
  stop("write_fstx: could not convert strings to UTF-8", call. = FALSE)
}

#' Read an FST-X file
#'
#' @param path file path.
#' @param columns NULL (all), column names, or column positions.
#' @param from,to first and last row to read (1-based, inclusive); to = NULL reads to the end.
#' @param threads number of threads (default: all cores; at most 2 under R CMD check core limits).
#' @param verify check every page's checksum (header and footer checksums are always checked). Lazy
#'   columns check a page when it is decoded.
#' @param lazy FALSE (default) reads the data now. TRUE returns at once: every column is an ALTREP vector
#'   backed by the memory-mapped file and decoded a page at a time when it is used, so RAM covers only the
#'   columns (and pages) that are touched. The file stays open (and cannot be overwritten from this session)
#'   until those columns are fully used or garbage collected.
#' @param strings "auto" (default since 0.6.0) decodes string pages in parallel, closes the file at once and
#'   returns each character column as an ALTREP vector whose R strings are made a page at a time on first
#'   use (the first page used alone, later pages into one full vector, which becomes the column once every
#'   page is made). Since 0.6.1 a column whose pages are all dictionary-coded (few distinct values) is
#'   made during the read and returned as a plain character vector, as that costs little.
#'   saveRDS() / serialize() write such columns in R's standard format, so the file reads
#'   back without fstx. "deferred" (alias "altrep") is the same, but an unmade column is serialised in a
#'   compact form that needs fstx to read back (smaller and faster to write). "eager" makes every R string
#'   during the read and returns plain character vectors.
#' @param as_factor FALSE (default), TRUE (every character column whose pages are all dictionary-coded
#'   becomes a factor, built from the dictionaries without hashing the rows), or column names (those
#'   columns become factors; ones that are not dictionary-coded go through factor()). Levels are the sorted
#'   union of the dictionaries read, so a full read gives the same factor as factor(x).
#' @param rows row numbers to read (1-based, any order, repeats allowed), returned in that order. Only the
#'   pages that hold them are read. A row list is read eagerly (lazy and strings are ignored, as_factor goes
#'   through factor()). Cannot be combined with from / to or where.
#' @param where a condition on the file's columns, such as x > 5 & y == "a" (also a formula ~ cond, or a
#'   quoted call held in a variable): terms joined by &, each a comparison of a column with a value (==, !=,
#'   <, <=, >, >=), %in%, between(), is.na(), !is.na() or a logical column. Pages are skipped by their
#'   min / max / NA count, then the remaining rows are tested exactly; rows where the condition is NA are
#'   dropped, as subset() does. Applied within from / to.
#' @return a data frame with the class it was written with.
read_fstx <- function(path, columns = NULL, from = 1, to = NULL, threads = fstx_hw_threads(), verify = TRUE,
                      lazy = FALSE, strings = c("auto", "eager", "deferred", "altrep"), as_factor = FALSE,
                      rows = NULL, where = NULL) {
  path <- .path_arg(path)
  wq <- substitute(where)
  if (!is.null(wq)) {
    if (!is.null(rows)) stop("read_fstx: use either 'rows' or 'where', not both", call. = FALSE)
    w <- .window(from, to)
    preds <- .where_preds(path, wq, parent.frame())
    rows <- fstx_filter_impl(path, preds, w[1L], w[2L], .whole(threads, "threads", 1), isTRUE(verify), FALSE)$rows
    from <- 1
    to <- NULL
  }
  if (!is.null(rows)) {
    if (!is.numeric(rows) || is.object(rows)) stop("read_fstx: 'rows' must be a numeric vector of row numbers", call. = FALSE)
    if (!isTRUE(all.equal(from, 1)) || !is.null(to))
      stop("read_fstx: use either 'rows' or 'from' / 'to', not both", call. = FALSE)
    n <- length(rows)
    if (n > 0L && !anyNA(rows) && all(rows == trunc(rows)) && rows[n] - rows[1L] == n - 1 &&
        (n == 1L || all(rows[-1L] - rows[-n] == 1))) {
      from <- rows[1L]       # one ascending run: the range read
      to <- rows[n]
      rows <- NULL
    }
  }
  if (!is.null(columns)) {
    if (is.character(columns)) columns <- enc2utf8(columns)
    else if (!is.numeric(columns)) stop("'columns' must be NULL, column names or column positions", call. = FALSE)
  }
  if (!is.numeric(from) || length(from) != 1L || is.na(from)) stop("'from' must be a single number", call. = FALSE)
  if (!is.null(to) && (!is.numeric(to) || length(to) != 1L || is.na(to))) stop("'to' must be NULL or a single number", call. = FALSE)
  threads <- .whole(threads, "threads", 1)
  if (!is.logical(lazy) || length(lazy) != 1L || is.na(lazy)) stop("'lazy' must be TRUE or FALSE", call. = FALSE)
  strings <- match.arg(strings)
  if (is.logical(as_factor) && length(as_factor) == 1L && !is.na(as_factor)) {
    if (!as_factor) as_factor <- NULL
  } else if (is.character(as_factor) && !anyNA(as_factor)) {
    as_factor <- enc2utf8(as_factor)
  } else {
    stop("'as_factor' must be TRUE, FALSE or column names", call. = FALSE)
  }
  mode <- if (lazy) 2L else switch(strings, eager = 0L, auto = 3L, 1L)
  presize <- if (mode == 0L && isTRUE(getOption("fstx.presize", FALSE))) .presize else NULL
  r <- fstx_read_impl(path, columns, as.double(from), if (is.null(to)) NA_real_ else as.double(to),
                      threads, isTRUE(verify), mode, as_factor, presize, rows)
  res <- r$columns
  fd <- r$factor_dicts
  for (i in which(!vapply(fd, is.null, NA))) {   # factors straight from the dictionaries
    d <- fd[[i]]
    lv <- sort(unique(unlist(d, use.names = FALSE)))
    res[[i]] <- fstx_codes_impl(res[[i]], lapply(d, match, lv), lv, FALSE, lazy)
  }
  for (i in which(r$factor_fallback)) res[[i]] <- factor(res[[i]])
  ca <- r$col_attrs
  for (i in which(!vapply(ca, is.null, NA))) {
    col <- res[[i]]
    res[i] <- list(NULL)                     # drop the list's reference so the attributes are set in place
    attributes(col) <- c(attributes(col), unserialize(ca[[i]]))
    res[[i]] <- col
  }
  n <- r$nrow
  ta <- if (is.null(r$table_attrs)) list(class = "data.frame") else unserialize(r$table_attrs)
  part_rows <- n != r$file_rows || !is.null(rows)
  if (is.null(ta[["row.names"]])) {
    ta[["row.names"]] <- .set_row_names(as.integer(n))
  } else if (!is.null(rows)) {
    rn <- ta[["row.names"]][rows]
    if (anyDuplicated(rn)) rn <- make.unique(as.character(rn))   # as x[rows, ] does
    ta[["row.names"]] <- rn
  } else if (part_rows) {
    ta[["row.names"]] <- ta[["row.names"]][seq.int(from, length.out = n)]
  }
  if (part_rows || !r$all_columns) {
    ta[["index"]] <- NULL                                   # data.table secondary indices
    if (!is.null(ta[["sorted"]]) && !all(ta[["sorted"]] %in% r$names)) ta[["sorted"]] <- NULL
  }
  nms <- r$names
  if (!r$all_columns && anyDuplicated(nms)) nms <- make.unique(nms)   # as x[, cols] does
  attributes(res) <- c(list(names = nms), ta)
  if (inherits(res, "data.table") && requireNamespace("data.table", quietly = TRUE))
    res <- data.table::setalloccol(res)
  res
}

# Grows R's heap before an eager read that will build `nodes` R strings from `bytes` of string pages, when the
# current collection thresholds are too low for them (measured: a cold 6M-string read spent 3.6-4.4 s of 7 s
# in full collections; growing the heap first cut that to 0.4 s). Opt-in with options(fstx.presize = TRUE): in the
# M3 benchmark it cut cold text reads by 13-19% but slowed the mixed table and later reads, and adds ~170 MB.
.presize <- function(nodes, bytes) {
  g <- gc(full = FALSE)
  vbytes <- bytes + 16 * nodes
  grow_n <- if (g[1L, 3L] < 1.1 * (g[1L, 1L] + nodes)) nodes else 0
  grow_v <- if (g[2L, 3L] < 1.1 * (g[2L, 1L] + vbytes / 8)) vbytes else 0
  if (grow_n > 0 || grow_v > 0) fstx_presize_impl(grow_n, grow_v)
  invisible(NULL)
}

#' Describe an FST-X file without reading its data
#'
#' @param path file path.
#' @return an object of class "fstx_meta": row and column counts, file size, and per column the type,
#'   class and compressed size.
fstx_meta <- function(path) {
  path <- .path_arg(path)
  m <- fstx_meta_impl(path)
  cls <- ifelse(m$logical == "", m$phys, m$logical)
  for (i in which(!vapply(m$col_attrs, is.null, NA))) {
    a <- unserialize(m$col_attrs[[i]])
    if (!is.null(a[["class"]])) cls[i] <- paste(a[["class"]], collapse = "/")
  }
  ta <- if (is.null(m$table_attrs)) list() else unserialize(m$table_attrs)
  structure(list(
    path = path, nrow = m$nrow, ncol = m$ncol, file_size = m$file_size, row_groups = m$row_groups,
    pages = m$pages, format_version = m$format_version, profile = m$profile, table_class = ta[["class"]],
    columns = data.frame(name = m$name, type = m$phys, class = cls, tzone = m$tzone,
                         compressed_bytes = m$compressed_bytes, raw_bytes = m$raw_bytes,
                         pages = m$col_pages, stringsAsFactors = FALSE)
  ), class = "fstx_meta")
}

#' @export
print.fstx_meta <- function(x, ...) {
  cat(sprintf("FST-X file: %s\n%.0f rows x %.0f columns, %.1f MB, %.0f row group(s), %.0f pages, profile %s, class %s\n",
              x$path, x$nrow, x$ncol, x$file_size / 2^20, x$row_groups, x$pages, x$profile,
              paste(x$table_class, collapse = "/")))
  print(x$columns, row.names = FALSE)
  invisible(x)
}

#' List the stored pages of an FST-X file
#'
#' One row per page (string dictionary pages included) with its column, rows, sizes, codec and the
#' encoding the writer chose.
#'
#' @param path file path.
#' @return a data frame.
fstx_pages <- function(path) {
  path <- .path_arg(path)
  m <- fstx_meta_impl(path)
  p <- fstx_pages_impl(path)
  p <- as.data.frame(p, stringsAsFactors = FALSE)
  p$name <- m$name[p$column]
  p[c("row_group", "column", "name", setdiff(names(p), c("row_group", "column", "name")))]
}

# ---------------------------------------------------------------- SIMD kernels (M4)
#' SIMD kernels: show or choose the instruction set the decoder uses.
#'
#' FST-X decodes with SIMD kernels (Google Highway, compiled for SSE2, SSE4 and AVX2 on x86-64, for Highway's
#' own targets on ARM, and picked at run time)
#' that give the same bytes as its plain loops. `fstx_simd()` returns the current state;
#' `fstx_simd("scalar")` turns the kernels off, `fstx_simd("auto")` picks the best target the CPU runs, and
#' `fstx_simd("avx2")` (or "sse2", "sse4") limits them to one target. The environment variable
#' FSTX_SIMD or the option fstx.simd, read when the package loads, set the same. AVX-512 ("avx3") is not
#' compiled in until it is tested on a CPU that has it; CPUs with AVX-512 use AVX2.
#' @param target NULL to only report, or "scalar", "auto", "sse2", "sse4", "avx2".
#' @return A list: simd (on or off), target (in use), cpu (targets this CPU runs), compiled (targets built in).
fstx_simd <- function(target = NULL) {
  if (!is.null(target)) {
    stopifnot(is.character(target), length(target) == 1L, !is.na(target))
    fstx_simd_set_impl(target)
  }
  fstx_simd_info_impl()
}

.load_msg <- new.env(parent = emptyenv())

.onLoad <- function(libname, pkgname) {
  t <- getOption("fstx.simd", Sys.getenv("FSTX_SIMD", ""))
  if (is.character(t) && length(t) == 1L && nzchar(t)) {
    r <- tryCatch(fstx_simd_set_impl(t), error = function(e) {
      .load_msg$simd <- conditionMessage(e)   # shown by .onAttach (startup messages belong there)
      NULL
    })
  }
  invisible()
}

.onAttach <- function(libname, pkgname) {
  if (!is.null(.load_msg$simd)) packageStartupMessage("fstx: ignoring FSTX_SIMD / fstx.simd: ", .load_msg$simd)
}
