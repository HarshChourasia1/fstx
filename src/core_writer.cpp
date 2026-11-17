// FST-X core engine: writer. A parallel prep pass plans every string column chunk (dictionary, FSST or
// plain); then workers encode (chooser + encodings, enc.h) and compress pages straight from host memory with
// their own codec contexts and buffers, and the calling thread is the single writer that puts pages on disk
// in order. In-flight memory is bounded to about (threads x 2) pages. No R headers.
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#include "core_file.h"

namespace fstx {

namespace {

struct PageMeta {
  uint64_t offset = 0, first_row = 0, hash = 0;
  double mn = 0, mx = 0;
  uint32_t comp_len = 0, raw_len = 0, row_count = 0, null_count = 0, chain = 0;
  uint8_t codec = 0, flags = 0;
};

struct ColDef {
  std::string name;
  int phys = 0, logical = 0, ordered = 0;
  std::vector<std::string> levels;
  bool has_tz = false;
  std::string tz;
  std::vector<uint8_t> attrs;
};

struct RowGroupMeta {
  uint64_t first_row = 0, row_count = 0;
  std::vector<std::vector<PageMeta>> chunks;  // [column][page]
  std::vector<PageMeta> dicts;                // [column] string dictionary page, when has_dict
  std::vector<uint8_t> has_dict;
};

struct Task {
  uint32_t col;
  uint32_t rg;          // index into Writer::rgs
  uint64_t row_start;   // row within this append call
  uint32_t row_count;
  uint8_t what;         // 0 = data page, 1 = the chunk's string dictionary page
};

struct Slot {
  enc::Buf raw;
  std::vector<uint8_t> comp;
  std::vector<uint8_t> probe;  // slices of a numeric page for the codec probe
  const uint8_t* out = nullptr;
  size_t out_len = 0;
  PageMeta meta;
  int64_t task = -1;
  bool ready = false;
};

const int32_t kNaInt = std::numeric_limits<int32_t>::min();

// Codec probe for numeric data pages of 64 KB and more: the codec first runs on 4 slices of 8 KB spread over the
// page, and when they save under half of keep_pct the page is stored as it is, without the full pass. Bit-packed
// ALP / ALP-RD / FOR pages of noisy values give the codec nothing and used to cost a full pass each.
constexpr size_t kProbeMin = 65536, kProbeSlice = 8192, kProbeSlices = 4;

// Page statistics in 16-byte vectors (GCC / Clang vector extensions: SSE2 on x86-64, NEON on arm64), 8 values a step
// in 4 independent chains; GCC's -O2 cost model did not vectorize the plain loops. Blends are bitwise (masks from the
// compares). NaNs fail every compare, so they never become min or max; a min or max of zero gets the sign of the
// first zero, as the sequential loop gave. NA count: NaNs whose low word is 1954 (R's NA_real_), counted in a second
// pass only when the page has a NaN.
typedef double vd2 __attribute__((vector_size(16)));
typedef int64_t vl2 __attribute__((vector_size(16)));
typedef int32_t vi4 __attribute__((vector_size(16)));
inline vd2 vsel(vl2 m, vd2 a, vd2 b) { return (vd2)(((vl2)a & m) | ((vl2)b & ~m)); }
inline vi4 vsel(vi4 m, vi4 a, vi4 b) { return (a & m) | (b & ~m); }

void dbl_stats(const double* x, uint32_t n, double& mn, double& mx, uint32_t& na) {
  const double inf = std::numeric_limits<double>::infinity();
  vd2 a[4], b[4];
  vl2 c[4];
  for (int k = 0; k < 4; ++k) {
    a[k] = vd2{inf, inf};
    b[k] = vd2{-inf, -inf};
    c[k] = vl2{0, 0};
  }
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8)
    for (int k = 0; k < 4; ++k) {
      vd2 v;
      std::memcpy(&v, x + i + 2 * k, 16);
      a[k] = vsel(v < a[k], v, a[k]);
      b[k] = vsel(v > b[k], v, b[k]);
      c[k] -= (vl2)(v != v);
    }
  double sa = inf, sb = -inf;
  int64_t nan = 0;
  for (; i < n; ++i) {
    const double v = x[i];
    sa = v < sa ? v : sa;
    sb = v > sb ? v : sb;
    nan += v != v;
  }
  mn = sa;
  mx = sb;
  for (int k = 0; k < 4; ++k)
    for (int l = 0; l < 2; ++l) {
      mn = std::min(mn, a[k][l]);
      mx = std::max(mx, b[k][l]);
      nan += c[k][l];
    }
  na = 0;
  if (nan)
    for (uint32_t j = 0; j < n; ++j) {
      uint64_t bits;
      std::memcpy(&bits, &x[j], 8);
      na += (x[j] != x[j]) & ((uint32_t)bits == 1954u);
    }
  if (mn == 0 || mx == 0)
    for (uint32_t j = 0; j < n; ++j)
      if (x[j] == 0) {
        if (mn == 0) mn = x[j];
        if (mx == 0) mx = x[j];
        break;
      }
}

void int_stats(const int32_t* x, uint32_t n, int32_t& mn, int32_t& mx, uint32_t& na) {
  const int32_t hi = std::numeric_limits<int32_t>::max(), lo = std::numeric_limits<int32_t>::min();
  const vi4 vna = {kNaInt, kNaInt, kNaInt, kNaInt}, vhi = {hi, hi, hi, hi};
  vi4 a[2] = {vhi, vhi}, b[2], c[2];
  for (int k = 0; k < 2; ++k) {
    b[k] = vi4{lo, lo, lo, lo};
    c[k] = vi4{0, 0, 0, 0};
  }
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8)
    for (int k = 0; k < 2; ++k) {
      vi4 v;
      std::memcpy(&v, x + i + 4 * k, 16);
      const vi4 m = v == vna;
      const vi4 w = vsel(m, vhi, v);  // NA (the smallest int32) never the min
      c[k] -= m;
      a[k] = vsel(w < a[k], w, a[k]);
      b[k] = vsel(v > b[k], v, b[k]);
    }
  int32_t sa = hi, sb = lo;
  uint32_t t = 0;
  for (; i < n; ++i) {
    const int32_t v = x[i];
    const bool isna = v == kNaInt;
    const int32_t w = isna ? hi : v;
    t += isna;
    sa = w < sa ? w : sa;
    sb = v > sb ? v : sb;
  }
  mn = sa;
  mx = sb;
  for (int k = 0; k < 2; ++k)
    for (int l = 0; l < 4; ++l) {
      mn = std::min(mn, a[k][l]);
      mx = std::max(mx, b[k][l]);
      t += (uint32_t)c[k][l];
    }
  na = t;
}

}  // namespace

struct WriterImpl {
  OutFile out;
  std::vector<ColDef> cols;
  std::vector<uint8_t> table_attrs;
  int codec = FSTX_CODEC_ZSTD, level = 1, threads = 1;
  int profile = enc::P_BALANCED, keep_pct = 10, force_num = 0, force_str = 0;
  int force_codes = 0;     // tests: encoding forced on string dictionary code pages
  bool keep_codec = false;  // tests: keep the codec on every page, even when it does not save space
  // format of the file written: always version 3 (the forge alone may set another); appends and compaction keep
  // the file's feature fields
  uint32_t version = kFormatVersion;
  uint64_t req_features = 0, opt_features = 0;
  std::vector<std::string> feature_names;  // built at close from the bits (forge: given)
  std::string file_names[64];              // names from the file's own list, by bit
  int stored_profile = -1;                 // appends and compaction: the file's profile, kept as stored
  const Forge* forge = nullptr;  // tests only (forge_copy)
  std::vector<std::vector<enc::StrChunk>> sch;  // [row group of this append][column]: string chunk plans
  size_t rg0 = 0;                               // first row group of the current append call
  uint64_t rg_size = kDefaultRowGroup;
  uint32_t page_bytes = kDefaultPageBytes;
  uint64_t total_rows = 0;
  int test_crash = 0;  // tests only: 1 = an append stops after its tail, before the header is reset
  std::vector<RowGroupMeta> rgs;
  std::vector<std::unique_ptr<CodecContext>> ctxs;  // one per worker, plus one for the calling thread
  // M5 appends: the file's committed end before this append, and its header with no append in progress
  bool appending = false;
  uint64_t commit_end = 0;
  uint8_t hdr0[kHeaderSize] = {0};

  ~WriterImpl() {
    if (appending) rollback();
  }

  CodecContext& ctx(int id) {
    while ((int)ctxs.size() <= id) ctxs.emplace_back(new CodecContext());
    return *ctxs[(size_t)id];
  }

  void open(const std::string& path) {
    out.create(path);
    uint8_t h[kHeaderSize] = {0};
    std::memcpy(h, "FSTX", 4);
    put_u16(h + 4, (uint16_t)version);
    put_u16(h + 6, (uint16_t)(forge && forge->hdr_flags >= 0 ? forge->hdr_flags : req_features & kFeatHeaderMask));
    put_u32(h + 8, kEndianMark);
    put_u64(h + 24, hash64(h, 24));
    out.write(h, kHeaderSize);
    out.pad_to(kDataStart);
  }

  // ---- M5: the existing file's row groups become this writer's first row groups
  static PageMeta meta_of(const fb::Page* p) {
    PageMeta m;
    m.offset = p->offset();
    m.first_row = p->first_row();
    m.hash = p->hash();
    m.mn = p->min();
    m.mx = p->max();
    m.comp_len = p->comp_len();
    m.raw_len = p->raw_len();
    m.row_count = p->row_count();
    m.null_count = p->null_count();
    m.chain = p->chain();
    m.codec = (uint8_t)p->codec();
    m.flags = p->flags();
    return m;
  }

  void load_row_groups(const fstx_file& F) {
    const auto* g = F.footer->row_groups();
    const uint32_t nrg = g ? g->size() : 0;
    for (uint32_t i = 0; i < nrg; ++i) {
      const fb::RowGroup* rg = g->Get(i);
      RowGroupMeta m;
      m.first_row = rg->first_row();
      m.row_count = rg->row_count();
      m.chunks.resize(cols.size());
      m.dicts.resize(cols.size());
      m.has_dict.assign(cols.size(), 0);
      for (uint32_t c = 0; c < cols.size(); ++c) {
        const fb::ColumnChunk* ch = rg->columns()->Get(c);
        for (const fb::Page* p : *ch->pages()) m.chunks[c].push_back(meta_of(p));
        if (ch->dictionary()) {
          m.dicts[c] = meta_of(ch->dictionary());
          m.has_dict[c] = 1;
        }
      }
      rgs.push_back(std::move(m));
    }
    total_rows = F.n_rows;
    rg_size = F.footer->row_group_size();
    if (F.profile <= enc::P_SMALL) profile = F.profile;  // a newer profile: encode with the given one
    keep_features(F);
  }

  // Appends and compaction keep the file's profile (as stored) and feature fields. Only format 3 files get here
  // (open refuses others). An optional bit this build does not know may describe data the change would make
  // wrong, so the file is refused (unknown required bits were already refused when it was opened).
  void keep_features(const fstx_file& F) {
    stored_profile = F.profile;
    const uint64_t req = F.footer->required_features(), opt = F.footer->optional_features();
    if (opt & ~features_known())
      fail(FSTX_ERR_FORMAT, "'%s' uses file features this fstx (%s) does not know. Update fstx to change this file; reading works.",
           F.path.c_str(), kVersion);
    req_features |= req;
    opt_features = opt;
    if (const auto* nm = F.footer->feature_names()) {
      uint32_t n = 0;
      for (int b = 0; b < 64; ++b) n += (uint32_t)((req >> b & 1u) + (opt >> b & 1u));
      if (nm->size() == n) {
        uint32_t i = 0;
        for (int pass = 0; pass < 2; ++pass)
          for (int b = 0; b < 64; ++b)
            if (((pass ? opt : req) >> b) & 1u) file_names[b] = nm->Get(i++)->str();
      }
    }
  }

  // One name per set bit, required bits first, then optional, each in ascending bit order.
  void name_features() {
    feature_names.clear();
    for (int pass = 0; pass < 2; ++pass)
      for (int b = 0; b < 64; ++b) {
        if (!(((pass ? opt_features : req_features) >> b) & 1u)) continue;
        if (!file_names[b].empty()) feature_names.push_back(file_names[b]);
        else if (feature_name(b)) feature_names.push_back(feature_name(b));
        else fail(FSTX_ERR_INTERNAL, "file feature bit %d has no name", b);
      }
  }

  void check_same_columns(const fstx_file& F) {
    if (F.n_cols != cols.size())
      fail(FSTX_ERR_ARG, "the file has %u columns, the new rows %u", F.n_cols, (unsigned)cols.size());
    for (uint32_t c = 0; c < F.n_cols; ++c) {
      const fb::Column* x = F.col(c);
      const ColDef& d = cols[c];
      if (F.col_name(c) != d.name)
        fail(FSTX_ERR_ARG, "column %u is '%s' in the file but '%s' in the new rows", c + 1, F.col_name(c).c_str(),
             d.name.c_str());
      if ((int)x->phys() != d.phys || (int)x->logical() != d.logical || (x->ordered() ? 1 : 0) != (d.ordered ? 1 : 0))
        fail(FSTX_ERR_ARG, "column '%s' has a different type in the new rows", d.name.c_str());
      if (d.logical == FSTX_LT_FACTOR) {
        const auto* lv = x->levels();
        const uint32_t nl = lv ? lv->size() : 0;
        bool ok = d.levels.size() >= nl;
        for (uint32_t i = 0; ok && i < nl; ++i) ok = lv->Get(i)->str() == d.levels[i];
        if (!ok) fail(FSTX_ERR_ARG, "factor column '%s': the new levels must start with the file's levels", d.name.c_str());
      }
    }
  }

  // Header bytes 16..23 = the committed end, flushed to disk before any new byte is written; from here until
  // close resets them, readers see the old table and ignore everything past it.
  void begin_append(const std::string& path, const uint8_t* hdr, uint64_t end) {
    std::memcpy(hdr0, hdr, kHeaderSize);
    put_u16(hdr0 + 6, (uint16_t)(req_features & kFeatHeaderMask));
    put_u64(hdr0 + 16, 0);
    put_u64(hdr0 + 24, hash64(hdr0, 24));
    uint8_t h1[kHeaderSize];
    std::memcpy(h1, hdr0, kHeaderSize);
    put_u64(h1 + 16, end);
    put_u64(h1 + 24, hash64(h1, 24));
    out.open_existing(path);
    commit_end = end;
    appending = true;
    out.write_at(0, h1, kHeaderSize);
    out.sync();
    out.truncate(end);  // drops what an interrupted append left behind
  }

  // Back to the committed table. Never throws; if it fails, the header still holds the committed end.
  void rollback() noexcept {
    appending = false;
    try {
      out.truncate(commit_end);
      out.sync();
      out.write_at(0, hdr0, kHeaderSize);
      out.sync();
    } catch (...) {
    }
    try {
      out.close();
    } catch (...) {
    }
  }

  // ---- string prep pass, in parallel over (string column, row group): reads every string once, checks the
  // encoding, records per-vector sizes for page planning and picks dictionary / FSST / plain for the chunk.
  void prep_strings(const fstx_column_data* data, uint64_t n) {
    FSTX_PROF_SCOPE(W_PREP_STR);
    std::vector<uint32_t> scols;
    for (uint32_t c = 0; c < cols.size(); ++c)
      if (cols[c].phys == FSTX_STRING) scols.push_back(c);
    const uint64_t nrg = (n + rg_size - 1) / rg_size;
    sch.clear();
    sch.resize((size_t)nrg);
    for (auto& g : sch) g.resize(cols.size());
    if (scols.empty() || n == 0) return;
    const uint64_t ntasks = nrg * scols.size();
    std::atomic<uint64_t> next(0);
    std::atomic<int> bad(0);  // 0 ok, 1 translate, 2 impossible
    std::atomic<int64_t> bad_col(std::numeric_limits<int64_t>::max());
    const int k = (int)std::min<uint64_t>((uint64_t)threads, ntasks);
    for (int i = 0; i < std::max(k, 1); ++i) ctx(i);  // contexts are made here, before the workers start

    auto job = [&](int id) {
      enc::EncCtx cx;
      cx.profile = profile;
      cx.codec = codec;
      cx.cc = ctxs[(size_t)id].get();
      for (;;) {
        uint64_t t = next.fetch_add(1);
        if (t >= ntasks) return;
        const uint32_t c = scols[(size_t)(t / nrg)];
        const uint64_t g = t % nrg;
        const uint64_t r0 = g * rg_size, r1 = std::min(n, r0 + rg_size);
        int rc = enc::plan_str_chunk(data[c], r0, r1, cx, force_str, sch[(size_t)g][c]);
        if (rc != 0) {
          int prev = bad.load();
          while (rc > prev && !bad.compare_exchange_weak(prev, rc)) {
          }
          int64_t pc = bad_col.load();
          while ((int64_t)c < pc && !bad_col.compare_exchange_weak(pc, (int64_t)c)) {
          }
        }
      }
    };
    if (k <= 1) {
      job(0);
    } else {
      ThreadPool::instance().start(k, job);
      ThreadPool::instance().wait();
    }
    if (bad.load() == 1)
      throw Error(FSTX_ERR_TRANSLATE, "column '" + cols[(size_t)bad_col.load()].name + "' has strings that are not UTF-8",
                  bad_col.load());
    if (bad.load() >= 2)
      throw Error(FSTX_ERR_ARG,
                  "column '" + cols[(size_t)bad_col.load()].name +
                      "' has strings marked as 'bytes' encoding, which cannot be stored as UTF-8 text",
                  bad_col.load());
  }

  // ---- page plan for one column of one row group (r0, r1: rows within this append call)
  void plan_column(uint32_t c, uint32_t rg, uint64_t r0, uint64_t r1, const fstx_column_data& d,
                   std::vector<Task>& tasks) {
    const int phys = cols[c].phys;
    if (phys != FSTX_STRING) {
      const uint64_t w = phys == FSTX_FLOAT64 ? 8 : 4;
      uint64_t rpp = std::max<uint64_t>(kVector, (page_bytes / w) / kVector * kVector);
      for (uint64_t s = r0; s < r1; s += rpp) tasks.push_back({c, rg, s, (uint32_t)std::min(rpp, r1 - s), 0});
      return;
    }
    const enc::StrChunk& sc = sch[(size_t)(rg - rg0)][c];
    if (sc.mode == enc::T_DICT) {
      // the dictionary page first, then the int32 code pages
      tasks.push_back({c, rg, r0, (uint32_t)sc.dptr.size(), 1});
      uint64_t rpp = std::max<uint64_t>(kVector, (page_bytes / 4) / kVector * kVector);
      for (uint64_t s = r0; s < r1; s += rpp) tasks.push_back({c, rg, s, (uint32_t)std::min(rpp, r1 - s), 0});
      return;
    }
    // plain / FSST strings: grow in 1024-row vectors until the page holds page_bytes; vectors far over the
    // cap (very long strings) are split row by row.
    const uint64_t big = 4ull * page_bytes;
    uint64_t start = r0, acc = 0;
    for (uint64_t vs = r0; vs < r1; vs += kVector) {
      const uint64_t ve = std::min(r1, vs + kVector);
      const uint64_t sz = sc.vsizes[(size_t)((vs - r0) / kVector)];
      if (sz > big) {
        if (vs > start) tasks.push_back({c, rg, start, (uint32_t)(vs - start), 0});
        std::vector<const char*> ptrs(kVector);
        std::vector<int32_t> lens(kVector);
        uint32_t m = (uint32_t)(ve - vs);
        if (d.get_strings(d.ctx, vs, m, ptrs.data(), lens.data()) != 0)
          fail(FSTX_ERR_INTERNAL, "string source changed during write");
        uint64_t ps = vs, pacc = 0;
        for (uint32_t i = 0; i < m; ++i) {
          uint64_t rsz = 4ull + (lens[i] > 0 ? (uint64_t)lens[i] : 0);
          if (rsz > kMaxRawPage / 2)
            throw Error(FSTX_ERR_ARG, "column '" + cols[c].name + "' has a string longer than 1 GB", c);
          if (pacc > 0 && pacc + rsz > page_bytes) {
            tasks.push_back({c, rg, ps, (uint32_t)(vs + i - ps), 0});
            ps = vs + i;
            pacc = 0;
          }
          pacc += rsz;
        }
        tasks.push_back({c, rg, ps, (uint32_t)(ve - ps), 0});
        start = ve;
        acc = 0;
        continue;
      }
      if (acc > 0 && acc + sz > kMaxRawPage / 2) {  // keep encoded pages well below 2 GB
        tasks.push_back({c, rg, start, (uint32_t)(vs - start), 0});
        start = vs;
        acc = 0;
      }
      acc += sz;
      if (acc >= page_bytes) {
        tasks.push_back({c, rg, start, (uint32_t)(ve - start), 0});
        start = ve;
        acc = 0;
      }
    }
    if (start < r1) tasks.push_back({c, rg, start, (uint32_t)(r1 - start), 0});
  }

  // ---- encode + compress one page into a slot (worker side)
  void encode(const Task& t, const fstx_column_data* data, Slot& s, CodecContext& cc, std::vector<const char*>& ptrs,
              std::vector<int32_t>& lens) {
    const ColDef& col = cols[t.col];
    const fstx_column_data& d = data[t.col];
    PageMeta& pm = s.meta;
    pm = PageMeta();
    pm.row_count = t.row_count;
    const uint32_t n = t.row_count;
    enc::EncCtx cx;
    cx.profile = profile;
    cx.codec = codec;
    cx.cc = &cc;

    if (col.phys == FSTX_FLOAT64) {
      const double* x = (const double*)d.data + t.row_start;
      FSTX_PROF_BEGIN(prof_st, W_STATS);
      double mn, mx;
      uint32_t na;
      dbl_stats(x, n, mn, mx, na);
      pm.null_count = na;
      if (mn <= mx) {  // some value is not NaN
        pm.mn = mn;
        pm.mx = mx;
        pm.flags |= kFlagMinMax;
      }
      FSTX_PROF_END(prof_st);
      cx.force = force_num;
      pm.chain = enc::encode_dbl_page(x, n, cx, s.raw);
    } else if (col.phys == FSTX_INT32 || col.phys == FSTX_LOGICAL) {
      const int32_t* x = (const int32_t*)d.data + t.row_start;
      FSTX_PROF_BEGIN(prof_st, W_STATS);
      int32_t mn, mx;
      uint32_t na;
      int_stats(x, n, mn, mx, na);
      pm.null_count = na;
      if (na < n) {
        pm.mn = mn;
        pm.mx = mx;
        pm.flags |= kFlagMinMax;
      }
      FSTX_PROF_END(prof_st);
      cx.force = force_num;
      pm.chain = enc::encode_int_page(x, n, col.phys == FSTX_LOGICAL ? enc::K_LGL : enc::K_INT, kNaInt, cx, s.raw);
    } else {
      const enc::StrChunk& sc = sch[(size_t)(t.rg - rg0)][t.col];
      FSTX_PROF_SCOPE(W_STR_ENC);
      if (t.what == 1) {  // the chunk's dictionary (FSST-coded only when the plan made a symbol table for it)
        pm.chain = enc::encode_str_page(enc::K_DICT, sc.dptr.data(), sc.dlen.data(), n,
                                        sc.fsst ? enc::T_FSST : enc::T_PLAIN, sc.fsst.get(), cx, s.raw);
      } else if (sc.mode == enc::T_DICT) {
        const uint64_t r0 = (uint64_t)(t.rg - rg0) * rg_size;
        const int32_t* codes = sc.codes.data() + (t.row_start - r0);
        uint32_t na = 0;
        for (uint32_t i = 0; i < n; ++i) na += codes[i] < 0;
        pm.null_count = na;
        cx.force = force_codes;
        pm.chain = enc::encode_int_page(codes, n, enc::K_CODES, -1, cx, s.raw);
      } else {
        if (ptrs.size() < n) {
          ptrs.resize(n);
          lens.resize(n);
        }
        FSTX_PROF_BEGIN(prof_get, W_STR_GET);
        for (uint32_t done = 0; done < n;) {
          uint32_t m = std::min<uint32_t>(kVector, n - done);
          if (d.get_strings(d.ctx, t.row_start + done, m, ptrs.data() + done, lens.data() + done) != 0)
            fail(FSTX_ERR_INTERNAL, "string source changed during write");
          done += m;
        }
        FSTX_PROF_END(prof_get);
        uint32_t na = 0;
        for (uint32_t i = 0; i < n; ++i) na += lens[i] < 0;
        pm.null_count = na;
        pm.chain = enc::encode_str_page(enc::K_STR, ptrs.data(), lens.data(), n, sc.mode, sc.fsst.get(), cx, s.raw);
      }
    }

    const uint8_t* raw = s.raw.v.data();
    const size_t raw_len = s.raw.size();
    if (raw_len > kMaxRawPage) fail(FSTX_ERR_INTERNAL, "encoded page over 2 GB in column '%s'", col.name.c_str());
    pm.raw_len = (uint32_t)raw_len;
    s.out = raw;
    s.out_len = raw_len;
    pm.codec = FSTX_CODEC_NONE;
    if (codec != FSTX_CODEC_NONE && raw_len > 0) {
      size_t cap = compress_bound(codec, raw_len);
      if (s.comp.size() < cap) s.comp.resize(cap);
      FSTX_PROF_SCOPE(W_COMPRESS);
      bool skip = false;
      if (!keep_codec && t.what != 1 && raw_len >= kProbeMin &&
          (col.phys == FSTX_FLOAT64 || col.phys == FSTX_INT32 || col.phys == FSTX_LOGICAL)) {
        const size_t pn = kProbeSlice * kProbeSlices;
        if (s.probe.size() < pn) s.probe.resize(pn);
        for (size_t k = 0; k < kProbeSlices; ++k)
          std::memcpy(s.probe.data() + k * kProbeSlice, raw + (raw_len - kProbeSlice) * k / (kProbeSlices - 1),
                      kProbeSlice);
        const size_t pl = compress_page(cc, codec, level, s.probe.data(), pn, s.comp.data(), cap);
        skip = (uint64_t)pl * 200 > (uint64_t)pn * (uint64_t)(200 - keep_pct);
      }
      if (!skip) {
        size_t cl = compress_page(cc, codec, level, raw, raw_len, s.comp.data(), cap);
        // keep the codec only when it saves at least keep_pct percent (decoding it costs time)
        if ((cl < raw_len && (uint64_t)cl * 100 <= (uint64_t)raw_len * (uint64_t)(100 - keep_pct)) ||
            (keep_codec && cl <= kMaxRawPage)) {
          s.out = s.comp.data();
          s.out_len = cl;
          pm.codec = (uint8_t)codec;
        }
      }
    }
    pm.comp_len = (uint32_t)s.out_len;
    FSTX_PROF_SCOPE(W_HASH);
    pm.hash = hash64(s.out, s.out_len);
  }

  void put_page(const Task& t, Slot& s, uint64_t base_row) {
    FSTX_PROF_SCOPE(W_PUT);
    out.pad_to(align_up(out.pos()));
    s.meta.offset = out.pos();
    if (s.out_len) out.write(s.out, s.out_len);
    RowGroupMeta& g = rgs[t.rg];
    if (t.what == 1) {
      s.meta.first_row = g.first_row;
      g.dicts[t.col] = s.meta;
      g.has_dict[t.col] = 1;
    } else {
      s.meta.first_row = base_row + t.row_start;
      g.chunks[t.col].push_back(s.meta);
    }
  }

  void append(const fstx_column_data* data, uint64_t n) {
    if (n == 0) return;
    for (uint32_t c = 0; c < cols.size(); ++c) {
      if (cols[c].phys == FSTX_STRING) {
        if (!data[c].get_strings) fail(FSTX_ERR_ARG, "string column %u has no string source", c);
      } else if (!data[c].data) {
        fail(FSTX_ERR_ARG, "column %u has no data", c);
      }
    }
    rg0 = rgs.size();
    FSTX_PROF_SCOPE(W_TOTAL);
    prep_strings(data, n);

    // row groups and the page plan, in file order
    const uint64_t base_row = total_rows;
    std::vector<Task> tasks;
    for (uint64_t r0 = 0; r0 < n; r0 += rg_size) {
      uint64_t r1 = std::min(n, r0 + rg_size);
      RowGroupMeta g;
      g.first_row = base_row + r0;
      g.row_count = r1 - r0;
      g.chunks.resize(cols.size());
      g.dicts.resize(cols.size());
      g.has_dict.assign(cols.size(), 0);
      rgs.push_back(std::move(g));
      uint32_t rg = (uint32_t)(rgs.size() - 1);
      for (uint32_t c = 0; c < cols.size(); ++c) plan_column(c, rg, r0, r1, data[c], tasks);
    }

    const int64_t N = (int64_t)tasks.size();
    const int k = (int)std::min<int64_t>(threads, N);
    if (k <= 1) {
      Slot s;
      std::vector<const char*> ptrs(kVector);
      std::vector<int32_t> lens(kVector);
      for (int64_t i = 0; i < N; ++i) {
        encode(tasks[(size_t)i], data, s, ctx(0), ptrs, lens);
        put_page(tasks[(size_t)i], s, base_row);
      }
    } else {
      const int64_t W = 2 * (int64_t)k;
      std::vector<Slot> slots((size_t)W);
      std::mutex m;
      std::condition_variable cv_main, cv_work;  // writer waits for encoded pages; workers wait for room
      int64_t next = 0, written = 0;
      bool abort = false;
      std::exception_ptr err;
      for (int i = 0; i < k; ++i) ctx(i);  // create contexts on this thread, before the workers start

      auto job = [&](int id) {
        std::vector<const char*> ptrs(kVector);
        std::vector<int32_t> lens(kVector);
        CodecContext& cc = *ctxs[(size_t)id];
        for (;;) {
          int64_t i;
          {
            std::unique_lock<std::mutex> lk(m);
            cv_work.wait(lk, [&] { return abort || next >= N || next < written + W; });
            if (abort || next >= N) return;
            i = next++;
          }
          Slot& s = slots[(size_t)(i % W)];
          try {
            encode(tasks[(size_t)i], data, s, cc, ptrs, lens);
          } catch (...) {
            std::lock_guard<std::mutex> g(m);
            if (!err) err = std::current_exception();
            abort = true;
            cv_main.notify_all();
            cv_work.notify_all();
            return;
          }
          {
            std::lock_guard<std::mutex> g(m);
            s.task = i;
            s.ready = true;
          }
          cv_main.notify_one();
        }
      };
      ThreadPool::instance().start(k, job);
      try {
        for (int64_t i = 0; i < N; ++i) {
          Slot& s = slots[(size_t)(i % W)];
          {
            std::unique_lock<std::mutex> lk(m);
            cv_main.wait(lk, [&] { return abort || (s.ready && s.task == i); });
            if (abort) break;
          }
          put_page(tasks[(size_t)i], s, base_row);
          {
            std::lock_guard<std::mutex> g(m);
            s.ready = false;
            ++written;
          }
          cv_work.notify_one();
        }
      } catch (...) {
        std::lock_guard<std::mutex> g(m);
        if (!err) err = std::current_exception();
        abort = true;
        cv_main.notify_all();
        cv_work.notify_all();
      }
      ThreadPool::instance().wait();
      if (err) std::rethrow_exception(err);
    }
    sch.clear();
    sch.shrink_to_fit();
    total_rows += n;
  }

  void close() {
    namespace fb = fstx::fb;
    flatbuffers::FlatBufferBuilder b(1 << 16);
    std::vector<flatbuffers::Offset<fb::Column>> cvec;
    for (const ColDef& c : cols) {
      auto name = b.CreateString(c.name);
      flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>> levels = 0;
      if (c.logical == FSTX_LT_FACTOR) {
        std::vector<flatbuffers::Offset<flatbuffers::String>> lv;
        lv.reserve(c.levels.size());
        for (const std::string& s : c.levels) lv.push_back(b.CreateString(s));
        levels = b.CreateVector(lv);
      }
      flatbuffers::Offset<flatbuffers::String> tz = 0;
      if (c.has_tz) tz = b.CreateString(c.tz);
      flatbuffers::Offset<flatbuffers::Vector<uint8_t>> attrs = 0;
      if (!c.attrs.empty()) attrs = b.CreateVector(c.attrs);
      cvec.push_back(fb::CreateColumn(b, name, (fb::PhysType)c.phys, (fb::LogicalType)c.logical, c.ordered != 0,
                                      levels, tz, attrs));
    }
    auto columns = b.CreateVector(cvec);
    std::vector<flatbuffers::Offset<fb::RowGroup>> gvec;
    std::vector<fb::Page> pv;
    for (const RowGroupMeta& g : rgs) {
      std::vector<flatbuffers::Offset<fb::ColumnChunk>> chv;
      for (const auto& ch : g.chunks) {
        const size_t c = chv.size();
        pv.clear();
        for (const PageMeta& p : ch)
          pv.emplace_back(p.offset, p.first_row, p.hash, p.mn, p.mx, p.comp_len, p.raw_len, p.row_count,
                          p.null_count, p.chain, (fb::Codec)p.codec, p.flags);
        auto pages = b.CreateVectorOfStructs(pv);
        if (g.has_dict[c]) {
          const PageMeta& p = g.dicts[c];
          fb::Page dp(p.offset, p.first_row, p.hash, p.mn, p.mx, p.comp_len, p.raw_len, p.row_count, p.null_count,
                      p.chain, (fb::Codec)p.codec, p.flags);
          chv.push_back(fb::CreateColumnChunk(b, pages, &dp));
        } else {
          chv.push_back(fb::CreateColumnChunk(b, pages));
        }
      }
      gvec.push_back(fb::CreateRowGroup(b, g.first_row, g.row_count, b.CreateVector(chv)));
    }
    auto groups = b.CreateVector(gvec);
    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> tattrs = 0;
    if (!table_attrs.empty()) tattrs = b.CreateVector(table_attrs);
    auto created = b.CreateString(std::string("fstx ") + kVersion);
    if (!forge) name_features();
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>> fnames = 0;
    if (!feature_names.empty()) {
      std::vector<flatbuffers::Offset<flatbuffers::String>> v;
      for (const std::string& x : feature_names) v.push_back(b.CreateString(x));
      fnames = b.CreateVector(v);
    }
    auto footer = fb::CreateFooter(b, (uint16_t)version, total_rows, rg_size, columns, groups, tattrs, created,
                                   (fb::Profile)(stored_profile >= 0 ? stored_profile : profile), req_features,
                                   opt_features, fnames);
    fb::FinishFooterBuffer(b, footer);

    out.pad_to(align_up(out.pos()));
    uint64_t foff = out.pos();
    uint64_t flen = b.GetSize();
    out.write(b.GetBufferPointer(), (size_t)flen);
    uint8_t tail[kTailSize];
    put_u64(tail, foff);
    put_u64(tail + 8, flen);
    put_u64(tail + 16, hash64(b.GetBufferPointer(), (size_t)flen));
    put_u32(tail + 24, version);
    std::memcpy(tail + 28, "FSTX", 4);
    out.write(tail, kTailSize);
    if (appending) {  // the new table is on disk: only now drop the committed end from the header
      if (test_crash == 1) {  // tests: stop as a killed process would, after the new tail
        out.close();
        appending = false;
        return;
      }
      out.sync();
      out.write_at(0, hdr0, kHeaderSize);
      out.sync();
      appending = false;
    }
    out.close();
  }

  // ---- M5 compaction: copies every page (verified, not re-encoded) in file order to a fresh file
  void copy_page(const fstx_file& F, uint32_t c, const fb::Page* p, PageMeta& m) {
    const uint8_t* src = page_src(F, c, p, true);
    out.pad_to(align_up(out.pos()));
    m = meta_of(p);
    m.offset = out.pos();
    if (p->comp_len()) out.write(src, p->comp_len());
    F.map.release(p->offset(), p->comp_len());
  }

  void compact_from(const fstx_file& F, const std::string& tmp) {
    for (uint32_t c = 0; c < F.n_cols; ++c) {
      const fb::Column* x = F.col(c);
      ColDef d;
      d.name = F.col_name(c);
      d.phys = (int)x->phys();
      d.logical = (int)x->logical();
      d.ordered = x->ordered() ? 1 : 0;
      if (x->levels())
        for (const auto* l : *x->levels()) d.levels.push_back(l->str());
      if (x->tzone()) {
        d.has_tz = true;
        d.tz = x->tzone()->str();
      }
      if (x->attrs()) d.attrs.assign(x->attrs()->begin(), x->attrs()->end());
      cols.push_back(std::move(d));
    }
    if (const auto* a = F.footer->table_attrs()) table_attrs.assign(a->begin(), a->end());
    keep_features(F);
    if (forge) {
      if (forge->version >= 0) version = (uint32_t)forge->version;
      req_features = forge->req;
      opt_features = forge->opt;
      feature_names = forge->names;
      if (forge->profile >= 0) stored_profile = forge->profile;
      const bool page0 = forge->codec0 >= 0 || forge->kind0 >= 0 || forge->na0 >= 0 || forge->tag0 >= 0;
      if (cols.empty() && (page0 || forge->phys0 >= 0 || forge->logical0 >= 0))
        fail(FSTX_ERR_ARG, "cannot forge column 0: the file has no columns");
      if (forge->phys0 >= 0) cols[0].phys = forge->phys0;
      if (forge->logical0 >= 0) cols[0].logical = forge->logical0;
    }
    open(tmp);
    const auto* g = F.footer->row_groups();
    const uint32_t nrg = g ? g->size() : 0;
    for (uint32_t i = 0; i < nrg; ++i) {
      const fb::RowGroup* rg = g->Get(i);
      RowGroupMeta m;
      m.first_row = rg->first_row();
      m.row_count = rg->row_count();
      m.chunks.resize(cols.size());
      m.dicts.resize(cols.size());
      m.has_dict.assign(cols.size(), 0);
      for (uint32_t c = 0; c < cols.size(); ++c) {
        const fb::ColumnChunk* ch = rg->columns()->Get(c);
        if (ch->dictionary()) {
          copy_page(F, c, ch->dictionary(), m.dicts[c]);
          m.has_dict[c] = 1;
        }
        for (const fb::Page* p : *ch->pages()) {
          m.chunks[c].emplace_back();
          copy_page(F, c, p, m.chunks[c].back());
        }
      }
      rgs.push_back(std::move(m));
    }
    total_rows = F.n_rows;
    rg_size = F.footer->row_group_size();
    if (F.profile <= enc::P_SMALL) profile = F.profile;
    if (forge && !rgs.empty() && (forge->codec0 >= 0 || forge->kind0 >= 0 || forge->na0 >= 0 || forge->tag0 >= 0)) {
      // the first page of column 0 (a file may have row groups but no columns, or an empty first chunk)
      if (rgs[0].chunks.empty() || rgs[0].chunks[0].empty())
        fail(FSTX_ERR_ARG, "cannot forge the first page of column 0: the file has none");
      PageMeta& p = rgs[0].chunks[0][0];
      if (forge->codec0 >= 0) p.codec = (uint8_t)forge->codec0;
      if (forge->kind0 >= 0) p.chain = (p.chain & ~15u) | (uint32_t)forge->kind0;
      if (forge->na0 >= 0) p.chain = (p.chain & ~0xf0u) | (uint32_t)forge->na0 << 4;
      if (forge->tag0 >= 0) p.chain = (p.chain & ~0xff00u) | (uint32_t)forge->tag0 << 8;
    }
    close();
  }
};

}  // namespace fstx

struct fstx_writer {
  fstx::WriterImpl impl;
};

using fstx::Error;

static void configure(fstx::WriterImpl& W, const fstx_write_column* cols, uint32_t n_cols, const uint8_t* table_attrs,
                      size_t table_attrs_len, const fstx_write_options* opt) {
  {
    if (!fstx::codec_supported(opt->codec)) fstx::fail(FSTX_ERR_ARG, "unknown codec %d", opt->codec);
    W.codec = opt->codec;
    W.level = opt->level;
    if (W.codec == FSTX_CODEC_ZSTD && (W.level < fstx::zstd_level_min() || W.level > fstx::zstd_level_max()))
      fstx::fail(FSTX_ERR_ARG, "ZSTD level must be between %d and %d", fstx::zstd_level_min(), fstx::zstd_level_max());
    W.threads = fstx::resolve_threads(opt->threads);
    if (opt->row_group_size) {
      if (opt->row_group_size % fstx::kVector != 0 || opt->row_group_size > (1ull << 31))
        fstx::fail(FSTX_ERR_ARG, "row group size must be a multiple of 1024 and at most 2^31");
      W.rg_size = opt->row_group_size;
    }
    if (opt->profile < fstx::enc::P_FAST || opt->profile > fstx::enc::P_SMALL)
      fstx::fail(FSTX_ERR_ARG, "unknown profile %d", opt->profile);
    W.profile = opt->profile;
    if (opt->keep_pct < 0 || opt->keep_pct > 90) fstx::fail(FSTX_ERR_ARG, "keep_pct must be between 0 and 90");
    W.keep_pct = opt->keep_pct;
    if (opt->force_encoding) {
      namespace e = fstx::enc;
      const int f = opt->force_encoding & 0xff, fl = opt->force_encoding & ~0xff;
      if (fl & ~(e::F_STR_DICT | e::F_DICT_FSST | e::F_KEEP_CODEC)) fstx::fail(FSTX_ERR_ARG, "unknown encoding flags");
      if (f == e::T_FSST) W.force_str = f;
      else if (f == e::T_DICT || f == e::T_PLAIN) W.force_num = W.force_str = f;
      else if (f >= 1 && f <= e::kMaxTag) W.force_num = f;
      else if (f != 0) fstx::fail(FSTX_ERR_ARG, "unknown encoding %d", f);
      if (fl & (e::F_STR_DICT | e::F_DICT_FSST)) {
        W.force_str = e::T_DICT | (fl & e::F_DICT_FSST);
        W.force_codes = W.force_num;
      }
      W.keep_codec = (fl & e::F_KEEP_CODEC) != 0;
    }
    if (opt->page_bytes) {
      if (opt->page_bytes < 4096 || opt->page_bytes > (1u << 28)) fstx::fail(FSTX_ERR_ARG, "page size out of range");
      W.page_bytes = opt->page_bytes;
    }
    if (n_cols > fstx::kMaxColumns) fstx::fail(FSTX_ERR_ARG, "too many columns (the format allows %u)", fstx::kMaxColumns);
    auto text_ok = [](const std::string& x) {
      return fstx::valid_utf8(x.data(), x.size()) && x.find('\0') == std::string::npos;
    };
    for (uint32_t c = 0; c < n_cols; ++c) {
      const fstx_write_column& s = cols[c];
      fstx::ColDef d;
      d.name.assign(s.name ? s.name : "", s.name ? s.name_len : 0);
      if (!text_ok(d.name)) fstx::fail(FSTX_ERR_ARG, "column %u: its name is not valid UTF-8 text", c + 1);
      if (s.phys < FSTX_INT32 || s.phys > FSTX_STRING) fstx::fail(FSTX_ERR_ARG, "column %u: unknown type", c);
      if (s.logical < FSTX_LT_NONE || s.logical > FSTX_LT_POSIXCT) fstx::fail(FSTX_ERR_ARG, "column %u: unknown logical type", c);
      d.phys = s.phys;
      d.logical = s.logical;
      d.ordered = s.ordered;
      if (s.logical == FSTX_LT_FACTOR) {
        if (s.phys != FSTX_INT32) fstx::fail(FSTX_ERR_ARG, "column %u: a factor must have integer codes", c);
        d.levels.reserve(s.n_levels);
        for (uint32_t i = 0; i < s.n_levels; ++i) d.levels.emplace_back(s.levels[i], s.level_lens[i]);
      }
      if (s.has_tzone) {
        d.has_tz = true;
        d.tz.assign(s.tzone ? s.tzone : "", s.tzone ? s.tzone_len : 0);
        if (!text_ok(d.tz)) fstx::fail(FSTX_ERR_ARG, "column %u: its time zone is not valid UTF-8 text", c + 1);
      }
      if (s.attrs && s.attrs_len) d.attrs.assign(s.attrs, s.attrs + s.attrs_len);
      W.cols.push_back(std::move(d));
    }
    if (table_attrs && table_attrs_len) W.table_attrs.assign(table_attrs, table_attrs + table_attrs_len);
    W.test_crash = opt->test_crash;
    W.req_features |= fstx::codec_feature_bit(W.codec);
  }
}

static int writer_open(const char* path, const fstx_write_column* cols, uint32_t n_cols, const uint8_t* table_attrs,
                       size_t table_attrs_len, const fstx_write_options* opt, bool append, fstx_writer** out,
                       fstx_error* err) {
  fstx::set_error(err, FSTX_OK, "");
  *out = nullptr;
  fstx_writer* w = nullptr;
  try {
    if (!fstx::host_is_little_endian()) fstx::fail(FSTX_ERR_FORMAT, "big-endian machines are not supported yet");
    if (!path || !opt) fstx::fail(FSTX_ERR_ARG, "missing path or options");
    if (n_cols > 0 && !cols) fstx::fail(FSTX_ERR_ARG, "missing column definitions");
    w = new fstx_writer();
    fstx::WriterImpl& W = w->impl;
    configure(W, cols, n_cols, table_attrs, table_attrs_len, opt);
    if (!append) {
      W.open(path);
    } else {
      uint8_t hdr[fstx::kHeaderSize];
      uint64_t end;
      {
        fstx_file F;  // mapped only while its directory is copied
        fstx::open_file(F, path);
        W.check_same_columns(F);
        W.load_row_groups(F);
        std::memcpy(hdr, F.map.data(), fstx::kHeaderSize);
        end = F.size;
      }
      W.begin_append(path, hdr, end);
    }
    *out = w;
    return FSTX_OK;
  } catch (const Error& e) {
    fstx::set_error(err, e.code, e.msg.c_str(), e.column);
  } catch (const std::bad_alloc&) {
    fstx::set_error(err, FSTX_ERR_MEMORY, "out of memory");
  } catch (const std::exception& e) {
    fstx::set_error(err, FSTX_ERR_INTERNAL, e.what());
  } catch (...) {
    fstx::set_error(err, FSTX_ERR_INTERNAL, "unknown error");
  }
  delete w;  // OutFile destructor deletes a partly created file; an append is rolled back
  return err ? err->code : FSTX_ERR_INTERNAL;
}

extern "C" int fstx_writer_open(const char* path, const fstx_write_column* cols, uint32_t n_cols,
                                const uint8_t* table_attrs, size_t table_attrs_len, const fstx_write_options* opt,
                                fstx_writer** out, fstx_error* err) {
  return writer_open(path, cols, n_cols, table_attrs, table_attrs_len, opt, false, out, err);
}

extern "C" int fstx_writer_open_append(const char* path, const fstx_write_column* cols, uint32_t n_cols,
                                       const uint8_t* table_attrs, size_t table_attrs_len,
                                       const fstx_write_options* opt, fstx_writer** out, fstx_error* err) {
  return writer_open(path, cols, n_cols, table_attrs, table_attrs_len, opt, true, out, err);
}

extern "C" int fstx_compact_copy(const char* path, const char* tmp, fstx_error* err) {
  fstx::set_error(err, FSTX_OK, "");
  try {
    if (!path || !tmp) fstx::fail(FSTX_ERR_ARG, "missing path");
    if (fstx::registry_has(fstx::file_id_of_path(path)))
      fstx::fail(FSTX_ERR_IO,
                 "cannot compact '%s': it is open in this R session, memory-mapped by a lazy read_fstx() result that "
                 "is still alive. Remove those objects (rm()) first",
                 path);
    fstx_file F;
    fstx::open_file(F, path);
    fstx::WriterImpl W;
    W.compact_from(F, tmp);
    return FSTX_OK;
  } catch (...) {
    return fstx::to_error(err);
  }
}

void fstx::forge_copy(const std::string& src, const std::string& dst, const Forge& f) {
  fstx_file F;
  fstx::open_file(F, src);
  fstx::WriterImpl W;
  W.forge = &f;
  W.compact_from(F, dst);
}

extern "C" int fstx_file_sizes(const char* path, uint64_t* committed, uint64_t* physical, uint64_t* valid_end,
                               fstx_error* err) {
  fstx::set_error(err, FSTX_OK, "");
  try {
    if (!path) fstx::fail(FSTX_ERR_ARG, "missing path");
    fstx_file F;
    fstx::open_file(F, path);
    *committed = F.size;
    *physical = F.map.size();
    *valid_end = fstx::get_u64(F.map.data() + 16);
    return FSTX_OK;
  } catch (...) {
    return fstx::to_error(err);
  }
}

extern "C" int fstx_writer_append(fstx_writer* w, const fstx_column_data* cols, uint64_t n_rows, fstx_error* err) {
  fstx::set_error(err, FSTX_OK, "");
  try {
    if (!w) fstx::fail(FSTX_ERR_ARG, "writer is closed");
    if (!w->impl.cols.empty() && !cols) fstx::fail(FSTX_ERR_ARG, "missing column data");
    w->impl.append(cols, n_rows);
    return FSTX_OK;
  } catch (const Error& e) {
    fstx::set_error(err, e.code, e.msg.c_str(), e.column);
  } catch (const std::bad_alloc&) {
    fstx::set_error(err, FSTX_ERR_MEMORY, "out of memory while writing");
  } catch (const std::exception& e) {
    fstx::set_error(err, FSTX_ERR_INTERNAL, e.what());
  } catch (...) {
    fstx::set_error(err, FSTX_ERR_INTERNAL, "unknown error");
  }
  return err ? err->code : FSTX_ERR_INTERNAL;
}

extern "C" int fstx_writer_close(fstx_writer* w, fstx_error* err) {
  fstx::set_error(err, FSTX_OK, "");
  int rc = FSTX_OK;
  try {
    if (!w) fstx::fail(FSTX_ERR_ARG, "writer is closed");
    w->impl.close();
  } catch (const Error& e) {
    fstx::set_error(err, e.code, e.msg.c_str(), e.column);
    rc = e.code;
  } catch (const std::bad_alloc&) {
    fstx::set_error(err, FSTX_ERR_MEMORY, "out of memory while writing the footer");
    rc = FSTX_ERR_MEMORY;
  } catch (const std::exception& e) {
    fstx::set_error(err, FSTX_ERR_INTERNAL, e.what());
    rc = FSTX_ERR_INTERNAL;
  } catch (...) {
    fstx::set_error(err, FSTX_ERR_INTERNAL, "unknown error");
    rc = FSTX_ERR_INTERNAL;
  }
  delete w;  // on failure the destructor removes the partial file
  return rc;
}

extern "C" void fstx_writer_abort(fstx_writer* w) {
  if (!w) return;
  if (w->impl.appending)
    w->impl.rollback();
  else
    w->impl.out.close_and_delete();
  delete w;
}
