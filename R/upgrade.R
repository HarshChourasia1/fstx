# FST-X 1.0: rewrite a format 2 (fstx 0.2-0.7) file as format 3 (WS8). Page-copy compaction with the version
# set to 3: no page is decoded or encoded again. See man/fstx_upgrade.Rd and inst/spec/fstx-format-1.0.md 9.4.
fstx_upgrade <- function(path, dest = NULL) {
  path <- .path_arg(path)
  if (!file.exists(path)) stop(sprintf("fstx_upgrade: '%s' does not exist", path), call. = FALSE)
  out <- if (is.null(dest)) path else .path_arg(dest, "dest")
  in_place <- identical(out, path)
  tmp <- paste0(out, ".fstx-upgrade")
  on.exit(if (file.exists(tmp)) unlink(tmp), add = TRUE)
  run <- function() fstx_upgrade_impl(list(path = path, tmp = tmp, in_place = in_place))
  from <- tryCatch(run(), error = function(e) e)
  if (inherits(from, "error")) {
    if (!grepl("it is open in this R session", conditionMessage(from), fixed = TRUE)) stop(from)
    gc()
    from <- run()
  }
  if (from >= 3L) {  # already format 3: nothing to rewrite
    if (!in_place && !file.copy(path, out, overwrite = TRUE))
      stop(sprintf("fstx_upgrade: could not write '%s'", out), call. = FALSE)
    return(invisible(c(old = from, new = from)))
  }
  # the new file must open and describe the same table before it replaces anything
  a <- fstx_meta_impl(path)
  b <- fstx_meta_impl(tmp)
  if (b$format_version != 3 || b$nrow != a$nrow || !identical(b$name, a$name) || !identical(b$phys, a$phys))
    stop("fstx_upgrade: the rewritten file does not match the original; nothing was changed", call. = FALSE)
  if (file.exists(out) && !in_place) unlink(out)
  if (!file.rename(tmp, out)) stop(sprintf("fstx_upgrade: could not replace '%s'", out), call. = FALSE)
  invisible(c(old = from, new = 3L))
}
