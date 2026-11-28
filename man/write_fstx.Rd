\name{write_fstx}
\alias{write_fstx}
\title{Write a data frame to an fstx file}
\description{
Writes a data frame to an fstx file, or appends its rows to an existing one.
}
\usage{
write_fstx(x, path, profile = c("balanced", "fast", "small"), compress = NULL,
           level = NULL, threads = fstx_hw_threads(), append = FALSE,
           int64 = c("native", "integer64"))
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
    appended to. Appending rows to a data.table drops its key and indices (the new rows may not follow
    them; set them again with \code{setkey()} / \code{setindex()}); appending 0 rows keeps them.
    The profile defaults to the file's (a profile written by a later fstx: the new rows are encoded as
    \code{"balanced"} and the file keeps its stored value). An interrupted append leaves the old table readable.
    A file with an optional feature this fstx does not know cannot be appended to ("Update fstx to change this
    file; reading works"). Appending reads the file's table and column attributes, with \code{unserialize()}
    for any stored with R's \code{serialize()} (append has no \code{attributes = FALSE} switch), so the
    trusted-source note in \code{\link{read_fstx}} applies to those files.
    An append writes \code{integer64} rows in the layout the file's column already has, whatever
    \code{int64} says.}
  \item{int64}{how \code{bit64::integer64} columns are stored. \code{"native"} (default): as 64-bit integer
    columns (fstx 1.2; exact, compact, and filterable with \code{where}); files with such a column need fstx
    1.2 or later. \code{"integer64"}: as fstx 1.0 stored them (a double column carrying the class); such
    columns cannot be filtered.}
}
\details{
Every page is encoded with the lightweight encoding that fits it best (bit-packing, frame-of-reference,
delta, run-length, dictionary, ALP for doubles, FSST for strings); the profile decides what goes on top.
Columns may be logical, integer, double or character vectors, \code{bit64::integer64} vectors, or
binary columns. Factors (and ordered factors), Date and
POSIXct are stored natively; any other attributes (classes, units, labels) are kept and restored on read,
in fstx's own checked encoding when they are plain vectors, else with R's \code{serialize()} (see
\code{\link{read_fstx}}).
Strings are stored as UTF-8.

64-bit integers: a \code{bit64::integer64} column is a double vector that holds the integers' bits; fstx
stores those bits as they are (see \code{int64}), so bit64 is not needed to write or read one. Its
\code{NA} is the format's missing value.

Binary columns (fstx 1.2): a list column whose elements are all raw vectors or \code{NULL} (a missing value)
is stored as a binary column, bytes as they are (any byte, \code{NUL} included; a value can have up to 1 GB).
Its class may be none, \code{"AsIs"} (\code{I(list(...))}), \code{"blob"} (blob package),
\code{"arrow_binary"} or another vctrs \code{list_of} of raw vectors; it is restored on read. Files with a
binary column need fstx 1.2 or later. Other list columns, and matrix columns, are not supported.
Memory: besides the data and the pages in flight, the writer keeps the string pointers of character
columns it does not dictionary-code, 12 bytes per string and at most 8 MB per call (past that it reads
those strings a second time instead).

Files are written in format version 3. A file with int64 or binary columns needs fstx 1.2 or later to read.
}
\value{\code{x} as it was passed in, invisibly (also after an append that added factor levels).}
\seealso{\code{\link{read_fstx}}, \code{\link{fstx_compact}}}
\examples{
p <- tempfile(fileext = ".fstx")
x <- data.frame(id = 1:1000, g = sample(c("a", "b", "c"), 1000, TRUE), v = runif(1000))
write_fstx(x, p, threads = 1)
write_fstx(x[1:10, ], p, append = TRUE, threads = 1)
fstx_meta(p)
unlink(p)
}
