"""The Arrow stream's string layout (src/r_arrow.cpp): pages decoded and written into the Arrow buffers in
parallel, page by page. Strings with missing values at every position of the validity bytes, dictionary and
plain pages, row windows that start and end inside a byte, batches of odd sizes, a file whose row groups mix
dictionary and plain pages, and the same result on one thread and on all."""
import random

import pyarrow as pa
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
