\name{fstx_arrow_stream}
\alias{fstx_arrow_stream}
\title{Read an fstx file as an Arrow stream}
\usage{
fstx_arrow_stream(path, columns = NULL, from = 1, to = NULL, where = NULL,
  threads = fstx_hw_threads(), batch_rows = NULL)
}
\arguments{
  \item{path}{file path.}
  \item{columns}{column names or positions, or \code{NULL} for all.}
  \item{from, to}{first and last row (1-based, inclusive), as in \code{\link{read_fstx}}.}
  \item{where}{row filter, as in \code{\link{read_fstx}}. It runs when the stream is made; the stream then
    reads only the matching rows.}
  \item{threads}{threads used to decode each batch (at most the hardware threads): its columns are filled in
    parallel, and when there are fewer columns than threads, so are row parts of each integer, double,
    logical, factor, Date or POSIXct column (cut at page edges). The batches are the same for any number of
    threads.}
  \item{batch_rows}{most rows per batch, or \code{NULL} for one batch per row group. A batch never spans two
    row groups.}
}
\description{
Returns an Arrow C stream (a \code{nanoarrow_array_stream}) filled straight from the file, without making R
vectors. Arrow-based tools such as arrow, DuckDB or Polars can take it, so a text column goes from the file to
the tool without R strings being made. Needs the 'nanoarrow' package.
}
\details{
Each batch is a record batch, one per row group by default. Column types:
\itemize{
  \item integer: int32; double: float64; logical: bool.
  \item character: utf8 (large_utf8 when a column's text could pass 2 GB in one batch). When every page of
    the column in the rows read is dictionary-coded, it is a dictionary (int32 indexes into utf8 values); each
    batch carries the dictionary of its row group.
  \item factor: dictionary of its levels (int32 indexes, utf8 levels), marked ordered for ordered factors.
  \item Date: date32. A Date with a fraction of a day is rounded down to its day, as R prints it.
  \item POSIXct: timestamp in microseconds with the column's time zone (none if the column had none), rounded
    to the nearest microsecond.
}
Text is passed on as stored in the file; it is not checked for valid UTF-8 (factor levels are, when the stream is
made). Missing values become Arrow nulls. A double \code{NaN} stays \code{NaN}; \code{NaN} and infinite times and
Dates become nulls. Other attributes and the table class are not carried.

nanoarrow 0.9.0 cannot convert an ordered dictionary (an ordered factor) back to R.

The stream holds the file open until it is released (dropped and garbage-collected). Its callbacks never call
R, so a consumer may pull batches from its own threads.
}
\value{a \code{nanoarrow_array_stream}.}
\examples{
if (requireNamespace("nanoarrow", quietly = TRUE)) {
  p <- tempfile(fileext = ".fstx")
  x <- data.frame(id = 1:1000, g = rep(c("a", "b"), 500))
  write_fstx(x, p, threads = 1)
  s <- fstx_arrow_stream(p, where = id > 990, threads = 1)
  print(nanoarrow::convert_array_stream(s))
  rm(s); invisible(gc())   # releases the stream, which closes the file
  unlink(p)
}
}
