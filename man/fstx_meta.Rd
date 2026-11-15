\name{fstx_meta}
\alias{fstx_meta}
\alias{print.fstx_meta}
\title{Describe an FST-X file without reading its data}
\description{
Reads the footer of an FST-X file and reports its size, layout and columns without reading any data.
}
\usage{
fstx_meta(path)

\method{print}{fstx_meta}(x, ...)
}
\arguments{
  \item{path}{file path.}
  \item{x}{an \code{"fstx_meta"} object.}
  \item{...}{ignored.}
}
\value{
\code{fstx_meta()} returns an object of class \code{"fstx_meta"}: a list with \code{path}, \code{nrow},
\code{ncol}, \code{file_size}, \code{row_groups}, \code{pages}, \code{format_version}, \code{profile},
\code{table_class} and \code{columns}, a data frame with per column the name, stored type, class, time
zone, compressed and raw bytes and page count. \code{profile} is \code{"fast"}, \code{"balanced"},
\code{"small"}, or \code{"unknown"} for a profile a newer fstx wrote (informational; the file reads
normally). Only the footer is read. The print method returns
\code{x} invisibly.
}
\seealso{\code{\link{fstx_pages}}}
\examples{
p <- tempfile(fileext = ".fstx")
write_fstx(data.frame(a = 1:100, d = Sys.Date() + 0:99), p, threads = 1)
m <- fstx_meta(p)
m
m$columns
unlink(p)
}
