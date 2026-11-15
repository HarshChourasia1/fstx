# FST-X 1.0: rewrite a format 2 (0.7) file as format 1.0 (WS1, then WS8). Phase 0 placeholder: returns NULL.
fstx_upgrade <- function(path, to = NULL) {
  fstx_upgrade_impl(list(path = path, to = to))
}
