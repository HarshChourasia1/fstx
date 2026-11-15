// fstx fuzz target "forge": the test-only forge (fstx::forge_copy, behind fstx:::.fstx_forge) as an input surface.
// The control bytes give the forged fields (header bits, version, feature bits and names, profile, type, codec,
// page kind, encoding tag, NA mode of the first page); the forged copy is then opened and read. Checks: whatever
// the forge writes, the reader keeps the format rules (only format 3 opens, unknown required bit refused, header
// and footer bits agree) and never crashes.
#include "core_file.h"
#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < fz::kCtl) return 0;
  fz::Ctl c(data);
  std::vector<uint8_t> b = fz::file_bytes(data, size, c);
  static const std::string path = fz::tmp_path("forge"), out = fz::tmp_path("forge_out");
  if (!fz::write_file(path, b.data(), b.size())) return 0;
  ::unlink(out.c_str());
  fstx::Forge g;
  auto pick = [&](int byte, int bit, int v) { return c.bit(byte, bit) ? v : -1; };
  g.hdr_flags = pick(5, 0, (int)(c.next() & 0xffff));
  g.version = pick(5, 1, (int)c.below(6));
  g.phys0 = pick(5, 2, (int)(c.next() & 0xff));
  g.logical0 = pick(5, 3, (int)(c.next() & 0xff));
  g.codec0 = pick(5, 4, (int)(c.next() & 0xff));
  g.kind0 = pick(5, 5, (int)(c.next() & 0xf));
  g.tag0 = pick(5, 6, (int)(c.next() & 0xff));
  g.na0 = pick(5, 7, (int)(c.next() & 0xf));
  g.profile = pick(6, 0, (int)(c.next() & 0xff));
  if (c.bit(6, 1)) g.req = 1ull << c.below(64);
  if (c.bit(6, 2)) g.opt = 1ull << c.below(64);
  if (c.bit(6, 3)) g.opt |= 1ull << c.below(64);
  const uint32_t nn = c.bit(6, 4) ? (uint32_t)c.below(4) : 0;
  for (uint32_t i = 0; i < nn; ++i) {
    std::string s;
    const uint32_t len = (uint32_t)c.below(80);
    for (uint32_t k = 0; k < len; ++k) s.push_back(c.bit(6, 5) ? (char)(c.next() & 0xff) : (char)('a' + c.below(26)));
    g.names.push_back(s);
  }
  try {
    fstx::forge_copy(path, out, g);
  } catch (const std::exception&) {
    return 0;
  }
  std::vector<uint8_t> nb = fz::read_file(out);
  fstx_file* f = nullptr;
  fstx_error err;
  if (fstx_open(out.c_str(), &f, &err) == FSTX_OK) {
    const fz::Facts x = fz::facts_of(nb);
    if (!x.ok) fz::fail("opened a forged file whose footer does not verify");
    if (x.req & ~fstx::features_known()) fz::fail("opened a forged file with an unknown required bit");
    if (x.version != 3) fz::fail("opened a forged file that is not format 3");
    if ((fstx::get_u16(nb.data() + 6) & 0xffff) != (x.req & 0xffff)) fz::fail("header and footer bits differ");
    if (fz::table_small(f)) {
      fstx_file_info fi;
      fstx_file_info_get(f, &fi);
      fz::Table t;
      fz::read_range(f, 0, fi.n_rows, t, 1, c.bit(1, 0) ? 1 : 0);
    }
    fstx_close(f);
  }
  ::unlink(out.c_str());
  return 0;
}
