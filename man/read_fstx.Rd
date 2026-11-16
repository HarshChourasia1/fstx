\name{read_fstx}
\alias{read_fstx}
\title{Read an fstx file}
\description{
Reads all or part of an fstx file into a data frame: chosen columns, a row window, a row list or
the rows that match a filter, eagerly or lazily.
}
\usage{
read_fstx(path, columns = NULL, from = 1, to = NULL, threads = fstx_hw_threads(),
          verify = TRUE, lazy = FALSE, strings = c("auto", "eager", "deferred"),
          as_factor = FALSE, rows = NULL, where = NULL)
}
\arguments{
  \item{path}{file path.}
  \item{columns}{\code{NULL} (all), column names, or column positions.}
  \item{from, to}{first and last row to read (1-based, inclusive); \code{to = NULL} reads to the end.}
  \item{threads}{number of threads (default: all cores; see \code{\link{fstx-package}} for limits).}
  \item{verify}{check every page's checksum (header and footer checksums are always checked). Lazy
    columns check a page when it is decoded. Must be \code{TRUE} or \code{FALSE}.}
  \item{lazy}{\code{FALSE} (default) reads the data now. \code{TRUE} returns at once: every column is an
    ALTREP vector backed by the memory-mapped file and decoded a page at a time when it is used, so RAM
    covers only the columns (and pages) that are touched. The file stays open (and cannot be overwritten
    from this session) until those columns are fully used or garbage collected.}
  \item{strings}{\code{"auto"} (default) decodes string pages in parallel, closes the file at
    once and returns each character column as an ALTREP vector whose R strings are made a page at a time
    on first use (the first page used alone, later pages into one full vector, which becomes the column
    once every page is made). A column whose pages are all dictionary-coded (few distinct
    values) is made during the read and returned as a plain character vector, as that costs little.
    \code{saveRDS()} / \code{serialize()} write such columns in R's standard format, so the file reads
    back without fstx. \code{"deferred"} is the same, but keeps dictionary
    columns deferred too, and an unmade column is serialised in a compact form that needs fstx to read
    back (smaller and faster to write). \code{"eager"} makes every R string during the read and returns
    plain character vectors. The old name \code{"altrep"} still works as \code{"deferred"}, with a
    warning, through fstx 1.x.}
  \item{as_factor}{\code{FALSE} (default), \code{TRUE} (every character column whose pages are all
    dictionary-coded becomes a factor, built from the dictionaries without hashing the rows), or column
    names (those columns become factors; ones that are not dictionary-coded go through \code{factor()}).
    Levels are the sorted union of the dictionaries read, so a full read gives the same factor as
    \code{factor(x)}.}
  \item{rows}{row numbers to read (1-based, any order, repeats allowed), returned in that order. Only the
    pages that hold them are read. A row list is read eagerly (\code{lazy} and \code{strings} are ignored,
    \code{as_factor} goes through \code{factor()}). Cannot be combined with \code{from} / \code{to} or
    \code{where}.}
  \item{where}{a condition on the file's columns, such as \code{x > 5 & y == "a"} (also a formula
    \code{~ cond}, or a quoted call held in a variable): terms joined by \code{&}, each a comparison of a
    column with a value (\code{==}, \code{!=}, \code{<}, \code{<=}, \code{>}, \code{>=}), \code{\%in\%},
    \code{between()}, \code{is.na()}, \code{!is.na()} or a logical column. Pages are skipped by their
    min / max / NA count, then the remaining rows are tested exactly; rows where the condition is
    \code{NA} are dropped, as \code{subset()} does. Applied within \code{from} / \code{to}.}
}
\details{
Reads files in fstx format 1.0 (format version 3); files from fstx 0.x development versions are
refused. A file written by a later fstx that uses something this version does not know (a column type, codec or file feature) is refused
with a message that names it and ends "Update fstx", rather than reported as damaged. A damaged file gives
an R error; with \code{verify = TRUE} (the default) every page checksum is checked as well.

Column and table attributes are stored with R's \code{serialize()} and restored with \code{unserialize()},
as \code{readRDS()} does. Like an .rds file, only read .fstx files from sources you trust: a crafted file
can crash R when its attributes are restored. The checksum catches accidental damage, not deliberate
tampering.
}
\value{a data frame with the class it was written with (data.frame, data.table or tibble).}
\seealso{\code{\link{write_fstx}}, \code{\link{fstx_which}}, \code{\link{fstx_explain}}}
\examples{
p <- tempfile(fileext = ".fstx")
x <- data.frame(id = 1:5000, g = sample(c("a", "b", "c"), 5000, TRUE),
                key = sprintf("k\%05d", sample.int(5000)), v = runif(5000))
write_fstx(x, p, threads = 1)

identical(read_fstx(p, threads = 1), x)
read_fstx(p, columns = c("id", "v"), from = 11, to = 15, threads = 1)
read_fstx(p, rows = c(5000, 1, 42), threads = 1)
head(read_fstx(p, where = g == "a" & v > 0.9, threads = 1))
str(read_fstx(p, as_factor = TRUE, threads = 1)$g)

z <- read_fstx(p, lazy = TRUE, threads = 1)   # columns decoded when used
mean(z$v)
rm(z); invisible(gc())                        # releases the file
unlink(p)
}
