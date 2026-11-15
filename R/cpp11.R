# Generated in the form of cpp11::cpp_register(); do not edit by hand

fstx_write_impl <- function(cols, path, names, phys, logical, ordered, levels, tzone, attrs, table_attrs, nrow, codec, level, threads, utf8_locale, profile, keep_pct, force_encoding, append, test_crash) {
  .Call(`_fstx_fstx_write_impl`, cols, path, names, phys, logical, ordered, levels, tzone, attrs, table_attrs, nrow, codec, level, threads, utf8_locale, profile, keep_pct, force_encoding, append, test_crash)
}

fstx_read_impl <- function(path, columns, from, to, threads, verify, mode, as_factor, presize, rows) {
  .Call(`_fstx_fstx_read_impl`, path, columns, from, to, threads, verify, mode, as_factor, presize, rows)
}

fstx_codes_impl <- function(xp, remaps, levels, ordered, lazy) {
  .Call(`_fstx_fstx_codes_impl`, xp, remaps, levels, ordered, lazy)
}

fstx_presize_impl <- function(nodes, bytes) {
  invisible(.Call(`_fstx_fstx_presize_impl`, nodes, bytes))
}

fstx_col_state_impl <- function(x) {
  .Call(`_fstx_fstx_col_state_impl`, x)
}

fstx_meta_impl <- function(path) {
  .Call(`_fstx_fstx_meta_impl`, path)
}

fstx_pages_impl <- function(path) {
  .Call(`_fstx_fstx_pages_impl`, path)
}

fstx_filter_impl <- function(path, preds, from, to, threads, verify, plan_only) {
  .Call(`_fstx_fstx_filter_impl`, path, preds, from, to, threads, verify, plan_only)
}

fstx_compact_impl <- function(path, tmp) {
  invisible(.Call(`_fstx_fstx_compact_impl`, path, tmp))
}

fstx_sizes_impl <- function(path) {
  .Call(`_fstx_fstx_sizes_impl`, path)
}

fstx_hw_threads <- function() {
  .Call(`_fstx_fstx_hw_threads`)
}

fstx_prof_impl <- function(reset) {
  .Call(`_fstx_fstx_prof_impl`, reset)
}

fstx_simd_info_impl <- function() {
  .Call(`_fstx_fstx_simd_info_impl`)
}

fstx_simd_set_impl <- function(target) {
  .Call(`_fstx_fstx_simd_set_impl`, target)
}

fstx_kernel_selftest_impl <- function(seed, iters) {
  .Call(`_fstx_fstx_kernel_selftest_impl`, seed, iters)
}

fstx_kernel_bench_impl <- function(min_time) {
  .Call(`_fstx_fstx_kernel_bench_impl`, min_time)
}

fstx_features_impl <- function(args) {
  .Call(`_fstx_fstx_features_impl`, args)
}

fstx_arrow_stream_impl <- function(args) {
  .Call(`_fstx_fstx_arrow_stream_impl`, args)
}
