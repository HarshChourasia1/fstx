"""The stream's row-list batches (src/arrow_rows.h): filters and lists of rows on files with several pages per
column and several row groups, for every column type, with and without a codec, against the full read."""
import datetime as dt
import random

import pyarrow as pa
import pyarrow.compute as pc
import pytest

import fstx
from conftest import same

N1, N2 = 70_000, 30_000  # 70,000 rows: three pages per fixed-width column; the append adds a second row group


def _table(n, seed):
    rnd = random.Random(seed)
    words = ["w%03d" % k for k in range(300)]

    def maybe(v, p=0.05):
        return None if rnd.random() < p else v

    i = [maybe(rnd.randrange(-1000, 1000)) for _ in range(n)]
    d = []
    for _ in range(n):
        r = rnd.random()
        d.append(None if r < 0.03 else float("nan") if r < 0.05 else rnd.uniform(-100, 100))
    b = [maybe(rnd.random() < 0.3) for _ in range(n)]
    day = [maybe(dt.date(2020, 1, 1) + dt.timedelta(days=rnd.randrange(3000))) for _ in range(n)]
    ts = [maybe(dt.datetime(2021, 1, 1, tzinfo=dt.timezone.utc) + dt.timedelta(seconds=rnd.randrange(10**8),
                                                                              microseconds=rnd.randrange(10**6)))
          for _ in range(n)]
    lv = ["lo", "mid", "hi", "über"]
    fac = [maybe(rnd.randrange(4)) for _ in range(n)]
    code = [maybe("c%02d" % rnd.randrange(40), 0.02) for _ in range(n)]
    text = [maybe(" ".join(rnd.choice(words) for _ in range(rnd.randrange(0, 9))), 0.1) for _ in range(n)]
    uniq = [maybe("u-%d-%s" % (k, "x" * rnd.randrange(30))) for k in range(n)]
    return pa.table({
        "i": pa.array(i, pa.int32()),
        "d": pa.array(d, pa.float64()),
        "b": pa.array(b, pa.bool_()),
        "day": pa.array(day, pa.date32()),
        "ts": pa.array(ts, pa.timestamp("us", "UTC")),
        "fac": pa.DictionaryArray.from_arrays(pa.array(fac, pa.int32()), pa.array(lv)),
        "code": pa.array(code, pa.string()),
        "text": pa.array(text, pa.string()),
        "uniq": pa.array(uniq, pa.string()),
    })


@pytest.fixture(scope="module", params=["fast", "balanced"])
def two_groups(request, tmp_path_factory):
    p = tmp_path_factory.mktemp("rows") / ("t_%s.fstx" % request.param)
    fstx.write_fstx(_table(N1, 1), p, profile=request.param)
    fstx.write_fstx(_table(N2, 2), p, append=True)
    return p


def _mask(t, m):
    return t.filter(pc.fill_null(m, False))


@pytest.mark.parametrize("threads", [1, None])
def test_filters_match_the_full_read(two_groups, threads):
    full = fstx.read_fstx(two_groups, threads=threads)
    assert full.num_rows == N1 + N2
    c = {k: full.column(k) for k in full.column_names}
    cases = [
        ([("code", "==", "c07")], pc.equal(c["code"], "c07")),                          # about 1 row in 40
        ([("code", "in", ["c01", "c39", None])], pc.or_(pc.is_in(c["code"], pa.array(["c01", "c39"])),
                                                       c["code"].is_null())),
        ([("i", ">", 990)], pc.greater(c["i"], 990)),                                   # about 1 row in 200
        ([("i", "<", 0)], pc.less(c["i"], 0)),                                          # about half
        ([("d", "is_null")], pc.fill_null(pc.is_nan(c["d"]), True)),
        ([("text", "==", "")], pc.equal(c["text"], "")),
        ([("uniq", "==", "u-12345-")], pc.equal(c["uniq"], "u-12345-")),
        ([("i", ">", -2000)], pc.greater(c["i"], -2000)),                               # every row that is not NA
        ([("fac", "==", "über"), ("b", "==", True)], pc.and_(pc.equal(c["fac"].cast(pa.string()), "über"), c["b"])),
        ([("i", ">", 5000)], pc.greater(c["i"], 5000)),                                 # no row
    ]
    for flt, m in cases:
        want = _mask(full, m)
        got = fstx.read_fstx(two_groups, filters=flt, threads=threads)
        assert got.num_rows == want.num_rows, flt
        assert same(got, want), flt
        cols = ["text", "d", "code"]
        assert same(fstx.read_fstx(two_groups, columns=cols, filters=flt, threads=threads), want.select(cols)), flt
    # a window with the filter, and batches
    w = fstx.read_fstx(two_groups, rows=slice(60_000, 80_000), filters=[("i", "<", -900)], threads=threads)
    assert same(w, _mask(full.slice(60_000, 20_000), pc.less(c["i"].slice(60_000, 20_000), -900)))
    with fstx.open(two_groups) as f:
        rd = f.to_batches(batch_rows=333, filters=[("i", "<", 0)], threads=threads)
        bs = list(rd)
    assert all(b.num_rows <= 333 for b in bs)
    assert same(pa.Table.from_batches(bs, schema=rd.schema), _mask(full, pc.less(c["i"], 0)))


@pytest.mark.parametrize("threads", [1, None])
def test_row_lists_match_the_full_read(two_groups, threads):
    full = fstx.read_fstx(two_groups, threads=threads)
    n = full.num_rows
    rnd = random.Random(7)
    lists = [
        sorted(rnd.sample(range(n), 50)),                       # sparse, ascending
        sorted(rnd.sample(range(n), 20_000)),                   # dense, ascending
        [5, 5, 5, 69_999, 70_000, 70_000, n - 1],               # repeats, across the row-group edge
        [rnd.randrange(n) for _ in range(3000)],                # any order, repeats
        list(range(0, n, 1023)),                                # one row per 1,024-row vector, nearly
        [n - 1, 0],
    ]
    for rows in lists:
        got = fstx.read_fstx(two_groups, rows=rows, threads=threads)
        assert same(got, full.take(pa.array(rows, pa.int64()))), rows[:5]


def test_as_factor_after_a_filter(two_groups):
    full = fstx.read_fstx(two_groups, as_factor=True)
    got = fstx.read_fstx(two_groups, filters=[("i", ">", 900)], as_factor=True)
    assert got.schema.field("code").type == full.schema.field("code").type
    want = _mask(full, pc.greater(full.column("i"), 900))
    assert got.column("code").cast(pa.string()).equals(want.column("code").cast(pa.string()))


def test_corrupt_page_is_refused_by_filters_and_row_lists(two_groups, tmp_path):
    # pages of rows plans and filters are copied out of the file (no mapping): their checksums are still checked
    bad = tmp_path / "bad.fstx"
    raw = bytearray(two_groups.read_bytes())
    mid = len(raw) // 2
    raw[mid:mid + 4096] = bytes(b ^ 0xFF for b in raw[mid:mid + 4096])
    bad.write_bytes(bytes(raw))
    for kw in (dict(filters=[("i", ">", -2000)]), dict(rows=list(range(0, N1 + N2, 7)))):
        with pytest.raises(fstx.FstxError, match="checksum"):
            fstx.read_fstx(bad, **kw)


def test_reads_one_after_another_do_not_mix(two_groups, tmp_path):
    # per-thread buffers outlive a read: a read of another file right after must not see the first one's bytes
    other = tmp_path / "other.fstx"
    fstx.write_fstx(_table(5000, 9), other, profile="balanced")
    fa, fb = fstx.read_fstx(two_groups), fstx.read_fstx(other)
    for _ in range(3):
        for p, full in ((two_groups, fa), (other, fb)):
            got = fstx.read_fstx(p, filters=[("i", ">", 500)])
            assert same(got, _mask(full, pc.greater(full.column("i"), 500)))


@pytest.mark.parametrize("batch_rows", [None, 777])
def test_batches_of_a_selection_keep_their_row_groups(two_groups, batch_rows):
    # a selection that spans row groups is decoded once and cut into the same batches as before: one per row group
    # (or batch_rows at most), never across a row-group edge
    full = fstx.read_fstx(two_groups)
    m = pc.less(full.column("i"), -500)
    want = _mask(full, m)
    n1 = pc.sum(pc.fill_null(m.slice(0, N1), False).cast(pa.int64())).as_py()
    with fstx.open(two_groups) as f:
        bs = list(f.to_batches(batch_rows=batch_rows, filters=[("i", "<", -500)]))
    sizes = [b.num_rows for b in bs]
    if batch_rows is None:
        assert sizes == [n1, want.num_rows - n1]
    else:
        assert max(sizes) <= batch_rows
        edge = 0
        for k in sizes:
            assert not (edge < n1 < edge + k)  # no batch holds rows of both row groups
            edge += k
    assert same(pa.Table.from_batches(bs), want)


def test_dictionary_columns_after_a_filter_carry_their_row_groups_dictionary(two_groups):
    # dictionary columns come from the rows plan as codes plus the row group's dictionary (fstx_plan_rows_codes)
    full = fstx.read_fstx(two_groups, as_factor=["code"])
    with fstx.open(two_groups) as f:
        rd = f.to_batches(filters=[("i", ">", 800)])
        typ = rd.schema.field("code").type
        bs = list(rd)
    got = pa.Table.from_batches(bs)
    want = _mask(full, pc.greater(full.column("i"), 800))
    assert got.column("code").cast(pa.string()).equals(want.column("code").cast(pa.string()))
    for rows in ([0, 5, 69_999], [70_000, 99_999], [3, 70_001]):
        assert fstx.read_fstx(two_groups, rows=rows, columns=["code"]).column(0).equals(
            full.column("code").cast(pa.string()).take(rows))
