// fstx damaged-file maker (WS6): writes a tiny synthetic table through the engine, then damages one structural field
// at a time and puts the checksums back (fz::fixup), so the damage reaches the reader's structure checks instead
// of stopping at a checksum. Usage: craft <out_dir>. Writes <name>.fstx for every case, and seed form
// (16 zero control bytes + file) as <out_dir>/seed/<name>; tests/testthat/test-hardening-structure.R embeds them.
// Cases: memory bombs (huge raw / stored page length), offsets that wrap around 2^64, overlapping pages, a page
// inside the footer, a footer inside the pages or inside its own tail, header bits that differ from the footer;
// fstx 1.2: page kind 7 under an int32 column, int64 values holding INT64_MIN that no NA block marks.
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "fuzz_common.h"

namespace {

std::string g_out;

int get_s(void*, uint64_t, uint32_t n, const char** p, int32_t* lens) {
  for (uint32_t k = 0; k < n; ++k) {
    p[k] = "ab";
    lens[k] = 2;
  }
  return 0;
}

// 2048 rows: i = r * 7 % 1000 (int32), s = "ab"; plain encoding, 2 row groups of 1024 rows, one page each per column
bool write_base(const std::string& file, int codec) {
  std::vector<int32_t> iv(2048);
  for (int r = 0; r < 2048; ++r) iv[r] = r * 7 % 1000;
  fstx_write_column wc[2];
  std::memset(wc, 0, sizeof wc);
  wc[0].name = "i";
  wc[0].name_len = 1;
  wc[0].phys = FSTX_INT32;
  wc[1].name = "s";
  wc[1].name_len = 1;
  wc[1].phys = FSTX_STRING;
  fstx_column_data cd[2];
  std::memset(cd, 0, sizeof cd);
  cd[0].data = iv.data();
  cd[1].get_strings = get_s;
  fstx_write_options o;
  std::memset(&o, 0, sizeof o);
  o.codec = codec;
  o.level = 3;
  o.threads = 1;
  o.row_group_size = 1024;
  o.page_bytes = 65536;
  o.profile = codec == 0 ? 0 : 1;
  o.keep_pct = 0;
  o.force_encoding = 1;
  fstx_writer* w = nullptr;
  fstx_error err;
  if (fstx_writer_open(file.c_str(), wc, 2, nullptr, 0, &o, &w, &err) != FSTX_OK ||
      fstx_writer_append(w, cd, 2048, &err) != FSTX_OK || fstx_writer_close(w, &err) != FSTX_OK) {
    std::fprintf(stderr, "write %s: %s\n", file.c_str(), err.message);
    return false;
  }
  return true;
}

// 300 strings, FSST forced (force_encoding 10), codec none; only the last string holds byte 0x01, which the symbol
// table never has, so FSST writes it as escape 255 + 0x01 at the very end of the page's FSST bytes
std::vector<std::string> g_fs;
int get_fs(void*, uint64_t row, uint32_t n, const char** p, int32_t* lens) {
  for (uint32_t k = 0; k < n; ++k) {
    p[k] = g_fs[row + k].data();
    lens[k] = (int32_t)g_fs[row + k].size();
  }
  return 0;
}
bool write_fsst(const std::string& file, const std::string& last) {
  const char* w[] = {"north", "south", "east", "west", "market", "street"};
  g_fs.clear();
  for (int r = 0; r < 300; ++r) g_fs.push_back(std::string(w[r % 6]) + w[(r / 6) % 6] + std::to_string(r % 17));
  g_fs.back() = last;
  fstx_write_column wc;
  std::memset(&wc, 0, sizeof wc);
  wc.name = "s";
  wc.name_len = 1;
  wc.phys = FSTX_STRING;
  fstx_column_data cd;
  std::memset(&cd, 0, sizeof cd);
  cd.get_strings = get_fs;
  fstx_write_options o;
  std::memset(&o, 0, sizeof o);
  o.threads = 1;
  o.row_group_size = 1024;
  o.page_bytes = 65536;
  o.force_encoding = 10;
  fstx_writer* wr = nullptr;
  fstx_error err;
  if (fstx_writer_open(file.c_str(), &wc, 1, nullptr, 0, &o, &wr, &err) != FSTX_OK ||
      fstx_writer_append(wr, &cd, 300, &err) != FSTX_OK || fstx_writer_close(wr, &err) != FSTX_OK) {
    std::fprintf(stderr, "write %s: %s\n", file.c_str(), err.message);
    return false;
  }
  return true;
}

// fstx 1.2: one int64 column of 1024 rows, plain (force 1), codec none; with_na: every 9th row NA (bitmap)
bool write_i64(const std::string& file, bool with_na) {
  std::vector<int64_t> v(1024);
  for (int r = 0; r < 1024; ++r) v[r] = with_na && r % 9 == 4 ? INT64_MIN : (1LL << 40) + r * 13;
  fstx_write_column wc;
  std::memset(&wc, 0, sizeof wc);
  wc.name = "l";
  wc.name_len = 1;
  wc.phys = FSTX_INT64;
  fstx_column_data cd;
  std::memset(&cd, 0, sizeof cd);
  cd.data = v.data();
  fstx_write_options o;
  std::memset(&o, 0, sizeof o);
  o.threads = 1;
  o.row_group_size = 1024;
  o.page_bytes = 65536;
  o.force_encoding = 1;
  fstx_writer* wr = nullptr;
  fstx_error err;
  if (fstx_writer_open(file.c_str(), &wc, 1, nullptr, 0, &o, &wr, &err) != FSTX_OK ||
      fstx_writer_append(wr, &cd, 1024, &err) != FSTX_OK || fstx_writer_close(wr, &err) != FSTX_OK) {
    std::fprintf(stderr, "write %s: %s\n", file.c_str(), err.message);
    return false;
  }
  return true;
}

// page k in footer order (row group 0: i, s; row group 1: i, s), as a writable pointer into b
uint8_t* page_at(std::vector<uint8_t>& b, int k) {
  const fstx::fb::Footer* ft = fz::footer_of(b.data(), b.size());
  if (!ft) return nullptr;
  uint8_t* hit = nullptr;
  int i = 0;
  fz::each_page(ft, [&](uint32_t, const fstx::fb::Page* p, bool) {
    if (i++ == k) hit = (uint8_t*)p;
  });
  return hit;
}
// Page struct: offset 0, first_row 8, hash 16, min 24, max 32, comp_len 40, raw_len 44, row_count 48,
// null_count 52, chain 56, codec 60, flags 61
uint64_t pu64(const uint8_t* p, int at) { return fstx::get_u64(p + at); }
uint32_t pu32(const uint8_t* p, int at) { return fstx::get_u32(p + at); }

void emit(const std::string& name, std::vector<uint8_t> b, bool fix = true) {
  if (fix) fz::fixup(b, false);
  fz::write_file(g_out + "/" + name + ".fstx", b.data(), b.size());
  std::vector<uint8_t> s(16, 0);
  s.insert(s.end(), b.begin(), b.end());
  s[0] = 0x80;  // seed form: control byte 0 bit 7 = no further fixup, the damage is kept as made
  fz::write_file(g_out + "/seed/craft_" + name, s.data(), s.size());
  std::printf("%-22s %6zu bytes\n", name.c_str(), b.size());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: craft <out_dir>\n");
    return 2;
  }
  g_out = argv[1];
  mkdir(g_out.c_str(), 0755);
  mkdir((g_out + "/seed").c_str(), 0755);
  const char* cn[] = {"none", "lz4", "zstd"};
  std::vector<uint8_t> base[3];
  for (int c = 0; c < 3; ++c) {
    const std::string f = g_out + "/base_" + cn[c] + ".fstx";
    if (!write_base(f, c)) return 1;
    base[c] = fz::read_file(f);
    const uint8_t* p = page_at(base[c], 0);
    if (!p) return 1;
    std::printf("base_%-17s %6zu bytes, page 0 codec %d comp %u raw %u\n", cn[c], base[c].size(), p[60],
                pu32(p, 40), pu32(p, 44));
  }
  auto edit = [&](int c, const std::string& name, const std::function<bool(std::vector<uint8_t>&)>& f, bool fix = true) {
    std::vector<uint8_t> b = base[c];
    if (!f(b)) {
      std::printf("%-22s NOT MADE\n", name.c_str());
      return;
    }
    emit(name, b, fix);
  };
  const uint32_t kBomb = 0x7ffffff8u;  // 2^31 - 8: the largest raw_len the format allows
  // memory bombs: a page of a few bytes that declares a 2 GB body
  for (int c = 1; c < 3; ++c)
    edit(c, std::string("bomb_raw_") + cn[c], [&](std::vector<uint8_t>& b) {
      uint8_t* p = page_at(b, 0);
      if (!p || p[60] != c) return false;
      fstx::put_u32(p + 44, kBomb);
      return true;
    });
  edit(2, "bomb_comp", [&](std::vector<uint8_t>& b) {
    uint8_t* p = page_at(b, 0);
    fstx::put_u32(p + 40, kBomb);
    fstx::put_u32(p + 44, kBomb);
    return true;
  });
  // offsets that wrap around 2^64
  edit(2, "wrap_page_offset", [&](std::vector<uint8_t>& b) {
    fstx::put_u64(page_at(b, 0), 0xffffffffffffffc0ull);
    return true;
  });
  edit(2, "wrap_first_row", [&](std::vector<uint8_t>& b) {
    fstx::put_u64(page_at(b, 2) + 8, 0xffffffffffffff80ull);
    return true;
  });
  edit(2, "wrap_footer_offset", [&](std::vector<uint8_t>& b) {
    fstx::put_u64(b.data() + b.size() - 32, 0xffffffffffffffc0ull);
    return true;
  });
  edit(2, "wrap_footer_len", [&](std::vector<uint8_t>& b) {
    fstx::put_u64(b.data() + b.size() - 24, 0xffffffffffffff00ull);
    return true;
  });
  edit(2, "wrap_committed_end", [&](std::vector<uint8_t>& b) {
    fstx::put_u64(b.data() + 16, 0xffffffffffffffffull);
    return true;
  });
  // overlapping pages: the string page of row group 0 reads the int page's bytes (whole or from its middle)
  edit(2, "overlap_same", [&](std::vector<uint8_t>& b) {
    uint8_t* a = page_at(b, 0);
    uint8_t* s = page_at(b, 1);
    std::memcpy(s, a, 8);
    std::memcpy(s + 40, a + 40, 8);
    return true;
  });
  edit(2, "overlap_part", [&](std::vector<uint8_t>& b) {
    uint8_t* a = page_at(b, 0);
    uint8_t* s = page_at(b, 1);
    if (pu32(a, 40) < 128) return false;
    fstx::put_u64(s, pu64(a, 0) + 64);
    return true;
  });
  // a page that lies inside the footer (the last 64-byte boundary before the footer that still fits)
  edit(2, "page_in_footer", [&](std::vector<uint8_t>& b) {
    uint64_t foff = 0, flen = 0;
    if (!fz::footer_of(b.data(), b.size(), &foff, &flen)) return false;
    uint8_t* p = page_at(b, 3);
    const uint64_t cl = pu32(p, 40);
    if (flen < cl) return false;
    fstx::put_u64(p, (foff + flen - cl) / 64 * 64);
    return true;
  });
  // a footer that points at the first page, or into its own tail
  edit(2, "footer_in_pages", [&](std::vector<uint8_t>& b) {
    fstx::put_u64(b.data() + b.size() - 32, 64);
    return true;
  });
  edit(2, "footer_in_tail", [&](std::vector<uint8_t>& b) {
    fstx::put_u64(b.data() + b.size() - 32, b.size() - 32);
    fstx::put_u64(b.data() + b.size() - 24, 32);
    return true;
  });
  // header feature bits that the footer does not have
  edit(2, "hdr_bits_only", [&](std::vector<uint8_t>& b) {
    fstx::put_u16(b.data() + 6, 1);
    return true;
  });
  // row counts that do not add up (huge page row count, declared table rows)
  edit(2, "rows_page_huge", [&](std::vector<uint8_t>& b) {
    fstx::put_u32(page_at(b, 0) + 48, 0xffffffffu);
    return true;
  });
  // FSST (WS8's report): the page body's blocks are walked to the FSST block (tag 10: u64 total, symbol table
  // block, compressed bytes block, lengths); then the last compressed byte becomes a lone escape (255), or the
  // first compressed byte becomes a code the symbol table does not have (254).
  // The lone escape must be a string's last code with an odd number of codes left after the 4-code loop, so the
  // last string varies: one of them compresses to an odd length.
  const char* lasts[] = {"north", "street", "q", "northsouth", "eastwest1", "x"};
  for (int k = 0; k < 6; ++k) {
    const std::string f = g_out + "/base_fsst" + std::to_string(k) + ".fstx";
    if (!write_fsst(f, lasts[k])) continue;
    std::vector<uint8_t> b = fz::read_file(f);
    std::remove(f.c_str());
    uint8_t* pg = page_at(b, 0);
    uint8_t* bytes = nullptr;
    uint32_t blen = 0;
    if (pg && pg[60] == 0) {
      const uint8_t* p = b.data() + pu64(pg, 0) + 8;  // after the 8-byte page header
      const uint8_t* e = p + pu32(pg, 40);
      while (e - p >= 8) {
        const uint32_t len = fstx::get_u32(p + 4);
        if (p[0] == 10 && len >= 24) {  // T_FSST
          const uint8_t* q = p + 16;     // block header + total
          const uint32_t tl = fstx::get_u32(q + 4);
          q += 8 + ((tl + 7) / 8) * 8;
          if (q[0] == 1) {
            blen = fstx::get_u32(q + 4);
            bytes = (uint8_t*)q + 8;
          }
          break;
        }
        p += 8 + ((len + 7) / 8) * 8;
      }
    }
    if (!bytes || !blen) {
      std::printf("fsst case %d            NOT MADE (no FSST block found)\n", k);
      continue;
    }
    std::vector<uint8_t> v = b;
    v[bytes - b.data() + blen - 1] = 255;
    emit("fsst_lone_escape" + std::to_string(k), v);
    if (k == 0) {
      v = b;
      v[bytes - b.data()] = 254;
      emit("fsst_unused_code", v);
    }
  }
  // fstx 1.2: page kind 7 under an int32 column (the base's page 0), and an int64 value block holding INT64_MIN
  // that no NA block marks (a page without NA, and a page with an NA bitmap where row 5 is not marked)
  edit(2, "kind7_under_int32", [&](std::vector<uint8_t>& b) {
    uint8_t* p = page_at(b, 0);
    fstx::put_u32(p + 56, (pu32(p, 56) & ~15u) | 7u);
    return true;
  });
  for (int with_na = 0; with_na < 2; ++with_na) {
    const std::string f = g_out + "/base_i64.fstx";
    const std::string name = with_na ? "i64_unmarked_min_na" : "i64_unmarked_min";
    if (!write_i64(f, with_na != 0)) continue;
    std::vector<uint8_t> b = fz::read_file(f);
    std::remove(f.c_str());
    uint8_t* pg = page_at(b, 0);
    uint8_t* vals = nullptr;
    if (pg && pg[60] == 0) {
      const uint8_t* p = b.data() + pu64(pg, 0) + 8;  // after the 8-byte page header
      const uint8_t* e = p + pu32(pg, 40);
      while (e - p >= 8) {
        const uint32_t len = fstx::get_u32(p + 4);
        if (p[0] == 1 && len >= 8 * 8) {  // T_PLAIN: the values
          vals = (uint8_t*)p + 8;
          break;
        }
        p += 8 + ((len + 7) / 8) * 8;
      }
    }
    if (!vals) {
      std::printf("%-22s NOT MADE (no plain block found)\n", name.c_str());
      continue;
    }
    fstx::put_u64(vals + 5 * 8, 0x8000000000000000ull);
    emit(name, b);
  }
  std::remove((g_out + "/base_none.fstx").c_str());
  std::remove((g_out + "/base_lz4.fstx").c_str());
  return 0;
}
