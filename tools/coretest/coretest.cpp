// fstx coretest: the engine (src/, no R) on its own: kernel self-test, write, read, row lists, filters, append
// (with a simulated crash), compaction, column sources and error paths, under every SIMD target the CPU runs.
// Synthetic data from a fixed seed with integer-only arithmetic, so every platform writes the same bytes; the
// FNV-1a hash of each file is printed for comparing platforms. Build: tools/coretest/Makefile. Not in the
// package tarball. MIT licence.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core_file.h"  // fstx 1.2 tests: forge_copy, the footer's page statistics, cmp_d_i64
#include "fstx_core.h"
#include "kern_test.h"
#include "simd.h"

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const std::string& what) {
  if (ok) {
    ++g_pass;
  } else {
    ++g_fail;
    std::printf("FAIL %s\n", what.c_str());
  }
}
static bool rc_ok(int rc, const fstx_error& e, const std::string& what) {
  if (rc != FSTX_OK) std::printf("FAIL %s: code %d: %s\n", what.c_str(), e.code, e.message);
  check(rc == FSTX_OK, what);
  return rc == FSTX_OK;
}

// ---------------------------------------------------------------- data (same values on every platform)
static uint64_t splitmix(uint64_t& s) {
  uint64_t z = (s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
static double na_real() {  // R's NA_real_: a NaN with payload 1954
  const uint64_t b = 0x7FF00000000007A2ull;
  double d;
  std::memcpy(&d, &b, 8);
  return d;
}
static const int32_t NA_INT = INT32_MIN;
static const char* kCats[] = {"alpha", "beta", "gamma", "delta", "caf\xC3\xA9", "\xE6\x97\xA5\xE6\x9C\xAC", "x", ""};
static const char* kWords[] = {"red", "green", "blue", "fox", "jumps", "over", "lazy", "dog", "store", "pages"};
static const char* kLevels[] = {"lo", "mid", "hi"};

enum { C_ID, C_GRP, C_PRICE, C_NOISE, C_FLAG, C_CAT, C_TXT, C_FAC, C_DAY, NCOL };
static const char* kNames[NCOL] = {"id", "grp", "price", "noise", "flag", "cat", "txt", "fac", "day"};
static const int kPhys[NCOL] = {FSTX_INT32, FSTX_INT32,  FSTX_FLOAT64, FSTX_FLOAT64, FSTX_LOGICAL,
                                FSTX_STRING, FSTX_STRING, FSTX_INT32,   FSTX_FLOAT64};

struct Table {
  uint64_t n = 0;
  std::vector<int32_t> i32[NCOL];
  std::vector<double> f64[NCOL];
  std::vector<std::string> str[NCOL];
  std::vector<char> na[NCOL];  // strings only
};

static Table make_table(uint64_t n, uint64_t seed, int32_t id0) {
  Table t;
  t.n = n;
  for (int c = 0; c < NCOL; ++c) {
    if (kPhys[c] == FSTX_FLOAT64) t.f64[c].resize(n);
    else if (kPhys[c] == FSTX_STRING) t.str[c].resize(n), t.na[c].resize(n);
    else t.i32[c].resize(n);
  }
  uint64_t s = seed;
  for (uint64_t i = 0; i < n; ++i) {
    const uint64_t r = splitmix(s), r2 = splitmix(s);
    t.i32[C_ID][i] = id0 + (int32_t)i;
    t.i32[C_GRP][i] = i % 97 == 5 ? NA_INT : (int32_t)(r % 100);
    t.f64[C_PRICE][i] = i % 101 == 7 ? na_real() : (double)(int64_t)(r2 % 1000000) / 100.0;
    uint64_t bits = (r2 >> 12) | 0x3FF0000000000000ull;  // a double in [1, 2) from the raw bits
    double d;
    std::memcpy(&d, &bits, 8);
    t.f64[C_NOISE][i] = d - 1.0;
    t.i32[C_FLAG][i] = i % 13 == 0 ? NA_INT : (int32_t)((r >> 7) & 1);
    t.na[C_CAT][i] = i % 89 == 3;
    t.str[C_CAT][i] = kCats[(r >> 20) % 8];
    std::string w;
    const int k = 1 + (int)((r >> 30) % 4);
    for (int j = 0; j < k; ++j) w += (j ? " " : "") + std::string(kWords[(r >> (34 + 4 * j)) % 10]);
    if (i % 7 == 0) w += " " + std::to_string((r >> 40) % 100000);
    t.na[C_TXT][i] = i % 211 == 11;
    t.str[C_TXT][i] = w;
    t.i32[C_FAC][i] = i % 53 == 1 ? NA_INT : 1 + (int32_t)((r >> 50) % 3);
    t.f64[C_DAY][i] = 18000.0 + (double)(i / 1000);
  }
  return t;
}

static void append_rows(Table& a, const Table& b) {
  for (int c = 0; c < NCOL; ++c) {
    if (kPhys[c] == FSTX_FLOAT64) a.f64[c].insert(a.f64[c].end(), b.f64[c].begin(), b.f64[c].end());
    else if (kPhys[c] == FSTX_STRING) {
      a.str[c].insert(a.str[c].end(), b.str[c].begin(), b.str[c].end());
      a.na[c].insert(a.na[c].end(), b.na[c].begin(), b.na[c].end());
    } else a.i32[c].insert(a.i32[c].end(), b.i32[c].begin(), b.i32[c].end());
  }
  a.n += b.n;
}

// ---------------------------------------------------------------- writing
static int get_strings(void* ctx, uint64_t row, uint32_t n, const char** ptrs, int32_t* lens) {
  const auto* p = (const std::pair<const Table*, int>*)ctx;
  const Table& t = *p->first;
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t r = row + i;
    if (t.na[p->second][r]) {
      ptrs[i] = nullptr;
      lens[i] = -1;
    } else {
      ptrs[i] = t.str[p->second][r].data();
      lens[i] = (int32_t)t.str[p->second][r].size();
    }
  }
  return 0;
}

// get_values: mode 1 copies a page's rows into the writer's buffer, 2 hands them over in place, 3 fails after the
// first page
struct NumCtx {
  const Table* t;
  int c, mode;
};
static const void* get_values(void* ctx, uint64_t row, uint32_t n, void* buf) {
  const NumCtx& x = *(const NumCtx*)ctx;
  if (x.mode == 3 && row > 0) return nullptr;
  const bool dbl = kPhys[x.c] == FSTX_FLOAT64;
  const void* src = dbl ? (const void*)(x.t->f64[x.c].data() + row) : (const void*)(x.t->i32[x.c].data() + row);
  if (x.mode == 2) return src;
  std::memcpy(buf, src, (size_t)n * (dbl ? 8 : 4));
  return buf;
}

static std::vector<fstx_write_column> col_defs(std::vector<uint32_t>& level_lens) {
  level_lens = {2, 3, 2};
  std::vector<fstx_write_column> d(NCOL);
  for (int c = 0; c < NCOL; ++c) {
    std::memset(&d[c], 0, sizeof(fstx_write_column));
    d[c].name = kNames[c];
    d[c].name_len = (uint32_t)std::strlen(kNames[c]);
    d[c].phys = kPhys[c];
  }
  d[C_FAC].logical = FSTX_LT_FACTOR;
  d[C_FAC].n_levels = 3;
  d[C_FAC].levels = kLevels;
  d[C_FAC].level_lens = level_lens.data();
  d[C_DAY].logical = FSTX_LT_DATE;
  return d;
}

struct WriteArgs {
  int codec = FSTX_CODEC_ZSTD, level = 3, threads = 1, profile = FSTX_PROFILE_BALANCED, test_crash = 0;
  bool append = false;
  int values = 0;            // number columns through get_values in this mode (0: data)
  int* append_rc = nullptr;  // set: the append's code goes here, a failure is not a failed check
};

static bool write_table(const std::string& path, const Table& t, const WriteArgs& a) {
  std::vector<uint32_t> ll;
  std::vector<fstx_write_column> defs = col_defs(ll);
  std::vector<std::pair<const Table*, int>> ctx(NCOL);
  std::vector<NumCtx> nctx(NCOL);
  std::vector<fstx_column_data> data(NCOL);
  for (int c = 0; c < NCOL; ++c) {
    std::memset(&data[c], 0, sizeof(fstx_column_data));
    ctx[c] = {&t, c};
    nctx[c] = {&t, c, a.values};
    if (kPhys[c] == FSTX_STRING) data[c].get_strings = get_strings, data[c].ctx = &ctx[c];
    else if (a.values) data[c].get_values = get_values, data[c].ctx = &nctx[c];
    else if (kPhys[c] == FSTX_FLOAT64) data[c].data = t.f64[c].data();
    else data[c].data = t.i32[c].data();
  }
  fstx_write_options o;
  std::memset(&o, 0, sizeof(o));
  o.codec = a.codec;
  o.level = a.level;
  o.threads = a.threads;
  o.profile = a.profile;
  o.keep_pct = 10;
  o.row_group_size = 65536;
  o.page_bytes = 65536;
  o.test_crash = a.test_crash;
  fstx_error e;
  fstx_writer* w = nullptr;
  int rc = a.append ? fstx_writer_open_append(path.c_str(), defs.data(), NCOL, nullptr, 0, &o, &w, &e)
                    : fstx_writer_open(path.c_str(), defs.data(), NCOL, nullptr, 0, &o, &w, &e);
  if (!rc_ok(rc, e, "writer open " + path)) return false;
  rc = fstx_writer_append(w, data.data(), t.n, &e);
  if (a.append_rc) *a.append_rc = rc;
  if (rc != FSTX_OK) {
    fstx_writer_abort(w);
    return a.append_rc ? false : rc_ok(rc, e, "writer append " + path);
  }
  return rc_ok(fstx_writer_close(w, &e), e, "writer close " + path);
}

// ---------------------------------------------------------------- reading
struct Host {
  uint64_t n = 0;
  std::vector<uint32_t> cols;            // file column of each output
  std::vector<std::vector<uint8_t>> fx;  // fixed outputs
  std::vector<std::vector<std::string>> s;
  std::vector<std::vector<char>> na;
  std::vector<std::vector<std::string>> dict;
};
static void* h_alloc(void* ctx, uint32_t out, uint32_t, int phys, uint64_t n) {
  Host& h = *(Host*)ctx;
  if (phys == FSTX_STRING || phys == FSTX_BINARY) {
    h.s[out].assign(n, std::string());
    h.na[out].assign(n, 0);
    return &h;
  }
  h.fx[out].assign((size_t)n * ((phys == FSTX_FLOAT64 || phys == FSTX_INT64) ? 8 : 4) + 8, 0);
  return h.fx[out].data();
}
static void put_str(Host& h, uint32_t out, uint64_t row, int32_t len, const char* p) {
  h.na[out][row] = len < 0;
  h.s[out][row] = len < 0 ? std::string() : std::string(p, (size_t)len);
}
static int h_strings(void* ctx, uint32_t out, uint64_t dest, uint32_t n, const int32_t* lens, const char* bytes) {
  Host& h = *(Host*)ctx;
  for (uint32_t i = 0; i < n; ++i) {
    put_str(h, out, dest + i, lens[i], bytes);
    if (lens[i] > 0) bytes += lens[i];
  }
  return 0;
}
static int h_dict(void* ctx, uint32_t out, uint32_t n, const int32_t* lens, const char* bytes) {
  Host& h = *(Host*)ctx;
  h.dict[out].clear();
  for (uint32_t i = 0; i < n; ++i) {
    h.dict[out].emplace_back(bytes, (size_t)lens[i]);
    bytes += lens[i];
  }
  return 0;
}
static int h_codes(void* ctx, uint32_t out, uint64_t dest, uint32_t n, const int32_t* codes) {
  Host& h = *(Host*)ctx;
  for (uint32_t i = 0; i < n; ++i) {
    if (codes[i] < 0) put_str(h, out, dest + i, -1, nullptr);
    else put_str(h, out, dest + i, (int32_t)h.dict[out][(size_t)codes[i]].size(), h.dict[out][(size_t)codes[i]].data());
  }
  return 0;
}
static int h_strings_at(void* ctx, uint32_t out, uint32_t n, const uint64_t* dest, const int32_t* lens,
                        const char* bytes) {
  Host& h = *(Host*)ctx;
  for (uint32_t i = 0; i < n; ++i) {
    put_str(h, out, dest[i], lens[i], bytes);
    if (lens[i] > 0) bytes += lens[i];
  }
  return 0;
}

// rows: nullptr = the range [from, to)
static bool read_into(const std::string& path, const std::vector<uint32_t>& cols, uint64_t from, uint64_t to,
                      const std::vector<uint64_t>* rows, int threads, Host& h, int verify = 1) {
  fstx_error e;
  fstx_file* f = nullptr;
  if (!rc_ok(fstx_open(path.c_str(), &f, &e), e, "open " + path)) return false;
  fstx_plan* p = nullptr;
  int rc = rows ? fstx_plan_create_rows(f, cols.data(), (uint32_t)cols.size(), rows->data(), rows->size(), &p, &e)
                : fstx_plan_create(f, cols.data(), (uint32_t)cols.size(), from, to, &p, &e);
  bool ok = rc_ok(rc, e, "plan " + path);
  if (ok) {
    h = Host();
    h.n = fstx_plan_rows(p);
    h.cols = cols;
    h.fx.resize(cols.size());
    h.s.resize(cols.size());
    h.na.resize(cols.size());
    h.dict.resize(cols.size());
    fstx_read_host host = {&h, h_alloc, h_strings, h_dict, h_codes, h_strings_at};
    ok = rc_ok(fstx_read(p, &host, threads, verify, &e), e, "read " + path);
    fstx_plan_free(p);
  }
  fstx_close(f);
  return ok;
}

// output o of h holds source rows src_rows (or [from, from + h.n)) of t
static bool same(const Host& h, const Table& t, uint64_t from, const std::vector<uint64_t>* src_rows) {
  for (size_t o = 0; o < h.cols.size(); ++o) {
    const uint32_t c = h.cols[o];
    for (uint64_t i = 0; i < h.n; ++i) {
      const uint64_t r = src_rows ? (*src_rows)[i] : from + i;
      bool eq;
      if (kPhys[c] == FSTX_FLOAT64) eq = std::memcmp(h.fx[o].data() + i * 8, &t.f64[c][r], 8) == 0;
      else if (kPhys[c] == FSTX_STRING) eq = h.na[o][i] == t.na[c][r] && (t.na[c][r] || h.s[o][i] == t.str[c][r]);
      else eq = std::memcmp(h.fx[o].data() + i * 4, &t.i32[c][r], 4) == 0;
      if (!eq) {
        std::printf("  mismatch column %s output row %llu (source row %llu)\n", kNames[c], (unsigned long long)i,
                    (unsigned long long)r);
        return false;
      }
    }
  }
  return true;
}

static std::vector<uint32_t> all_cols() {
  std::vector<uint32_t> v;
  for (uint32_t c = 0; c < NCOL; ++c) v.push_back(c);
  return v;
}

static bool read_all_same(const std::string& path, const Table& t, int threads) {
  Host h;
  if (!read_into(path, all_cols(), 0, t.n, nullptr, threads, h)) return false;
  return h.n == t.n && same(h, t, 0, nullptr);
}

// ---------------------------------------------------------------- files
static std::vector<uint8_t> slurp(const std::string& path) {
  std::vector<uint8_t> b;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return b;
  uint8_t buf[65536];
  size_t k;
  while ((k = std::fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + k);
  std::fclose(f);
  return b;
}
static uint64_t fnv1a(const std::vector<uint8_t>& b) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (uint8_t x : b) h = (h ^ x) * 0x100000001b3ull;
  return h;
}
static void copy_file(const std::string& a, const std::string& b, size_t keep) {
  std::vector<uint8_t> x = slurp(a);
  if (keep < x.size()) x.resize(keep);
  FILE* f = std::fopen(b.c_str(), "wb");
  if (f) {
    if (!x.empty()) std::fwrite(x.data(), 1, x.size(), f);
    std::fclose(f);
  }
}

// ---------------------------------------------------------------- tests
static std::string target_label() {
  const int64_t a = fstx::simd::active_target();
  return a ? fstx::simd::target_name(a) : "scalar";
}

static void test_kernels() {
  std::vector<fstx::ktest::KResult> res;
  fstx::ktest::selftest(42, 30, res);
  int cases = 0, bad = 0;
  for (const auto& r : res) {
    cases += r.cases;
    bad += r.mismatches;
    if (r.mismatches) std::printf("  kernel %s: %d mismatches\n", r.kernel.c_str(), r.mismatches);
  }
  std::printf("kernels [%s]: %d kernels, %d cases, %d mismatches\n", target_label().c_str(), (int)res.size(), cases,
              bad);
  check(bad == 0 && cases > 0, "kernel self-test " + target_label());
}

static const char* codec_name(int c) { return c == FSTX_CODEC_NONE ? "none" : c == FSTX_CODEC_LZ4 ? "lz4" : "zstd"; }

// write with each codec at 1 and 4 threads: same bytes, read back the same; returns the hashes
static std::vector<uint64_t> test_write_read(const std::string& dir, const Table& t) {
  std::vector<uint64_t> hs;
  for (int codec : {FSTX_CODEC_NONE, FSTX_CODEC_LZ4, FSTX_CODEC_ZSTD}) {
    std::vector<uint8_t> b1;
    for (int th : {1, 4}) {
      const std::string p = dir + "/w_" + codec_name(codec) + "_" + std::to_string(th) + ".fstx";
      WriteArgs a;
      a.codec = codec;
      a.threads = th;
      if (!write_table(p, t, a)) continue;
      check(read_all_same(p, t, th), std::string("read back ") + codec_name(codec) + " threads " + std::to_string(th));
      std::vector<uint8_t> b = slurp(p);
      if (th == 1) {
        b1 = b;
        hs.push_back(fnv1a(b));
      } else {
        check(b == b1, std::string("same bytes at 1 and 4 threads, ") + codec_name(codec));
      }
      std::remove(p.c_str());
    }
  }
  return hs;
}

// number columns through get_values (copied into the writer's buffer, or handed over in place) give the same file
// as data; a get_values failure fails the write with FSTX_ERR_HOST and leaves no file
static void test_get_values(const std::string& dir, const Table& t) {
  const std::string p0 = dir + "/gv_data.fstx", p = dir + "/gv.fstx";
  WriteArgs a;
  a.threads = 4;
  if (!write_table(p0, t, a)) return;
  const std::vector<uint8_t> b0 = slurp(p0);
  std::remove(p0.c_str());
  for (int mode : {1, 2})
    for (int th : {1, 4}) {
      a.values = mode;
      a.threads = th;
      if (write_table(p, t, a))
        check(slurp(p) == b0, "get_values mode " + std::to_string(mode) + " threads " + std::to_string(th) +
                                  ": same bytes as data");
      std::remove(p.c_str());
    }
  int rc = FSTX_OK;
  a.values = 3;
  a.append_rc = &rc;
  write_table(p, t, a);
  check(rc == FSTX_ERR_HOST, "a failing get_values fails the write with FSTX_ERR_HOST");
  FILE* f = std::fopen(p.c_str(), "rb");
  check(!f, "a failed write leaves no file");
  if (f) std::fclose(f);
  std::remove(p.c_str());
}

static void test_rows_filter_colsrc(const std::string& path, const Table& t) {
  // row list: unsorted, with repeats, strings included
  std::vector<uint64_t> rows;
  uint64_t s = 99;
  for (int i = 0; i < 5000; ++i) rows.push_back(splitmix(s) % t.n);
  rows.push_back(rows[10]);
  rows.push_back(0);
  rows.push_back(t.n - 1);
  Host h;
  const std::vector<uint32_t> cols = {C_TXT, C_PRICE, C_CAT, C_ID, C_FAC};
  check(read_into(path, cols, 0, 0, &rows, 4, h) && h.n == rows.size() && same(h, t, 0, &rows), "rows plan");
  // a row window that starts and ends inside pages
  const uint64_t a = 123457, b = std::min<uint64_t>(234567, t.n);
  check(read_into(path, all_cols(), a, b, nullptr, 3, h) && h.n == b - a && same(h, t, a, nullptr), "range plan");

  // filter: grp >= 50 AND cat == "beta" AND flag is not NA, against a plain scan
  fstx_error e;
  fstx_file* f = nullptr;
  if (!rc_ok(fstx_open(path.c_str(), &f, &e), e, "open for filter")) return;
  fstx_pred pr[3];
  std::memset(pr, 0, sizeof(pr));
  pr[0].col = C_GRP, pr[0].op = FSTX_OP_GE, pr[0].value = 50;
  const char* beta = "beta";
  const int32_t beta_len = 4;
  pr[1].col = C_CAT, pr[1].op = FSTX_OP_EQ, pr[1].strs = &beta, pr[1].str_lens = &beta_len, pr[1].n_strs = 1;
  pr[2].col = C_FLAG, pr[2].op = FSTX_OP_NOTNA;
  fstx_selection* sel = nullptr;
  fstx_filter_stats st;
  if (rc_ok(fstx_filter(f, pr, 3, 0, t.n, 4, 1, 0, &sel, &st, &e), e, "filter")) {
    uint64_t n = 0;
    const uint64_t* got = fstx_selection_rows(sel, &n);
    std::vector<uint64_t> want;
    for (uint64_t i = 0; i < t.n; ++i)
      if (t.i32[C_GRP][i] != NA_INT && t.i32[C_GRP][i] >= 50 && !t.na[C_CAT][i] && t.str[C_CAT][i] == "beta" &&
          t.i32[C_FLAG][i] != NA_INT)
        want.push_back(i);
    check(n == want.size() && (n == 0 || std::memcmp(got, want.data(), n * 8) == 0), "filter rows");
    std::printf("filter: %llu rows matched of %llu, pages read %llu of %llu\n", (unsigned long long)n,
                (unsigned long long)t.n, (unsigned long long)st.pages_read, (unsigned long long)st.pages_total);
    fstx_selection_free(sel);
  }
  // column source: a double column decoded page by page, and loaded in one go on 4 threads
  fstx_colsrc* cs = nullptr;
  if (rc_ok(fstx_colsrc_open(f, C_PRICE, 1000, t.n - 1000, 1, 0, &cs, &e), e, "colsrc open")) {
    fstx_colsrc_info ci;
    fstx_colsrc_info_get(cs, &ci);
    bool ok = ci.rows == t.n - 2000;
    for (uint32_t pg = 0; ok && pg < ci.n_pages; ++pg) {
      uint64_t first;
      uint32_t n;
      const void* v = nullptr;
      fstx_colsrc_page_span(cs, pg, &first, &n);
      ok = rc_ok(fstx_colsrc_fixed_page(cs, pg, &v, &e), e, "colsrc page") &&
           std::memcmp(v, &t.f64[C_PRICE][1000 + first], (size_t)n * 8) == 0;
    }
    std::vector<double> all((size_t)ci.rows);
    ok = ok && rc_ok(fstx_colsrc_load(cs, all.data(), 4, &e), e, "colsrc load") &&
         std::memcmp(all.data(), &t.f64[C_PRICE][1000], all.size() * 8) == 0;
    check(ok, "column source pages and load");
    fstx_colsrc_free(cs);
  }
  fstx_close(f);
}

// append, a crashed append, append again, compaction
static uint64_t test_append_compact(const std::string& dir, const Table& t0) {
  const std::string p = dir + "/app.fstx", tmp = dir + "/app.tmp";
  std::remove(p.c_str());
  std::remove(tmp.c_str());
  Table all = t0;
  WriteArgs a;
  if (!write_table(p, t0, a)) return 0;
  a.append = true;
  const Table t1 = make_table(70001, 7, (int32_t)t0.n);
  check(write_table(p, t1, a), "append 1");
  append_rows(all, t1);
  check(read_all_same(p, all, 4), "read after append 1");

  const Table t2 = make_table(5000, 8, (int32_t)all.n);  // the process "dies" after writing the new tail
  a.test_crash = 1;
  write_table(p, t2, a);
  uint64_t committed = 0, physical = 0, valid_end = 0;
  fstx_error e;
  rc_ok(fstx_file_sizes(p.c_str(), &committed, &physical, &valid_end, &e), e, "sizes after crash");
  check(valid_end > 0 && physical > committed, "crashed append leaves the committed end in the header");
  check(read_all_same(p, all, 2), "read after crashed append = the committed table");

  a.test_crash = 0;
  const Table t3 = make_table(3000, 9, (int32_t)all.n);
  check(write_table(p, t3, a), "append after crash");
  append_rows(all, t3);
  check(read_all_same(p, all, 4), "read after append 2");
  rc_ok(fstx_file_sizes(p.c_str(), &committed, &physical, &valid_end, &e), e, "sizes after append 2");
  check(valid_end == 0 && physical == committed, "no append in progress");

  check(rc_ok(fstx_compact_copy(p.c_str(), tmp.c_str(), &e), e, "compact"), "compact copy");
  const std::vector<uint8_t> cb = slurp(tmp);
  check(cb.size() < physical, "compaction drops the old footers");
  std::remove(p.c_str());
  check(std::rename(tmp.c_str(), p.c_str()) == 0, "rename compacted file");
  check(read_all_same(p, all, 4), "read after compaction");
  std::remove(p.c_str());
  return fnv1a(cb);
}

static void test_errors(const std::string& dir, const std::string& good) {
  fstx_error e;
  fstx_file* f = nullptr;
  int rc = fstx_open((dir + "/does_not_exist.fstx").c_str(), &f, &e);
  check(rc == FSTX_ERR_IO, "missing file gives an I/O error");
  const std::vector<uint8_t> b = slurp(good);
  int bad = 0;
  for (size_t keep : {(size_t)0, (size_t)10, (size_t)100, b.size() / 2, b.size() - 1}) {
    const std::string p = dir + "/cut.fstx";
    copy_file(good, p, keep);
    f = nullptr;
    rc = fstx_open(p.c_str(), &f, &e);
    if (rc == FSTX_OK) {
      fstx_close(f);
      ++bad;
    }
    std::remove(p.c_str());
  }
  check(bad == 0, "cut files are refused");
  // one flipped byte inside the first page: a verified read must stop with a checksum error, not crash
  std::vector<uint8_t> x = b;
  x[4096 + 77] ^= 0x5A;
  const std::string p = dir + "/flip.fstx";
  FILE* fo = std::fopen(p.c_str(), "wb");
  if (fo) {
    std::fwrite(x.data(), 1, x.size(), fo);
    std::fclose(fo);
  }
  int code = -1;
  f = nullptr;
  if (fstx_open(p.c_str(), &f, &e) == FSTX_OK) {
    fstx_file_info fi;
    fstx_file_info_get(f, &fi);
    const std::vector<uint32_t> cols = all_cols();
    fstx_plan* pl = nullptr;
    if (fstx_plan_create(f, cols.data(), NCOL, 0, fi.n_rows, &pl, &e) == FSTX_OK) {
      Host h;
      h.fx.resize(NCOL), h.s.resize(NCOL), h.na.resize(NCOL), h.dict.resize(NCOL);
      fstx_read_host host = {&h, h_alloc, h_strings, h_dict, h_codes, h_strings_at};
      code = fstx_read(pl, &host, 2, 1, &e);
      fstx_plan_free(pl);
    }
    fstx_close(f);
  }
  check(code == FSTX_ERR_CHECKSUM, "flipped byte gives a checksum error");
  std::remove(p.c_str());
}

// ---------------------------------------------------------------- fstx 1.2: int64 and binary columns
// Tables of int64 columns (values past 2^53, INT64_MAX, INT64_MIN + 1, every missing-value mode, all missing) and
// binary columns (NUL bytes, empty values, bytes that are not UTF-8), written with each encoding forced.
struct T12 {
  uint64_t n = 0;
  std::vector<std::string> names;
  std::vector<int> phys;
  std::vector<std::vector<int64_t>> i64;  // int64 columns
  std::vector<std::vector<int32_t>> idx;  // binary columns: index into tab, -1 = missing
  std::vector<std::string> tab;           // distinct binary values (equal values share a pointer: dictionaries)
};
static const int64_t NA_I64 = INT64_MIN;

static std::vector<int64_t> wide_values() {  // 50 values over the whole int64 range
  std::vector<int64_t> w = {INT64_MAX, INT64_MIN + 1, (1LL << 53) + 1, -((1LL << 53) + 1), 0, -1, 1LL << 62,
                            (1LL << 53) - 1, 9007199254740993LL, -9223372036854775000LL};
  uint64_t s = 1234;
  while (w.size() < 50) {
    int64_t v = (int64_t)splitmix(s);
    if (v == NA_I64) ++v;
    w.push_back(v);
  }
  return w;
}

// int64 columns: a (bitmap NAs), b (a run of NAs per page), c (no NA), k (constant, runs of NAs), m (all missing),
// s (ids past 2^53, all distinct)
static T12 make_i64(uint64_t n) {
  T12 t;
  t.n = n;
  t.names = {"a", "b", "c", "k", "m", "s"};
  t.phys.assign(6, FSTX_INT64);
  t.i64.assign(6, std::vector<int64_t>(n));
  const std::vector<int64_t> w = wide_values();
  for (uint64_t i = 0; i < n; ++i) {
    t.i64[0][i] = i % 3 == 1 ? NA_I64 : w[(i / 3) % 50];
    t.i64[1][i] = (i % 8192 >= 100 && i % 8192 < 600) ? NA_I64 : w[(i / 3) % 50];
    t.i64[2][i] = w[(i / 7) % 50];
    t.i64[3][i] = i % 1000 < 10 ? NA_I64 : 4611686018427387911LL;
    t.i64[4][i] = NA_I64;
    t.i64[5][i] = (1LL << 53) + (int64_t)i * 3;
  }
  return t;
}

// binary column "bin" (NUL bytes, empty values, missing rows) and an int64 column "i"
static T12 make_bin(uint64_t n) {
  T12 t;
  t.n = n;
  t.names = {"bin", "i"};
  t.phys = {FSTX_BINARY, FSTX_INT64};
  t.idx.assign(1, std::vector<int32_t>(n));
  t.i64.assign(1, std::vector<int64_t>(n));
  uint64_t s = 777;
  t.tab.push_back(std::string());  // the empty value
  for (int k = 1; k < 300; ++k) {
    std::string v((size_t)(1 + k % 40), '\0');
    for (char& ch : v) ch = (char)(splitmix(s) & 0xff);
    if (k % 5 == 1) v[0] = '\0';                        // a leading NUL
    if (k % 7 == 2) v[v.size() / 2] = '\0';             // one inside
    if (k % 11 == 3) v[0] = (char)0xff, v.back() = '\0';  // not UTF-8, trailing NUL
    t.tab.push_back(v);
  }
  t.tab.push_back(std::string("\0\0\0", 3));
  for (uint64_t i = 0; i < n; ++i) {
    const uint64_t r = splitmix(s);
    t.idx[0][i] = i % 17 == 4 ? -1 : (int32_t)(r % 31 == 0 ? 0 : (r >> 8) % t.tab.size());
    t.i64[0][i] = i % 29 == 0 ? NA_I64 : (int64_t)(r >> 1) - (1LL << 62);
  }
  return t;
}

struct BinCtx {
  const T12* t;
  int k;     // index into t->idx
  int fail;  // tests: give a non-zero code
};
static int bin_get(void* ctx, uint64_t row, uint32_t n, const char** ptrs, int32_t* lens) {
  const BinCtx& c = *(const BinCtx*)ctx;
  if (c.fail) return 1;
  for (uint32_t i = 0; i < n; ++i) {
    const int32_t k = c.t->idx[(size_t)c.k][row + i];
    ptrs[i] = k < 0 ? nullptr : c.t->tab[(size_t)k].data();
    lens[i] = k < 0 ? -1 : (int32_t)c.t->tab[(size_t)k].size();
  }
  return 0;
}

// fixed column j of a T12 -> its index among the int64 vectors (or binary vectors)
static int sub_of(const T12& t, size_t j) {
  int k = 0;
  for (size_t q = 0; q < j; ++q) k += t.phys[q] == t.phys[j];
  return k;
}

static int g_profile12 = FSTX_PROFILE_BALANCED;  // write12's profile (tests of the fast profile change it)

// returns the append's code (FSTX_OK when written)
static int write12(const std::string& path, const T12& t, int codec, int threads, int force, int fail_bin = 0,
                   int logical0 = 0, int values = 0, bool append = false) {
  const size_t nc = t.names.size();
  std::vector<fstx_write_column> defs(nc);
  std::vector<fstx_column_data> data(nc);
  std::vector<BinCtx> bc(nc);
  for (size_t j = 0; j < nc; ++j) {
    std::memset(&defs[j], 0, sizeof(fstx_write_column));
    std::memset(&data[j], 0, sizeof(fstx_column_data));
    defs[j].name = t.names[j].c_str();
    defs[j].name_len = (uint32_t)t.names[j].size();
    defs[j].phys = t.phys[j];
    if (t.phys[j] == FSTX_BINARY) {
      bc[j] = {&t, sub_of(t, j), fail_bin};
      data[j].get_strings = bin_get;
      data[j].ctx = &bc[j];
    } else if (values) {
      data[j].get_values = [](void* ctx, uint64_t row, uint32_t n, void* buf) -> const void* {
        const int64_t* x = (const int64_t*)ctx + row;
        std::memcpy(buf, x, (size_t)n * 8);
        return buf;
      };
      data[j].ctx = (void*)t.i64[(size_t)sub_of(t, j)].data();
    } else {
      data[j].data = t.i64[(size_t)sub_of(t, j)].data();
    }
  }
  defs[0].logical = logical0;
  fstx_write_options o;
  std::memset(&o, 0, sizeof(o));
  o.codec = codec;
  o.level = 3;
  o.threads = threads;
  o.profile = g_profile12;
  o.keep_pct = 10;
  o.row_group_size = 16384;
  o.page_bytes = 65536;
  o.force_encoding = force;
  fstx_error e;
  fstx_writer* w = nullptr;
  int rc = append ? fstx_writer_open_append(path.c_str(), defs.data(), (uint32_t)nc, nullptr, 0, &o, &w, &e)
                  : fstx_writer_open(path.c_str(), defs.data(), (uint32_t)nc, nullptr, 0, &o, &w, &e);
  if (rc != FSTX_OK) return rc;
  rc = fstx_writer_append(w, data.data(), t.n, &e);
  if (rc != FSTX_OK) {
    fstx_writer_abort(w);
    return rc;
  }
  return fstx_writer_close(w, &e);
}

static bool same12(const Host& h, const T12& t, const std::vector<uint64_t>* src_rows) {
  for (size_t o = 0; o < h.cols.size(); ++o) {
    const uint32_t c = h.cols[o];
    const int k = sub_of(t, c);
    for (uint64_t i = 0; i < h.n; ++i) {
      const uint64_t r = src_rows ? (*src_rows)[i] : i;
      bool eq;
      if (t.phys[c] == FSTX_INT64) {
        eq = std::memcmp(h.fx[o].data() + i * 8, &t.i64[(size_t)k][r], 8) == 0;
      } else {
        const int32_t x = t.idx[(size_t)k][r];
        eq = (h.na[o][i] != 0) == (x < 0) && (x < 0 || h.s[o][i] == t.tab[(size_t)x]);
      }
      if (!eq) {
        std::printf("  mismatch column %s output row %llu (source row %llu)\n", t.names[c].c_str(),
                    (unsigned long long)i, (unsigned long long)r);
        return false;
      }
    }
  }
  return true;
}

static std::vector<uint32_t> cols_of(const T12& t) {
  std::vector<uint32_t> v;
  for (uint32_t c = 0; c < t.names.size(); ++c) v.push_back(c);
  return v;
}

// whole reads (both verify modes), unsorted rows with repeats, rows plans with codes, column sources
static void check12(const std::string& p, const T12& t, const std::string& lab) {
  Host h;
  check(read_into(p, cols_of(t), 0, t.n, nullptr, 1, h) && h.n == t.n && same12(h, t, nullptr), lab + ": read");
  check(read_into(p, cols_of(t), 0, t.n, nullptr, 4, h, FSTX_VERIFY_HASH | FSTX_VERIFY_NULLS) && same12(h, t, nullptr),
        lab + ": read, 4 threads, null counts checked");
  std::vector<uint64_t> rows;
  uint64_t s = 5;
  for (int i = 0; i < 3000; ++i) rows.push_back(splitmix(s) % t.n);
  rows.push_back(rows[3]);
  rows.push_back(t.n - 1);
  check(read_into(p, cols_of(t), 0, 0, &rows, 4, h) && h.n == rows.size() && same12(h, t, &rows), lab + ": rows plan");
  check(read_into(p, cols_of(t), 0, 0, &rows, 2, h, FSTX_VERIFY_HASH | FSTX_VERIFY_NULLS) && same12(h, t, &rows),
        lab + ": rows plan, null counts checked");
  fstx_error e;
  fstx_file* f = nullptr;
  if (!rc_ok(fstx_open(p.c_str(), &f, &e), e, lab + ": open")) return;
  // rows plan, ascending, binary columns as dictionary codes
  std::vector<uint64_t> asc = rows;
  std::sort(asc.begin(), asc.end());
  {
    const std::vector<uint32_t> cols = cols_of(t);
    fstx_plan* pl = nullptr;
    bool ok = rc_ok(fstx_plan_create_rows(f, cols.data(), (uint32_t)cols.size(), asc.data(), asc.size(), &pl, &e), e,
                    lab + ": plan rows");
    for (uint32_t c = 0; ok && c < cols.size(); ++c)
      if (t.phys[c] == FSTX_BINARY) ok = rc_ok(fstx_plan_rows_codes(pl, c, &e), e, lab + ": rows codes");
    if (ok) {
      Host hh;
      hh.n = fstx_plan_rows(pl);
      hh.cols = cols;
      hh.fx.resize(cols.size()), hh.s.resize(cols.size()), hh.na.resize(cols.size()), hh.dict.resize(cols.size());
      fstx_read_host host = {&hh, h_alloc, h_strings, h_dict, h_codes, h_strings_at};
      ok = rc_ok(fstx_read(pl, &host, 3, 1, &e), e, lab + ": read rows codes") && same12(hh, t, &asc);
    }
    check(ok, lab + ": rows plan with dictionary codes");
    fstx_plan_free(pl);
  }
  // column sources over a window that starts and ends inside pages
  const uint64_t a = 1001, b = t.n - 999;
  for (uint32_t c = 0; c < t.names.size(); ++c) {
    const int k = sub_of(t, c);
    fstx_colsrc* cs = nullptr;
    if (!rc_ok(fstx_colsrc_open(f, c, a, b, 1, 0, &cs, &e), e, lab + ": colsrc open")) continue;
    fstx_colsrc_info ci;
    fstx_colsrc_info_get(cs, &ci);
    bool ok = ci.rows == b - a && ci.phys == t.phys[c];
    if (t.phys[c] == FSTX_INT64) {
      ok = ok && ci.mode == FSTX_SRC_FIXED;
      for (uint32_t pg = 0; ok && pg < ci.n_pages; ++pg) {
        uint64_t first;
        uint32_t n;
        const void* v = nullptr;
        fstx_colsrc_page_span(cs, pg, &first, &n);
        ok = rc_ok(fstx_colsrc_fixed_page(cs, pg, &v, &e), e, lab + ": colsrc page") &&
             std::memcmp(v, &t.i64[(size_t)k][a + first], (size_t)n * 8) == 0;
      }
      std::vector<int64_t> all((size_t)ci.rows);
      ok = ok && rc_ok(fstx_colsrc_load(cs, all.data(), 4, &e), e, lab + ": colsrc load") &&
           std::memcmp(all.data(), &t.i64[(size_t)k][a], all.size() * 8) == 0;
      check(ok, lab + ": int64 column source " + t.names[c]);
    } else {
      ok = ok && ci.mode == FSTX_SRC_STRINGS;
      const int32_t* const* none = nullptr;
      check(fstx_colsrc_set_codes(cs, none, &e) == FSTX_ERR_ARG, lab + ": a binary source never becomes factor codes");
      for (uint32_t pg = 0; ok && pg < ci.n_pages; ++pg) {
        uint64_t first;
        uint32_t n;
        const int32_t* lens = nullptr;
        const char* bytes = nullptr;
        int32_t dict = -1;
        fstx_colsrc_page_span(cs, pg, &first, &n);
        ok = rc_ok(fstx_colsrc_str_page(cs, pg, &lens, &bytes, &dict, &e), e, lab + ": colsrc string page");
        uint32_t dn = 0;
        const int32_t* dl = nullptr;
        const char* db = nullptr;
        std::vector<uint64_t> doff;
        if (ok && dict >= 0) {
          ok = rc_ok(fstx_colsrc_dict(cs, (uint32_t)dict, &dn, &dl, &db, &e), e, lab + ": colsrc dict");
          doff.assign((size_t)dn + 1, 0);
          for (uint32_t q = 0; ok && q < dn; ++q) doff[q + 1] = doff[q] + (uint64_t)dl[q];
        }
        uint64_t off = 0;
        for (uint32_t i = 0; ok && i < n; ++i) {
          const int32_t x = t.idx[(size_t)k][a + first + i];
          if (dict >= 0) {
            const int32_t q = lens[i];
            ok = (q < 0) == (x < 0) &&
                 (x < 0 || std::string(db + doff[(size_t)q], (size_t)dl[q]) == t.tab[(size_t)x]);
          } else {
            ok = (lens[i] < 0) == (x < 0) && (x < 0 || std::string(bytes + off, (size_t)lens[i]) == t.tab[(size_t)x]);
            if (lens[i] > 0) off += (uint64_t)lens[i];
          }
        }
      }
      ok = ok && rc_ok(fstx_colsrc_load(cs, nullptr, 4, &e), e, lab + ": colsrc load strings");
      check(ok, lab + ": binary column source " + t.names[c]);
    }
    fstx_colsrc_free(cs);
  }
  fstx_close(f);
}

// page kinds, top tags and missing-value modes; int64 statistics against the values (rounded outward)
static void check_pages12(const std::string& p, const T12& t, int force, const std::string& lab) {
  fstx_error e;
  fstx_file* f = nullptr;
  if (!rc_ok(fstx_open(p.c_str(), &f, &e), e, lab + ": open")) return;
  fstx_file_info fi;
  fstx_file_info_get(f, &fi);
  uint64_t want = 0;
  for (int ph : t.phys) want |= ph == FSTX_INT64 ? 1ull << 16 : ph == FSTX_BINARY ? 1ull << 17 : 0;
  check(fi.required_features == want && fi.optional_features == 0, lab + ": feature bits set exactly");
  std::vector<fstx_page_info> pi((size_t)fstx_pages_get(f, nullptr, 0));
  fstx_pages_get(f, pi.data(), pi.size());
  const int tag = force & 0xff;
  bool kinds = true, tags = true, nas = true, dict_seen = false, stats = true;
  for (const fstx_page_info& q : pi) {
    const uint32_t c = q.column;
    if (t.phys[c] == FSTX_INT64) {
      kinds = kinds && q.kind == 7 && !q.is_dictionary;
      const std::string& nm = t.names[c];
      if (tag && nm != "i") {
        int want_tag = tag;
        if (tag == 2 && nm != "k" && nm != "m") want_tag = 0;  // CONST only where the page is constant
        if (tag == 6 && nm == "s") want_tag = 3;               // all distinct: a forced DICT falls back to FOR
        if (want_tag && q.enc[0] != want_tag) {
          tags = false;
          std::printf("  %s: column %s page at row %llu has tag %d, want %d\n", lab.c_str(), nm.c_str(),
                      (unsigned long long)q.first_row, q.enc[0], want_tag);
        }
      }
      if (nm != "i") {
        const int want_na = nm == "a" ? 1 : (nm == "b" || nm == "k" || nm == "m") ? 2 : 0;
        nas = nas && q.na_mode == want_na;
      }
    } else {
      if (q.is_dictionary) {
        dict_seen = true;
        kinds = kinds && q.kind == 6 && q.enc[0] == ((force & 0x200) ? 10 : 1);
      } else if (force & 0x100) {
        kinds = kinds && q.kind == 5;
      } else if (force == 0) {  // the chooser: plain, FSST or a dictionary
        kinds = kinds && (q.kind == 4 || q.kind == 5);
      } else {
        kinds = kinds && q.kind == 4 && (!tag || q.enc[0] == tag);
      }
    }
  }
  // int64 statistics, from the footer: the largest double not above the page minimum, the smallest not below the
  // maximum; flag clear on an all-missing page
  const auto* rgs = f->footer->row_groups();
  for (uint32_t g = 0; rgs && g < rgs->size(); ++g)
    for (uint32_t c = 0; c < t.names.size(); ++c) {
      if (t.phys[c] != FSTX_INT64) continue;
      const std::vector<int64_t>& x = t.i64[(size_t)sub_of(t, c)];
      for (const fstx::fb::Page* pg : *rgs->Get(g)->columns()->Get(c)->pages()) {
        int64_t mn = INT64_MAX, mx = INT64_MIN;
        uint32_t nna = 0;
        for (uint64_t r = pg->first_row(); r < pg->first_row() + pg->row_count(); ++r) {
          if (x[r] == NA_I64) {
            ++nna;
            continue;
          }
          mn = std::min(mn, x[r]);
          mx = std::max(mx, x[r]);
        }
        const bool has = nna < pg->row_count(), flag = (pg->flags() & 1) != 0;
        bool ok = has == flag && pg->null_count() == nna;
        if (ok && has)
          ok = fstx::cmp_d_i64(pg->min(), mn) <= 0 && fstx::cmp_d_i64(std::nextafter(pg->min(), HUGE_VAL), mn) > 0 &&
               fstx::cmp_d_i64(pg->max(), mx) >= 0 && fstx::cmp_d_i64(std::nextafter(pg->max(), -HUGE_VAL), mx) < 0;
        if (!ok) {
          stats = false;
          std::printf("  %s: column %s page at row %llu: statistics %.17g .. %.17g (flag %d), values %lld .. %lld\n",
                      lab.c_str(), t.names[c].c_str(), (unsigned long long)pg->first_row(), pg->min(), pg->max(),
                      (int)flag, (long long)mn, (long long)mx);
        }
      }
    }
  check(kinds, lab + ": page kinds");
  check(tags, lab + ": forced encodings");
  check(nas, lab + ": missing-value modes");
  check(stats, lab + ": int64 statistics rounded outward");
  if (force & 0x100) check(dict_seen, lab + ": dictionary pages");
  fstx_close(f);
}

static const char* kTagNames[] = {"", "plain", "const", "for", "delta", "rle", "dict", "split"};

// Every kind-7 mix and every binary mix of spec section 7 (int64: tags 1-7 x missing-value modes 0-2 x codecs 0-2 =
// 63; binary: kind 4 tags 1 / 10 x modes x codecs = 18, kind 5 tags 1-7 x modes x codecs = 63, kind 6 tags 1 / 10 x
// codecs = 6). Each tag is forced with the codec kept on every page; the mixes the pages show are collected with
// fstx_pages_get and must be all of them. Every file is read back (check12). Returns one hash per kind family over
// all its files, so the SIMD targets compare them.
static T12 make_i64_mix(uint64_t n) {
  T12 t;
  t.n = n;
  t.names = {"a", "b", "c", "k0", "k1", "k2"};  // modes 1, 2, 0 on wide values; constant columns with modes 0, 1, 2
  t.phys.assign(6, FSTX_INT64);
  t.i64.assign(6, std::vector<int64_t>(n));
  const std::vector<int64_t> w = wide_values();
  const int64_t k = 4611686018427387911LL;
  for (uint64_t i = 0; i < n; ++i) {
    t.i64[0][i] = i % 3 == 1 ? NA_I64 : w[(i / 3) % 50];
    t.i64[1][i] = (i % 8192 >= 100 && i % 8192 < 600) ? NA_I64 : w[(i / 3) % 50];
    t.i64[2][i] = w[(i / 7) % 50];
    t.i64[3][i] = k;
    t.i64[4][i] = i % 3 == 1 ? NA_I64 : k;
    t.i64[5][i] = i % 1000 < 10 ? NA_I64 : k;
  }
  return t;
}

static T12 make_bin_mix(uint64_t n) {
  T12 t = make_bin(n);  // its value table (NUL, empty, not UTF-8)
  t.names = {"v0", "v1", "v2", "c0", "c1", "c2"};
  t.phys.assign(6, FSTX_BINARY);
  t.i64.clear();
  t.idx.assign(6, std::vector<int32_t>(n));
  uint64_t s = 99;
  const int32_t nt = (int32_t)t.tab.size();
  for (uint64_t i = 0; i < n; ++i) {
    const int32_t v = (int32_t)((splitmix(s) >> 8) % (uint64_t)nt);
    const bool na1 = i % 17 == 4, na2 = i % 8192 >= 100 && i % 8192 < 600, na2c = i % 1000 < 10;
    t.idx[0][i] = v;
    t.idx[1][i] = na1 ? -1 : v;
    t.idx[2][i] = na2 ? -1 : v;
    t.idx[3][i] = 5;
    t.idx[4][i] = i % 3 == 1 ? -1 : 5;
    t.idx[5][i] = na2c ? -1 : 5;
  }
  return t;
}

static void test_mixes12(const std::string& dir, uint64_t& h_i64, uint64_t& h_bin) {
  std::vector<uint8_t> seen_i64(8 * 3 * 3, 0), seen_bin(16 * 7 * 3 * 3, 0);  // [tag][na][codec], [kind][tag][na][codec]
  auto collect = [&](const std::string& p) {
    fstx_file* f = nullptr;
    fstx_error e;
    if (!rc_ok(fstx_open(p.c_str(), &f, &e), e, "mix open")) return;
    std::vector<fstx_page_info> pi((size_t)fstx_pages_get(f, nullptr, 0));
    fstx_pages_get(f, pi.data(), pi.size());
    for (const fstx_page_info& q : pi) {
      const int tag = q.enc[0], na = q.na_mode, cd = q.codec;
      if (na < 0 || na > 2 || cd < 0 || cd > 2) continue;
      if (q.kind == 7 && tag >= 1 && tag <= 7) seen_i64[(size_t)((tag * 3 + na) * 3 + cd)] = 1;
      const int bt = tag == 10 ? 0 : tag;  // 0 stands for FSST
      if ((q.kind == 4 || q.kind == 5 || q.kind == 6) && bt >= 0 && bt <= 7)
        seen_bin[(size_t)(((q.kind * 8 + bt) * 3 + na) * 3 + cd)] = 1;
    }
    fstx_close(f);
  };
  std::vector<uint8_t> all_i64, all_bin;
  auto add_hash = [](std::vector<uint8_t>& all, const std::string& p) {
    const uint64_t h = fnv1a(slurp(p));
    for (int b = 0; b < 8; ++b) all.push_back((uint8_t)(h >> (8 * b)));
  };
  const int codecs[3] = {FSTX_CODEC_NONE, FSTX_CODEC_LZ4, FSTX_CODEC_ZSTD};
  const T12 ti = make_i64_mix(20000);
  for (int cd : codecs)
    for (int tag = 1; tag <= 7; ++tag) {
      const std::string p = dir + "/mix_i64.fstx";
      const std::string lab = std::string("int64 mix ") + kTagNames[tag] + " " + codec_name(cd);
      if (!rc_ok(write12(p, ti, cd, 2, tag | 0x400), fstx_error{}, lab + ": write")) continue;
      check12(p, ti, lab);
      collect(p);
      add_hash(all_i64, p);
      std::remove(p.c_str());
    }
  const T12 tb = make_bin_mix(20000);
  // plain, FSST, then code pages with each tag (dictionary page plain or FSST in turn)
  const int bforce[9] = {1, 10, 0x100 | 1, 0x300 | 2, 0x100 | 3, 0x300 | 4, 0x100 | 5, 0x300 | 6, 0x100 | 7};
  for (int cd : codecs)
    for (int bf : bforce) {
      const std::string p = dir + "/mix_bin.fstx";
      const std::string lab = "binary mix force " + std::to_string(bf) + " " + codec_name(cd);
      if (!rc_ok(write12(p, tb, cd, 2, bf | 0x400), fstx_error{}, lab + ": write")) continue;
      check12(p, tb, lab);
      collect(p);
      add_hash(all_bin, p);
      std::remove(p.c_str());
    }
  int n_i64 = 0, n_bin = 0, want_bin = 0;
  std::string miss;
  for (int tag = 1; tag <= 7; ++tag)
    for (int na = 0; na < 3; ++na)
      for (int cd = 0; cd < 3; ++cd) {
        if (seen_i64[(size_t)((tag * 3 + na) * 3 + cd)]) ++n_i64;
        else miss += " i64/t" + std::to_string(tag) + "/na" + std::to_string(na) + "/c" + std::to_string(cd);
      }
  for (int kind = 4; kind <= 6; ++kind)
    for (int bt = 0; bt <= 7; ++bt)
      for (int na = 0; na < 3; ++na)
        for (int cd = 0; cd < 3; ++cd) {
          const bool legal = kind == 5 ? bt >= 1 : (bt == 0 || bt == 1) && (kind == 4 || na == 0);
          if (!legal) continue;
          ++want_bin;
          if (seen_bin[(size_t)(((kind * 8 + bt) * 3 + na) * 3 + cd)]) ++n_bin;
          else miss += " k" + std::to_string(kind) + "/t" + std::to_string(bt ? bt : 10) + "/na" + std::to_string(na) +
                       "/c" + std::to_string(cd);
        }
  std::printf("1.2 mixes: int64 %d of 63, binary %d of %d%s%s\n", n_i64, n_bin, want_bin, miss.empty() ? "" : "; missing:",
              miss.c_str());
  check(n_i64 == 63 && n_bin == 87 && want_bin == 87, "every int64 and binary mix written and read back");
  h_i64 = fnv1a(all_i64);
  h_bin = fnv1a(all_bin);
}

// every int64 tag (codec none / lz4 / zstd in turn) and the binary layouts: same bytes at 1 and 4 threads and through
// get_values; returns the file hashes
static std::vector<uint64_t> test_i64_bin(const std::string& dir, std::vector<std::string>& names) {
  std::vector<uint64_t> hs;
  names.clear();
  const T12 ti = make_i64(20000);
  for (int tag = 1; tag <= 7; ++tag) {
    const int codec = tag % 3 == 1 ? FSTX_CODEC_NONE : tag % 3 == 2 ? FSTX_CODEC_LZ4 : FSTX_CODEC_ZSTD;
    const std::string lab = std::string("int64 ") + kTagNames[tag];
    const std::string p = dir + "/i64_" + kTagNames[tag] + ".fstx", p4 = dir + "/i64_4.fstx";
    if (!rc_ok(write12(p, ti, codec, 1, tag), fstx_error{}, lab + ": write")) continue;
    check(write12(p4, ti, codec, 4, tag, 0, 0, tag == 4) == FSTX_OK && slurp(p4) == slurp(p),
          lab + ": same bytes at 1 and 4 threads" + (tag == 4 ? " (get_values)" : ""));
    check12(p, ti, lab);
    check_pages12(p, ti, tag, lab);
    hs.push_back(fnv1a(slurp(p)));
    names.push_back(std::string("i64_") + kTagNames[tag]);
    std::remove(p.c_str());
    std::remove(p4.c_str());
  }
  {  // the chooser, zstd
    const std::string p = dir + "/i64_auto.fstx";
    if (rc_ok(write12(p, ti, FSTX_CODEC_ZSTD, 4, 0), fstx_error{}, "int64 auto: write")) {
      check12(p, ti, "int64 auto");
      check_pages12(p, ti, 0, "int64 auto");
      hs.push_back(fnv1a(slurp(p)));
      names.push_back("i64_auto");
    }
    std::remove(p.c_str());
  }
  const T12 tb = make_bin(30000);
  const struct {
    const char* nm;
    int force;
  } modes[] = {{"plain", 1}, {"fsst", 10}, {"dict", 0x100}, {"dict_fsst", 0x100 | 0x200}, {"auto", 0}};
  for (const auto& m : modes) {
    const std::string lab = std::string("binary ") + m.nm;
    const std::string p = dir + "/bin_" + m.nm + ".fstx", p4 = dir + "/bin_4.fstx";
    if (!rc_ok(write12(p, tb, FSTX_CODEC_ZSTD, 1, m.force), fstx_error{}, lab + ": write")) continue;
    check(write12(p4, tb, FSTX_CODEC_ZSTD, 4, m.force) == FSTX_OK && slurp(p4) == slurp(p),
          lab + ": same bytes at 1 and 4 threads");
    check12(p, tb, lab);
    check_pages12(p, tb, m.force, lab);
    hs.push_back(fnv1a(slurp(p)));
    names.push_back(std::string("bin_") + m.nm);
    std::remove(p.c_str());
    std::remove(p4.c_str());
  }
  uint64_t hm_i64 = 0, hm_bin = 0;
  test_mixes12(dir, hm_i64, hm_bin);
  hs.push_back(hm_i64);
  names.push_back("mix_i64");
  hs.push_back(hm_bin);
  names.push_back("mix_bin");
  return hs;
}

// a binary column of random 16-byte values (all distinct, at distinct pointers: what R raw vectors give): n rows,
// plus an ASCII column of the same size when ascii (16 random letters and digits a value)
static T12 make_rand16(uint64_t n, bool ascii) {
  T12 t;
  t.n = n;
  t.names = {ascii ? "a16" : "r16"};
  t.phys = {FSTX_BINARY};
  t.idx.assign(1, std::vector<int32_t>(n));
  t.tab.resize(n);
  uint64_t s = ascii ? 4242 : 2424;
  static const char kAl[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  for (uint64_t i = 0; i < n; ++i) {
    std::string v(16, '\0');
    for (char& ch : v) {
      const uint64_t r = splitmix(s);
      ch = ascii ? kAl[r % 36] : (char)(r & 0xff);
    }
    t.tab[i] = v;
    t.idx[0][i] = (int32_t)i;
  }
  return t;
}

// random 16-byte binary values: plain pages (no dictionary, no FSST) under the fast and the balanced profile
static void test_rand16(const std::string& dir) {
  const T12 t = make_rand16(100000, false);
  const struct {
    int profile, codec;
    const char* nm;
  } ps[] = {{FSTX_PROFILE_FAST, FSTX_CODEC_NONE, "fast"}, {FSTX_PROFILE_BALANCED, FSTX_CODEC_ZSTD, "balanced"}};
  for (const auto& q : ps) {
    const std::string p = dir + "/r16.fstx", lab = std::string("random 16-byte binary, ") + q.nm;
    g_profile12 = q.profile;
    const int rc = write12(p, t, q.codec, 4, 0);
    g_profile12 = FSTX_PROFILE_BALANCED;
    if (!rc_ok(rc, fstx_error{}, lab + ": write")) continue;
    fstx_file* f = nullptr;
    fstx_error e;
    if (!rc_ok(fstx_open(p.c_str(), &f, &e), e, lab + ": open")) continue;
    std::vector<fstx_page_info> pi((size_t)fstx_pages_get(f, nullptr, 0));
    fstx_pages_get(f, pi.data(), pi.size());
    bool plain = !pi.empty();
    for (const fstx_page_info& g : pi) plain = plain && !g.is_dictionary && g.kind == 4 && g.enc[0] == 1;
    fstx_close(f);
    check(plain, lab + ": plain pages chosen");
    check12(p, t, lab);
    std::remove(p.c_str());
  }
}

// coretest <dir> time-bin16 [rows]: fast-profile write time of a random 16-byte binary column against an ASCII
// column of the same size (codec none, all threads), median of 7 alternating writes each
static int time_bin16(const std::string& dir, uint64_t n) {
  const T12 tb = make_rand16(n, false), ta = make_rand16(n, true);
  const int th = (int)fstx_hardware_threads();
  std::vector<double> sb, sa;
  g_profile12 = FSTX_PROFILE_FAST;
  for (int k = 0; k < 8; ++k)
    for (int which = 0; which < 2; ++which) {
      const std::string p = dir + "/t16.fstx";
      const auto t0 = std::chrono::steady_clock::now();
      const int rc = write12(p, which ? ta : tb, FSTX_CODEC_NONE, th, 0);
      const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::remove(p.c_str());
      if (rc != FSTX_OK) {
        std::printf("time-bin16: write failed (%d)\n", rc);
        return 1;
      }
      if (k == 0) continue;  // warm-up
      (which ? sa : sb).push_back(sec);
    }
  std::sort(sb.begin(), sb.end());
  std::sort(sa.begin(), sa.end());
  const double mb = sb[sb.size() / 2], ma = sa[sa.size() / 2];
  std::printf("time-bin16: %llu rows x 16 bytes, %d threads, fast profile, codec none: binary %.4f s, ascii %.4f s "
              "(median of %zu), ratio %.3f (min %.4f / %.4f, max %.4f / %.4f)\n",
              (unsigned long long)n, th, mb, ma, sb.size(), mb / ma, sb.front(), sa.front(), sb.back(), sa.back());
  std::printf(mb <= 1.10 * ma ? "TIME-BIN16 OK (within 10%%)\n" : "TIME-BIN16 SLOWER than 10%%\n");
  return 0;
}

static int open_code(const std::string& p) {
  fstx_error e;
  fstx_file* f = nullptr;
  const int rc = fstx_open(p.c_str(), &f, &e);
  if (rc == FSTX_OK) fstx_close(f);
  return rc;
}

static bool forge(const std::string& src, const std::string& dst, const fstx::Forge& g) {
  try {
    fstx::forge_copy(src, dst, g);
    return true;
  } catch (const std::exception& x) {
    std::printf("  forge failed: %s\n", x.what());
    return false;
  }
}

// feature bits, names and kinds through the C API; refusal and corrupt rules; writer argument and host errors;
// filters (drop 1: int64 refused, binary through the string path)
static void test_rules12(const std::string& dir) {
  check((fstx_features_known() & (3ull << 16)) == (3ull << 16), "features_known has bits 16 and 17");
  check(fstx_feature_name(16) && std::strcmp(fstx_feature_name(16), "int64") == 0 && fstx_feature_name(17) &&
            std::strcmp(fstx_feature_name(17), "binary") == 0 && !fstx_feature_name(20) && !fstx_feature_name(64) &&
            !fstx_feature_name(-1),
        "feature names");
  check(std::strcmp(fstx_kind_name(7), "int64") == 0 && std::strcmp(fstx_kind_name(8), "") == 0, "kind 7 is int64");
  // exact double / int64 comparison and the outward rounding of statistics
  check(fstx::cmp_d_i64(9007199254740992.0, (1LL << 53) + 1) < 0 && fstx::cmp_d_i64(2.5, 2) > 0 &&
            fstx::cmp_d_i64(-2.5, -2) < 0 && fstx::cmp_d_i64(9223372036854775808.0, INT64_MAX) > 0 &&
            fstx::cmp_d_i64(-9223372036854775808.0, INT64_MIN) == 0 && fstx::cmp_d_i64(-1e300, INT64_MIN) < 0,
        "cmp_d_i64");
  bool round_ok = true;
  const std::vector<int64_t> w = wide_values();
  for (int64_t v : w) {
    const double lo = fstx::i64_floor_d(v), hi = fstx::i64_ceil_d(v);
    round_ok = round_ok && fstx::cmp_d_i64(lo, v) <= 0 && fstx::cmp_d_i64(std::nextafter(lo, HUGE_VAL), v) > 0 &&
               fstx::cmp_d_i64(hi, v) >= 0 && fstx::cmp_d_i64(std::nextafter(hi, -HUGE_VAL), v) < 0;
    if (v > -(1LL << 53) && v < (1LL << 53)) round_ok = round_ok && lo == (double)v && hi == (double)v;
  }
  check(round_ok, "int64 statistics rounded outward, exact to 2^53");

  // writer: logical types on the new types, a failing binary source
  const T12 tb = make_bin(5000);
  const std::string p = dir + "/r12.fstx", q = dir + "/r12_forged.fstx";
  check(write12(p, tb, FSTX_CODEC_ZSTD, 2, 0, 0, FSTX_LT_FACTOR) == FSTX_ERR_ARG, "a binary column takes no logical type");
  T12 ti = make_i64(3000);
  check(write12(p, ti, FSTX_CODEC_ZSTD, 2, 0, 0, FSTX_LT_DATE) == FSTX_ERR_ARG, "an int64 column takes no logical type");
  check(write12(p, tb, FSTX_CODEC_ZSTD, 2, 0, 1) == FSTX_ERR_HOST, "a failing binary source fails with FSTX_ERR_HOST");
  check(open_code(p) == FSTX_ERR_IO, "a failed write leaves no file");

  // filters: binary == with NUL bytes (string path), int64 refused until drop 2
  if (write12(p, tb, FSTX_CODEC_ZSTD, 2, 0) == FSTX_OK) {
    fstx_error e;
    fstx_file* f = nullptr;
    if (rc_ok(fstx_open(p.c_str(), &f, &e), e, "open binary file for filter")) {
      const std::string& v = tb.tab[1];  // starts with a NUL
      const char* sp = v.data();
      const int32_t sl = (int32_t)v.size();
      fstx_pred pr;
      std::memset(&pr, 0, sizeof(pr));
      pr.col = 0, pr.op = FSTX_OP_EQ, pr.strs = &sp, pr.str_lens = &sl, pr.n_strs = 1;
      fstx_selection* sel = nullptr;
      fstx_filter_stats st;
      if (rc_ok(fstx_filter(f, &pr, 1, 0, tb.n, 2, 1, 0, &sel, &st, &e), e, "binary filter")) {
        uint64_t n = 0;
        const uint64_t* got = fstx_selection_rows(sel, &n);
        std::vector<uint64_t> want;
        for (uint64_t i = 0; i < tb.n; ++i)
          if (tb.idx[0][i] >= 0 && tb.tab[(size_t)tb.idx[0][i]] == v) want.push_back(i);
        check(!want.empty() && n == want.size() && std::memcmp(got, want.data(), n * 8) == 0, "binary == with a NUL byte");
        fstx_selection_free(sel);
      }
      pr.op = FSTX_OP_LT;
      check(fstx_filter(f, &pr, 1, 0, tb.n, 2, 1, 0, &sel, &st, &e) == FSTX_ERR_ARG, "binary < is refused");
      std::memset(&pr, 0, sizeof(pr));
      pr.col = 1, pr.op = FSTX_OP_ISNA;
      if (rc_ok(fstx_filter(f, &pr, 1, 0, tb.n, 2, 1, 0, &sel, &st, &e), e, "int64 is.na filter")) {
        uint64_t n = 0, k = 0;
        fstx_selection_rows(sel, &n);
        for (uint64_t i = 0; i < tb.n; ++i) k += tb.i64[0][i] == NA_I64;
        check(n == k && k > 0, "int64 is.na filter");
        fstx_selection_free(sel);
      }
      fstx_close(f);
    }
  }

  // open: a type 4 / 5 column without its bit, kind 7 under int32, logical types on int64 are corrupt; bits 16 / 17
  // without such a column read; phys 6 is a newer file
  fstx::Forge g;
  g.req = 0;
  check(forge(p, q, g) && open_code(q) == FSTX_ERR_CORRUPT, "binary and int64 columns without their bits are corrupt");
  g.req = 1ull << 17;
  g.names = {"binary"};
  check(forge(p, q, g) && open_code(q) == FSTX_ERR_CORRUPT, "an int64 column without bit 16 is corrupt");
  g.req = 3ull << 16;
  g.names = {"int64", "binary"};
  g.phys0 = FSTX_STRING;  // binary pages under a string column: opens (same pages)
  check(forge(p, q, g) && open_code(q) == FSTX_OK, "string pages are the same for both types");
  g.phys0 = 6;
  check(forge(p, q, g) && open_code(q) == FSTX_ERR_FORMAT, "type 6 is a newer file");
  if (write12(p, ti, FSTX_CODEC_ZSTD, 2, 0) == FSTX_OK) {  // column 0 is int64
    for (int lt : {FSTX_LT_FACTOR, FSTX_LT_DATE, FSTX_LT_POSIXCT}) {
      fstx::Forge k;
      k.req = 1ull << 16;
      k.names = {"int64"};
      k.logical0 = lt;
      check(forge(p, q, k) && open_code(q) == FSTX_ERR_CORRUPT, "logical type " + std::to_string(lt) + " on int64 is corrupt");
    }
  }

  WriteArgs a;
  const Table small = make_table(3000, 5, 0);
  if (write_table(p, small, a)) {
    fstx::Forge h;
    h.kind0 = 7;
    check(forge(p, q, h) && open_code(q) == FSTX_ERR_CORRUPT, "kind 7 under an int32 column is corrupt");
    h = fstx::Forge();
    h.phys0 = FSTX_INT64;
    h.req = 1ull << 16;
    h.names = {"int64"};
    check(forge(p, q, h) && open_code(q) == FSTX_ERR_CORRUPT, "kind 1 pages under an int64 column are corrupt");
    h = fstx::Forge();
    h.kind0 = 8;
    check(forge(p, q, h) && open_code(q) == FSTX_ERR_FORMAT, "kind 8 is a newer file");
    h = fstx::Forge();
    h.req = 3ull << 16;
    h.names = {"int64", "binary"};
    check(forge(p, q, h) && read_all_same(q, small, 2), "bits 16 and 17 without such columns: reads");
  }

  // append (the bits stay, the rows add up) and page-copy compaction (the feature fields are kept)
  T12 tb2 = make_bin(7000);
  if (write12(p, tb, FSTX_CODEC_ZSTD, 2, 0) == FSTX_OK && write12(p, tb2, FSTX_CODEC_LZ4, 2, 0, 0, 0, 0, true) == FSTX_OK) {
    T12 all = tb;
    all.n += tb2.n;
    all.idx[0].insert(all.idx[0].end(), tb2.idx[0].begin(), tb2.idx[0].end());
    all.i64[0].insert(all.i64[0].end(), tb2.i64[0].begin(), tb2.i64[0].end());
    check12(p, all, "binary + int64 appended");
    check_pages12(p, all, 0, "binary + int64 appended");
    fstx_error e;
    check(rc_ok(fstx_compact_copy(p.c_str(), q.c_str(), &e), e, "compact 1.2 file"), "compact 1.2 file");
    check12(q, all, "binary + int64 compacted");
    check_pages12(q, all, 0, "binary + int64 compacted");
  }
  std::remove(p.c_str());
  std::remove(q.c_str());
}

// int64 filters against a plain scan with exact comparisons: int64 literals (has_i64), doubles compared as real
// numbers, IN with both kinds of set, NA rows; pages are skipped only when no row can match
static bool ref_i64(int op, bool has, int64_t iv, double dv, const std::vector<int64_t>& iset,
                    const std::vector<double>& dset, bool set_na, int64_t x) {
  if (x == NA_I64) return op == FSTX_OP_ISNA || (op == FSTX_OP_IN && set_na);
  switch (op) {
    case FSTX_OP_NOTNA: return true;
    case FSTX_OP_ISNA: return false;
    case FSTX_OP_IN:
      if (has) return std::find(iset.begin(), iset.end(), x) != iset.end();
      for (double d : dset)
        if (d == d && fstx::cmp_d_i64(d, x) == 0) return true;
      return false;
    default: break;
  }
  if (has ? iv == NA_I64 : dv != dv) return false;  // a comparison with NA / NaN matches no row
  const int c = has ? (x < iv ? -1 : x > iv ? 1 : 0) : -fstx::cmp_d_i64(dv, x);  // x against the literal
  switch (op) {
    case FSTX_OP_EQ: return c == 0;
    case FSTX_OP_NE: return c != 0;
    case FSTX_OP_LT: return c < 0;
    case FSTX_OP_LE: return c <= 0;
    case FSTX_OP_GT: return c > 0;
    case FSTX_OP_GE: return c >= 0;
    default: return false;
  }
}

static void test_filter12(const std::string& dir) {
  T12 t = make_i64(20000);
  t.names.push_back("r");  // sorted values near 2^62: page bounds rounded to multiples of 1024
  t.phys.push_back(FSTX_INT64);
  t.i64.emplace_back();
  for (uint64_t i = 0; i < t.n; ++i) t.i64.back().push_back((1LL << 62) + (int64_t)i);
  const std::string p = dir + "/f12.fstx";
  if (!rc_ok(write12(p, t, FSTX_CODEC_ZSTD, 2, 0), fstx_error{}, "int64 filter file")) return;
  fstx_error e;
  fstx_file* f = nullptr;
  if (!rc_ok(fstx_open(p.c_str(), &f, &e), e, "open int64 filter file")) return;
  const std::vector<int64_t> w = wide_values();
  const std::vector<int64_t> ints = {0, -1, 1, INT64_MAX, INT64_MIN + 1, INT64_MIN, (1LL << 53), (1LL << 53) + 1,
                                     (1LL << 53) + 3, 1LL << 62, (1LL << 62) + 8191, (1LL << 62) + 8192,
                                     (1LL << 62) + 19999, 4611686018427387911LL, w[7], w[20], w[33]};
  const double inf = HUGE_VAL;
  const std::vector<double> dbls = {0.0, 0.5, -0.5, 2.5, -1.0, 9007199254740992.0, 9007199254740994.0,
                                    4611686018427387904.0, 4611686018427395072.0, 4611686018427388928.0,
                                    9223372036854775808.0, -9223372036854775808.0, 1e19, -1e19, inf, -inf,
                                    std::nan(""), (double)w[7], (double)w[20], (double)4611686018427387911LL};
  std::vector<uint64_t> got_rows, want;
  long calls = 0, bad = 0;
  uint64_t skipped = 0;
  auto run = [&](const fstx_pred& pr, int verify, const std::vector<int64_t>& iset, const std::vector<double>& dset) {
    fstx_selection* sel = nullptr;
    fstx_filter_stats st;
    ++calls;
    if (fstx_filter(f, &pr, 1, 0, t.n, 3, verify, 0, &sel, &st, &e) != FSTX_OK) {
      ++bad;
      std::printf("  filter failed: %s\n", e.message);
      return;
    }
    skipped += st.pages_skipped;
    uint64_t n = 0;
    const uint64_t* got = fstx_selection_rows(sel, &n);
    const std::vector<int64_t>& x = t.i64[pr.col];
    want.clear();
    for (uint64_t r = 0; r < t.n; ++r)
      if (ref_i64(pr.op, pr.has_i64 != 0, pr.i64, pr.value, iset, dset, pr.set_na != 0, x[r])) want.push_back(r);
    if (n != want.size() || (n && std::memcmp(got, want.data(), n * 8) != 0)) {
      if (bad++ < 5)
        std::printf("  int64 filter mismatch: column %s op %d %s %lld / %.17g: %llu rows, want %llu\n",
                    t.names[pr.col].c_str(), pr.op, pr.has_i64 ? "int" : "double", (long long)pr.i64, pr.value,
                    (unsigned long long)n, (unsigned long long)want.size());
    }
    fstx_selection_free(sel);
  };
  const std::vector<int64_t> none_i;
  const std::vector<double> none_d;
  for (uint32_t c = 0; c < t.names.size(); ++c) {
    for (int op = FSTX_OP_EQ; op <= FSTX_OP_GE; ++op) {
      for (int64_t v : ints) {
        fstx_pred pr;
        std::memset(&pr, 0, sizeof(pr));
        pr.col = c, pr.op = op, pr.has_i64 = 1, pr.i64 = v, pr.value = std::nan("");  // value is ignored
        run(pr, 1, none_i, none_d);
      }
      for (double v : dbls) {
        fstx_pred pr;
        std::memset(&pr, 0, sizeof(pr));
        pr.col = c, pr.op = op, pr.value = v;
        run(pr, op == FSTX_OP_LE ? FSTX_VERIFY_HASH | FSTX_VERIFY_NULLS : 1, none_i, none_d);
      }
    }
    for (int na = 0; na < 2; ++na) {
      std::vector<int64_t> is = {w[3], w[7], INT64_MIN, 12345, (1LL << 62) + 100, (1LL << 53) + 3, 4611686018427387911LL};
      std::vector<double> ds = {2.5, (double)w[3], 9007199254740996.0, std::nan(""), 4611686018427387904.0, -inf};
      fstx_pred pr;
      std::memset(&pr, 0, sizeof(pr));
      pr.col = c, pr.op = FSTX_OP_IN, pr.set_na = na, pr.has_i64 = 1, pr.i64_set = is.data(), pr.n_i64_set = is.size();
      run(pr, 1, is, ds);
      std::memset(&pr, 0, sizeof(pr));
      pr.col = c, pr.op = FSTX_OP_IN, pr.set_na = na, pr.set = ds.data(), pr.n_set = ds.size();
      run(pr, 1, is, ds);
    }
    for (int op : {FSTX_OP_ISNA, FSTX_OP_NOTNA, FSTX_OP_NONE}) {
      fstx_pred pr;
      std::memset(&pr, 0, sizeof(pr));
      pr.col = c, pr.op = op;
      run(pr, 1, none_i, none_d);
      run(pr, FSTX_VERIFY_HASH | FSTX_VERIFY_NULLS, none_i, none_d);
    }
  }
  std::printf("int64 filters: %ld filters, %ld wrong, %llu pages skipped\n", calls, bad, (unsigned long long)skipped);
  check(bad == 0 && calls > 1000, "int64 filters exact against a plain scan");
  // page skipping on the rounded bounds: r >= 2^62 + 17408 skips pages 0 and 1 (max rounded up to 2^62 + 16384)
  fstx_pred pr;
  std::memset(&pr, 0, sizeof(pr));
  pr.col = 6, pr.op = FSTX_OP_GE, pr.has_i64 = 1, pr.i64 = (1LL << 62) + 17408;
  fstx_selection* sel = nullptr;
  fstx_filter_stats st;
  if (rc_ok(fstx_filter(f, &pr, 1, 0, t.n, 2, 1, 0, &sel, &st, &e), e, "int64 page skipping")) {
    uint64_t n = 0;
    fstx_selection_rows(sel, &n);
    check(n == 2592 && st.pages_skipped == 2 && st.pages_total == 3, "int64 pages skipped by their rounded bounds");
    fstx_selection_free(sel);
  }
  // a page whose rounded max reaches the literal but whose values do not: read, no rows (not wrongly skipped / kept)
  pr.i64 = (1LL << 62) + 8192;  // page 0 holds 2^62 .. 2^62 + 8191, its max rounds up to 2^62 + 8192
  pr.op = FSTX_OP_GE;
  if (rc_ok(fstx_filter(f, &pr, 1, 0, 8192, 2, 1, 0, &sel, &st, &e), e, "int64 rounded bound")) {
    uint64_t n = 0;
    fstx_selection_rows(sel, &n);
    check(n == 0 && st.pages_skipped == 0 && st.pages_read == 1, "a rounded bound keeps the page, the rows decide");
    fstx_selection_free(sel);
  }
  fstx_close(f);
  std::remove(p.c_str());
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".";
  if (argc > 2 && std::string(argv[2]) == "time-bin16")
    return time_bin16(dir, argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 4000000);
  const uint64_t n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 200000;
  std::printf("fstx coretest: %llu rows, folder %s, %u hardware threads\n", (unsigned long long)n, dir.c_str(),
              fstx_hardware_threads());
  std::printf("SIMD targets compiled:");
  for (int b = 0; b < 63; ++b)
    if (fstx::simd::compiled_targets() & (1LL << b)) std::printf(" %s", fstx::simd::target_name(1LL << b));
  std::printf("\nSIMD targets this CPU runs:");
  for (int b = 0; b < 63; ++b)
    if (fstx::simd::cpu_targets() & (1LL << b)) std::printf(" %s", fstx::simd::target_name(1LL << b));
  std::printf("\n");

  const Table t = make_table(n, 42, 0);
  // scalar first, then every target the CPU runs: same file bytes under each
  std::vector<int64_t> modes = {0};
  for (int b = 0; b < 63; ++b)
    if (fstx::simd::cpu_targets() & (1LL << b)) modes.push_back(1LL << b);
  std::vector<uint64_t> ref, ref12;
  uint64_t ref_app = 0;
  for (int64_t m : modes) {
    check(m ? fstx::simd::set_mode(2, m) : fstx::simd::set_mode(0, 0), "set SIMD mode");
    const std::string lab = target_label();
    test_kernels();
    const std::vector<uint64_t> hs = test_write_read(dir, t);
    if (m == 0) test_get_values(dir, t);
    const std::string p = dir + "/main.fstx";
    WriteArgs a;
    if (write_table(p, t, a)) test_rows_filter_colsrc(p, t);
    const uint64_t ha = test_append_compact(dir, t);
    std::printf("[%s] hashes:", lab.c_str());
    for (size_t i = 0; i < hs.size(); ++i) std::printf(" %s=%016llx", codec_name((int)i), (unsigned long long)hs[i]);
    std::printf(" compacted=%016llx\n", (unsigned long long)ha);
    if (m == 0) {
      ref = hs;
      ref_app = ha;
    } else {
      check(hs == ref && ha == ref_app, "same bytes as scalar under " + lab);
    }
    if (m == modes.back()) test_errors(dir, p);
    std::remove(p.c_str());
    // fstx 1.2 files (int64 and binary columns): their own line, so the hashes above stay comparable with 1.0
    std::vector<std::string> nm12;
    const std::vector<uint64_t> h12 = test_i64_bin(dir, nm12);
    std::printf("[%s] 1.2 hashes:", lab.c_str());
    for (size_t i = 0; i < h12.size(); ++i) std::printf(" %s=%016llx", nm12[i].c_str(), (unsigned long long)h12[i]);
    std::printf("\n");
    if (m == 0) {
      ref12 = h12;
      test_rules12(dir);
      test_filter12(dir);
      test_rand16(dir);
    } else {
      check(h12 == ref12, "1.2 files: same bytes as scalar under " + lab);
    }
  }
  fstx::simd::set_mode(1, 0);
  fstx_shutdown();
  std::printf("coretest: %d checks passed, %d failed\n", g_pass, g_fail);
  std::printf(g_fail ? "CORETEST FAIL\n" : "CORETEST OK\n");
  return g_fail ? 1 : 0;
}
