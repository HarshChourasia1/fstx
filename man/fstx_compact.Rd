\name{fstx_compact}
\alias{fstx_compact}
\title{Compact an FST-X file}
\usage{
fstx_compact(path, merge = FALSE, threads = fstx_hw_threads())
}
\arguments{
  \item{path}{file path.}
  \item{merge}{\code{FALSE} (default): copy the pages; \code{TRUE}: rewrite the table.}
  \item{threads}{threads for \code{merge = TRUE}.}
}
\description{
Appends leave the old footers (and anything an interrupted append wrote) inside the file.
\code{fstx_compact()} copies the pages into a fresh file without re-encoding them, then replaces the
original. With \code{merge = TRUE} it reads the whole table and writes it again (same profile), so the
small row groups that appends add are merged into full ones.
}
\section{File format}{
The default page copy keeps the file's format version, profile and feature flags: a file written by fstx
0.7 (format 2) stays format 2, so fstx 0.7 can still read it.

\code{merge = TRUE} writes a fresh file in the current format (fstx format 1.0, version 3), as
\code{\link{write_fstx}} does. So a format 2 file becomes format 3, and fstx 0.7 can no longer read it.
The fresh file also drops optional features this fstx does not know (the data they described is not
copied), and a profile written by a later fstx becomes \code{"balanced"}.

A file with an optional feature this fstx does not know can be read, but page copy refuses it with
"Update fstx to change this file; reading works". \code{merge = TRUE} works on it, as described above.
}
\value{a named vector: file size \code{before} and \code{after}, invisibly.}
\seealso{\code{\link{write_fstx}}, \code{\link{fstx_meta}}}
\examples{
p <- tempfile(fileext = ".fstx")
x <- data.frame(a = 1:1000, b = runif(1000))
write_fstx(x, p, threads = 1)
for (i in 1:3) write_fstx(x, p, append = TRUE, threads = 1)
print(fstx_compact(p))
print(fstx_compact(p, merge = TRUE, threads = 1))
nrow(read_fstx(p, threads = 1))
unlink(p)
}
