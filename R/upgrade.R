# FST-X 1.0: rewrite a format 2 (0.7) file as format 3 (WS8). Not in this build yet.
fstx_upgrade <- function(path, to = NULL) {
  fstx_upgrade_impl(list(path = path, to = to))
}
