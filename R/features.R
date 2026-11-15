# FST-X 1.0: what this build can read and write (WS1; WS8 adds the upgrade path).
fstx_features <- function() {
  r <- fstx_features_impl(list())
  list(
    version = r$version,
    format_read = r$format_read,
    format_write = r$format_write,
    features = data.frame(bit = r$feature_bit, name = r$feature_name, read = r$feature_read,
                          stringsAsFactors = FALSE),
    codecs = data.frame(id = r$codec_id, name = r$codec_name, read = r$codec_read, write = r$codec_write,
                        stringsAsFactors = FALSE),
    types = data.frame(id = 0:5, name = c("int32", "float64", "logical", "string", "int64", "binary"),
                       read = c(TRUE, TRUE, TRUE, TRUE, FALSE, FALSE), stringsAsFactors = FALSE)
  )
}

# Tests only: copy a file through compaction with header, footer or first-page fields overridden, to make the
# files a newer fstx could write. Fields: hdr_flags, version, phys0, logical0, codec0, kind0, tag0, na0, profile
# (integers), required, optional (bit numbers), names (feature names).
.fstx_forge <- function(src, dst, ...) {
  f <- list(src = .path_arg(src), dst = .path_arg(dst), ...)
  for (k in setdiff(names(f), c("src", "dst", "names"))) f[[k]] <- as.integer(f[[k]])
  if (!is.null(f$names)) f$names <- enc2utf8(as.character(f$names))
  invisible(fstx_features_impl(list(forge = f)))
}
