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
\value{a named vector: file size \code{before} and \code{after}, invisibly.}
\seealso{\code{\link{write_fstx}}}
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
