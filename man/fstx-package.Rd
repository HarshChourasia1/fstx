\name{fstx-package}
\alias{fstx-package}
\alias{fstx}
\docType{package}
\title{Fast, Safe Column-Store Files for R Data Frames}
\description{
Reads and writes data frames in the FST-X column-store format. A file holds row groups of 64-byte
aligned pages; each page gets the lightweight encoding that fits it best (bit-packing,
frame-of-reference, delta, run-length, dictionary, ALP for doubles, FSST for strings) under an optional
LZ4 or ZSTD codec. A FlatBuffers footer keeps a page directory with per-page statistics, and every page
has an XXH3 checksum. Reads and writes run in parallel.

Main functions:
\itemize{
  \item \code{\link{write_fstx}} writes or appends a data frame.
  \item \code{\link{read_fstx}} reads all or part of a file: columns, a row window, a row list or a
    filter; optionally lazily (ALTREP columns over the memory-mapped file).
  \item \code{\link{fstx_meta}} and \code{\link{fstx_pages}} describe a file without reading its data.
  \item \code{\link{fstx_which}} and \code{\link{fstx_explain}} run a filter and show how it is served.
  \item \code{\link{fstx_compact}} removes what appends leave behind.
  \item \code{\link{fstx_simd}} shows or chooses the SIMD instruction set of the decoder.
}
}
\section{Options}{
\describe{
  \item{\code{fstx.simd}, environment variable \code{FSTX_SIMD}}{SIMD target chosen when the package
    loads; see \code{\link{fstx_simd}}.}
  \item{Threads}{Functions with a \code{threads} argument use all cores by default. The default is at
    most 2 when the environment variable \code{_R_CHECK_LIMIT_CORES_} is set (and not \code{"false"}), as
    during \command{R CMD check --as-cran}, and at most \code{OMP_THREAD_LIMIT} when that is set.}
}
}
\seealso{\code{\link{write_fstx}}, \code{\link{read_fstx}}}
\keyword{package}
