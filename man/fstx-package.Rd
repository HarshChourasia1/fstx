\name{fstx-package}
\alias{fstx-package}
\alias{fstx}
\docType{package}
\title{Fast, Safe Column-Store Files for R Data Frames}
\description{
Reads and writes data frames in the fstx column-store file format. A file holds row groups of 64-byte
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
  \item \code{\link{fstx_arrow_stream}} reads a file as an Arrow stream.
  \item \code{\link{fstx_features}} lists what this build can read and write.
  \item \code{\link{fstx_simd}} shows or chooses the SIMD instruction set of the decoder.
}
}
\section{Guide}{
\strong{Write and read.} \code{write_fstx(x, path)} writes a data frame (data.table and tibble keep their
class). The profile picks the trade-off: \code{"balanced"} (default), \code{"fast"} (no codec, fastest
reads) or \code{"small"}. \code{read_fstx(path)} reads it back; \code{columns}, \code{from} / \code{to} and
\code{rows} read only part of it, and only the pages that hold those rows are read.

\strong{Filter.} \code{read_fstx(path, where = amount > 100 & city == "b")} returns the matching rows.
Pages whose statistics rule a condition out are skipped. \code{fstx_which()} returns the row numbers and
\code{fstx_explain()} shows how many pages were skipped.

\strong{Lazy reads and strings.} \code{lazy = TRUE} returns at once and decodes each column from the
memory-mapped file when it is used. By default (\code{strings = "auto"}) the R strings of a character
column are made on first use; \code{as_factor = TRUE} turns dictionary-coded columns into factors.

\strong{Append and compact.} \code{write_fstx(x, path, append = TRUE)} adds rows to the end of a file; an
interrupted append leaves the old table readable. \code{fstx_compact(path)} removes what appends leave
behind, and \code{merge = TRUE} also merges small row groups into full ones.

\strong{Arrow.} \code{fstx_arrow_stream()} hands the data to Arrow-based tools without making R
strings; see its help page.

\strong{File format.} Files are written in fstx format 1.0 (format version 3 inside the file). Its
specification ships with the package:
\code{system.file("spec", "fstx-format-1.0.md", package = "fstx")}. Files from fstx 0.x development
versions are not read. A file from a later fstx that needs something this version does not know is refused with a message that
names it and ends "Update fstx".
}
\section{Options}{
\describe{
  \item{\code{fstx.simd}, environment variable \code{FSTX_SIMD}}{SIMD target chosen when the package
    loads; see \code{\link{fstx_simd}}.}
  \item{\code{fstx.verify_nulls}}{\code{TRUE} counts the missing values of every page that
    \code{\link{read_fstx}} or \code{\link{fstx_arrow_stream}} decodes and refuses a page whose count differs
    from the one it records, as corrupt. Default \code{FALSE}; see \code{\link{read_fstx}}.}
  \item{Threads}{Functions with a \code{threads} argument use all cores by default. The default is at
    most 2 when the environment variable \code{_R_CHECK_LIMIT_CORES_} is set (and not \code{"false"}), as
    during \command{R CMD check --as-cran}, and at most \code{OMP_THREAD_LIMIT} when that is set.}
}
}
\seealso{\code{\link{write_fstx}}, \code{\link{read_fstx}}}
\examples{
p <- tempfile(fileext = ".fstx")
x <- data.frame(id = 1:20000, city = sample(c("a", "b", "c"), 20000, TRUE),
                amount = round(runif(20000, 0, 200), 2))

write_fstx(x, p, threads = 1)                                # write
y <- read_fstx(p, threads = 1)                               # read
identical(x, y)
read_fstx(p, columns = c("id", "amount"), from = 1, to = 3, threads = 1)
head(read_fstx(p, where = amount > 199 & city == "b", threads = 1))   # filter
fstx_explain(p, id > 19990)                                  # pages skipped

z <- read_fstx(p, lazy = TRUE, threads = 1)                  # lazy
sum(z$amount)
rm(z); invisible(gc())                                       # releases the file

write_fstx(x[1:100, ], p, append = TRUE, threads = 1)        # append
fstx_compact(p, merge = TRUE, threads = 1)                   # merge row groups
fstx_meta(p)$format_version
unlink(p)
}
\keyword{package}
