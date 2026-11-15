# FST-X 1.0: Arrow stream straight from the file (WS2). Phase 0 placeholder: returns NULL.
fstx_arrow_stream <- function(path, columns = NULL, from = 1, to = NULL, where = NULL, threads = fstx_hw_threads(),
                              batch_rows = NULL) {
  fstx_arrow_stream_impl(list(path = path, columns = columns, from = from, to = to, where = substitute(where),
                              threads = threads, batch_rows = batch_rows))
}
