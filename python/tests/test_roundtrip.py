"""Reading and writing together: what read_fstx hands out, write_fstx stores again the way R wrote it.

R checks the files these tests leave in golden/from_python/ (check_roundtrip.R: the golden cases written by
test_write.py; check_native.R: the partial reads written here). Here the same files are read back in Python.
"""
import json
import threading
import warnings

import pyarrow as pa
import pytest

import fstx
import golden_tables
from conftest import CASES, DROP2, GOLDEN, ids, quiet, same
from fstx import _fxa1 as fxa
from fstx import _meta
from fstx._read import row_names

OUT = GOLDEN / "from_python"
NATIVE = OUT / "native"
NATIVE.mkdir(parents=True, exist_ok=True)
HINDI = "हिन्दी"
MANIFEST = {c["file"]: c for c in json.loads((GOLDEN / "manifest.json").read_text(encoding="utf-8"))["cases"]}


def write(data, path, **kw):
    """write_fstx with every warning an error."""
    with warnings.catch_warnings():
        warnings.simplefilter("error", UserWarning)
        fstx.write_fstx(data, path, **kw)
    return path


def bare(t: pa.Table) -> pa.Table:
    """The table without any metadata."""
    if not t.num_columns:  # (replace_schema_metadata would lose the row count of a table without columns)
        return t
    return pa.Table.from_arrays(t.columns, schema=pa.schema([pa.field(f.name, f.type) for f in t.schema]))


def table_attrs(t: pa.Table):
    b = _meta.blob((t.schema.metadata or {}).get(b"fstx.table_attrs"))
    return fxa.decode(b) if fxa.is_fxa1(b) else b


# ---------------------------------------------------------------- the reader against the sidecars
@pytest.mark.parametrize("path,want", CASES, ids=ids(CASES))
def test_reader_gives_the_sidecar_values(path, want):
    # golden_tables builds the table from the JSON sidecar without the reader
    for mode in ("replace", "binary"):
        expect = golden_tables.golden_table(path, want, invalid_utf8=mode)
        got = bare(quiet(path, invalid_utf8=mode))
        assert got.num_rows == expect.num_rows == want["nrow"]
        assert same(got, expect), mode


# ---------------------------------------------------------------- golden file -> read -> write -> read
@pytest.mark.parametrize("path,want", CASES, ids=ids(CASES))
def test_pyarrow_round_trip_reads_back_the_same(path, want, tmp_path):
    t = quiet(path)
    back = quiet(write(t, tmp_path / "a.fstx"))
    assert same(back, t)  # values and every column's fstx.r / fstx.attrs
    if path.stem == "datatable":  # the secondary index goes (the rows may have moved), the key stays
        a, b = table_attrs(t), table_attrs(back)
        del a["index"]
        assert a == b
    else:
        assert back.schema.metadata == t.schema.metadata
    # a second trip changes nothing more
    again = quiet(write(back, tmp_path / "b.fstx"))
    assert same(again, back, schema_metadata=True)
    assert fstx.metadata(tmp_path / "b.fstx").table_attrs == fstx.metadata(tmp_path / "a.fstx").table_attrs


@pytest.mark.parametrize("path,want", CASES, ids=ids(CASES))
def test_binary_and_float_reads_write_back_the_stored_bytes(path, want, tmp_path):
    # datetimes="float" and invalid_utf8="binary" hand out exactly what is stored: the file comes back whole
    with fstx.open(path) as f:
        t = f.read(datetimes="float", invalid_utf8="binary")
    was = fstx.metadata(path)
    m = fstx.metadata(write(t, tmp_path / "a.fstx"))
    assert [(c.name, c.phys, c.logical, c.ordered, c.levels, c.tzone, c.attrs) for c in m.columns] == \
        [(c.name, c.phys, c.logical, c.ordered, c.levels, c.tzone, c.attrs) for c in was.columns]
    with fstx.open(tmp_path / "a.fstx") as f:
        back = f.read(datetimes="float", invalid_utf8="binary")
    assert same(back, t)


@pytest.mark.parametrize("path,want", CASES, ids=ids(CASES))
def test_pandas_round_trip(path, want, tmp_path):
    pytest.importorskip("pandas")
    t = quiet(path)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", fstx.FstxWarning)  # the factor with an NA level becomes strings
        df = fstx.to_pandas(t)
    json.dumps(df.attrs["fstx"])
    back = quiet(write(df, tmp_path / "a.fstx"))
    assert back.num_rows == t.num_rows and back.column_names == t.column_names
    if path.stem == "factor_na_level":
        assert back.column("f").type == pa.string()
        assert back.column("f").to_pylist() == t.column("f").cast(pa.string()).to_pylist()
        return
    # nothing of how R held the table is lost: types, classes, attribute blobs, row names
    assert [f.type for f in back.schema] == [f.type for f in t.schema]
    assert [f.metadata for f in back.schema] == [f.metadata for f in t.schema]
    assert row_names(back.schema) == row_names(t.schema)
    if path.stem != "datatable":
        assert back.schema.metadata == t.schema.metadata
    if MANIFEST[path.name]["roundtrip_pandas"].startswith("identical"):
        assert same(back, t)
    # the second trip through pandas gives the same file as the first
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", fstx.FstxWarning)
        again = quiet(write(fstx.to_pandas(back), tmp_path / "b.fstx"))
    assert same(again, back, schema_metadata=True)


@pytest.mark.parametrize("path,want", CASES, ids=ids(CASES))
def test_polars_round_trip(path, want, tmp_path):
    pl = pytest.importorskip("polars")
    t = quiet(path)
    if len(set(t.column_names)) != len(t.column_names):
        return  # polars refuses repeated names (test_read.py)
    if path.stem != "factor_na_level":
        # the table goes into polars as it is: its metadata is ASCII text, which polars takes
        assert pl.from_arrow(t).shape == (t.num_rows, t.num_columns)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", fstx.FstxWarning)  # the factor with an NA level becomes strings
        df = fstx.to_polars(t)
    if path.stem == "integer64" and not DROP2:
        pytest.skip("waits for engine drop 2: through polars (no metadata) the values past int32 become an int64 column")
    back = quiet(write(df, tmp_path / "a.fstx"))
    assert back.column_names == t.column_names
    if t.num_columns:
        assert back.num_rows == t.num_rows
    # polars has no metadata: the table comes back a plain data frame, plain values as they were
    assert table_attrs(back) == {"class": fxa.character(["data.frame"])}
    for f in t.schema:
        r = json.loads(f.metadata[b"fstx.r"])
        if r["logical"] == "none" and "class" not in r and r["phys"] in ("logical", "character"):
            assert back.column(f.name).to_pylist() == t.column(f.name).to_pylist(), f.name


# ---------------------------------------------------------------- metadata that other libraries can carry
def _is_text(md):
    for k, v in (md or {}).items():
        k.decode("ascii")
        v.decode("ascii")
    return True


@pytest.mark.parametrize("path,want", CASES, ids=ids(CASES))
def test_metadata_is_ascii_text(path, want):
    t = quiet(path)
    assert _is_text(t.schema.metadata) and all(_is_text(f.metadata) for f in t.schema)
    assert all(b"fstx.r" in f.metadata for f in t.schema)


def test_polars_takes_tables_with_r_serialized_and_non_ascii_blobs(tmp_path):
    pl = pytest.importorskip("polars")
    # R serialization: bytes that are not text at all
    ser = fstx.read_fstx(GOLDEN / "attrs_serialized.fstx")
    raw = fstx.metadata(GOLDEN / "attrs_serialized.fstx")
    assert not fxa.is_fxa1(raw.table_attrs) and any(c.attrs and not fxa.is_fxa1(c.attrs) for c in raw.columns)
    df = pl.from_arrow(ser)
    assert df.shape == (ser.num_rows, ser.num_columns) and df.columns == ser.column_names
    assert pl.DataFrame(ser).shape == df.shape
    # FXA1 blobs with text that is not ASCII, and with bytes that are not UTF-8 at all (a latin1 label)
    col = fxa.encode({"label": fxa.character([HINDI, b"caf\xe9".decode("utf-8", "surrogateescape")])})
    tab = fxa.encode({"class": fxa.character(["data.frame"]), "note": fxa.character([HINDI]),
                      "row.names": fxa.character(["r" + HINDI, "b"])})
    src = pa.table({"x": pa.array([1, 2], pa.int32()), "s": ["a", HINDI]})
    src = src.replace_schema_metadata({b"fstx.table_attrs": tab})  # raw bytes are taken by the writer
    src = src.set_column(0, pa.field("x", pa.int32(), metadata={b"fstx.attrs": col}), src.column(0))
    p = write(src, tmp_path / "hindi.fstx")
    m = fstx.metadata(p)
    assert m.columns[0].attrs == col and m.table_attrs == tab
    t = fstx.read_fstx(p)
    assert _meta.blob(t.schema.field("x").metadata[b"fstx.attrs"]) == col
    assert row_names(t.schema) == ["r" + HINDI, "b"]
    df = pl.from_arrow(t)
    assert df["x"].to_list() == [1, 2] and df["s"].to_list() == ["a", HINDI]
    assert pl.DataFrame(t).equals(df)
    # and back from the table: byte for byte
    m2 = fstx.metadata(write(t, tmp_path / "hindi2.fstx"))
    assert m2.columns[0].attrs == col and m2.table_attrs == tab


def test_writer_takes_base64_and_raw_blobs_alike(tmp_path):
    blob = fxa.encode({"label": fxa.character(["x"])})
    tab = fxa.encode({"class": fxa.character(["data.frame"]), "note": fxa.character(["n"])})
    got = []
    for enc in (lambda b: b, _meta.b64, lambda b: _meta.b64(b).decode("ascii")):
        t = pa.table({"x": pa.array([1], pa.int32())})
        t = t.set_column(0, pa.field("x", pa.int32(), metadata={"fstx.attrs": enc(blob)}), t.column(0))
        t = t.replace_schema_metadata({"fstx.table_attrs": enc(tab)})
        m = fstx.metadata(write(t, tmp_path / "a.fstx"))
        got.append((m.columns[0].attrs, m.table_attrs))
    assert got == [(blob, tab)] * 3


def test_columns_without_fstx_r_are_plain_data(tmp_path):
    t = quiet(GOLDEN / "basic.fstx")
    stripped = bare(t)
    m = fstx.metadata(write(stripped, tmp_path / "a.fstx"))
    was = fstx.metadata(GOLDEN / "basic.fstx")
    assert [(c.name, c.phys, c.logical) for c in m.columns] == [(c.name, c.phys, c.logical) for c in was.columns]
    # some columns with fstx.r and some without, in one table
    mixed = t.set_column(0, pa.field(t.schema.field(0).name, t.schema.field(0).type), t.column(0))
    assert same(quiet(write(mixed, tmp_path / "b.fstx")), t)


# ---------------------------------------------------------------- a partial read, then a write
def test_partial_reads_write_the_right_row_names(tmp_path):
    p = GOLDEN / "rownames_char.fstx"
    cases = {
        "rownames_char.rows20": dict(rows=[2, 0]),
        "rownames_char.slice": dict(rows=slice(1, 3)),
        "rownames_char.repeat": dict(rows=[1, 1, 1]),
        "rownames_char.filter": dict(filters=[("v", ">", 10)]),
    }
    want = {"rownames_char.rows20": ["rन", "r1"], "rownames_char.slice": ["r2", "rन"],
            "rownames_char.repeat": ["r2", "r2.1", "r2.2"], "rownames_char.filter": ["r2", "rन"]}
    for stem, kw in cases.items():
        t = fstx.read_fstx(p, **kw)
        m = fstx.metadata(write(t, NATIVE / (stem + ".fstx")))
        assert fxa.decode(m.table_attrs)["row.names"] == fxa.character(want[stem]), stem
        assert m.nrow == len(want[stem])
        assert row_names(fstx.read_fstx(NATIVE / (stem + ".fstx")).schema) == want[stem]
    ints = fstx.read_fstx(GOLDEN / "rownames_int.fstx", rows=[1, 0])
    m = fstx.metadata(write(ints, NATIVE / "rownames_int.rows10.fstx"))
    assert fxa.decode(m.table_attrs)["row.names"] == fxa.integer(row_names(ints.schema))
    # all rows in file order through the open file: nothing changes
    with fstx.open(p) as f:
        whole = f.read(rows=slice(0, None))
    assert fstx.metadata(write(whole, tmp_path / "w.fstx")).table_attrs == fstx.metadata(p).table_attrs
    # no rows: the names go with them
    none = fstx.read_fstx(p, rows=[])
    assert fxa.decode(fstx.metadata(write(none, tmp_path / "n.fstx")).table_attrs)["row.names"].values == []


def test_partial_read_through_pandas_writes_the_right_row_names():
    pytest.importorskip("pandas")
    df = fstx.to_pandas(fstx.read_fstx(GOLDEN / "rownames_char.fstx", rows=[2, 0]))
    assert df.index.tolist() == ["rन", "r1"]
    m = fstx.metadata(write(df, NATIVE / "rownames_char.rows20.pandas.fstx"))
    assert fxa.decode(m.table_attrs)["row.names"] == fxa.character(["rन", "r1"])
    # rows dropped in pandas: the index is what is stored
    m = fstx.metadata(write(fstx.to_pandas(fstx.read_fstx(GOLDEN / "rownames_char.fstx")).iloc[[1]],
                            NATIVE / "rownames_char.iloc1.pandas.fstx"))
    assert fxa.decode(m.table_attrs)["row.names"] == fxa.character(["r2"])


def test_partial_read_of_columns_keeps_their_metadata(tmp_path):
    path = GOLDEN / "classes_blob.fstx"
    was = {c.name: c for c in fstx.metadata(path).columns}
    names = fstx.metadata(path).names
    t = fstx.read_fstx(path, columns=names[::-1][:3])
    m = fstx.metadata(write(t, NATIVE / "classes_blob.part.fstx"))
    assert m.names == names[::-1][:3] and m.nrow == fstx.metadata(path).nrow
    for c in m.columns:
        assert (c.phys, c.logical, c.attrs) == (was[c.name].phys, was[c.name].logical, was[c.name].attrs)


def test_partial_read_with_r_serialized_table_blob(tmp_path):
    path = GOLDEN / "attrs_serialized.fstx"
    was = fstx.metadata(path)
    whole = fstx.read_fstx(path)
    assert int(whole.schema.metadata[b"fstx.nrow"]) == was.nrow
    assert fstx.metadata(write(whole, tmp_path / "a.fstx")).table_attrs == was.table_attrs
    part = fstx.read_fstx(path, rows=slice(0, 1))
    assert b"fstx.nrow" not in part.schema.metadata
    if b"row.names" in was.table_attrs:  # it may hold row names for the rows that are gone: dropped, said so
        with pytest.warns(UserWarning, match="R serialization"):
            fstx.write_fstx(part, tmp_path / "b.fstx")
        assert fxa.decode(fstx.metadata(tmp_path / "b.fstx").table_attrs) == {"class": fxa.character(["data.frame"])}
    else:
        assert fstx.metadata(write(part, tmp_path / "b.fstx")).table_attrs == was.table_attrs
    # the column blobs are kept either way
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        fstx.write_fstx(part, tmp_path / "c.fstx")
    assert [c.attrs for c in fstx.metadata(tmp_path / "c.fstx").columns] == [c.attrs for c in was.columns]


# ---------------------------------------------------------------- one engine, many threads
def test_reads_and_writes_from_threads_at_once(tmp_path):
    big = GOLDEN / "codec_zstd1.fstx"  # 2,000 rows
    full = fstx.read_fstx(big)
    n = 60000
    made = pa.table({"x": pa.array(range(n), pa.int32()), "s": pa.array([f"v{i % 977}" for i in range(n)])})
    errs = []
    start = threading.Barrier(8)

    def guard(fn):
        def run(k):
            try:
                start.wait(timeout=60)
                for i in range(6):
                    fn(k, i)
            except Exception as e:  # noqa: BLE001
                errs.append(repr(e))
        return run

    def writer(k, i):
        p = tmp_path / f"w{k}.fstx"
        fstx.write_fstx(made, p, threads=2)
        assert fstx.read_fstx(p, threads=2).equals(made)

    def reader(k, i):
        assert fstx.read_fstx(big, threads=1 + k % 3).equals(full)

    def filterer(k, i):
        got = fstx.read_fstx(big, filters=[("i", ">", 1500)], columns=["i"])
        assert got.num_rows == sum(1 for v in full.column("i").to_pylist() if v is not None and v > 1500)

    def batcher(k, i):
        with fstx.open(big) as f:
            rows = sum(b.num_rows for b in f.to_batches(batch_rows=300))
        assert rows == 2000

    def appender(k, i):
        p = tmp_path / f"a{k}.fstx"
        fstx.write_fstx(made.slice(0, 1000), p, append=True)  # each thread its own file
        assert fstx.metadata(p).nrow == 1000 * (i + 1)

    jobs = [writer, writer, reader, reader, filterer, batcher, appender, reader]
    th = [threading.Thread(target=guard(fn), args=(k,)) for k, fn in enumerate(jobs)]
    for x in th:
        x.start()
    for x in th:
        x.join(timeout=300)
    assert not any(x.is_alive() for x in th)
    assert errs == []


# Two threads appending to the SAME file. Until 18 Nov 2026 an append read the file's metadata before it took the
# engine lock, and appends failed at random ("file is in use by another process", the engine's "memory-mapped by a
# lazy read_fstx()" check, or "the new levels must start with the file's levels" when both threads added factor
# levels). Now planning and writing are one step under the lock: every append must succeed, whole, with the
# right levels.


@pytest.mark.parametrize("kind", ["plain", "factor_levels"])
def test_two_threads_appending_to_one_file(tmp_path, kind):
    n, rounds = 1000, 20
    p = tmp_path / "a.fstx"

    def make(k, i):
        ids = pa.array([(k + 1) * 100000 + i] * n, pa.int32())
        if kind == "plain":
            return pa.table({"id": ids, "s": [f"t{k}r{i}"] * n})
        lv = ["a", "b"] if k < 0 else ["a", "b", f"t{k}_{i}"]
        return pa.table({"id": ids, "f": pa.array([lv[j % len(lv)] for j in range(n)]).dictionary_encode()})

    fstx.write_fstx(make(-1, 0), p)
    done, errs = {0: [], 1: []}, []
    start = threading.Barrier(2)

    def go(k):
        start.wait(timeout=60)
        for i in range(rounds):
            try:
                fstx.write_fstx(make(k, i), p, append=True)
                done[k].append(i)
            except Exception as e:  # noqa: BLE001
                errs.append(f"thread {k} round {i}: {type(e).__name__}: {e}")

    th = [threading.Thread(target=go, args=(k,)) for k in (0, 1)]
    for x in th:
        x.start()
    for x in th:
        x.join(timeout=300)
    assert not any(x.is_alive() for x in th)
    assert errs == [], " | ".join(errs)
    assert done == {0: list(range(rounds)), 1: list(range(rounds))}
    t = fstx.read_fstx(p)
    assert t.num_rows == n * (1 + len(done[0]) + len(done[1]))
    ids = t.column("id").to_pylist()
    for k in (0, 1):  # every append that returned is there, whole; nothing else
        got = sorted({v - (k + 1) * 100000 for v in ids if v // 100000 == k + 1})
        assert got == sorted(done[k])
        assert all(ids.count((k + 1) * 100000 + i) == n for i in done[k])
    if kind == "factor_levels":  # each row shows its own thread's level, never the other one's
        for v, i in zip(t.column("f").to_pylist(), ids):
            k, r = divmod(i, 100000)
            assert v in ("a", "b") or (k > 0 and v == f"t{k - 1}_{r}")


def test_abandoned_batch_reader_does_not_block_a_write(tmp_path):
    big = GOLDEN / "codec_zstd1.fstx"
    f = fstx.open(big)
    r = f.to_batches(batch_rows=100)
    assert r.read_next_batch().num_rows == 100  # the stream stays open, the engine lock is not held
    t = pa.table({"x": pa.array([1, 2, 3], pa.int32())})
    fstx.write_fstx(t, tmp_path / "a.fstx")
    assert fstx.read_fstx(tmp_path / "a.fstx").column("x").to_pylist() == [1, 2, 3]
    assert r.read_next_batch().num_rows == 100
    del r
    f.close()
    fstx.write_fstx(t, tmp_path / "a.fstx")
