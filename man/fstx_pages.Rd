\name{fstx_pages}
\alias{fstx_pages}
\title{List the stored pages of an FST-X file}
\usage{
fstx_pages(path)
}
\arguments{
  \item{path}{file path.}
}
\description{
One row per page (string dictionary pages included) with its row group, column, rows, sizes, codec and
the encoding the writer chose. Only the footer is read.
}
\value{a data frame.}
\seealso{\code{\link{fstx_meta}}}
\examples{
p <- tempfile(fileext = ".fstx")
write_fstx(data.frame(a = 1:100, s = rep(c("x", "y"), 50)), p, threads = 1)
fstx_pages(p)
unlink(p)
}
