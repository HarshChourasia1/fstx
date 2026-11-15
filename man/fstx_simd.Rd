\name{fstx_simd}
\alias{fstx_simd}
\title{SIMD kernels: show or choose the instruction set the decoder uses}
\usage{
fstx_simd(target = NULL)
}
\arguments{
  \item{target}{\code{NULL} to only report, or \code{"scalar"}, \code{"auto"}, \code{"sse2"},
    \code{"sse4"}, \code{"avx2"}.}
}
\description{
FST-X decodes with SIMD kernels (Google Highway, compiled for SSE2, SSE4 and AVX2 on x86-64, for
Highway's own targets on ARM, and picked at run time) that give the same bytes as its plain loops. \code{fstx_simd()} returns the current state;
\code{fstx_simd("scalar")} turns the kernels off, \code{fstx_simd("auto")} picks the best target the CPU
runs, and \code{fstx_simd("avx2")} (or \code{"sse2"}, \code{"sse4"}) limits them to one target. The
environment variable \code{FSTX_SIMD} or the option \code{fstx.simd}, read when the package loads, set
the same. AVX-512 (\code{"avx3"}) is not compiled in until it is tested on a CPU that has it; CPUs with
AVX-512 use AVX2. Asking for a target that is not compiled in is an error.
}
\value{a list: \code{simd} (on or off), \code{target} (in use), \code{cpu} (targets this CPU runs),
\code{compiled} (targets built in).}
\examples{
fstx_simd()
fstx_simd("scalar")
fstx_simd("auto")
}
