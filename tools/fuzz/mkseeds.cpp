// fstx fuzz seed corpus (WS6): small synthetic files written through the engine itself, so a changed writer gives
// fresh seeds on every run. Usage: mkseeds <out_dir>. Every seed is 16 zero control bytes + one fstx file.
//   - every forced encoding (auto, plain..alprd, fsst, string dictionaries with forced code pages, FSST
//     dictionaries, keep_codec) x codec none / lz4 / zstd, with no NA, scattered NA (bitmap) or NA runs;
//   - two bigger files with several row groups and pages;
//   - forged files (fstx::forge_copy, the engine of fstx:::.fstx_forge): header bits, unknown required and optional
//     feature bits with and without names, reserved bits 16-19 and 63, profile 3-255, format 2, reserved type,
//     logical type, codec, page kind, encoding tag and NA mode numbers;
//   - an interrupted append (committed end in the header, junk after it), factor levels that are not UTF-8 or hold
//     a NUL, an empty table.
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "core_file.h"
#include "core_internal.h"
#include "fstx_core.h"

namespace {

const int32_t kNaI = std::numeric_limits<int32_t>::min();
std::string g_out;
int g_count = 0;

uint32_t rng = 12345;
uint32_t next() {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}
double bits(uint64_t b) {
  double x;
  std::memcpy(&x, &b, 8);
  return x;
}

struct Data {
  uint64_t n = 0;
  std::vector<int32_t> i, l, f, dt;
  std::vector<double> d, ts;
  std::vector<std::string> s, s2;
  std::vector<uint8_t> s_na, s2_na;
};

int get_s(void* ctx, uint64_t row, uint32_t n, const char** p, int32_t* lens) {
  const Data* x = (const Data*)ctx;
  for (uint32_t k = 0; k < n; ++k) {
    p[k] = x->s[row + k].data();
    lens[k] = x->s_na[row + k] ? -1 : (int32_t)x->s[row + k].size();
  }
  return 0;
}
int get_s2(void* ctx, uint64_t row, uint32_t n, const char** p, int32_t* lens) {
  const Data* x = (const Data*)ctx;
  for (uint32_t k = 0; k < n; ++k) {
    p[k] = x->s2[row + k].data();
    lens[k] = x->s2_na[row + k] ? -1 : (int32_t)x->s2[row + k].size();
  }
  return 0;
}

// na: 0 none, 1 scattered (bitmap), 2 runs
Data make(uint64_t n, int na, int pat) {
  Data x;
  x.n = n;
  const char* words[] = {"north", "south", "", "caf\xc3\xa9", "\xe2\x82\xac" "5", "x"};
  for (uint64_t r = 0; r < n; ++r) {
    const uint32_t z = next();
    const bool isna = na == 1 ? (z % 7 == 0) : na == 2 ? ((r / 40) % 3 == 1) : false;
    int32_t iv;
    switch (pat % 4) {
      case 0: iv = (int32_t)(r * 3); break;              // sequence (DELTA)
      case 1: iv = (int32_t)(z % 50) + 1000; break;       // narrow range (FOR)
      case 2: iv = (int32_t)((r / 64) % 5); break;        // runs (RLE)
      default: iv = (int32_t)(z % 4) * 1000003; break;    // few distinct (DICT)
    }
    x.i.push_back(isna ? kNaI : iv);
    x.l.push_back(isna ? kNaI : (int32_t)(z >> 3 & 1));
    x.f.push_back(isna ? kNaI : 1 + (int32_t)(z % 3));
    x.dt.push_back(isna ? kNaI : 19000 + (int32_t)r);
    double dv;
    switch (pat % 3) {
      case 0: dv = (double)(int32_t)(z % 100000) / 100.0; break;  // decimals (ALP)
      case 1: dv = bits(((uint64_t)next() << 32 | z) & 0x3FFFFFFFFFFFFFFFull); break;  // raw bits (ALPRD)
      default: dv = (r % 11 == 5) ? bits(0x7FF8000000000000ull) : (double)r * 0.5; break;  // with a NaN
    }
    x.d.push_back(isna ? bits(0x7FF00000000007A2ull) : dv);
    x.ts.push_back(isna ? bits(0x7FF00000000007A2ull) : 1.7e9 + (double)r * 60.0);
    x.s_na.push_back(isna ? 1 : 0);
    x.s.push_back(isna ? std::string() : std::string(words[z % 6]) + std::to_string(r % 13));
    x.s2_na.push_back(isna ? 1 : 0);
    x.s2.push_back(isna ? std::string() : "https://example.com/p/" + std::to_string(z % 100000));
  }
  return x;
}

std::string path_of(const std::string& name) { return g_out + "/" + name + ".fstx"; }

bool write(const std::string& file, const Data& x, int codec, int force, uint64_t rg = 1024,
           const std::vector<std::string>* levels = nullptr, bool append_crash = false) {
  const char* lv_default[] = {"a", "b", "c"};
  std::vector<const char*> lp;
  std::vector<uint32_t> ll;
  if (levels) {
    for (const auto& s : *levels) {
      lp.push_back(s.data());
      ll.push_back((uint32_t)s.size());
    }
  } else {
    for (const char* s : lv_default) {
      lp.push_back(s);
      ll.push_back((uint32_t)std::strlen(s));
    }
  }
  const char* tz = "UTC";
  struct C {
    const char* name;
    int phys, logical;
  } defs[] = {{"i", FSTX_INT32, 0},        {"d", FSTX_FLOAT64, 0},          {"l", FSTX_LOGICAL, 0},
              {"f", FSTX_INT32, FSTX_LT_FACTOR}, {"dt", FSTX_INT32, FSTX_LT_DATE}, {"ts", FSTX_FLOAT64, FSTX_LT_POSIXCT},
              {"s", FSTX_STRING, 0},       {"s2", FSTX_STRING, 0}};
  const uint32_t nc = sizeof defs / sizeof defs[0];
  std::vector<fstx_write_column> wc(nc);
  std::vector<fstx_column_data> cd(nc);
  for (uint32_t j = 0; j < nc; ++j) {
    fstx_write_column& w = wc[j];
    std::memset(&w, 0, sizeof w);
    w.name = defs[j].name;
    w.name_len = (uint32_t)std::strlen(defs[j].name);
    w.phys = defs[j].phys;
    w.logical = defs[j].logical;
    if (defs[j].logical == FSTX_LT_FACTOR) {
      w.n_levels = (uint32_t)lp.size();
      w.levels = lp.data();
      w.level_lens = ll.data();
    }
    if (defs[j].logical == FSTX_LT_POSIXCT) {
      w.has_tzone = 1;
      w.tzone = tz;
      w.tzone_len = 3;
    }
    cd[j].ctx = (void*)&x;
  }
  cd[0].data = x.i.data();
  cd[1].data = x.d.data();
  cd[2].data = x.l.data();
  cd[3].data = x.f.data();
  cd[4].data = x.dt.data();
  cd[5].data = x.ts.data();
  cd[6].get_strings = get_s;
  cd[7].get_strings = get_s2;
  fstx_write_options o;
  std::memset(&o, 0, sizeof o);
  o.codec = codec;
  o.level = 3;
  o.threads = 1;
  o.row_group_size = rg;
  o.page_bytes = 4096;
  o.profile = codec == 0 ? 0 : 1;
  o.keep_pct = 0;  // keep the codec whenever it saves anything, so codec pages are in the seeds
  o.force_encoding = force;
  fstx_writer* w = nullptr;
  fstx_error err;
  if (fstx_writer_open(file.c_str(), wc.data(), nc, nullptr, 0, &o, &w, &err) != FSTX_OK ||
      fstx_writer_append(w, cd.data(), x.n, &err) != FSTX_OK || fstx_writer_close(w, &err) != FSTX_OK) {
    std::fprintf(stderr, "write %s: %s\n", file.c_str(), err.message);
    return false;
  }
  if (append_crash) {  // a second append that "dies" after its tail: the header keeps the committed end
    o.test_crash = 1;
    if (fstx_writer_open_append(file.c_str(), wc.data(), nc, nullptr, 0, &o, &w, &err) != FSTX_OK ||
        fstx_writer_append(w, cd.data(), x.n, &err) != FSTX_OK) {
      std::fprintf(stderr, "append %s: %s\n", file.c_str(), err.message);
      return false;
    }
    fstx_writer_close(w, &err);
  }
  return true;
}

// the seed = 16 zero control bytes + the file
void seal(const std::string& file, const std::string& name) {
  FILE* in = std::fopen(file.c_str(), "rb");
  if (!in) return;
  std::vector<uint8_t> b(16, 0);
  uint8_t buf[65536];
  size_t k;
  while ((k = std::fread(buf, 1, sizeof buf, in)) > 0) b.insert(b.end(), buf, buf + k);
  std::fclose(in);
  FILE* out = std::fopen((g_out + "/" + name).c_str(), "wb");
  std::fwrite(b.data(), 1, b.size(), out);
  std::fclose(out);
  std::remove(file.c_str());
  ++g_count;
}

void forged(const std::string& base, const std::string& name, const fstx::Forge& g) {
  const std::string out = path_of(name);
  try {
    fstx::forge_copy(base, out, g);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "forge %s: %s\n", name.c_str(), e.what());
    return;
  }
  seal(out, "seed_" + name);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: mkseeds <out_dir>\n");
    return 2;
  }
  g_out = argv[1];
  mkdir(g_out.c_str(), 0755);
  const int forces[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 6 | 256, 6 | 512, 1 | 1024};
  const char* fnames[] = {"auto", "plain", "const", "for", "delta", "rle", "dict", "split", "alp", "alprd",
                          "fsst", "dictcodes", "fsstdict", "keepcodec"};
  const char* cnames[] = {"none", "lz4", "zstd"};
  int k = 0;
  for (int e = 0; e < 14; ++e)
    for (int codec = 0; codec < 3; ++codec, ++k) {
      const Data x = make(300, k % 3, e);
      const std::string name = std::string(fnames[e]) + "_" + cnames[codec] + "_na" + std::to_string(k % 3);
      if (write(path_of(name), x, codec, forces[e])) seal(path_of(name), "seed_" + name);
    }
  {  // several row groups and pages
    const Data x = make(2600, 2, 1);
    if (write(path_of("multi_none"), x, 0, 0)) seal(path_of("multi_none"), "seed_multi_none");
    const Data y = make(2600, 1, 3);
    if (write(path_of("multi_zstd"), y, 2, 0)) seal(path_of("multi_zstd"), "seed_multi_zstd");
  }
  {  // empty table, interrupted append, factor levels that are not valid UTF-8 or hold a NUL
    const Data e = make(0, 0, 0);
    if (write(path_of("empty"), e, 0, 0)) seal(path_of("empty"), "seed_empty");
    const Data x = make(200, 1, 0);
    if (write(path_of("append_crash"), x, 1, 0, 1024, nullptr, true)) seal(path_of("append_crash"), "seed_append_crash");
    const std::vector<std::string> bad = {"ok", "\xff\xfe", "a\xc0\xaf", std::string("n\0l", 3)};
    if (write(path_of("levels_bad"), x, 0, 0, 1024, &bad)) seal(path_of("levels_bad"), "seed_levels_bad");
  }
  // forged files from one plain base file
  const Data x = make(300, 1, 0);
  const std::string base = path_of("forge_base");
  if (!write(base, x, 0, 0)) return 1;
  auto F = [] { return fstx::Forge(); };
  fstx::Forge g;
  g = F(); g.hdr_flags = 1 << 3; g.req = 1ull << 3; forged(base, "hdrbit3", g);
  g = F(); g.req = 1ull << 40; g.names = {"future_req"}; forged(base, "req40_named", g);
  g = F(); g.req = 1ull << 41; forged(base, "req41_unnamed", g);
  g = F(); g.opt = 1ull << 40; g.names = {"future_opt"}; forged(base, "opt40_named", g);
  g = F(); g.opt = (1ull << 33) | (1ull << 50); forged(base, "opt33_50_unnamed", g);
  for (int bit : {16, 17, 18, 19, 63}) {
    g = F(); g.req = 1ull << bit; forged(base, "req_reserved" + std::to_string(bit), g);
  }
  for (int p : {3, 7, 255}) {
    g = F(); g.profile = p; forged(base, "profile" + std::to_string(p), g);
  }
  g = F(); g.version = 2; forged(base, "format2", g);
  g = F(); g.version = 2; g.profile = 3; forged(base, "format2_profile3", g);
  g = F(); g.version = 4; forged(base, "format4", g);
  g = F(); g.phys0 = 4; forged(base, "phys4", g);
  g = F(); g.phys0 = 5; forged(base, "phys5", g);
  g = F(); g.logical0 = 4; forged(base, "logical4", g);
  for (int cdc : {3, 4, 77, 200}) {
    g = F(); g.codec0 = cdc; forged(base, "codec" + std::to_string(cdc), g);
  }
  g = F(); g.kind0 = 7; forged(base, "kind7", g);
  g = F(); g.tag0 = 11; forged(base, "tag11", g);
  g = F(); g.na0 = 3; forged(base, "na3", g);
  std::remove(base.c_str());
  std::printf("mkseeds: %d seeds in %s\n", g_count, g_out.c_str());
  return 0;
}
