// FST-X core engine: open (header, tail, footer checks), plan (which pages a request needs) and read
// (parallel decode straight into host memory; strings, string dictionaries and dictionary codes handed to
// the host on the calling thread). File format 3 (encoded pages, enc.h). No R headers.
#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <unordered_map>

#include "core_file.h"
#include "vendor/zstd/zstd.h"

namespace fb = fstx::fb;

namespace {
struct ReadTask {
  uint32_t out;
  uint32_t col;
  const fb::Page* page;
  uint32_t skip;  // rows to skip at the start of the page
  uint32_t take;  // rows to deliver
  uint64_t dest;  // first output row
  uint8_t what;   // 0 = data page, 1 = a string dictionary page
  uint32_t dict_rows;  // code pages: entries in the chunk's dictionary (0 = the chunk has none)
  uint64_t e0 = 0, e1 = 0;  // rows plans: entries [e0, e1) of the sorted row list fall in this page
  int32_t dict = -1;        // rows plans, code pages: index of the chunk's dictionary in fstx_plan::dicts
};
struct OutCol {
  uint32_t col;
  int phys;
  int logical;
  uint32_t n_levels;
};
}  // namespace

struct fstx_plan {
  const fstx_file* f = nullptr;
  uint64_t from = 0, to = 0;
  std::vector<OutCol> outs;
  std::vector<ReadTask> fixed, strings;  // strings are kept in delivery order
  bool has_dict = false;
  // rows plans (M5): scattered rows, any order, repeats allowed
  bool rows_mode = false;
  std::vector<uint64_t> rows;  // requested rows, sorted ascending (0-based)
  std::vector<uint64_t> pos;   // output position of each sorted entry; empty = rows were already sorted
  std::vector<const fb::Page*> dicts;  // string dictionaries the code pages need (decoded once each)
  std::vector<uint32_t> dict_cols;
  std::vector<uint8_t> codes_out;  // rows plans, per output column: dictionary pages deliver codes (fstx_plan_rows_codes)
  uint64_t out_rows() const { return rows_mode ? (uint64_t)rows.size() : to - from; }
};

namespace fstx {

const char* feature_name(int bit) {
  switch (bit) {
    case kFeatInt64: return "int64";
    case kFeatBinary: return "binary";
    case kFeatZstdDict: return "zstd_dict";
    case kFeatPcodec: return "pcodec";
    case kFeatExperimental: return "experimental";
    default: return nullptr;
  }
}

uint64_t features_known() { return codec_feature_bits(); }

namespace {
// A number this reader does not know, in a header or footer whose checksum is valid: a newer fstx wrote it.
[[noreturn]]
#if defined(__MINGW32__)
__attribute__((format(gnu_printf, 2, 3)))
#elif defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void refuse_newer(const std::string& path, const char* fmt, ...) {
  char what[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(what, sizeof(what), fmt, ap);
  va_end(ap);
  fail(FSTX_ERR_FORMAT, "'%s' uses %s, which this fstx (%s) cannot read. Update fstx.", path.c_str(), what, kVersion);
}

uint32_t count_bits(uint64_t x) {
  uint32_t n = 0;
  for (; x; x &= x - 1) ++n;
  return n;
}

// A feature bit the reader lacks: named from the file's own list when it has one, else from the reserved names.
[[noreturn]] void refuse_feature(const std::string& path, int bit, const fb::Footer* ft) {
  std::string nm;
  if (ft && ft->feature_names()) {
    const uint64_t req = ft->required_features(), opt = ft->optional_features();
    const auto* names = ft->feature_names();
    const uint32_t i = count_bits(req & ((1ull << bit) - 1));
    if (names->size() == count_bits(req) + count_bits(opt) && i < names->size()) {
      const auto* s = names->Get(i);
      for (uint32_t k = 0; k < s->size() && k < 64; ++k) {
        const char ch = s->c_str()[k];
        nm += (ch >= 0x20 && ch < 0x7f && ch != '\'') ? ch : '?';
      }
    }
  }
  if (nm.empty() && feature_name(bit)) nm = feature_name(bit);
  if (nm.empty()) refuse_newer(path, "the file feature bit %d", bit);
  refuse_newer(path, "the file feature '%s' (bit %d)", nm.c_str(), bit);
}
}  // namespace

void open_file(fstx_file& F, const std::string& path) {
  F.path = path;
  F.map.open(path);
  uint64_t size = F.map.size();
  if (size == 0) fail(FSTX_ERR_FORMAT, "'%s' is empty (0 bytes), not an FSTX file", path.c_str());
  if (size < kDataStart + kTailSize)
    fail(FSTX_ERR_FORMAT, "'%s' is not an FSTX file (only %llu bytes)", path.c_str(), (unsigned long long)size);
  const uint8_t* d = F.map.data();
  if (std::memcmp(d, "FSTX", 4) != 0) fail(FSTX_ERR_FORMAT, "'%s' is not an FSTX file (wrong magic bytes)", path.c_str());
  const uint32_t em = get_u32(d + 8);
  if (em != kEndianMark) {
    if (em == 0x04030201u) fail(FSTX_ERR_FORMAT, "'%s' was written with the other byte order (endianness)", path.c_str());
    fail(FSTX_ERR_CORRUPT, "'%s' has a corrupt header (endian mark)", path.c_str());
  }
  if (get_u64(d + 24) != hash64(d, 24)) fail(FSTX_ERR_CHECKSUM, "'%s' has a corrupt header (checksum mismatch)", path.c_str());
  F.version = get_u16(d + 4);
  // Header bytes 16..23 (reserved, zero before M5): the committed end of the file while an append is in
  // progress or was interrupted. The table then ends there and anything after it is ignored.
  const uint64_t valid_end = get_u64(d + 16);
  if (valid_end) {
    if (valid_end > size || valid_end < kDataStart + kTailSize)
      fail(FSTX_ERR_CORRUPT, "'%s' has a corrupt header (committed length)", path.c_str());
    size = valid_end;
  }
  F.size = size;
  if (F.version == 0) fail(FSTX_ERR_CORRUPT, "'%s' has a corrupt header (format version 0)", path.c_str());
  if (F.version < kFormatVersion)
    fail(FSTX_ERR_FORMAT,
         "'%s' was written by fstx 0.x (file format %u); not supported by fstx 1.0 or later, which read file "
         "format %u only. Read it with the fstx 0.x that wrote it and write it again.",
         path.c_str(), F.version, kFormatVersion);
  if (F.version > kFormatVersion)
    fail(FSTX_ERR_FORMAT, "'%s' uses FSTX format version %u; this reader supports up to %u. Update fstx.",
         path.c_str(), F.version, kFormatVersion);
  // Header bytes 6..7: required feature bits 0..15, checked before the footer is read.
  const uint16_t hflags = get_u16(d + 6);
  for (int b = 0; b < 16; ++b)
    if ((hflags >> b & 1u) && !(features_known() >> b & 1u)) refuse_feature(path, b, nullptr);

  const uint8_t* tail = d + size - kTailSize;
  if (std::memcmp(tail + 28, "FSTX", 4) != 0)
    fail(FSTX_ERR_CORRUPT, "'%s' is truncated or incomplete (end marker missing)", path.c_str());
  if (get_u32(tail + 24) != F.version)
    fail(FSTX_ERR_CORRUPT, "'%s' has a corrupt tail (format version)", path.c_str());
  const uint64_t foff = get_u64(tail), flen = get_u64(tail + 8);
  if (foff < kDataStart || foff % 8 != 0 || foff > size - kTailSize || flen != size - kTailSize - foff || flen < 8 ||
      flen >= 0x7fffffffULL)
    fail(FSTX_ERR_CORRUPT, "'%s' is corrupt or truncated (footer location is invalid)", path.c_str());
  if (hash64(d + foff, (size_t)flen) != get_u64(tail + 16))
    fail(FSTX_ERR_CHECKSUM, "'%s' is corrupt (footer checksum mismatch)", path.c_str());
  flatbuffers::Verifier::Options o;
  o.max_depth = 16;
  o.max_tables = 0x7fffffff;
  o.check_alignment = true;
  flatbuffers::Verifier v(d + foff, (size_t)flen, o);
  if (!fb::VerifyFooterBuffer(v)) fail(FSTX_ERR_CORRUPT, "'%s' is corrupt (footer structure is invalid)", path.c_str());
  F.footer = fb::GetFooter(d + foff);
  F.footer_off = foff;
  const fb::Footer* ft = F.footer;
  if (ft->format_version() != F.version) fail(FSTX_ERR_CORRUPT, "'%s': header and footer versions differ", path.c_str());
  // ---- feature bits: unknown required bits are refused, unknown optional bits ignored
  const uint64_t req = ft->required_features();
  if ((req & kFeatHeaderMask) != hflags) fail(FSTX_ERR_CORRUPT, "'%s': header and footer feature bits differ", path.c_str());
  for (int b = 0; b < 64; ++b)
    if ((req >> b & 1u) && !(features_known() >> b & 1u)) refuse_feature(path, b, ft);
  F.profile = (int)ft->profile();
  // a profile above 2 came from a newer fstx; it is informational, so it is read as "unknown"
  const uint64_t rg_size = ft->row_group_size();
  if (rg_size == 0 || rg_size > kMaxRowGroup) fail(FSTX_ERR_CORRUPT, "'%s' has an invalid row group size", path.c_str());

  // ---- schema
  const auto* cols = ft->columns();
  F.n_cols = cols ? cols->size() : 0;
  if (F.n_cols > kMaxColumns) fail(FSTX_ERR_CORRUPT, "'%s' has more columns than the format allows", path.c_str());
  auto text_ok = [](const flatbuffers::String* s) {
    return !s || (valid_utf8(s->c_str(), s->size()) && std::memchr(s->c_str(), 0, s->size()) == nullptr);
  };
  for (uint32_t c = 0; c < F.n_cols; ++c) {
    const fb::Column* x = cols->Get(c);
    int phys = (int)x->phys(), lt = (int)x->logical();
    if (!text_ok(x->name()) || !text_ok(x->tzone()))
      fail(FSTX_ERR_CORRUPT, "column %u: its name or time zone is not valid UTF-8 text", c + 1);
    if (phys == 4) refuse_newer(path, "64-bit integer columns (column '%s', type 4)", F.col_name(c).c_str());
    if (phys == 5) refuse_newer(path, "binary columns (column '%s', type 5)", F.col_name(c).c_str());
    if (phys > FSTX_STRING) refuse_newer(path, "column type %d (column '%s')", phys, F.col_name(c).c_str());
    if (lt > FSTX_LT_POSIXCT) refuse_newer(path, "logical type %d (column '%s')", lt, F.col_name(c).c_str());
    bool ok = true;
    switch (lt) {
      case FSTX_LT_NONE: break;
      case FSTX_LT_FACTOR: ok = phys == FSTX_INT32 && x->levels() != nullptr; break;
      case FSTX_LT_DATE: ok = phys == FSTX_INT32 || phys == FSTX_FLOAT64; break;
      case FSTX_LT_POSIXCT: ok = phys == FSTX_FLOAT64; break;
      default: ok = false;
    }
    if (!ok) fail(FSTX_ERR_CORRUPT, "column %u has an invalid type combination", c);
  }
  F.n_rows = ft->row_count();
  if (F.n_rows > (1ull << 62)) fail(FSTX_ERR_CORRUPT, "row count out of range");

  // ---- page directory: contiguous rows, pages inside the data area, sizes consistent with types
  F.sums.assign(F.n_cols, ColSummary());
  const auto* rgs = ft->row_groups();
  const uint32_t nrg = rgs ? rgs->size() : 0;
  uint64_t expect = 0;
  for (uint32_t g = 0; g < nrg; ++g) {
    const fb::RowGroup* rg = rgs->Get(g);
    if (rg->first_row() != expect || rg->row_count() == 0 || rg->row_count() > F.n_rows - expect ||
        rg->row_count() > rg_size)
      fail(FSTX_ERR_CORRUPT, "row group %u has inconsistent row numbers", g);
    const auto* chunks = rg->columns();
    if ((chunks ? chunks->size() : 0) != F.n_cols) fail(FSTX_ERR_CORRUPT, "row group %u has the wrong number of columns", g);
    for (uint32_t c = 0; c < F.n_cols; ++c) {
      const int phys = (int)cols->Get(c)->phys();
      const fb::ColumnChunk* ch = chunks->Get(c);
      const auto* pages = ch->pages();
      if (!pages || pages->size() == 0) fail(FSTX_ERR_CORRUPT, "row group %u, column %u has no pages", g, c);
      // the writer aligns every page vector; the verifier checks only its 4-byte length, and a Page has 8-byte fields
      if ((reinterpret_cast<const uint8_t*>(pages->Data()) - (d + foff)) % alignof(fb::Page) != 0)
        fail(FSTX_ERR_CORRUPT, "row group %u, column %u: the page directory is misaligned", g, c);
      // a page is encoded (enc.h): the body is a whole number of 8-byte words, the chain names a known
      // encoding and the kind the column type implies
      auto page_ok = [&](const fb::Page* p, int kind) {
        const uint64_t cl = p->comp_len(), rl = p->raw_len();
        const uint32_t ch2 = p->chain();
        const int top = enc::chain_tag(ch2, 0);
        // numbers reserved for later versions first, so a newer file gets "Update fstx", not "corrupt"
        const int codec = (int)p->codec();
        if (!codec_supported(codec)) {
          const char* cn = codec_name(codec);
          if (cn) refuse_newer(path, "the codec '%s' (codec %d, column '%s')", cn, codec, F.col_name(c).c_str());
          if (codec >= kCodecExperimental)
            refuse_newer(path, "the experimental codec %d (column '%s'); codecs 128-255 are never written by a release",
                         codec, F.col_name(c).c_str());
          refuse_newer(path, "codec %d (column '%s')", codec, F.col_name(c).c_str());
        }
        if (enc::chain_kind(ch2) > enc::K_DICT)
          refuse_newer(path, "page kind %d (column '%s')", enc::chain_kind(ch2), F.col_name(c).c_str());
        if (enc::chain_na(ch2) > enc::NA_RUNS)
          refuse_newer(path, "missing-value mode %d (column '%s')", enc::chain_na(ch2), F.col_name(c).c_str());
        for (int i = 0; i < 3; ++i)
          if (enc::chain_tag(ch2, i) > enc::kMaxTag)
            refuse_newer(path, "encoding %d (column '%s')", enc::chain_tag(ch2, i), F.col_name(c).c_str());
        bool ok = p->row_count() > 0 && p->offset() >= kDataStart && p->offset() % kAlign == 0 && cl <= foff &&
                  p->offset() <= foff - cl && cl <= kMaxRawPage && rl <= kMaxRawPage && rl >= 8 &&
                  rl % 8 == 0 && enc::chain_kind(ch2) == kind && enc::chain_na(ch2) <= enc::NA_RUNS && top >= 1 &&
                  top <= enc::kMaxTag && p->null_count() <= p->row_count();
        if (ok && p->codec() == fb::Codec::None) ok = cl == rl;
        // a raw size the codec cannot reach from cl bytes is refused before anything is allocated for it: LZ4
        // expands at most 255x; zstd at most one 128 KB block per 4-byte (RLE) block
        if (ok && p->codec() == fb::Codec::LZ4) ok = rl <= cl * 255 + 64;
        if (ok && p->codec() == fb::Codec::ZSTD) ok = rl <= cl * 32768 + 131072;
        // fixed-width values (enc.h): chains nest at most 3 deep and each level at most doubles the values, so
        // 64 x the plain size plus 64 KB of block headers is far above any page the writer can make
        if (ok && kind != enc::K_STR && kind != enc::K_DICT)
          ok = rl <= (uint64_t)p->row_count() * (kind == enc::K_DBL ? 8u : 4u) * 64u + 65536u;
        return ok;
      };
      const fb::Page* dp = ch->dictionary();
      if (dp) {
        if (phys != FSTX_STRING || !page_ok(dp, enc::K_DICT) || dp->null_count() != 0 || dp->row_count() > kMaxDictEntries)
          fail(FSTX_ERR_CORRUPT, "the dictionary of column %u, row group %u is invalid", c, g);
        F.sums[c].comp += dp->comp_len();
        F.sums[c].raw += dp->raw_len();
        F.sums[c].pages += 1;
        ++F.n_pages;
      }
      const int kind = phys == FSTX_STRING && dp ? enc::K_CODES : kind_of(phys);
      uint64_t pr = rg->first_row();
      for (uint32_t i = 0; i < pages->size(); ++i) {
        const fb::Page* p = pages->Get(i);
        if (p->first_row() != pr || !page_ok(p, kind))
          fail(FSTX_ERR_CORRUPT, "page directory entry %u of column %u, row group %u is invalid", i, c, g);
        pr += p->row_count();
        F.sums[c].comp += p->comp_len();
        F.sums[c].raw += p->raw_len();
        F.sums[c].pages += 1;
        ++F.n_pages;
      }
      if (pr != rg->first_row() + rg->row_count())
        fail(FSTX_ERR_CORRUPT, "row group %u, column %u: page rows do not add up", g, c);
    }
    expect += rg->row_count();
  }
  if (expect != F.n_rows) fail(FSTX_ERR_CORRUPT, "row groups do not add up to the row count");

  F.names.reserve(F.n_cols * 2);
  for (uint32_t c = 0; c < F.n_cols; ++c) F.names.emplace(F.col_name(c), c);  // first wins on duplicates
}

namespace {

void make_plan(fstx_plan& P, const fstx_file& F, const uint32_t* cols, uint32_t n, uint64_t from, uint64_t to) {
  if (from > to || to > F.n_rows) fail(FSTX_ERR_ARG, "row range is outside the table (%llu rows)", (unsigned long long)F.n_rows);
  P.f = &F;
  P.from = from;
  P.to = to;
  const auto* rgs = F.footer->row_groups();
  const uint32_t nrg = rgs ? rgs->size() : 0;
  for (uint32_t j = 0; j < n; ++j) {
    const uint32_t c = cols[j];
    if (c >= F.n_cols) fail(FSTX_ERR_ARG, "column index %u out of range", c);
    const fb::Column* x = F.col(c);
    OutCol oc{c, (int)x->phys(), (int)x->logical(), x->levels() ? x->levels()->size() : 0u};
    P.outs.push_back(oc);
    if (from == to) continue;
    // first row group that ends after `from`
    uint32_t lo = 0, hi = nrg;
    while (lo < hi) {
      uint32_t mid = (lo + hi) / 2;
      const fb::RowGroup* rg = rgs->Get(mid);
      if (rg->first_row() + rg->row_count() <= from) lo = mid + 1; else hi = mid;
    }
    for (uint32_t g = lo; g < nrg; ++g) {
      const fb::RowGroup* rg = rgs->Get(g);
      if (rg->first_row() >= to) break;
      const fb::ColumnChunk* ch = rg->columns()->Get(c);
      const auto* pages = ch->pages();
      const fb::Page* dp = ch->dictionary();
      bool dict_sent = false;
      uint32_t plo = 0, phi = pages->size();
      while (plo < phi) {
        uint32_t mid = (plo + phi) / 2;
        const fb::Page* p = pages->Get(mid);
        if (p->first_row() + p->row_count() <= from) plo = mid + 1; else phi = mid;
      }
      for (uint32_t i = plo; i < pages->size(); ++i) {
        const fb::Page* p = pages->Get(i);
        if (p->first_row() >= to) break;
        uint64_t s = std::max(from, p->first_row()), e = std::min(to, p->first_row() + p->row_count());
        if (dp && !dict_sent) {
          P.strings.push_back(ReadTask{j, c, dp, 0, dp->row_count(), s - from, 1, 0});
          P.has_dict = true;
          dict_sent = true;
        }
        ReadTask t{j, c, p, (uint32_t)(s - p->first_row()), (uint32_t)(e - s), s - from, 0, dp ? dp->row_count() : 0u};
        (oc.phys == FSTX_STRING ? P.strings : P.fixed).push_back(t);
      }
    }
  }
}

// Rows plan (M5): rows in any order (repeats allowed); tasks cover only the pages that hold requested rows.
void make_plan_rows(fstx_plan& P, const fstx_file& F, const uint32_t* cols, uint32_t n, const uint64_t* rows,
                    uint64_t nr) {
  P.f = &F;
  P.rows_mode = true;
  bool sorted = true;
  for (uint64_t i = 0; i < nr; ++i) {
    if (rows[i] >= F.n_rows)
      fail(FSTX_ERR_ARG, "row %llu is outside the table (%llu rows)", (unsigned long long)rows[i] + 1,
           (unsigned long long)F.n_rows);
    if (i && rows[i] < rows[i - 1]) sorted = false;
  }
  P.rows.assign(rows, rows + nr);
  if (!sorted) {
    P.pos.resize(nr);
    for (uint64_t i = 0; i < nr; ++i) P.pos[i] = i;
    std::stable_sort(P.pos.begin(), P.pos.end(), [&](uint64_t a, uint64_t b) { return rows[a] < rows[b]; });
    for (uint64_t i = 0; i < nr; ++i) P.rows[i] = rows[P.pos[i]];
  }
  const auto* rgs = F.footer->row_groups();
  const uint32_t nrg = rgs ? rgs->size() : 0;
  std::unordered_map<const fb::Page*, int32_t> dict_idx;
  for (uint32_t j = 0; j < n; ++j) {
    const uint32_t c = cols[j];
    if (c >= F.n_cols) fail(FSTX_ERR_ARG, "column index %u out of range", c);
    const fb::Column* x = F.col(c);
    OutCol oc{c, (int)x->phys(), (int)x->logical(), x->levels() ? x->levels()->size() : 0u};
    P.outs.push_back(oc);
    uint32_t g = 0;
    for (uint64_t e = 0; e < nr;) {
      const uint64_t r = P.rows[e];
      while (g < nrg && rgs->Get(g)->first_row() + rgs->Get(g)->row_count() <= r) ++g;
      if (g >= nrg) fail(FSTX_ERR_INTERNAL, "row %llu is in no row group", (unsigned long long)r + 1);
      const fb::ColumnChunk* ch = rgs->Get(g)->columns()->Get(c);
      const auto* pages = ch->pages();
      const fb::Page* dp = ch->dictionary();
      uint32_t lo = 0, hi = pages->size();
      while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const fb::Page* p = pages->Get(mid);
        if (p->first_row() + p->row_count() <= r) lo = mid + 1; else hi = mid;
      }
      if (lo >= pages->size()) fail(FSTX_ERR_INTERNAL, "row %llu is in no page", (unsigned long long)r + 1);
      const fb::Page* p = pages->Get(lo);
      const uint64_t pe = p->first_row() + p->row_count();
      const uint64_t e1 = (uint64_t)(std::lower_bound(P.rows.begin() + (ptrdiff_t)e, P.rows.end(), pe) - P.rows.begin());
      if (e1 - e > 0xffffffffull) fail(FSTX_ERR_ARG, "too many repeats of one row");
      ReadTask t{j, c, p, 0, (uint32_t)(e1 - e), e, 0, dp ? dp->row_count() : 0u};
      t.e0 = e;
      t.e1 = e1;
      if (dp && oc.phys == FSTX_STRING) {
        auto it = dict_idx.find(dp);
        if (it == dict_idx.end()) {
          it = dict_idx.emplace(dp, (int32_t)P.dicts.size()).first;
          P.dicts.push_back(dp);
          P.dict_cols.push_back(c);
        }
        t.dict = it->second;
      }
      (oc.phys == FSTX_STRING ? P.strings : P.fixed).push_back(t);
      e = e1;
    }
  }
}

}  // namespace

const uint8_t* page_src(const fstx_file& F, uint32_t col, const fb::Page* p, bool verify) {
  const uint8_t* src = F.map.data() + p->offset();
  FSTX_PROF_SCOPE(R_HASH);
  if (verify && hash64(src, p->comp_len()) != p->hash())
    fail(FSTX_ERR_CHECKSUM, "page checksum mismatch in column '%s' (rows %llu to %llu): the file is corrupt",
         F.col_name(col).c_str(), (unsigned long long)p->first_row() + 1,
         (unsigned long long)(p->first_row() + p->row_count()));
  return src;
}

const uint8_t* page_body(const fstx_file& F, uint32_t col, const fb::Page* p, bool verify, CodecContext& cc,
                         std::vector<uint8_t>& buf) {
  const uint8_t* src = page_src(F, col, p, verify);
  if (p->codec() == fb::Codec::None) return src;
  const size_t raw = p->raw_len();
  if (p->codec() == fb::Codec::ZSTD) {  // R6-1: every fstx writer stores the frame's content size; it must be raw_len
    const unsigned long long fs = ZSTD_getFrameContentSize(src, p->comp_len());
    if (fs != raw)
      fail(FSTX_ERR_CORRUPT, "column '%s' (rows %llu to %llu): the page size does not match its ZSTD frame: the file is "
           "corrupt", F.col_name(col).c_str(), (unsigned long long)p->first_row() + 1,
           (unsigned long long)(p->first_row() + p->row_count()));
  }
  if (buf.size() < raw) buf.resize(raw);
  FSTX_PROF_SCOPE(R_DECOMP);
  decompress_page(cc, (int)p->codec(), src, p->comp_len(), buf.data(), raw);
  return buf.data();
}

// As page_body, but the stored bytes are copied out of the file with a positional read (MappedFile::read_at)
// instead of being touched in the mapping: page faults on a mapped file are serialised per process, so a read of
// scattered rows that decodes many pages on many threads spends most of its time there. The body lands in buf (no
// codec) or the stored bytes in a per-thread buffer and the body in buf (codec). Used by rows plans and filters.
// The codec context is per thread too and outlives the call (a ZSTD context is some 100 KB to set up).
const uint8_t* page_body_read(const fstx_file& F, uint32_t col, const fb::Page* p, bool verify, CodecContext&,
                              std::vector<uint8_t>& buf) {
  static thread_local std::vector<uint8_t> stored;
  static thread_local CodecContext cc;
  const size_t cl = (size_t)p->comp_len();
  const bool none = p->codec() == fb::Codec::None;
  std::vector<uint8_t>& in = none ? buf : stored;
  if (in.size() < cl) in.resize(cl);
  F.map.read_at(p->offset(), in.data(), cl);
  {
    FSTX_PROF_SCOPE(R_HASH);
    if (verify && hash64(in.data(), cl) != p->hash())
      fail(FSTX_ERR_CHECKSUM, "page checksum mismatch in column '%s' (rows %llu to %llu): the file is corrupt",
           F.col_name(col).c_str(), (unsigned long long)p->first_row() + 1,
           (unsigned long long)(p->first_row() + p->row_count()));
  }
  if (none) return buf.data();
  const size_t raw = p->raw_len();
  if (p->codec() == fb::Codec::ZSTD) {
    const unsigned long long fs = ZSTD_getFrameContentSize(in.data(), cl);
    if (fs != raw)
      fail(FSTX_ERR_CORRUPT, "column '%s' (rows %llu to %llu): the page size does not match its ZSTD frame: the file is "
           "corrupt", F.col_name(col).c_str(), (unsigned long long)p->first_row() + 1,
           (unsigned long long)(p->first_row() + p->row_count()));
  }
  if (buf.size() < raw) buf.resize(raw);
  {
    FSTX_PROF_SCOPE(R_DECOMP);
    decompress_page(cc, (int)p->codec(), in.data(), cl, buf.data(), raw);
  }
  if (stored.capacity() > (4u << 20)) std::vector<uint8_t>().swap(stored);  // keep at most 4 MB per thread
  return buf.data();
}

void rethrow_for_page(const fstx_file& F, uint32_t col, const fb::Page* p) {
  try {
    throw;
  } catch (const Error& e) {
    if (e.code != FSTX_ERR_CORRUPT) throw;
    fail(FSTX_ERR_CORRUPT, "column '%s' (rows %llu to %llu): %s: the file is corrupt", F.col_name(col).c_str(),
         (unsigned long long)p->first_row() + 1, (unsigned long long)(p->first_row() + p->row_count()),
         e.msg.c_str());
  }
}

void check_no_nul(const char* bytes, const int32_t* lens, uint32_t n) {
  uint64_t tot = 0;
  for (uint32_t i = 0; i < n; ++i)
    if (lens[i] > 0) tot += (uint64_t)lens[i];
  if (tot && std::memchr(bytes, 0, (size_t)tot)) enc::corrupt("a string holds a NUL byte");
}

[[noreturn]] static void null_count_mismatch(uint32_t k, uint32_t nc) {
  char m[96];
  std::snprintf(m, sizeof(m), "%u missing values, null_count %u", (unsigned)k, (unsigned)nc);
  enc::corrupt(m);
}

void check_nulls(const fb::Page* p, const int32_t* x, uint32_t n, int32_t na) {
  uint32_t k = 0;
  for (uint32_t i = 0; i < n; ++i) k += x[i] == na;
  if (k != p->null_count()) null_count_mismatch(k, p->null_count());
}

void check_nulls(const fb::Page* p, const double* x, uint32_t n) {
  uint32_t k = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint64_t b;
    std::memcpy(&b, x + i, 8);
    k += (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (uint32_t)b == 1954u;
  }
  if (k != p->null_count()) null_count_mismatch(k, p->null_count());
}

namespace {

struct StrSlot {
  std::vector<uint8_t> buf;   // decompressed page body
  std::vector<uint8_t> fbuf;  // FSST output
  std::vector<int32_t> lv;    // lengths, or dictionary codes
  const int32_t* lens = nullptr;
  const char* bytes = nullptr;
  int kind = 0;  // K_STR, K_DICT or K_CODES
  int64_t task = -1;
  bool ready = false;
  // rows plans: the requested strings of the page, back to back
  std::vector<int32_t> olens;
  std::vector<char> obytes;
  std::vector<uint64_t> off;
  std::vector<uint8_t> vm;
};

// Rows plans: a worker's scratch for decoding a page, kept by the thread between pages and between reads (a read of
// scattered rows runs one plan per batch: buffers made and freed per batch were page faults on every thread), up
// to kKeepRowsScratch bytes per buffer.
constexpr size_t kKeepRowsScratch = 4u << 20;
struct RowsScratch {
  std::vector<uint8_t> buf, fbuf, vals, vm;
  std::vector<int32_t> lv;
  std::vector<uint64_t> off;
  std::vector<uint32_t> idx;
  template <typename V>
  static void trim(V& v) {
    if (v.capacity() * sizeof(typename V::value_type) > kKeepRowsScratch) V().swap(v);
  }
  void done() {
    trim(buf);
    trim(fbuf);
    trim(vals);
    trim(lv);
    trim(off);
    trim(idx);
  }
};
RowsScratch& rows_scratch() {
  static thread_local RowsScratch s;
  return s;
}

// A string dictionary decoded once for a rows plan.
struct DictData {
  std::vector<uint8_t> buf, fbuf;
  std::vector<int32_t> lens;
  std::vector<uint64_t> off;  // byte offset of each entry
  const char* bytes = nullptr;
  uint32_t n = 0;
};

struct Reader {
  const fstx_plan& P;
  const fstx_file& F;
  const fstx_read_host& H;
  bool verify, nulls;  // FSTX_VERIFY_HASH, FSTX_VERIFY_NULLS
  std::vector<uint8_t*> dest;
  std::vector<DictData> dicts;  // rows plans
  std::vector<int32_t> sent_dict;  // rows plans with codes: the dictionary each output column had last
  Reader(const fstx_plan& p, const fstx_read_host& h, int v)
      : P(p), F(*p.f), H(h), verify((v & FSTX_VERIFY_HASH) != 0), nulls((v & FSTX_VERIFY_NULLS) != 0) {}

  // rows plans: marks the 1,024-row vectors of the page that hold requested rows; false when all of them do
  bool vector_mask(const ReadTask& t, std::vector<uint8_t>& vm) {
    const uint32_t n = t.page->row_count();
    const uint64_t fr = t.page->first_row();
    const uint32_t nv = (n + kVector - 1) / kVector;
    vm.assign(nv, 0);
    uint32_t need = 0;
    for (uint64_t e = t.e0; e < t.e1; ++e) {
      const uint32_t v = (uint32_t)((P.rows[e] - fr) / kVector);
      if (!vm[v]) {
        vm[v] = 1;
        ++need;
      }
    }
    return need < nv;
  }

  void run_fixed_rows(const ReadTask& t, CodecContext& cc) {
    RowsScratch& T = rows_scratch();
    std::vector<uint8_t>& vals = T.vals;
    const OutCol& oc = P.outs[t.out];
    const size_t w = (size_t)width_of(oc.phys);
    const uint32_t n = t.page->row_count();
    const uint64_t fr = t.page->first_row();
    const uint8_t* vmp = (!nulls && vector_mask(t, T.vm)) ? T.vm.data() : nullptr;  // null counts need the whole page
    try {
      const uint8_t* b = body(t, cc, T.buf);
      if (vals.size() < (size_t)n * w) vals.resize((size_t)n * w);
      if (oc.phys == FSTX_FLOAT64)
        enc::decode_dbl_page_part(b, t.page->raw_len(), t.page->chain(), n, vmp, (double*)vals.data());
      else
        enc::decode_int_page_part(b, t.page->raw_len(), t.page->chain(), n, kind_of(oc.phys),
                                  std::numeric_limits<int32_t>::min(), vmp, (int32_t*)vals.data());
      if (nulls) {
        if (oc.phys == FSTX_FLOAT64) check_nulls(t.page, (const double*)vals.data(), n);
        else check_nulls(t.page, (const int32_t*)vals.data(), n, std::numeric_limits<int32_t>::min());
      }
    } catch (...) {
      rethrow_in(t);
    }
    const bool ident = P.pos.empty();
    if (w == 8) {
      const uint64_t* v = (const uint64_t*)vals.data();
      uint64_t* d = (uint64_t*)dest[t.out];
      for (uint64_t e = t.e0; e < t.e1; ++e) d[ident ? e : P.pos[e]] = v[P.rows[e] - fr];
    } else {
      const int32_t* v = (const int32_t*)vals.data();
      int32_t* d = (int32_t*)dest[t.out];
      const bool fac = oc.logical == FSTX_LT_FACTOR;  // never hand R a factor code outside its levels
      const int32_t nl = (int32_t)std::min<uint32_t>(oc.n_levels, 0x7fffffff);
      for (uint64_t e = t.e0; e < t.e1; ++e) {
        const int32_t x = v[P.rows[e] - fr];
        if (fac && x != std::numeric_limits<int32_t>::min() && (x < 1 || x > nl))
          fail(FSTX_ERR_CORRUPT, "column '%s' has factor codes outside its levels: the file is corrupt",
               F.col_name(t.col).c_str());
        d[ident ? e : P.pos[e]] = x;
      }
    }
    T.done();
  }

  void load_dict(uint32_t i, CodecContext& cc) {
    DictData& D = dicts[i];
    const fb::Page* p = P.dicts[i];
    const uint32_t col = P.dict_cols[i];
    try {
      const uint8_t* b = page_body_read(F, col, p, verify, cc, D.buf);
      enc::decode_str_page(b, p->raw_len(), p->chain(), p->row_count(), enc::K_DICT, D.lens, D.fbuf, &D.bytes);
      D.n = p->row_count();
      check_no_nul(D.bytes, D.lens.data(), D.n);
      D.off.resize((size_t)D.n + 1);
      D.off[0] = 0;
      for (uint32_t k = 0; k < D.n; ++k) D.off[k + 1] = D.off[k] + (uint64_t)(D.lens[k] > 0 ? D.lens[k] : 0);
    } catch (...) {
      rethrow_for_page(F, col, p);
    }
  }

  void run_string_rows(const ReadTask& t, CodecContext& cc, StrSlot& s) {
    RowsScratch& T = rows_scratch();
    const uint32_t n = t.page->row_count();
    const uint64_t fr = t.page->first_row();
    const uint64_t cnt = t.e1 - t.e0;
    try {
      const uint8_t* b = body(t, cc, T.buf);
      const size_t len = t.page->raw_len();
      s.olens.resize(cnt);
      s.obytes.clear();
      if (t.dict >= 0) {
        const DictData& D = dicts[(size_t)t.dict];
        const uint8_t* vmp = (!nulls && vector_mask(t, T.vm)) ? T.vm.data() : nullptr;
        if (T.lv.size() < n) T.lv.resize(n);
        enc::decode_int_page_part(b, len, t.page->chain(), n, enc::K_CODES, -1, vmp, T.lv.data());
        if (nulls) check_nulls(t.page, T.lv.data(), n, -1);
        const int32_t nd = (int32_t)D.n;
        if (t.out < P.codes_out.size() && P.codes_out[t.out]) {  // the codes themselves (dictionary sent apart)
          for (uint64_t e = t.e0; e < t.e1; ++e) {
            const int32_t k = T.lv[P.rows[e] - fr];
            if (k < -1 || k >= nd) enc::corrupt("dictionary code out of range");
            s.olens[e - t.e0] = k;
          }
          T.done();
          s.kind = enc::K_CODES;
          s.lens = s.olens.data();
          s.bytes = nullptr;
          return;
        }
        for (uint64_t e = t.e0; e < t.e1; ++e) {
          const int32_t k = T.lv[P.rows[e] - fr];
          if (k < -1 || k >= nd) enc::corrupt("dictionary code out of range");
          if (k < 0) {
            s.olens[e - t.e0] = -1;
          } else {
            s.olens[e - t.e0] = D.lens[k];
            s.obytes.insert(s.obytes.end(), D.bytes + D.off[k], D.bytes + D.off[k + 1]);
          }
        }
      } else if (!nulls) {  // only the strings asked for (FSST: only those are decompressed)
        T.idx.resize(cnt);
        for (uint64_t e = t.e0; e < t.e1; ++e) T.idx[e - t.e0] = (uint32_t)(P.rows[e] - fr);
        enc::decode_str_page_rows(b, len, t.page->chain(), n, T.idx.data(), (uint32_t)cnt, T.lv, s.olens, s.obytes);
        check_no_nul(s.obytes.data(), s.olens.data(), (uint32_t)cnt);
      } else {  // FSTX_VERIFY_NULLS counts the whole page
        const char* bytes = nullptr;
        enc::decode_str_page(b, len, t.page->chain(), n, enc::K_STR, T.lv, T.fbuf, &bytes);
        if (nulls) check_nulls(t.page, T.lv.data(), n, -1);
        T.off.resize((size_t)n + 1);
        T.off[0] = 0;
        for (uint32_t i = 0; i < n; ++i) T.off[i + 1] = T.off[i] + (uint64_t)(T.lv[i] > 0 ? T.lv[i] : 0);
        for (uint64_t e = t.e0; e < t.e1; ++e) {
          const uint64_t i = P.rows[e] - fr;
          s.olens[e - t.e0] = T.lv[i];
          if (T.lv[i] > 0) s.obytes.insert(s.obytes.end(), bytes + T.off[i], bytes + T.off[i + 1]);
        }
        check_no_nul(s.obytes.data(), s.olens.data(), (uint32_t)cnt);
      }
    } catch (...) {
      rethrow_in(t);
    }
    T.done();
    s.kind = enc::K_STR;
    s.lens = s.olens.data();
    s.bytes = s.obytes.data();
  }

  // the page body with the codec removed (the mapped file itself when there is no codec)
  const uint8_t* body(const ReadTask& t, CodecContext& cc, std::vector<uint8_t>& buf) {
    if (P.rows_mode) return page_body_read(F, t.col, t.page, verify, cc, buf);
    return page_body(F, t.col, t.page, verify, cc, buf);
  }

  [[noreturn]] void rethrow_in(const ReadTask& t) { rethrow_for_page(F, t.col, t.page); }

  void run_fixed(const ReadTask& t, CodecContext& cc, std::vector<uint8_t>& scratch, std::vector<uint8_t>& vals) {
    if (P.rows_mode) return run_fixed_rows(t, cc);
    const OutCol& oc = P.outs[t.out];
    const size_t w = (size_t)width_of(oc.phys);
    uint8_t* dst = dest[t.out] + t.dest * w;
    const uint32_t n = t.page->row_count();
    try {
      const uint8_t* b = body(t, cc, scratch);
      const bool whole = t.skip == 0 && t.take == n;
      uint8_t* to = dst;
      if (!whole) {
        if (vals.size() < (size_t)n * w) vals.resize((size_t)n * w);
        to = vals.data();
      }
      if (oc.phys == FSTX_FLOAT64)
        enc::decode_dbl_page(b, t.page->raw_len(), t.page->chain(), n, (double*)to);
      else
        enc::decode_int_page(b, t.page->raw_len(), t.page->chain(), n, kind_of(oc.phys),
                             std::numeric_limits<int32_t>::min(), (int32_t*)to);
      if (nulls) {
        if (oc.phys == FSTX_FLOAT64) check_nulls(t.page, (const double*)to, n);
        else check_nulls(t.page, (const int32_t*)to, n, std::numeric_limits<int32_t>::min());
      }
      if (!whole) {
        FSTX_PROF_SCOPE(R_COPY);
        std::memcpy(dst, vals.data() + (size_t)t.skip * w, (size_t)t.take * w);
      }
      FSTX_PROF_SCOPE(R_RELEASE);
      F.map.release(t.page->offset(), t.page->comp_len());
    } catch (...) {
      rethrow_in(t);
    }
    if (oc.logical == FSTX_LT_FACTOR) {  // never hand R a factor code outside its levels
      FSTX_PROF_SCOPE(R_FACTOR_CHECK);
      const int32_t* v = (const int32_t*)dst;
      const int32_t nl = (int32_t)std::min<uint32_t>(oc.n_levels, 0x7fffffff);
      for (uint32_t i = 0; i < t.take; ++i)
        if (v[i] != std::numeric_limits<int32_t>::min() && (v[i] < 1 || v[i] > nl))
          fail(FSTX_ERR_CORRUPT, "column '%s' has factor codes outside its levels: the file is corrupt",
               F.col_name(t.col).c_str());
    }
  }

  void run_string(const ReadTask& t, CodecContext& cc, StrSlot& s) {
    if (P.rows_mode) return run_string_rows(t, cc, s);
    const uint32_t n = t.page->row_count();
    try {
      const uint8_t* b = body(t, cc, s.buf);
      const size_t len = t.page->raw_len();
      const uint32_t chain = t.page->chain();
      if (t.what == 1) {
        s.kind = enc::K_DICT;
        FSTX_PROF_SCOPE(R_STR_DEC);
        enc::decode_str_page(b, len, chain, n, enc::K_DICT, s.lv, s.fbuf, &s.bytes);
        s.lens = s.lv.data();
        check_no_nul(s.bytes, s.lens, n);
      } else if (t.dict_rows) {
        s.kind = enc::K_CODES;
        if (s.lv.size() < n) s.lv.resize(n);
        enc::decode_int_page(b, len, chain, n, enc::K_CODES, -1, s.lv.data());
        if (nulls) check_nulls(t.page, s.lv.data(), n, -1);
        const int32_t nd = (int32_t)t.dict_rows;
        FSTX_PROF_SCOPE(R_CODES_CHECK);
        for (uint32_t i = 0; i < n; ++i)
          if (s.lv[i] < -1 || s.lv[i] >= nd) enc::corrupt("dictionary code out of range");
        s.lens = s.lv.data() + t.skip;
        s.bytes = nullptr;
      } else {
        s.kind = enc::K_STR;
        const char* bytes = nullptr;
        FSTX_PROF_SCOPE(R_STR_DEC);
        enc::decode_str_page(b, len, chain, n, enc::K_STR, s.lv, s.fbuf, &bytes);
        if (nulls) check_nulls(t.page, s.lv.data(), n, -1);
        uint64_t skip_bytes = 0;
        for (uint32_t i = 0; i < t.skip; ++i)
          if (s.lv[i] > 0) skip_bytes += (uint64_t)s.lv[i];
        s.lens = s.lv.data() + t.skip;
        s.bytes = bytes + skip_bytes;
        check_no_nul(s.bytes, s.lens, t.take);
      }
    } catch (...) {
      rethrow_in(t);
    }
  }

  void deliver(const ReadTask& t, const StrSlot& s) {
    FSTX_PROF_SCOPE(R_STR_DELIVER);
    int rc;
    if (P.rows_mode) {
      const uint32_t cnt = (uint32_t)(t.e1 - t.e0);
      if (s.kind == enc::K_CODES) {  // fstx_plan_rows_codes: the dictionary when it changes, then the codes
        if (sent_dict.size() <= t.out) sent_dict.resize((size_t)t.out + 1, -1);
        if (sent_dict[t.out] != t.dict) {
          const DictData& D = dicts[(size_t)t.dict];
          if (H.on_dict(H.ctx, t.out, D.n, D.lens.data(), D.bytes) != 0)
            fail(FSTX_ERR_HOST, "the host failed while building strings");
          sent_dict[t.out] = t.dict;
        }
        rc = H.on_codes(H.ctx, t.out, t.e0, cnt, s.lens);
      } else if (P.pos.empty())
        rc = H.on_strings(H.ctx, t.out, t.e0, cnt, s.lens, s.bytes);
      else
        rc = H.on_strings_at(H.ctx, t.out, cnt, P.pos.data() + t.e0, s.lens, s.bytes);
    } else if (s.kind == enc::K_DICT)
      rc = H.on_dict(H.ctx, t.out, t.page->row_count(), s.lens, s.bytes);
    else if (s.kind == enc::K_CODES)
      rc = H.on_codes(H.ctx, t.out, t.dest, t.take, s.lens);
    else
      rc = H.on_strings(H.ctx, t.out, t.dest, t.take, s.lens, s.bytes);
    if (rc != 0) fail(FSTX_ERR_HOST, "the host failed while building strings");
    if (!P.rows_mode)  // rows plans copy pages out of the file (page_body_read): nothing of the map to trim
      F.map.release(t.page->offset(), t.page->comp_len());  // plain pages are read from the map: trim after use
  }

  void run(int threads) {
    const uint64_t n = P.out_rows();
    dest.assign(P.outs.size(), nullptr);
    FSTX_PROF_SCOPE(R_TOTAL);
    for (uint32_t j = 0; j < P.outs.size(); ++j) {
      FSTX_PROF_SCOPE(R_ALLOC);
      void* p = H.alloc_column(H.ctx, j, P.outs[j].col, P.outs[j].phys, n);
      if (!p) fail(FSTX_ERR_HOST, "the host could not allocate column '%s'", F.col_name(P.outs[j].col).c_str());
      dest[j] = (uint8_t*)p;
    }
    const int64_t NF = (int64_t)P.fixed.size(), NS = (int64_t)P.strings.size();
    if (NF + NS == 0) return;
    int k = (int)std::min<int64_t>(resolve_threads(threads), NF + NS);
    // Strings are built on this thread: keep a core free for it when the workers would fill the machine.
    const int hw = (int)fstx_hardware_threads();   // cores this process may use
    if (NS > 0 && k > 1 && k >= hw) k = hw - 1;
    std::vector<std::unique_ptr<CodecContext>> ctxs;
    for (int i = 0; i < std::max(k, 1); ++i) ctxs.emplace_back(new CodecContext());

    if (!P.dicts.empty()) {  // rows plans: decode each needed string dictionary once, in parallel
      dicts.resize(P.dicts.size());
      const uint32_t nd = (uint32_t)P.dicts.size();
      const int kd = (int)std::min<int64_t>(std::max(k, 1), nd);
      if (kd <= 1) {
        for (uint32_t i = 0; i < nd; ++i) load_dict(i, *ctxs[0]);
      } else {
        std::atomic<uint32_t> next(0);
        std::mutex dm;
        std::exception_ptr derr;
        ThreadPool::instance().start(kd, [&](int id) {
          for (;;) {
            const uint32_t i = next.fetch_add(1);
            if (i >= nd) return;
            try {
              load_dict(i, *ctxs[(size_t)id]);
            } catch (...) {
              std::lock_guard<std::mutex> g(dm);
              if (!derr) derr = std::current_exception();
              next.store(nd);
              return;
            }
          }
        });
        ThreadPool::instance().wait();
        if (derr) std::rethrow_exception(derr);
      }
    }

    if (k <= 1 || NF + NS <= 1) {
      std::vector<uint8_t> scratch, vals;
      for (const ReadTask& t : P.fixed) run_fixed(t, *ctxs[0], scratch, vals);
      StrSlot s;
      for (const ReadTask& t : P.strings) {
        run_string(t, *ctxs[0], s);
        deliver(t, s);
      }
      return;
    }

    const int64_t WS = 4 * (int64_t)k;  // string pages decoded ahead of the main thread
    std::vector<StrSlot> slots((size_t)std::min<int64_t>(WS, std::max<int64_t>(NS, 1)));
    const int64_t nslots = (int64_t)slots.size();
    std::mutex m;
    std::condition_variable cv_main, cv_work;  // main waits for decoded string pages; workers wait for room
    int64_t next_f = 0, next_s = 0, consumed = 0;
    bool abort = false;
    std::exception_ptr err;

    auto job = [&](int id) {
      CodecContext& cc = *ctxs[(size_t)id];
      std::vector<uint8_t> scratch, vals;
      for (;;) {
        bool is_str = false;
        int64_t i = -1;
        {
          std::unique_lock<std::mutex> lk(m);
          for (;;) {
            if (abort) return;
            if (next_s < NS && next_s < consumed + nslots) {
              is_str = true;
              i = next_s++;
              break;
            }
            if (next_f < NF) {
              i = next_f++;
              break;
            }
            if (next_s >= NS) return;
            cv_work.wait(lk);
          }
        }
        try {
          if (is_str) {
            StrSlot& s = slots[(size_t)(i % nslots)];
            run_string(P.strings[(size_t)i], cc, s);
            {
              std::lock_guard<std::mutex> g(m);
              s.task = i;
              s.ready = true;
            }
            cv_main.notify_one();
          } else {
            run_fixed(P.fixed[(size_t)i], cc, scratch, vals);
          }
        } catch (...) {
          std::lock_guard<std::mutex> g(m);
          if (!err) err = std::current_exception();
          abort = true;
          cv_main.notify_all();
          cv_work.notify_all();
          return;
        }
      }
    };

    ThreadPool::instance().start(k, job);
    try {
      for (int64_t i = 0; i < NS; ++i) {
        StrSlot& s = slots[(size_t)(i % nslots)];
        {
          std::unique_lock<std::mutex> lk(m);
          cv_main.wait(lk, [&] { return abort || (s.ready && s.task == i); });
          if (abort) break;
        }
        deliver(P.strings[(size_t)i], s);  // R strings are made here, on the calling thread
        {
          std::lock_guard<std::mutex> g(m);
          s.ready = false;
          ++consumed;
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
};

}  // namespace

int to_error(fstx_error* err) {
  try {
    throw;
  } catch (const Error& e) {
    set_error(err, e.code, e.msg.c_str(), e.column);
    return e.code;
  } catch (const std::bad_alloc&) {
    set_error(err, FSTX_ERR_MEMORY, "out of memory");
    return FSTX_ERR_MEMORY;
  } catch (const std::exception& e) {
    set_error(err, FSTX_ERR_INTERNAL, e.what());
    return FSTX_ERR_INTERNAL;
  } catch (...) {
    set_error(err, FSTX_ERR_INTERNAL, "unknown error");
    return FSTX_ERR_INTERNAL;
  }
}

}  // namespace fstx

extern "C" int fstx_open(const char* path, fstx_file** out, fstx_error* err) {
  fstx::set_error(err, FSTX_OK, "");
  *out = nullptr;
  fstx_file* f = nullptr;
  try {
    if (!fstx::host_is_little_endian()) fstx::fail(FSTX_ERR_FORMAT, "big-endian machines are not supported yet");
    if (!path) fstx::fail(FSTX_ERR_ARG, "missing path");
    f = new fstx_file();
    fstx::open_file(*f, path);
    fstx::registry_add(f->map.id());
    *out = f;
    return FSTX_OK;
  } catch (...) {
    delete f;
    return fstx::to_error(err);
  }
}

extern "C" void fstx_close(fstx_file* f) {
  if (!f) return;
  fstx::registry_remove(f->map.id());
  delete f;
}

extern "C" void fstx_file_info_get(const fstx_file* f, fstx_file_info* o) {
  std::memset(o, 0, sizeof(*o));
  o->n_rows = f->n_rows;
  o->n_cols = f->n_cols;
  o->n_row_groups = f->footer->row_groups() ? f->footer->row_groups()->size() : 0;
  o->n_pages = f->n_pages;
  o->file_size = f->map.size();
  o->row_group_size = f->footer->row_group_size();
  o->format_version = f->version;
  o->profile = f->profile;
  if (const auto* a = f->footer->table_attrs()) {
    o->table_attrs = a->data();
    o->table_attrs_len = a->size();
  }
}

extern "C" int fstx_column_info_get(const fstx_file* f, uint32_t c, fstx_column_info* o) {
  if (c >= f->n_cols) return FSTX_ERR_ARG;
  std::memset(o, 0, sizeof(*o));
  const fb::Column* x = f->col(c);
  if (x->name()) {
    o->name = x->name()->c_str();
    o->name_len = x->name()->size();
  } else {
    o->name = "";
  }
  o->phys = (int)x->phys();
  o->logical = (int)x->logical();
  o->ordered = x->ordered() ? 1 : 0;
  o->n_levels = x->levels() ? x->levels()->size() : 0;
  if (x->tzone()) {
    o->has_tzone = 1;
    o->tzone = x->tzone()->c_str();
    o->tzone_len = x->tzone()->size();
  }
  if (x->attrs()) {
    o->attrs = x->attrs()->data();
    o->attrs_len = x->attrs()->size();
  }
  o->compressed_bytes = f->sums[c].comp;
  o->raw_bytes = f->sums[c].raw;
  o->n_pages = f->sums[c].pages;
  return FSTX_OK;
}

extern "C" int fstx_level_get(const fstx_file* f, uint32_t c, uint32_t i, const char** s, uint32_t* len) {
  if (c >= f->n_cols) return FSTX_ERR_ARG;
  const auto* lv = f->col(c)->levels();
  if (!lv || i >= lv->size()) return FSTX_ERR_ARG;
  const flatbuffers::String* x = lv->Get(i);
  *s = x->c_str();
  *len = x->size();
  return FSTX_OK;
}

extern "C" int64_t fstx_find_column(const fstx_file* f, const char* name, size_t len) {
  auto it = f->names.find(std::string(name, len));
  return it == f->names.end() ? -1 : (int64_t)it->second;
}

extern "C" int fstx_plan_create(const fstx_file* f, const uint32_t* cols, uint32_t n_cols, uint64_t from, uint64_t to,
                                fstx_plan** out, fstx_error* err) {
  fstx::set_error(err, FSTX_OK, "");
  *out = nullptr;
  fstx_plan* p = nullptr;
  try {
    p = new fstx_plan();
    fstx::make_plan(*p, *f, cols, n_cols, from, to);
    *out = p;
    return FSTX_OK;
  } catch (...) {
    delete p;
    return fstx::to_error(err);
  }
}

extern "C" int fstx_plan_create_rows(const fstx_file* f, const uint32_t* cols, uint32_t n_cols, const uint64_t* rows,
                                     uint64_t n_rows, fstx_plan** out, fstx_error* err) {
  fstx::set_error(err, FSTX_OK, "");
  *out = nullptr;
  fstx_plan* p = nullptr;
  try {
    if (n_rows && !rows) fstx::fail(FSTX_ERR_ARG, "missing rows");
    p = new fstx_plan();
    fstx::make_plan_rows(*p, *f, cols, n_cols, rows, n_rows);
    *out = p;
    return FSTX_OK;
  } catch (...) {
    delete p;
    return fstx::to_error(err);
  }
}

extern "C" int fstx_plan_rows_codes(fstx_plan* p, uint32_t out_index, fstx_error* err) {
  fstx::set_error(err, FSTX_OK, "");
  try {
    if (!p || !p->rows_mode) fstx::fail(FSTX_ERR_ARG, "dictionary codes are for rows plans");
    if (out_index >= p->outs.size() || p->outs[out_index].phys != FSTX_STRING)
      fstx::fail(FSTX_ERR_ARG, "dictionary codes need a string column");
    if (!p->pos.empty()) fstx::fail(FSTX_ERR_ARG, "dictionary codes need rows in ascending order");
    if (p->codes_out.size() < p->outs.size()) p->codes_out.resize(p->outs.size(), 0);
    p->codes_out[out_index] = 1;
    return FSTX_OK;
  } catch (...) {
    return fstx::to_error(err);
  }
}

extern "C" void fstx_plan_free(fstx_plan* p) { delete p; }
extern "C" uint64_t fstx_plan_rows(const fstx_plan* p) { return p->out_rows(); }

extern "C" void fstx_plan_string_stats(const fstx_plan* p, uint64_t* plain_rows, uint64_t* plain_raw_bytes) {
  uint64_t r = 0, b = 0;
  for (const auto& t : p->strings)
    if (t.what == 0 && t.dict_rows == 0) {
      r += t.take;
      b += t.page->raw_len();
    }
  *plain_rows = r;
  *plain_raw_bytes = b;
}

extern "C" int fstx_read(const fstx_plan* p, const fstx_read_host* host, int threads, int verify, fstx_error* err) {
  fstx::set_error(err, FSTX_OK, "");
  try {
    if (!p || !host || !host->alloc_column || !host->on_strings) fstx::fail(FSTX_ERR_ARG, "missing plan or host callbacks");
    if (p->rows_mode && !p->pos.empty() && !p->strings.empty() && !host->on_strings_at)
      fstx::fail(FSTX_ERR_ARG, "rows out of order need the host's on_strings_at callback");
    bool codes = false;
    for (uint8_t c : p->codes_out) codes = codes || c;
    if ((p->has_dict || codes) && (!host->on_dict || !host->on_codes))
      fstx::fail(FSTX_ERR_ARG, "the file has string dictionaries but the host gave no dictionary callbacks");
    fstx::Reader r(*p, *host, verify);
    r.run(threads);
    return FSTX_OK;
  } catch (...) {
    return fstx::to_error(err);
  }
}

extern "C" int64_t fstx_pages_get(const fstx_file* f, fstx_page_info* out, uint64_t cap) {
  const auto* rgs = f->footer->row_groups();
  const uint32_t nrg = rgs ? rgs->size() : 0;
  uint64_t k = 0;
  auto put = [&](uint32_t g, uint32_t c, const fb::Page* p, int is_dict) {
    if (out && k < cap) {
      fstx_page_info& o = out[k];
      o.row_group = g;
      o.column = c;
      o.is_dictionary = is_dict;
      o.first_row = p->first_row();
      o.row_count = p->row_count();
      o.null_count = p->null_count();
      o.offset = p->offset();
      o.comp_len = p->comp_len();
      o.raw_len = p->raw_len();
      o.codec = (int)p->codec();
      o.kind = fstx::enc::chain_kind(p->chain());
      o.na_mode = fstx::enc::chain_na(p->chain());
      o.enc[0] = fstx::enc::chain_tag(p->chain(), 0);
      o.enc[1] = fstx::enc::chain_tag(p->chain(), 1);
      o.enc[2] = fstx::enc::chain_tag(p->chain(), 2);
    }
    ++k;
  };
  for (uint32_t g = 0; g < nrg; ++g) {
    const fb::RowGroup* rg = rgs->Get(g);
    for (uint32_t c = 0; c < f->n_cols; ++c) {
      const fb::ColumnChunk* ch = rg->columns()->Get(c);
      if (ch->dictionary()) put(g, c, ch->dictionary(), 1);
      for (uint32_t i = 0; i < ch->pages()->size(); ++i) put(g, c, ch->pages()->Get(i), 0);
    }
  }
  return (int64_t)k;
}

extern "C" const char* fstx_encoding_name(int tag) {
  const char* s = fstx::enc::tag_name(tag);
  return s;
}

extern "C" const char* fstx_kind_name(int kind) { return fstx::enc::kind_name(kind); }
