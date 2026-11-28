# FST-X 1.0: what this build can read and write (WS1). fstx 1.2: int64 and binary columns (types 4 and 5) are read
# and written exactly when the engine knows their feature bits (16 and 17).
fstx_features <- function() {
  r <- fstx_features_impl(list())
  known <- function(b) any(r$feature_bit == b & r$feature_read)
  rw <- c(TRUE, TRUE, TRUE, TRUE, known(16L), known(17L))
  list(
    version = r$version,
    format_read = r$format_read,
    format_write = r$format_write,
    features = data.frame(bit = r$feature_bit, name = r$feature_name, read = r$feature_read,
                          stringsAsFactors = FALSE),
    codecs = data.frame(id = r$codec_id, name = r$codec_name, read = r$codec_read, write = r$codec_write,
                        stringsAsFactors = FALSE),
    types = data.frame(id = 0:5, name = c("int32", "float64", "logical", "string", "int64", "binary"),
                       read = rw, write = rw, stringsAsFactors = FALSE)
  )
}

# Tests only: copy a file through compaction with header, footer or first-page fields overridden, to make the
# files a newer fstx could write (and crafted pages). Fields: hdr_flags, version, phys0, logical0, codec0, kind0, tag0,
# na0, nulls0 (null_count), profile (integers), required, optional (bit numbers), names (feature names).
.fstx_forge <- function(src, dst, ...) {
  f <- list(src = .path_arg(src), dst = .path_arg(dst), ...)
  for (k in setdiff(names(f), c("src", "dst", "names"))) f[[k]] <- as.integer(f[[k]])
  if (!is.null(f$names)) f$names <- enc2utf8(as.character(f$names))
  invisible(fstx_features_impl(list(forge = f)))
}
