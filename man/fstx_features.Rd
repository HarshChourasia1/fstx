\name{fstx_features}
\alias{fstx_features}
\title{What this build of fstx can read and write}
\usage{
fstx_features()
}
\description{
Lists the package version, the file format versions this build reads and writes, the named file feature
bits, the page codecs and the column types, each marked with whether this build can read it. Names marked
\code{FALSE} are reserved for later versions: a file that uses one is refused with a message that names it.
}
\value{A list with \code{version}, \code{format_read}, \code{format_write}, and data frames \code{features}
(\code{bit}, \code{name}, \code{read}), \code{codecs} (\code{id}, \code{name}, \code{read}, \code{write}) and
\code{types} (\code{id}, \code{name}, \code{read}, \code{write}). fstx 1.2 reads and writes the column
types \code{int64} and \code{binary} and the feature bits of the same names.}
\examples{
fstx_features()$format_read
}
