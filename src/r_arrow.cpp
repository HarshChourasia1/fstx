// FST-X R layer, 1.0: Arrow C stream straight from the file (WS2). MIT licence.
// Phase 0 placeholder: registered and compiled, does nothing yet. The entry point takes one argument list and
// returns one R object, so its inner arguments can change without touching the registration files.
#include <cpp11.hpp>

#include "arrow_abi.h"

[[cpp11::register]]
SEXP fstx_arrow_stream_impl(SEXP args) {
  (void)args;
  return R_NilValue;
}
