\name{write_fstx}
\alias{write_fstx}
\title{Write a data frame to an FST-X file}
\description{
Writes a data frame to an FST-X file, or appends its rows to an existing one.
}
\usage{
write_fstx(x, path, profile = c("balanced", "fast", "small"), compress = NULL,
           level = NULL, threads = fstx_hw_threads(), append = FALSE, .encoding = NULL,
           .crash = 0L)
}
\arguments{
  \item{x}{a data.frame (data.table and tibble keep their class).}
  \item{path}{file path.}
  \item{profile}{\code{"balanced"} (default: encodings plus ZSTD level 3 where it saves at least 10\%),
    \code{"fast"} (encodings only, no codec: fastest reads) or \code{"small"} (encodings plus ZSTD level 9).}
  \item{compress}{outer codec per page, overriding the profile: \code{"zstd"}, \code{"lz4"} or \code{"none"}.}
  \item{level}{ZSTD level (ignored for lz4 and none); default 3, or 9 for profile \code{"small"}.}
  \item{threads}{number of threads (default: all cores; see \code{\link{fstx-package}} for limits).}
  \item{append}{\code{FALSE} (default) replaces the file. \code{TRUE} adds the rows of \code{x} at the end
    of an existing file (a missing file is simply written). Columns must match the file's in names, order,
    type and class; factor levels may grow (the file's levels first, new ones after; ordered factors need
    the same levels); POSIXct columns need the same time zone; files with stored row names cannot be
    appended to. The profile defaults to the file's (a profile written by a later fstx: the new rows are
    encoded as \code{"balanced"} and the file keeps its stored value). An interrupted append leaves the old
    table readable. The file keeps its format version: a file written by fstx 0.7 stays format 2. A file
    with an optional feature this fstx does not know cannot be appended to ("Update fstx to change this
    file; reading works").}
  \item{.encoding}{for tests only: force one encoding on every page where it applies, plus any of the flags
    \code{"dict_codes"}, \code{"fsst_dict"} and \code{"keep_codec"}.}
  \item{.crash}{for tests only.}
}
\details{
Every page is encoded with the lightweight encoding that fits it best (bit-packing, frame-of-reference,
delta, run-length, dictionary, ALP for doubles, FSST for strings); the profile decides what goes on top.
Columns may be logical, integer, double or character vectors. Factors (and ordered factors), Date and
POSIXct are stored natively; any other attributes (classes, units, labels) are kept and restored on read.
Matrix and list columns are not supported. Strings are stored as UTF-8.

New files are written in fstx format 1.0 (format version 3 inside the file), which fstx 0.7 cannot read.
}
\value{\code{x}, invisibly.}
\seealso{\code{\link{read_fstx}}, \code{\link{fstx_compact}}}
\examples{
p <- tempfile(fileext = ".fstx")
x <- data.frame(id = 1:1000, g = sample(c("a", "b", "c"), 1000, TRUE), v = runif(1000))
write_fstx(x, p, threads = 1)
write_fstx(x[1:10, ], p, append = TRUE, threads = 1)
fstx_meta(p)
unlink(p)
}
