// FST-X R layer, 1.0: format features and the 0.7 upgrade path (WS1, then WS8). MIT licence.
// Each entry point takes one argument list and returns one R object, so its inner arguments can change without
// touching the registration files.
#include <cpp11.hpp>

#include <string>

#include "core_file.h"

namespace {

SEXP arg(SEXP args, const char* name) {
  SEXP nm = Rf_getAttrib(args, R_NamesSymbol);
  for (R_xlen_t i = 0; i < Rf_xlength(args); ++i)
    if (nm != R_NilValue && std::string(CHAR(STRING_ELT(nm, i))) == name) return VECTOR_ELT(args, i);
  return R_NilValue;
}

int int_arg(SEXP l, const char* name) {
  SEXP x = arg(l, name);
  return x == R_NilValue ? -1 : Rf_asInteger(x);
}

uint64_t bits_arg(SEXP l, const char* name) {  // feature bits as an integer vector of bit numbers
  SEXP x = arg(l, name);
  uint64_t r = 0;
  if (x == R_NilValue) return r;
  for (R_xlen_t i = 0; i < Rf_xlength(x); ++i) {
    const int b = INTEGER(x)[i];
    if (b < 0 || b > 63) cpp11::stop("fstx: feature bit out of range");
    r |= 1ull << b;
  }
  return r;
}

// Tests only: copy a file through compaction with header, footer or first-page fields overridden.
SEXP forge(SEXP f) {
  fstx::Forge g;
  g.hdr_flags = int_arg(f, "hdr_flags");
  g.version = int_arg(f, "version");
  g.phys0 = int_arg(f, "phys0");
  g.logical0 = int_arg(f, "logical0");
  g.codec0 = int_arg(f, "codec0");
  g.kind0 = int_arg(f, "kind0");
  g.tag0 = int_arg(f, "tag0");
  g.na0 = int_arg(f, "na0");
  g.profile = int_arg(f, "profile");
  g.req = bits_arg(f, "required");
  g.opt = bits_arg(f, "optional");
  SEXP nm = arg(f, "names");
  for (R_xlen_t i = 0; nm != R_NilValue && i < Rf_xlength(nm); ++i) g.names.push_back(CHAR(STRING_ELT(nm, i)));
  const std::string src = CHAR(STRING_ELT(arg(f, "src"), 0)), dst = CHAR(STRING_ELT(arg(f, "dst"), 0));
  std::string msg;
  try {
    fstx::forge_copy(src, dst, g);
  } catch (const fstx::Error& e) {
    msg = e.msg;
  } catch (const std::exception& e) {
    msg = e.what();
  }
  if (!msg.empty()) cpp11::stop("fstx: %s", msg.c_str());
  return R_NilValue;
}

}  // namespace

[[cpp11::register]]
SEXP fstx_features_impl(SEXP args) {
  if (arg(args, "forge") != R_NilValue) return forge(arg(args, "forge"));
  using namespace cpp11::literals;
  // feature bits: every bit with a name (reserved or read by this build)
  cpp11::writable::integers fbit;
  cpp11::writable::strings fname;
  cpp11::writable::logicals fknown;
  for (int b = 0; b < 64; ++b) {
    const char* n = fstx::feature_name(b);
    if (!n) continue;
    fbit.push_back(b);
    fname.push_back(n);
    fknown.push_back(cpp11::r_bool((fstx::features_known() >> b & 1u) != 0));
  }
  cpp11::writable::integers cid;
  cpp11::writable::strings cname;
  cpp11::writable::logicals cread, cwrite;
  for (int c = 0; c < 128; ++c) {
    const char* n = fstx::codec_name(c);
    if (!n) continue;
    cid.push_back(c);
    cname.push_back(n);
    cread.push_back(cpp11::r_bool(fstx::codec_supported(c)));
    // the codecs the writer takes (core_writer.cpp configure()); a codec could be read-only in a later build
    cwrite.push_back(cpp11::r_bool(fstx::codec_supported(c) && c >= FSTX_CODEC_NONE && c <= FSTX_CODEC_ZSTD));
  }
  cpp11::writable::integers fr({(int)fstx::kFormatOldest, (int)fstx::kFormatVersion});
  return cpp11::writable::list({"version"_nm = cpp11::writable::strings({fstx::kVersion}),
                                "format_read"_nm = fr,
                                "format_write"_nm = cpp11::writable::integers({(int)fstx::kFormatVersion}),
                                "feature_bit"_nm = fbit, "feature_name"_nm = fname, "feature_read"_nm = fknown,
                                "codec_id"_nm = cid, "codec_name"_nm = cname, "codec_read"_nm = cread, "codec_write"_nm = cwrite});
}

// fstx_upgrade(): page-copy compaction of a format 2 file into `tmp` with the format version set to 3. Pages are
// copied unchanged (checksums checked); format 2 files have no feature fields, so the new footer has none.
// Returns the source file's format version (nothing is written when it is already 3). Refuses when `out`, the
// file the result will replace, is still memory-mapped in this session.
[[cpp11::register]]
SEXP fstx_upgrade_impl(SEXP args) {
  const std::string src = CHAR(STRING_ELT(arg(args, "path"), 0)), tmp = CHAR(STRING_ELT(arg(args, "tmp"), 0)),
                    out = CHAR(STRING_ELT(arg(args, "out"), 0));
  std::string msg;
  int version = 0;
  try {
    // the output replaces an existing file (the source itself when upgrading in place): refuse one still mapped
    const fstx::FileId oid = fstx::file_id_of_path(out);
    if (oid.ok && fstx::registry_has(oid))
      fstx::fail(FSTX_ERR_IO,
                 "cannot replace '%s': it is open in this R session, memory-mapped by a lazy read_fstx() result that "
                 "is still alive. Remove those objects (rm()) first",
                 out.c_str());
    {
      fstx_file F;
      fstx::open_file(F, src);
      version = (int)F.version;
    }
    if (version < (int)fstx::kFormatVersion) {
      fstx::Forge g;
      g.version = (int)fstx::kFormatVersion;
      fstx::forge_copy(src, tmp, g);
    }
  } catch (const fstx::Error& e) {
    msg = e.msg;
  } catch (const std::exception& e) {
    msg = e.what();
  }
  if (!msg.empty()) cpp11::stop("fstx_upgrade: %s", msg.c_str());
  return cpp11::as_sexp(version);
}
