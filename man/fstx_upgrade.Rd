\name{fstx_upgrade}
\alias{fstx_upgrade}
\title{Rewrite an fstx 0.7 file in format 1.0}
\usage{
fstx_upgrade(path, dest = NULL)
}
\arguments{
  \item{path}{path of an fstx file.}
  \item{dest}{output path; \code{NULL} (the default) replaces \code{path}.}
}
\description{
Rewrites a file written by fstx 0.2 to 0.7 (file format 2) in the format fstx 1.0 writes (file format 3,
"fstx format 1.0"). The pages are copied as they are, with their checksums checked, and only the header,
footer and tail are written again, so this is about as fast as copying the file and the data come back
exactly the same.
}
\details{
fstx 1.0 reads format 2 files as they are, so upgrading is never needed to read one. Appends
(\code{write_fstx(append = TRUE)}) and page-copy compaction (\code{fstx_compact()}) keep a file's format, so a
format 2 file stays readable by the fstx 0.7 that wrote it (\code{fstx_compact(merge = TRUE)} writes format 3). \code{fstx_upgrade()} is the one step that moves it to format 3.

The new file is written next to the output path first and checked (it must open and describe the same table)
before it replaces anything; if any step fails the original is left unchanged. A file that is already
format 3 is not rewritten (with \code{dest}, it is copied there, through the same temporary file). The
output path must not be held open by a lazy \code{read_fstx()} result in this session.

After the upgrade the file records this fstx as its writer; its profile, row groups, pages and attributes
are kept.
}
\section{Upgrading from 0.7}{
\itemize{
  \item fstx 1.0 reads every file fstx 0.2 to 0.7 wrote; nothing has to change to keep reading them.
  \item Once a file is format 3, fstx 0.7 refuses it ("Update fstx"). Upgrade only the files that no
    longer need to be read by 0.7.
  \item \code{fstx_compact(merge = TRUE)} also writes format 3, since it reads the table and writes a
    fresh file; \code{fstx_compact()} without \code{merge} keeps format 2.
  \item Files from fstx 0.1 (format 1) cannot be upgraded: read them with fstx 0.1.x and write them again.
}
}
\value{
Invisibly, an integer vector \code{c(old = , new = )}: the file's format version before and after.
}
\seealso{\code{\link{fstx_meta}} (its \code{format_version}), \code{\link{fstx_compact}},
\code{\link{fstx_features}}.}
\examples{
p <- tempfile(fileext = ".fstx")
write_fstx(data.frame(x = 1:10), p)
fstx_upgrade(p)  # already format 3: nothing to do
unlink(p)
}
