// FST-X R layer, 1.0: format features and the 0.7 upgrade path (WS1, then WS8). MIT licence.
// Phase 0 placeholder: registered and compiled, does nothing yet. Each entry point takes one argument list and
// returns one R object, so its inner arguments can change without touching the registration files.
#include <cpp11.hpp>

[[cpp11::register]]
SEXP fstx_features_impl(SEXP args) {
  (void)args;
  return R_NilValue;
}

[[cpp11::register]]
SEXP fstx_upgrade_impl(SEXP args) {
  (void)args;
  return R_NilValue;
}
