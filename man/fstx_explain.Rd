\name{fstx_explain}
\alias{fstx_explain}
\alias{fstx_which}
\title{Run a filter on an FST-X file: matching rows, and how the filter is served}
\description{
\code{fstx_which()} returns the rows that match a filter; \code{fstx_explain()} shows how many
pages the filter skips by their statistics and how many rows are left, without returning the data.
}
\usage{
fstx_explain(path, where, from = 1, to = NULL, exact = FALSE, threads = fstx_hw_threads())

fstx_which(path, where, from = 1, to = NULL, threads = fstx_hw_threads())
}
\arguments{
  \item{path}{file path.}
  \item{where}{a condition on the file's columns, as in \code{\link{read_fstx}}.}
  \item{from, to}{row window (1-based, inclusive).}
  \item{exact}{\code{FALSE} (default) only consults page statistics; \code{TRUE} also runs the filter
    and counts the rows.}
  \item{threads}{number of threads (default: all cores; see \code{\link{fstx-package}} for limits).}
}
\value{
\code{fstx_explain()}: a named numeric vector: \code{pages_total} (data pages of the filter columns in
the window), \code{pages_skipped} (ruled out by min / max / NA count or the dictionary),
\code{pages_pruned} (ruled out by other conditions), \code{pages_all} (every row matches),
\code{pages_read} (decoded), \code{rows_window}, \code{rows_candidate}, \code{rows_matched}.

\code{fstx_which()}: the matching row numbers (1-based, ascending, double).
}
\seealso{\code{\link{read_fstx}}}
\examples{
p <- tempfile(fileext = ".fstx")
x <- data.frame(id = 1:100000, g = rep(c("a", "b"), each = 50000))
write_fstx(x, p, threads = 1)
fstx_explain(p, id > 99990)
fstx_explain(p, id > 99990 & g == "b", exact = TRUE, threads = 1)
fstx_which(p, id > 99990, threads = 1)
unlink(p)
}
