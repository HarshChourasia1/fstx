\name{fstx_arrow_stream}
\alias{fstx_arrow_stream}
\title{Read an fstx file as an Arrow stream}
\usage{
fstx_arrow_stream(path, columns = NULL, from = 1, to = NULL, where = NULL,
  threads = fstx_hw_threads(), batch_rows = NULL)
}
\arguments{
  \item{path}{file path.}
  \item{columns}{column names, or \code{NULL} for all.}
  \item{from, to}{first and last row.}
  \item{where}{row filter, as in \code{\link{read_fstx}}.}
  \item{threads}{threads to use.}
  \item{batch_rows}{rows per batch, or \code{NULL} for one batch per row group.}
}
\description{
Placeholder in the fstx 1.0 development build: it does nothing yet.
}
\value{\code{NULL} for now.}
