"""The Arrow stream's string layout (src/r_arrow.cpp): pages decoded and written into the Arrow buffers in
parallel, page by page; dictionary pages laid out as text; the UTF-8 check per batch. Strings with missing values
at every position of the validity bytes, dictionary and plain pages, row windows that start and end inside a
byte, batches of odd sizes, a file whose row groups mix dictionary and plain pages, row lists, and the same result
on one thread and on all."""
import random

import pyarrow as pa
import pyarrow.compute as pc
import pytest

import fstx

N = 200_003


def _table(n, seed, few_values=("red", "green", "blue", "")):
    rnd = random.Random(seed)
    words = ["alpha", "beta", "gamma", "delta", "épsilon", "zeta", "日本", "x" * 40]
    plain = [None if rnd.random() < 0.13 else
             " ".join(rnd.choice(words) for _ in range(rnd.randint(0, 12))) + str(i) for i in range(n)]
    few = [None if rnd.random() < 0.07 else rnd.choice(few_values) for _ in range(n)]
    nonull = ["row %d" % i for i in range(n)]
    return pa.table({"plain": pa.array(plain, pa.string()), "few": pa.array(few, pa.string()),
                     "nonull": pa.array(nonull, pa.string()), "i": pa.array(range(n), pa.int32())})


@pytest.fixture(scope="module")
def written(tmp_path_factory):
    t = _table(N, 7)
    p = tmp_path_factory.mktemp("layout") / "s.fstx"
    fstx.write_fstx(t, p)
    return t, p


def test_full_read_on_one_thread_and_all(written):
    t, p = written
    for th in (1, 3, None):
        r = fstx.read_fstx(p, threads=th)
        assert [f.type for f in r.schema] == [pa.string(), pa.string(), pa.string(), pa.int32()]
        assert r.equals(t)
    assert fstx.read_fstx(p, columns=["few"]).column(0).equals(t.column("few"))
    assert fstx.read_fstx(p, columns=["plain", "plain"]).column(1).equals(t.column("plain"))


@pytest.mark.parametrize("start,stop", [(0, 1), (1, 2), (3, 70_001), (5, 13), (65_531, 131_077),
                                        (N - 13, N), (N // 2 - 1, N // 2 + 9)])
def test_windows_start_and_end_inside_a_byte(written, start, stop):
    t, p = written
    assert fstx.read_fstx(p, rows=slice(start, stop)).equals(t.slice(start, stop - start))


def test_batches_of_odd_sizes(written):
    t, p = written
    with fstx.open(p) as f:
        for br in (1, 7, 9_999, 65_537):
            if br == 1:
                got = pa.Table.from_batches(f.to_batches(batch_rows=br, rows=slice(100, 140)))
                assert got.equals(t.slice(100, 40))
            else:
                assert pa.Table.from_batches(f.to_batches(batch_rows=br)).equals(t)


def test_row_groups_with_dictionary_and_plain_pages(tmp_path):
    a = _table(5_000, 1)
    b = _table(5_000, 2, few_values=tuple("value %d" % i for i in range(4_000)))  # too many for a dictionary
    p = tmp_path / "mix.fstx"
    fstx.write_fstx(a, p)
    fstx.write_fstx(b, p, append=True)
    want = pa.concat_tables([a, b])
    assert fstx.read_fstx(p).equals(want)
    assert fstx.read_fstx(p, rows=slice(4_990, 5_011)).equals(want.slice(4_990, 21))
    with fstx.open(p) as f:
        assert pa.Table.from_batches(f.to_batches()).equals(want)


def test_dictionary_row_group_then_a_short_plain_one(tmp_path):
    # released 1.0.2 raised "dictionary and plain pages mixed in column 's'": one write whose last row group is too
    # short for a dictionary. Full read, one column, windows across the last group, batches, row lists, filters.
    n = 1_048_581
    t = pa.table({"s": [f"s{i % 5000}" for i in range(n)], "i": pa.array(range(n), pa.int32())})
    p = tmp_path / "tail.fstx"
    fstx.write_fstx(t, p)
    for th in (1, None):
        assert fstx.read_fstx(p, threads=th).equals(t)
    assert fstx.read_fstx(p, columns=["s"]).column(0).equals(t.column("s"))
    assert fstx.read_fstx(p, rows=slice(n - 9, n)).equals(t.slice(n - 9, 9))
    with fstx.open(p) as f:
        assert pa.Table.from_batches(f.to_batches(batch_rows=100_003)).equals(t)
    rows = [n - 1, 3, n - 5, 1_048_575, 1_048_576]
    assert fstx.read_fstx(p, rows=rows).equals(t.take(rows))
    assert fstx.read_fstx(p, filters=[("i", ">=", n - 7)]).equals(t.slice(n - 7, 7))
    assert fstx.read_fstx(p, filters=[("s", "==", "s5")]).equals(t.filter(pc.equal(t["s"], "s5")))


def test_row_lists_lay_out_dictionary_pages_as_text(written):
    t, p = written
    rows = [5, 3, 3, 100_000, N - 1, 0]
    got = fstx.read_fstx(p, rows=rows)
    assert got.column("few").type == pa.string() and got.equals(t.take(rows))
    assert fstx.read_fstx(p, filters=[("few", "==", "red")]).equals(t.filter(pc.equal(t["few"], "red")))


def test_dictionary_columns_with_and_without_as_factor(written):
    t, p = written
    fa = fstx.read_fstx(p, as_factor=["plain"])  # the other columns come from the stream as dictionaries here
    assert fa.column("few").type == pa.string() and fa.column("few").equals(t.column("few"))
    assert fa.column("plain").to_pylist() == t.column("plain").to_pylist()


def test_utf8_check_is_per_batch_and_per_path():
    from conftest import GOLDEN
    p = GOLDEN / "strings_invalid_utf8.fstx"  # rows 1 and 2 are not valid UTF-8
    assert fstx.read_fstx(p, rows=[0, 3, 4], invalid_utf8="error").column("s").to_pylist() == ["ok", "café", None]
    assert fstx.read_fstx(p, rows=slice(3, 5), invalid_utf8="error").num_rows == 2
    with pytest.raises(ValueError, match="not valid UTF-8"):
        fstx.read_fstx(p, rows=[2], invalid_utf8="error")
    with pytest.raises(ValueError, match="not valid UTF-8"):
        fstx.read_fstx(p, rows=slice(1, 2), invalid_utf8="error")
    with fstx.open(p) as f:
        with pytest.warns(fstx.FstxWarning) as rec:
            got = [b.column("s").to_pylist() for b in f.to_batches(batch_rows=1)]
        assert len(rec) == 1
        assert got == [["ok"], ["bad�byte"], ["�"], ["café"], [None]]
        raw = [b.column("s").to_pylist() for b in f.to_batches(batch_rows=2, invalid_utf8="binary")]
        assert raw == [[b"ok", bytes.fromhex("626164ff62797465")], [bytes.fromhex("e0a4"), "café".encode()], [None]]
    b = fstx.read_fstx(p, rows=[3, 1], invalid_utf8="binary")
    assert b.column("s").type == pa.binary() and b.column("s").to_pylist() == ["café".encode(), b"bad\xffbyte"]


def test_page_buffers_kept_between_batches(tmp_path):
    # row groups whose strings grow, shrink and grow again: buffers kept from one batch are too small, too big
    p = tmp_path / "sizes.fstx"
    parts = []
    for k, width in enumerate((3, 400, 1, 90, 2000)):
        n = 3_000 + 1_000 * k
        s = pa.array([None if (i * 7 + k) % 11 == 0 else ("%d-" % i) + "y" * (width + i % 5) for i in range(n)])
        parts.append(pa.table({"s": s, "d": pa.array(["a", "bb", None, "ccc"] * (n // 4) + ["a"] * (n % 4))}))
        fstx.write_fstx(parts[-1], p, append=k > 0)
    want = pa.concat_tables(parts)
    for th in (1, None):
        assert fstx.read_fstx(p, threads=th).equals(want)
    with fstx.open(p) as f:
        assert pa.Table.from_batches(f.to_batches(batch_rows=1_234)).equals(want)


def test_decode_state_kept_between_batches(tmp_path):
    # zstd pages of every kind, read batch after batch: the workers' codec contexts and buffers are reused
    n = 150_001
    t = pa.table({"i": pa.array([None if k % 97 == 0 else k * 3 for k in range(n)], pa.int32()),
                  "d": pa.array([k / 7 for k in range(n)], pa.float64()),
                  "b": pa.array([None if k % 5 == 0 else k % 3 == 0 for k in range(n)], pa.bool_()),
                  "s": pa.array(["v%d" % (k % 1_000) * (1 + k % 4) for k in range(n)]),
                  "f": pa.array([None if k % 13 == 0 else "lv%d" % (k % 7) for k in range(n)]).dictionary_encode()})
    p = tmp_path / "z.fstx"
    fstx.write_fstx(t, p, compress="zstd")
    with fstx.open(p) as f:
        for br in (None, 8_191, 50_000):
            got = pa.Table.from_batches(f.to_batches(batch_rows=br))
            assert got.drop_columns(["f"]).equals(t.drop_columns(["f"]))
            assert got.column("f").to_pylist() == t.column("f").to_pylist()
    assert fstx.read_fstx(p, columns=["d"], threads=2).column(0).equals(t.column("d"))


@pytest.mark.parametrize("compress", ["none", "lz4", "zstd"])
def test_pages_copied_out_of_the_file(tmp_path, compress):
    # the stream reads every page with a positional read instead of from the mapping: every codec, plain strings
    # stored without a codec (they used to be read in place), a dictionary column, numbers with missing values
    n = 70_001
    t = pa.table({"s": pa.array([None if k % 9 == 0 else "s%07d" % k for k in range(n)]),
                  "c": pa.array(["x", "yy", "zzz"] * (n // 3) + ["x"] * (n % 3)),
                  "i": pa.array([None if k % 31 == 0 else k for k in range(n)], pa.int32()),
                  "d": pa.array([k * 0.25 for k in range(n)], pa.float64())})
    p = tmp_path / ("c_%s.fstx" % compress)
    fstx.write_fstx(t, p, compress=compress)
    assert fstx.read_fstx(p).equals(t)
    assert fstx.read_fstx(p, rows=slice(12_345, 54_321)).equals(t.slice(12_345, 54_321 - 12_345))
    with fstx.open(p) as f:
        assert pa.Table.from_batches(f.to_batches(batch_rows=9_000)).equals(t)
