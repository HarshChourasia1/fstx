// fstx coretest: the engine (src/, no R) on its own: kernel self-test, write, read, row lists, filters, append
// (with a simulated crash), compaction, column sources and error paths, under every SIMD target the CPU runs.
// Synthetic data from a fixed seed with integer-only arithmetic, so every platform writes the same bytes; the
// FNV-1a hash of each file is printed for comparing platforms. Build: tools/coretest/Makefile. Not in the
// package tarball. MIT licence.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
  if (phys == FSTX_STRING) {
    h.s[out].assign(n, std::string());
    h.na[out].assign(n, 0);
    return &h;
  }
  h.fx[out].assign((size_t)n * (phys == FSTX_FLOAT64 ? 8 : 4) + 8, 0);
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
                      const std::vector<uint64_t>* rows, int threads, Host& h) {
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
    ok = rc_ok(fstx_read(p, &host, threads, 1, &e), e, "read " + path);
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

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".";
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
  std::vector<uint64_t> ref;
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
  }
  fstx::simd::set_mode(1, 0);
  fstx_shutdown();
  std::printf("coretest: %d checks passed, %d failed\n", g_pass, g_fail);
  std::printf(g_fail ? "CORETEST FAIL\n" : "CORETEST OK\n");
  return g_fail ? 1 : 0;
}
