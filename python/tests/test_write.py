"""write_fstx: the FXA1 encoder, the type mapping, refusals, options, append, pandas and polars.

What a written file holds is checked two ways:
  * here, through fstx.metadata(): column definitions, levels, time zones, attribute blobs, row counts
    (test_roundtrip.py reads the files back with read_fstx as well);
  * in R, which reads the values: every file these tests write stays in golden/from_python/ for
    golden/check_roundtrip.R (the golden cases: read_fstx -> write_fstx, also through fstx.to_pandas and
    fstx.to_polars) and golden/check_native.R (golden/from_python/native/).
"""
import datetime as dt
import json
import threading
import warnings

import pyarrow as pa
import pytest

import fstx
from conftest import CASES, DROP2, GOLDEN, ids, quiet
from fstx import _fxa1 as fxa

to_pandas = fstx.to_pandas

OUT = GOLDEN / "from_python"
NATIVE = OUT / "native"
NATIVE.mkdir(parents=True, exist_ok=True)
HINDI = "हिन्दी"


def golden_table(path, want=None, invalid_utf8="replace", metadata=True) -> pa.Table:
    """A golden file as read_fstx returns it. ``metadata=False`` leaves the fstx metadata out (a table that was
    rebuilt in Python and lost it)."""
    t = quiet(path, invalid_utf8=invalid_utf8)
    if not metadata:
        t = pa.Table.from_arrays(t.columns, schema=pa.schema([pa.field(f.name, f.type) for f in t.schema]))
    return t


def write(data, path, **kw):
    """write_fstx with every warning an error."""
    with warnings.catch_warnings():
        warnings.simplefilter("error", UserWarning)
        fstx.write_fstx(data, path, **kw)
    return fstx.metadata(path)


def kinds(m):
    return {c.name: (c.phys, c.logical) for c in m.columns}


def attrs(c):
    return fxa.parse(c.attrs) if c.attrs else {}


def defs(m):
    return [(c.name, c.phys, c.logical, c.ordered, c.levels, c.tzone, c.attrs) for c in m.columns]


# ---------------------------------------------------------------- FXA1
def _golden_blobs():
    out = []
    for path, want in CASES:
        for c in want["columns"]:
            if "blob_hex" in c["stored"]:
                out.append((f"{path.stem}:{c['name']}", bytes.fromhex(c["stored"]["blob_hex"])))
        if want["table"].get("blob_hex"):
            out.append((f"{path.stem}:<table>", bytes.fromhex(want["table"]["blob_hex"])))
    return out


def test_fxa1_worked_example():
    blob = fxa.encode({"class": fxa.character(["data.frame"])})
    assert len(blob) == 43 and blob[:4] == b"FXA\x01"
    assert blob == (b"FXA\x01" + (1).to_bytes(4, "little") + (5).to_bytes(4, "little") + b"class"
                    + (4).to_bytes(4, "little") + (1).to_bytes(4, "little") + (10).to_bytes(4, "little")
                    + b"data.frame" + (0).to_bytes(4, "little"))
    assert fxa.encode({}) is None


def test_fxa1_golden_blobs_reencode_byte_for_byte():
    blobs = _golden_blobs()
    seen = 0
    for name, blob in blobs:
        if not fxa.is_fxa1(blob):
            continue
        try:
            parsed = fxa.parse(blob)
        except UnicodeDecodeError:
            continue  # a string value that is not UTF-8: such a blob travels as bytes
        assert fxa.encode(parsed) == blob, name
        seen += 1
    assert seen >= 40
    assert any(not fxa.is_fxa1(b) for _, b in blobs)  # attrs_serialized: R serialization is told apart


def test_fxa1_values():
    v = {"l": fxa.logical([True, False, None]), "i": fxa.integer([1, None, -5]),
         "d": fxa.double([1.5, None, float("inf")]), "s": fxa.character(["a", None, HINDI, ""]),
         "n": fxa.integer([1], {"a": fxa.character(["x"], {"b": fxa.double([2.0])})})}
    blob = fxa.encode(v)
    back = fxa.parse(blob)
    assert list(back) == list(v) and back == v
    assert fxa.encode(back) == blob
    assert back["d"].values[1] is None  # NA_real_
    nan = fxa.parse(fxa.encode({"d": fxa.double([float("nan")])}))["d"].values[0]
    assert nan != nan  # any other NaN stays a NaN
    assert fxa.NA_REAL_BITS.to_bytes(8, "little") in blob


@pytest.mark.parametrize("bad", [
    lambda: fxa.encode({"a": fxa.character(["x\x00y"])}),
    lambda: fxa.encode({"a": fxa.integer([2**31])}),
    lambda: fxa.encode({"a": fxa.integer([-2**31])}),
    lambda: fxa.encode({"a": fxa.integer([1.5])}),
    lambda: fxa.encode({"a": fxa.logical([2])}),
    lambda: fxa.encode({"a": fxa.character([1])}),
    lambda: fxa.encode({"": fxa.integer([1])}),
    lambda: fxa.encode({"a": [1, 2]}),
    lambda: fxa.Vec("list", []),
    lambda: fxa.encode({"a": fxa.integer([1], {"b": fxa.integer([1], {"c": fxa.integer([1], {"d": fxa.integer([1])})})})}),
])
def test_fxa1_encode_refuses(bad):
    with pytest.raises(ValueError):
        bad()


def test_fxa1_parse_refuses_damage():
    blob = fxa.encode({"class": fxa.character(["data.frame"]), "n": fxa.integer([1, 2])})
    for cut in (3, 7, 20, len(blob) - 1):
        with pytest.raises(ValueError):
            fxa.parse(blob[:cut])
    with pytest.raises(ValueError):
        fxa.parse(blob + b"\x00")
    with pytest.raises(ValueError):
        fxa.parse(b"FXA\x02" + blob[4:])
    with pytest.raises(ValueError):
        fxa.parse(b"X\n\x00\x00\x00\x03")


# ---------------------------------------------------------------- the golden cases, R -> Python -> R
@pytest.mark.parametrize("path,want", CASES, ids=ids(CASES))
def test_golden_through_pyarrow(path, want):
    was = fstx.metadata(path)
    m = write(golden_table(path, want), OUT / path.name)
    assert (m.nrow, m.ncol) == (was.nrow, was.ncol)
    assert defs(m) == defs(was)
    if path.stem == "datatable":  # the secondary index goes, the key stays
        a, b = fxa.parse(was.table_attrs), fxa.parse(m.table_attrs)
        assert "index" in a and "index" not in b and b["sorted"] == a["sorted"]
        del a["index"]
        assert a == b
    else:
        assert m.table_attrs == was.table_attrs
    assert m.profile == "balanced"


@pytest.mark.parametrize("path,want", CASES, ids=ids(CASES))
def test_golden_through_pandas(path, want):
    pytest.importorskip("pandas")
    was = fstx.metadata(path)
    t = golden_table(path, want)
    if path.stem == "factor_na_level":
        # pandas categories cannot hold a missing value: to_pandas hands the column over as strings
        with pytest.warns(fstx.FstxWarning, match="NA as a level"):
            df = to_pandas(t)
        m = write(df, OUT / (path.stem + ".pandas.fstx"))
        assert kinds(m)["f"] == ("character", "none")
        return
    m = write(to_pandas(t), OUT / (path.stem + ".pandas.fstx"))
    assert (m.nrow, m.ncol) == (was.nrow, was.ncol)
    assert defs(m) == defs(was)
    if path.stem == "datatable":
        a = fxa.parse(was.table_attrs)
        del a["index"]
        assert a == fxa.parse(m.table_attrs)
    else:
        assert m.table_attrs == was.table_attrs


@pytest.mark.parametrize("path,want", CASES, ids=ids(CASES))
def test_golden_through_polars(path, want):
    pytest.importorskip("polars")
    was = fstx.metadata(path)
    t = golden_table(path, want)
    if len(set(t.column_names)) != len(t.column_names):
        with pytest.raises(ValueError, match="unique column names"):
            fstx.to_polars(t)
        return
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", fstx.FstxWarning)  # the factor with an NA level becomes strings
        df = fstx.to_polars(t)
    m = write(df, OUT / (path.stem + ".polars.fstx"))
    # polars carries no metadata: names and the column count survive, R's classes and attributes may not
    assert m.names == was.names
    if was.ncol:
        assert m.nrow == was.nrow


def test_golden_invalid_utf8_as_binary():
    path, want = next(c for c in CASES if c[0].stem == "strings_invalid_utf8")
    t = golden_table(path, want, invalid_utf8="binary")
    assert any(pa.types.is_binary(f.type) for f in t.schema)
    m = write(t, NATIVE / "strings_invalid_utf8.binary.fstx")
    assert defs(m) == defs(fstx.metadata(path))


def test_golden_without_metadata_is_plain_data():
    path, want = next(c for c in CASES if c[0].stem == "classes_blob")
    m = write(golden_table(path, want, metadata=False), OUT / "native" / "classes_blob.nometa.fstx")
    assert m.nrow == want["nrow"]
    assert fxa.parse(m.table_attrs) == {"class": fxa.character(["data.frame"])}


# ---------------------------------------------------------------- data that never came from R
def native_table():
    n = None
    ts = lambda v, unit, tz=None: pa.array(v, pa.int64()).cast(pa.timestamp(unit, tz))  # noqa: E731
    return pa.table({
        "b": pa.array([True, False, n]),
        "i8": pa.array([1, -128, n], pa.int8()),
        "i16": pa.array([1, -32768, n], pa.int16()),
        "u8": pa.array([0, 255, n], pa.uint8()),
        "u16": pa.array([0, 65535, n], pa.uint16()),
        "i32": pa.array([2**31 - 1, -(2**31) + 1, n], pa.int32()),
        "u32_small": pa.array([0, 7, n], pa.uint32()),
        "u32_big": pa.array([0, 2**32 - 1, n], pa.uint32()),
        "i64_small": pa.array([5, -5, n], pa.int64()),
        "i64_big": pa.array([2**62, -(2**63) + 1, n], pa.int64()),
        "u64": pa.array([0, 2**63 - 1, n], pa.uint64()),
        "f32": pa.array([1.5, -0.25, n], pa.float32()),
        "f64": pa.array([0.1, float("nan"), n], pa.float64()),
        "s": pa.array(["a", HINDI, n], pa.string()),
        "ls": pa.array(["", "x" * 300, n], pa.large_string()),
        "d32": pa.array([dt.date(2024, 2, 29), dt.date(1969, 12, 31), n], pa.date32()),
        "d64": pa.array([0, 2 * 86400000, n], pa.int64()).cast(pa.date64()),
        "ts_s": ts([0, 1700000000, n], "s"),
        "ts_ms": ts([1500, -1, n], "ms"),
        "ts_us": ts([1700000000123456, 0, n], "us"),
        "ts_ns": ts([1000000001, 0, n], "ns"),
        "ts_tz": ts([0, 1700000000000000, n], "us", "Asia/Kolkata"),
        "t32": pa.array([3661, 0, n], pa.int32()).cast(pa.time32("s")),
        "t64": pa.array([1500000, 0, n], pa.int64()).cast(pa.time64("us")),
        "dur": pa.array([90, -1, n], pa.int64()).cast(pa.duration("s")),
        "dur_ms": pa.array([1500, 0, n], pa.int64()).cast(pa.duration("ms")),
        "fac": pa.array(["lo", "hi", n]).dictionary_encode(),
        "ord": pa.DictionaryArray.from_arrays(pa.array([1, 0, n], pa.int8()), pa.array(["S", "M"]), ordered=True),
        "dint": pa.array([10, 20, n], pa.int32()).dictionary_encode(),
        "nul": pa.nulls(3),
    })


def test_native_types():
    m = write(native_table(), NATIVE / "native_types.fstx")
    k = kinds(m)
    c = {x.name: x for x in m.columns}
    assert m.nrow == 3
    assert k["b"] == ("logical", "none")
    for name in ("i8", "i16", "u8", "u16", "i32", "u32_small", "i64_small", "dint"):
        assert k[name] == ("integer", "none"), name
        assert c[name].attrs is None
    for name in ("u32_big", "i64_big", "u64"):  # fstx 1.2: int64="auto" stores values past int32 as int64
        assert k[name] == ("int64", "none"), name
        assert c[name].attrs is None
    assert k["f32"] == k["f64"] == ("double", "none")
    assert k["s"] == k["ls"] == ("character", "none")
    assert k["d32"] == k["d64"] == ("double", "Date")
    for name in ("ts_s", "ts_ms", "ts_us", "ts_ns"):
        assert k[name] == ("double", "POSIXct") and c[name].tzone == "UTC", name
    assert c["ts_tz"].tzone == "Asia/Kolkata"
    assert attrs(c["t32"]) == attrs(c["t64"]) == {"units": fxa.character(["secs"]),
                                                  "class": fxa.character(["hms", "difftime"])}
    assert attrs(c["dur"]) == attrs(c["dur_ms"]) == {"class": fxa.character(["difftime"]),
                                                     "units": fxa.character(["secs"])}
    assert k["fac"] == k["ord"] == ("integer", "factor")
    assert c["fac"].levels == ["lo", "hi"] and not c["fac"].ordered
    assert c["ord"].levels == ["S", "M"] and c["ord"].ordered
    assert k["nul"] == ("logical", "none")
    assert fxa.parse(m.table_attrs) == {"class": fxa.character(["data.frame"])}


def chunked_table():
    i = pa.array([None if v == 6 else v for v in range(12)], pa.int32())
    f = pa.array([None if v == 3 else v / 4 for v in range(12)], pa.float64())
    s = pa.array([None if v == 7 else f"s{v}" for v in range(12)], pa.string())
    ls = s.cast(pa.large_string())

    def cut(a):  # row 0 sliced off, three chunks (one empty), offsets inside the buffers
        return pa.chunked_array([a.slice(1, 4), a.slice(5, 0), a.slice(5, 7)])

    d1 = pa.DictionaryArray.from_arrays(pa.array([0, 1, None, 1], pa.int8()), pa.array(["a", "b"]))
    d2 = pa.DictionaryArray.from_arrays(pa.array([1, 0, 0, 1, None, 0, 1], pa.int8()), pa.array(["b", "c"]))
    return pa.table({"i": cut(i), "f": cut(f), "s": cut(s), "ls": cut(ls),
                     "d": pa.chunked_array([d1, d2.slice(0, 0), d2])})


def test_chunked_and_sliced():
    m = write(chunked_table(), NATIVE / "chunked.fstx")
    assert m.nrow == 11
    c = {x.name: x for x in m.columns}
    assert c["d"].levels == ["a", "b", "c"]
    assert kinds(m)["s"] == kinds(m)["ls"] == ("character", "none")


def test_number_chunks_over_row_groups(tmp_path):
    # number columns go to the engine a row group at a time, straight from their chunks (widened, NA written for
    # nulls): chunks that cross the row group edge, sliced chunks, an empty chunk and nulls in every type give the
    # same file as one array per column
    import numpy as np

    n = 1_048_576 + 70_001
    rng = np.random.default_rng(7)
    nulls = rng.random(n) < 0.1
    big = rng.integers(-2**40, 2**40, n)

    def arr(values, typ, with_nulls=True):
        return pa.array(values, typ, mask=nulls if with_nulls else None)

    cols = {
        "i8": arr(rng.integers(-100, 100, n), pa.int8()),
        "i16": arr(rng.integers(-3000, 3000, n), pa.int16()),
        "u8": arr(rng.integers(0, 250, n), pa.uint8()),
        "u16": arr(rng.integers(0, 60000, n), pa.uint16()),
        "i32": arr(rng.integers(-2**31 + 1, 2**31, n), pa.int32()),
        "i64_small": arr(rng.integers(-2**31 + 1, 2**31, n), pa.int64()),
        "i64_big": arr(big, pa.int64()),
        "b": arr(rng.random(n) < 0.5, pa.bool_()),
        "d32": arr(rng.integers(-30000, 30000, n).astype("int32"), pa.int32()).cast(pa.date32()),
        "f32": arr(rng.standard_normal(n).astype("float32"), pa.float32()),
        "f64": arr(rng.standard_normal(n), pa.float64()),
        "f64_full": arr(rng.standard_normal(n), pa.float64(), with_nulls=False),
        "i32_full": arr(rng.integers(-2**31 + 1, 2**31, n), pa.int32(), with_nulls=False),
        "fac": pa.DictionaryArray.from_arrays(arr(rng.integers(0, 3, n), pa.int8()), pa.array(["x", "y", "z"])),
    }
    one = pa.table(cols)
    cuts = [0, 3, 65_536, 65_536, 1_000_000, 1_048_577, n]  # an empty chunk, a chunk over the row group edge

    def chunked(a):
        return pa.chunked_array([a.slice(cuts[k], cuts[k + 1] - cuts[k]) for k in range(len(cuts) - 1)], a.type)

    many = pa.table({k: chunked(v) for k, v in cols.items()})
    p, q = tmp_path / "one.fstx", tmp_path / "many.fstx"
    for threads in (1, None):
        fstx.write_fstx(one, p, threads=threads)
        fstx.write_fstx(many, q, threads=threads)
        assert p.read_bytes() == q.read_bytes()
    m = fstx.metadata(q)
    assert m.row_groups == 2 and kinds(m)["i64_big"] == ("int64", "none")
    if not DROP2:  # reading the int64 column back waits for the engine's drop 2
        return
    got = fstx.read_fstx(q)
    for name, a in cols.items():
        g = got.column(name).combine_chunks()
        want = a.dictionary_decode() if name == "fac" else a
        if name == "fac":
            g = g.dictionary_decode()
        assert g.equals(want.cast(g.type)), name


@pytest.mark.parametrize("profile", ["balanced", "fast"])
def test_string_prep_on_many_threads(profile, tmp_path):
    # the engine reads a row group's strings on many threads once its dictionary is ruled out: dictionaries ruled
    # out early and late, kept, missing values, FSST and plain chunks give the same file on 1 thread and on all
    import numpy as np

    n = 1_048_576 + 300_001
    rng = np.random.default_rng(11)
    words = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa"]
    late = [words[i % 10] if i < 100_000 else f"late-{i}" for i in range(n)]
    nul = [None if r < 0.4 else f"n{i}" for i, r in enumerate(rng.random(n))]
    lens = rng.integers(0, 300, n)
    noise = [("x" * int(k)) if k % 3 else "".join(chr(97 + int(c)) for c in rng.integers(0, 26, int(k) % 40))
             for k in lens[:200_000]]
    t = pa.table({
        "distinct": [f"row{i}" for i in range(n)],
        "late": late,
        "low": [words[i % 7] for i in range(n)],
        "nulls": nul,
        "long": noise * (n // 200_000) + noise[: n % 200_000],
    })
    p, q = tmp_path / "one.fstx", tmp_path / "all.fstx"
    fstx.write_fstx(t, p, threads=1, profile=profile)
    fstx.write_fstx(t, q, threads=None, profile=profile)
    assert p.read_bytes() == q.read_bytes()
    back = fstx.read_fstx(q)
    for name in t.column_names:
        assert back.column(name).combine_chunks().equals(t.column(name).combine_chunks()), name


@pytest.mark.parametrize("opts", [{}, {"compress": "lz4"},{"compress": "none"}, {"profile": "small", "threads": 1},
                                  {"profile": "fast"}, {"compress": "zstd", "level": 1}],
                         ids=["default", "lz4", "none", "small_1thread", "fast", "zstd1"])
def test_many_rows(opts, request):
    n = 150000
    t = pa.table({"i": pa.array(range(n), pa.int32()),
                  "f": pa.array([v * 0.5 for v in range(n)], pa.float64()),
                  "s": pa.array([f"row{v}" for v in range(n)], pa.string()),
                  "g": pa.array([f"g{v % 7}" for v in range(n)]).dictionary_encode()})
    m = write(t, NATIVE / f"big.{request.node.callspec.id}.fstx", **opts)
    assert m.nrow == n and m.row_groups >= 1
    assert m.profile == opts.get("profile", "balanced")


def test_record_batch_and_reader_and_stream():
    t = pa.table({"x": pa.array([1, 2, 3], pa.int32()), "s": ["a", None, "c"]})
    a = write(t, NATIVE / "from_table.fstx")
    b = write(t.to_batches()[0], NATIVE / "from_batch.fstx")
    r = write(pa.RecordBatchReader.from_batches(t.schema, t.to_batches()), NATIVE / "from_reader.fstx")

    class Stream:
        def __arrow_c_stream__(self, requested_schema=None):
            return t.__arrow_c_stream__(requested_schema)

    s = write(Stream(), NATIVE / "from_stream.fstx")
    assert defs(a) == defs(b) == defs(r) == defs(s) and a.nrow == b.nrow == r.nrow == s.nrow == 3
    with pytest.raises(TypeError, match="pyarrow.Table"):
        fstx.write_fstx({"x": [1]}, NATIVE / "never.fstx")


def test_empty_tables():
    m = write(pa.table({}), NATIVE / "empty.fstx")
    assert (m.nrow, m.ncol) == (0, 0)
    m = write(pa.table({"x": pa.array([], pa.int32()), "s": pa.array([], pa.string()),
                        "f": pa.array([], pa.string()).dictionary_encode()}), NATIVE / "zero_rows.fstx")
    assert (m.nrow, m.ncol) == (0, 3) and m.columns[2].levels == []


def test_path_kinds(tmp_path):
    t = pa.table({"x": pa.array([1], pa.int32())})
    for p in (tmp_path / "a.fstx", str(tmp_path / "b.fstx"), bytes(tmp_path / "c.fstx"), tmp_path / f"{HINDI}.fstx"):
        fstx.write_fstx(t, p)
    assert sorted(x.name for x in tmp_path.iterdir()) == sorted(["a.fstx", "b.fstx", "c.fstx", f"{HINDI}.fstx"])
    assert fstx.metadata(tmp_path / f"{HINDI}.fstx").nrow == 1
    fstx.write_fstx(pa.table({"y": ["z", "w"]}), tmp_path / "a.fstx")  # a file that exists is replaced
    assert fstx.metadata(tmp_path / "a.fstx").names == ["y"]
    with pytest.raises(ValueError):
        fstx.write_fstx(t, "")
    with pytest.raises(Exception):
        fstx.write_fstx(t, tmp_path / "no_such_folder" / "a.fstx")


# ---------------------------------------------------------------- 64-bit integers
def test_int64_modes():
    small = pa.table({"a": pa.array([1, 2, None], pa.int64())})
    big = pa.table({"a": pa.array([1, 2**40, None], pa.int64())})
    assert kinds(write(small, NATIVE / "int64_auto_small.fstx")) == {"a": ("integer", "none")}
    m = write(big, NATIVE / "int64_auto_big.fstx")  # fstx 1.2: an int64 column
    assert kinds(m) == {"a": ("int64", "none")} and m.columns[0].attrs is None
    m = write(small, NATIVE / "int64_native.fstx", int64="native")
    assert kinds(m) == {"a": ("int64", "none")} and m.columns[0].attrs is None
    m = write(big, NATIVE / "int64_integer64.fstx", int64="integer64")  # the fstx 1.0 layout
    assert kinds(m) == {"a": ("double", "none")} and attrs(m.columns[0])["class"].values == ["integer64"]
    m = write(big, NATIVE / "int64_double.fstx", int64="double")
    assert kinds(m) == {"a": ("double", "none")} and m.columns[0].attrs is None
    with pytest.raises(ValueError, match="2\\^53"):
        fstx.write_fstx(pa.table({"a": pa.array([2**53 + 1], pa.int64())}), NATIVE / "never.fstx", int64="double")
    with pytest.raises(ValueError, match="'int64'"):
        fstx.write_fstx(small, NATIVE / "never.fstx", int64="float")


# ---------------------------------------------------------------- refusals
@pytest.mark.parametrize("arr,exc,match", [
    (pa.array([1], pa.int64()).cast(pa.decimal128(25, 2)), TypeError, "decimal"),
    (pa.array([[1, 2]], pa.list_(pa.int32())), TypeError, "list"),
    (pa.array([{"a": 1}], pa.struct([("a", pa.int32())])), TypeError, "struct"),
    (pa.array([[("k", 1)]], pa.map_(pa.string(), pa.int32())), TypeError, "map"),
    (pa.array([-(2**31)], pa.int32()), ValueError, "-2147483648"),
    (pa.array([-(2**63)], pa.int64()), ValueError, "-9223372036854775808"),
    (pa.array([2**63], pa.uint64()), ValueError, "above 9223372036854775807"),
    (pa.array(["a\x00b"]), ValueError, "bad"),
    (pa.DictionaryArray.from_arrays(pa.array([0, 1], pa.int8()), pa.array(["a", "a"])), ValueError, "repeated"),
    (pa.DictionaryArray.from_arrays(pa.array([0], pa.int8()), pa.array(["a\x00"])), ValueError, "NUL"),
], ids=["decimal", "list", "struct", "map", "int32_min", "int64_min", "uint64_big",
        "nul_in_string", "repeated_levels", "nul_in_level"])
def test_refused(arr, exc, match, tmp_path):
    p = tmp_path / "refused.fstx"
    with pytest.raises(exc, match=match) as e:
        fstx.write_fstx(pa.table({"bad": arr}), p)
    assert "bad" in str(e.value)  # the column is named
    assert not p.exists()


def test_refused_invalid_utf8(tmp_path):
    bad = pa.array([b"ok", b"\xff\xfe"], pa.binary()).view(pa.string())
    p = tmp_path / "refused.fstx"
    with pytest.raises(ValueError, match="UTF-8") as e:
        fstx.write_fstx(pa.table({"good": ["a", "b"], "txt": bad}), p)
    assert "'txt'" in str(e.value) and "(row 1)" in str(e.value)  # the column, and the row counted from 0
    assert not p.exists()


@pytest.mark.parametrize("chunks,row,match", [
    ([["a", "b", "a"], ["b", "a\x00b", "a", "a\x00b"]], 4, "NUL"),
    ([["a", None, "a"], [None, "b"] * 40 + ["c\x00"]], 83, "NUL"),
    ([["a"] * 5 + [b"\xff"], ["a\x00"]], 5, "UTF-8"),
], ids=["second_chunk", "after_nulls", "first_bad_wins"])
def test_refused_string_in_a_repeated_column(chunks, row, match, tmp_path):
    # a column that repeats its values goes to the engine dictionary-coded and only the dictionary is checked:
    # the error still names the first row that holds the bad value
    arrs = [pa.array([v.encode() if isinstance(v, str) else v for v in c], pa.binary()).view(pa.string())
            for c in chunks]
    p = tmp_path / "refused.fstx"
    with pytest.raises(ValueError, match=match) as e:
        fstx.write_fstx(pa.table({"txt": pa.chunked_array(arrs)}), p)
    assert "(row %d)" % row in str(e.value)
    assert not p.exists()


@pytest.mark.parametrize("case", ["distinct", "same", "half", "half_plus_one", "nulls", "empty_strings", "large",
                                  "binary", "sliced_chunks", "all_null", "long_values"])
def test_over_half_distinct_is_count_distinct(case):
    # write_fstx's dictionary choice for a string column: the binding's answer is pyarrow's count_distinct
    from fstx import _core
    import pyarrow.compute as pc

    n = 65_536
    vals = {
        "distinct": [f"d{i}" for i in range(n)],
        "same": ["x"] * n,
        "half": [f"h{i % (n // 2)}" for i in range(n)],
        "half_plus_one": [f"h{i % (n // 2)}" for i in range(n - 1)] + ["new"],
        "nulls": [None if i % 3 else f"n{i}" for i in range(n)],
        "empty_strings": ["" if i % 2 else f"e{i}" for i in range(n)],
        "large": [f"l{i % 40_000}" for i in range(n)],
        "binary": [f"b{i % 30_000}" for i in range(n)],
        "sliced_chunks": [f"s{i % 33_000}" for i in range(n)],
        "all_null": [None] * n,
        "long_values": [("v" * 50) + str(i % 32_769) for i in range(n)],
    }[case]
    a = pa.array(vals, pa.large_string() if case == "large" else pa.binary() if case == "binary" else pa.string())
    col = pa.chunked_array([a.slice(0, 7), a.slice(7, 30_000), a.slice(30_007)]) if case == "sliced_chunks" \
        else pa.chunked_array([a])
    want = pc.count_distinct(col).as_py() * 2 > len(col)
    assert _core.over_half_distinct([c.__arrow_c_array__() for c in col.chunks]) == want


def test_refused_damaged_offsets_in_a_long_column(tmp_path):
    # offsets that go backwards, past the first 65,536 rows' distinct count and into the string check
    import numpy as np

    n = 70_000
    offs = np.arange(n + 1, dtype="int32") * 2
    offs[40_000] = 5  # value 39,999 ends before it starts
    data = pa.py_buffer(b"ab" * n)
    bad = pa.Array.from_buffers(pa.string(), n, [None, pa.py_buffer(offs.tobytes()), data])
    p = tmp_path / "bad.fstx"
    with pytest.raises(ValueError, match="damaged string offsets") as e:
        fstx.write_fstx(pa.table({"s": bad}), p)
    assert "(row 39999)" in str(e.value)
    assert not p.exists()


def test_refused_int32_na_value_anywhere(tmp_path):
    # R's NA_integer_ as a value is looked for in every chunk, on all threads, before the file is touched; under
    # a missing value it does not count
    import numpy as np

    p = tmp_path / "kept.fstx"
    fstx.write_fstx(pa.table({"x": pa.array([1, 2], pa.int32())}), p)
    before = p.read_bytes()
    v = np.arange(3_000_000, dtype="int32")
    v[2_500_000] = -2**31
    chunks = [pa.array(v[a:a + 700_000]) for a in range(0, len(v), 700_000)]
    for threads in (1, None):
        with pytest.raises(ValueError, match="'x' holds -2147483648"):
            fstx.write_fstx(pa.table({"ok": pa.array(v.astype("float64")), "x": pa.chunked_array(chunks)}), p,
                            threads=threads)
        assert p.read_bytes() == before
    masked = pa.array(v, mask=v == -2**31)
    fstx.write_fstx(pa.table({"x": masked}), p)
    got = fstx.read_fstx(p).column("x")
    assert got.null_count == 1 and got[2_500_000].as_py() is None and got[2_499_999].as_py() == 2_499_999


def test_refused_strings_checked_on_many_threads(tmp_path):
    # the string check runs in slices of 65,536 values on all threads: the error still names the first column
    # with a problem and its first row, whatever slice found it
    n = 300_000
    plain = [f"v{i}" for i in range(n)]
    plain[140_001] = b"\xff"  # not UTF-8, in a later slice ...
    plain[70_000] = "a\x00b"  # ... than this NUL
    vals = [f"k{i % 9}" for i in range(n)]  # 9 values: goes to the engine dictionary-coded
    vals[100_500] = "bad\x00"

    def text(values):
        return pa.array([v.encode() if isinstance(v, str) else v for v in values], pa.binary()).view(pa.string())

    bad_plain, rep, good = text(plain), text(vals), pa.array([f"g{i}" for i in range(n)])
    p = tmp_path / "refused.fstx"
    for cols, name, row in (({"good": good, "a": bad_plain, "b": rep}, "a", 70_000),
                            ({"good": good, "b": rep, "a": bad_plain}, "b", 100_500)):
        for threads in (1, None):
            with pytest.raises(ValueError, match="NUL") as e:
                fstx.write_fstx(pa.table(cols), p, threads=threads)
            assert f"column '{name}'" in str(e.value) and f"(row {row})" in str(e.value), str(e.value)
            assert not p.exists()


def test_number_pages_in_place_and_filled(tmp_path):
    # the engine asks for number rows a page at a time: pages inside one chunk of the stored type go in place,
    # pages over a chunk edge, with nulls or of another type are filled; the file is the same as from one array
    import numpy as np

    n = 700_001
    rng = np.random.default_rng(3)
    vals = {
        "i32": pa.array(rng.integers(-2**31 + 1, 2**31, n).astype("int32")),
        "f64": pa.array(rng.standard_normal(n)),
        "i16": pa.array(rng.integers(-3000, 3000, n).astype("int16")),
        "i64": pa.array(rng.integers(-2**62, 2**62, n)),  # fstx 1.2: an int64 column (int64="auto", past int32)
    }
    nulls = np.zeros(n, bool)
    nulls[300_000:300_010] = True
    vals["f64_na"] = pa.array(rng.standard_normal(n), mask=nulls)
    vals["i64_na"] = pa.array(rng.integers(-2**62, 2**62, n), mask=nulls)
    one = pa.table(vals)
    p = tmp_path / "one.fstx"
    fstx.write_fstx(one, p, threads=1)
    assert [c.phys for c in fstx.metadata(p).columns][-3:] == ["int64", "double", "int64"]
    for size in (65_536, 40_000, 32_768, 1):
        cuts = list(range(0, n, size if size > 1 else 99_999)) + [n]
        many = pa.table({k: pa.chunked_array([v.slice(a, b - a) for a, b in zip(cuts, cuts[1:])], v.type)
                         for k, v in vals.items()})
        for threads in (1, None):
            q = tmp_path / f"many_{size}_{threads}.fstx"
            fstx.write_fstx(many, q, threads=threads)
            assert p.read_bytes() == q.read_bytes(), (size, threads)
    assert fstx.read_fstx(p, columns=["f64_na"]).column("f64_na").null_count == 10
    if DROP2:
        got = fstx.read_fstx(p, columns=["i64", "i64_na"])
        assert got.column("i64").equals(pa.chunked_array([vals["i64"]])) and got.column("i64_na").null_count == 10
        assert got.column("i64_na").equals(pa.chunked_array([vals["i64_na"]]))


def test_one_thread_team_for_every_step(tmp_path):
    # the checks and the window fills of one write share its threads (started once): any thread count gives the
    # same file over many windows, and a write refused on a worker leaves the next write working
    import numpy as np

    n = 3 * 1_048_576  # whole row groups: a short last one could go plain (see STATUS-FIX, read side)
    rng = np.random.default_rng(5)
    nulls = rng.random(n) < 0.2
    t = pa.table({
        "i": pa.array(rng.integers(-1000, 1000, n).astype("int32"), mask=nulls),
        "d": pa.array(rng.standard_normal(n), mask=~nulls),
        "s": pa.array([f"s{i % 5000}" for i in range(n)]),
    })
    p, q = tmp_path / "one.fstx", tmp_path / "many.fstx"
    fstx.write_fstx(t, p, threads=1)
    bad = pa.table({"s": pa.array([b"ok"] * 200_000 + [b"\xff"], pa.binary()).view(pa.string())})
    for threads in (2, 3, 8, None):
        with pytest.raises(ValueError, match="UTF-8"):
            fstx.write_fstx(bad, q, threads=threads)
        fstx.write_fstx(t, q, threads=threads)
        assert p.read_bytes() == q.read_bytes(), threads
    assert fstx.read_fstx(q).equals(t.cast(fstx.read_fstx(q).schema))


def test_repeated_strings_in_many_dictionaries(tmp_path):
    # a repeating column is coded in slices on many threads, each with its own dictionary; equal strings still
    # reach the engine as one pointer: the same file as one dictionary (one thread), and as chunks that each
    # bring a dictionary of their own (other values, other order, missing values, a value missing from some)
    n = 1_200_000
    vals = [None if i % 97 == 0 else f"code-{(i * 7919) % 300:03d}" for i in range(n)]
    plain = pa.array(vals)
    p, q, r = tmp_path / "one.fstx", tmp_path / "many.fstx", tmp_path / "own.fstx"
    fstx.write_fstx(pa.table({"s": plain}), p, threads=1)
    fstx.write_fstx(pa.table({"s": plain}), q, threads=None)
    assert p.read_bytes() == q.read_bytes()
    cuts = [0, 5, 70_000, 600_000, 1_100_000, n]
    own = pa.chunked_array([plain.slice(a, b - a).dictionary_encode() for a, b in zip(cuts, cuts[1:])])
    assert len({c.dictionary.buffers()[2].address for c in own.chunks}) == len(cuts) - 1
    # (a dictionary-typed column is a factor; as text it goes through the string path of the binding directly)
    from fstx import _write as W
    col = W._Col("s", W._STRING, list(own.chunks))
    spec = [(col.name, col.phys, col.logical, col.ordered, col.levels, col.tzone, col.attrs,
             [a.__arrow_c_array__() for a in col.data], 0, False)]
    from fstx import _core
    _core.write(str(r), spec, W._table_blob(pa.table({"s": plain}), W._NO_INDEX), n, _core.CODEC_ZSTD, 3, 0, 1, 10,
                False)
    assert r.read_bytes() == p.read_bytes()
    assert fstx.read_fstx(r).column("s").combine_chunks().equals(plain)


def test_repeated_strings_are_dictionary_coded(tmp_path):
    # equal strings reach the engine as one pointer, as R's string cache gives them, so a column of 20 values
    # is stored as a dictionary (as R stores it), across chunks and with missing values
    vals = [f"city-{i % 20:02d}" for i in range(200_000)]
    vals[5] = vals[150_000] = None
    rep = pa.chunked_array([pa.array(vals[:70_000]), pa.array(vals[70_000:])])
    uniq = pa.array([f"city-{i:06d}" for i in range(200_000)])  # same lengths, all distinct
    p, q = tmp_path / "rep.fstx", tmp_path / "uniq.fstx"
    fstx.write_fstx(pa.table({"s": rep}), p, profile="fast")
    fstx.write_fstx(pa.table({"s": uniq}), q, profile="fast")
    assert p.stat().st_size * 5 < q.stat().st_size
    assert fstx.read_fstx(p).column("s").to_pylist() == vals
    assert fstx.read_fstx(q).column("s").to_pylist() == uniq.to_pylist()


def test_refused_keeps_the_old_file(tmp_path):
    p = tmp_path / "kept.fstx"
    fstx.write_fstx(pa.table({"x": pa.array([1, 2], pa.int32())}), p)
    before = p.read_bytes()
    with pytest.raises(ValueError):
        fstx.write_fstx(pa.table({"x": pa.array(["a\x00b"])}), p)
    assert p.read_bytes() == before


def test_extension_type_refused(tmp_path):
    class Ext(pa.ExtensionType):
        def __init__(self):
            super().__init__(pa.int32(), "fstx.test.ext")

        def __arrow_ext_serialize__(self):
            return b""

        @classmethod
        def __arrow_ext_deserialize__(cls, storage_type, serialized):
            return cls()

    arr = pa.ExtensionArray.from_storage(Ext(), pa.array([1], pa.int32()))
    with pytest.raises(TypeError, match="extension"):
        fstx.write_fstx(pa.table({"e": arr}), tmp_path / "e.fstx")


def test_polars_lazyframe_refused(tmp_path):
    pl = pytest.importorskip("polars")
    with pytest.raises(TypeError, match="collect"):
        fstx.write_fstx(pl.DataFrame({"a": [1]}).lazy(), tmp_path / "l.fstx")


# ---------------------------------------------------------------- options
def test_options(tmp_path):
    t = pa.table({"x": pa.array(range(5000), pa.int32()), "s": ["abc"] * 5000})
    for prof in ("fast", "balanced", "small"):
        assert write(t, tmp_path / "p.fstx", profile=prof).profile == prof
    assert write(t, tmp_path / "p.fstx").profile == "balanced"
    raw = write(t, tmp_path / "none.fstx", compress="none").file_size
    assert write(t, tmp_path / "zstd.fstx", compress="zstd").file_size < raw
    assert write(t, tmp_path / "lz4.fstx", compress="lz4").file_size < raw
    assert write(t, tmp_path / "fast.fstx", profile="fast").file_size == raw
    write(t, tmp_path / "l.fstx", compress="zstd", level=19, threads=2)
    for bad in ({"profile": "tiny"}, {"compress": "gzip"}, {"level": 1.5}, {"level": True}, {"threads": 0},
                {"threads": "2"}, {"append": 1}, {"int64": None}):
        with pytest.raises(ValueError, match="write_fstx"):
            fstx.write_fstx(t, tmp_path / "bad.fstx", **bad)
    assert not (tmp_path / "bad.fstx").exists()


def test_two_writes_at_once(tmp_path):
    n = 200000
    t = pa.table({"x": pa.array(range(n), pa.int32()), "s": pa.array([f"v{i}" for i in range(n)])})
    errs = []

    def go(k):
        try:
            fstx.write_fstx(t, tmp_path / f"t{k}.fstx", threads=2)
        except Exception as e:  # noqa: BLE001
            errs.append(e)

    th = [threading.Thread(target=go, args=(k,)) for k in range(4)]
    for x in th:
        x.start()
    for x in th:
        x.join()
    assert not errs
    assert all(fstx.metadata(tmp_path / f"t{k}.fstx").nrow == n for k in range(4))


# ---------------------------------------------------------------- metadata that travels
def _case(stem):
    return next(c for c in CASES if c[0].stem == stem)


def test_metadata_that_no_longer_fits_is_ignored(tmp_path):
    path, want = _case("dates")
    t = golden_table(path, want)
    f0 = t.schema.field(0)
    t2 = t.set_column(0, pa.field(f0.name, pa.string(), metadata=f0.metadata), t.column(0).cast(pa.string()))
    with pytest.warns(UserWarning, match="metadata is ignored"):
        fstx.write_fstx(t2, tmp_path / "d.fstx")
    assert kinds(fstx.metadata(tmp_path / "d.fstx"))[f0.name] == ("character", "none")
    bad = t.set_column(0, pa.field(f0.name, f0.type, metadata={b"fstx.r": b"{not json"}), t.column(0))
    with pytest.warns(UserWarning, match="cannot be read"):
        fstx.write_fstx(bad, tmp_path / "d2.fstx")
    assert kinds(fstx.metadata(tmp_path / "d2.fstx"))[f0.name] == ("double", "Date")


def test_row_names_go_when_rows_change(tmp_path):
    path, want = _case("rownames_char")
    t = golden_table(path, want)
    assert "row.names" in fxa.parse(write(t, tmp_path / "a.fstx").table_attrs)
    m = write(t.slice(0, t.num_rows - 1), tmp_path / "b.fstx")
    assert "row.names" not in fxa.parse(m.table_attrs)
    assert fxa.parse(m.table_attrs)["class"].values == ["data.frame"]


def test_data_table_key_goes_when_rows_are_reordered(tmp_path):
    path, want = _case("datatable")
    t = golden_table(path, want)
    assert "sorted" in fxa.parse(write(t, tmp_path / "a.fstx").table_attrs)
    rev = t.take(pa.array(list(range(t.num_rows))[::-1]))
    b = fxa.parse(write(rev, tmp_path / "b.fstx").table_attrs)
    assert "sorted" not in b and "index" not in b and b["class"].values == ["data.table", "data.frame"]
    no_key = fxa.parse(write(t.drop_columns(["k"]), tmp_path / "c.fstx").table_attrs)
    assert "sorted" not in no_key


def test_serialized_table_blob(tmp_path):
    path, want = _case("attrs_serialized")
    t = golden_table(path, want)
    was = fstx.metadata(path)
    assert not fxa.is_fxa1(was.table_attrs)
    assert write(t, tmp_path / "a.fstx").table_attrs == was.table_attrs
    cut = t.slice(0, t.num_rows - 1)
    if b"row.names" in was.table_attrs:
        with pytest.warns(UserWarning, match="R serialization"):
            fstx.write_fstx(cut, tmp_path / "b.fstx")
        assert fxa.parse(fstx.metadata(tmp_path / "b.fstx").table_attrs)["class"].values == ["data.frame"]
    else:
        assert write(cut, tmp_path / "b.fstx").table_attrs == was.table_attrs


def test_damaged_table_blob_is_refused(tmp_path):
    t = pa.table({"x": pa.array([1], pa.int32())})
    with pytest.raises(ValueError, match="damaged"):
        fstx.write_fstx(t.replace_schema_metadata({b"fstx.table_attrs": b"FXA\x01\x05"}), tmp_path / "a.fstx")
    not_df = fxa.encode({"class": fxa.character(["zoo"])})
    with pytest.raises(ValueError, match="data frame"):
        fstx.write_fstx(t.replace_schema_metadata({b"fstx.table_attrs": not_df}), tmp_path / "a.fstx")


# ---------------------------------------------------------------- append
def test_append():
    p = NATIVE / "appended_py.fstx"
    tz = pa.timestamp("us", "Asia/Kolkata")
    a = pa.table({"x": pa.array([1, 2], pa.int32()), "f": pa.array(["a", "b"]).dictionary_encode(),
                  "s": ["p", None], "t": pa.array([0, 1000000], pa.int64()).cast(tz),
                  "d": pa.array([dt.date(2024, 1, 1), None], pa.date32()), "n": pa.array([7, 8], pa.int64())})
    b = pa.table({"x": pa.array([3], pa.int32()), "f": pa.array(["c"]).dictionary_encode(),
                  "s": ["q"], "t": pa.array([2000000], pa.int64()).cast(tz),
                  "d": pa.array([dt.date(2024, 1, 2)], pa.date32()), "n": pa.array([9], pa.int64())})
    p.unlink(missing_ok=True)
    m = write(a, p, append=True, profile="small")  # no file yet: simply written
    assert m.nrow == 2 and m.profile == "small"
    m2 = write(b, p, append=True)
    assert m2.nrow == 3 and m2.profile == "small"
    c = {x.name: x for x in m2.columns}
    assert c["f"].levels == ["a", "b", "c"]
    assert defs(m2)[0] == defs(m)[0] and c["t"].tzone == "Asia/Kolkata"
    m3 = write(b.slice(0, 0), p, append=True)  # no rows: nothing changes
    assert m3.nrow == 3


def test_append_refusals(tmp_path):
    p = tmp_path / "a.fstx"
    tz = pa.timestamp("us", "Asia/Kolkata")
    base = pa.table({"x": pa.array([1, 2], pa.int32()),
                     "o": pa.DictionaryArray.from_arrays(pa.array([0, 1], pa.int8()), pa.array(["S", "M"]), ordered=True),
                     "t": pa.array([0, 1], pa.int64()).cast(tz)})
    fstx.write_fstx(base, p)
    before = p.read_bytes()

    def app(**cols):
        fstx.write_fstx(pa.table({**{n: base.column(n) for n in base.schema.names}, **cols}), p, append=True)

    with pytest.raises(ValueError, match="columns"):
        fstx.write_fstx(base.rename_columns(["x", "o", "u"]), p, append=True)
    with pytest.raises(ValueError, match="columns"):
        fstx.write_fstx(base.drop_columns(["t"]), p, append=True)
    with pytest.raises(ValueError, match="'x'"):
        app(x=pa.array(["a", "b"]))
    with pytest.raises(ValueError, match="'x'"):
        app(x=pa.array([1.5, 2.5]))
    with pytest.raises(ValueError, match="levels"):
        app(o=pa.DictionaryArray.from_arrays(pa.array([0, 1], pa.int8()), pa.array(["S", "L"]), ordered=True))
    with pytest.raises(ValueError, match="time zone"):
        app(t=pa.array([0, 1], pa.int64()).cast(pa.timestamp("us", "UTC")))
    assert p.read_bytes() == before
    fstx.write_fstx(base, p, append=True)
    assert fstx.metadata(p).nrow == 4


def test_append_and_row_names(tmp_path):
    pd = pytest.importorskip("pandas")
    path, want = _case("rownames_char")
    p = tmp_path / "rn.fstx"
    t = golden_table(path, want)
    fstx.write_fstx(t, p)
    with pytest.raises(ValueError, match="row names"):
        fstx.write_fstx(golden_table(path, want, metadata=False), p, append=True)
    q = tmp_path / "plain.fstx"
    fstx.write_fstx(pa.table({"x": pa.array([1, 2], pa.int32())}), q)
    with pytest.raises(ValueError, match="row names"):
        fstx.write_fstx(pd.DataFrame({"x": pd.array([3], dtype="Int32")}, index=["r"]), q, append=True)
    fstx.write_fstx(pd.DataFrame({"x": pd.array([3], dtype="Int32")}), q, append=True)
    assert fstx.metadata(q).nrow == 3


# ---------------------------------------------------------------- pandas
def test_pandas_native():
    pd = pytest.importorskip("pandas")
    df = pd.DataFrame({
        "i": [1, 2, 3],
        "x": [1.5, None, 2.5],
        "s": ["a", None, "c"],
        "c": pd.Categorical(["u", "v", "u"], categories=["v", "u", "w"]),
        "o": pd.Categorical(["lo", "hi", None], categories=["lo", "hi"], ordered=True),
        "t": pd.to_datetime(["2024-01-01 10:00:00", None, "2024-06-30 23:59:59.5"], format="ISO8601"),
        "tz": pd.to_datetime(["2024-01-01 10:00:00", None, "2024-06-30 23:59:59.5"], format="ISO8601").tz_localize("Asia/Kolkata"),
        "I": pd.array([1, None, 3], dtype="Int64"),
        "bo": pd.array([True, None, False], dtype="boolean"),
        "d": [dt.date(2024, 1, 1), None, dt.date(2020, 2, 29)],
    }, index=["r1", "r2", "r3"])
    m = write(df, NATIVE / "pandas_native.fstx")
    k = kinds(m)
    c = {x.name: x for x in m.columns}
    assert k["i"] == k["I"] == ("integer", "none")
    assert k["x"] == ("double", "none") and k["s"] == ("character", "none") and k["bo"] == ("logical", "none")
    assert c["c"].levels == ["v", "u", "w"] and c["o"].ordered
    assert k["t"] == k["tz"] == ("double", "POSIXct") and (c["t"].tzone, c["tz"].tzone) == ("UTC", "Asia/Kolkata")
    assert k["d"] == ("double", "Date")
    ta = fxa.parse(m.table_attrs)
    assert ta["class"].values == ["data.frame"] and ta["row.names"] == fxa.character(["r1", "r2", "r3"])


def test_pandas_index(tmp_path):
    pd = pytest.importorskip("pandas")
    p = tmp_path / "i.fstx"
    df = pd.DataFrame({"a": [1, 2, 3]})
    assert "row.names" not in fxa.parse(write(df, p).table_attrs)
    assert fxa.parse(write(df.set_axis([5, 6, 7]), p).table_attrs)["row.names"] == fxa.integer([5, 6, 7])
    assert "row.names" not in fxa.parse(write(df.set_axis([0, 1, 2]), p).table_attrs)
    for bad in (df.set_axis(["a", "a", "b"]), df.set_axis([1.5, 2.5, 3.5]), df.set_axis([1, 1, 2]),
                df.set_axis(pd.MultiIndex.from_tuples([(1, "a"), (1, "b"), (2, "a")])),
                df.set_axis(pd.to_datetime(["2024-01-01", "2024-01-02", "2024-01-03"]))):
        with pytest.raises(TypeError, match="reset_index"):
            fstx.write_fstx(bad, p)
    m = write(pd.DataFrame({1: [1], 2: [2]}), p)  # column labels that are not strings are written as text
    assert m.names == ["1", "2"]


def test_pandas_metadata_follows_the_columns(tmp_path):
    pytest.importorskip("pandas")
    path, want = _case("classes_blob")
    was = {c.name: c for c in fstx.metadata(path).columns}
    df = to_pandas(golden_table(path, want))
    keep = list(df.columns)[::-1][:2]
    sub = df[keep]
    sub.attrs = df.attrs
    m = write(sub, tmp_path / "s.fstx")
    assert m.names == keep
    for c in m.columns:
        assert (c.phys, c.logical, c.attrs) == (was[c.name].phys, was[c.name].logical, was[c.name].attrs)
    json.dumps(df.attrs["fstx"])  # the metadata is plain JSON (base64 blobs)


# ---------------------------------------------------------------- polars
def test_polars_native():
    pl = pytest.importorskip("polars")
    df = pl.DataFrame({
        "i": [1, 2, None],
        "f": [1.5, None, 2.5],
        "s": ["a", None, "c"],
        "b": [True, None, False],
        "d": [dt.date(2024, 1, 1), None, dt.date(2020, 2, 29)],
        "t": [dt.datetime(2024, 1, 1, 10, 0, 0), None, dt.datetime(2024, 6, 30, 23, 59, 59, 500000)],
        "c": pl.Series(["x", "y", "x"], dtype=pl.Categorical),
        "i8": pl.Series([1, -1, None], dtype=pl.Int8),
        "u64": pl.Series([1, 2**40, None], dtype=pl.UInt64),
    })
    m = write(df, NATIVE / "polars_native.fstx")
    k = kinds(m)
    c = {x.name: x for x in m.columns}
    assert k["i"] == k["i8"] == ("integer", "none")
    assert k["f"] == ("double", "none") and k["s"] == ("character", "none") and k["b"] == ("logical", "none")
    assert k["d"] == ("double", "Date") and k["t"] == ("double", "POSIXct") and c["t"].tzone == "UTC"
    assert k["c"] == ("integer", "factor") and set(c["c"].levels) >= {"x", "y"}
    assert k["u64"] == ("int64", "none") and c["u64"].attrs is None  # fstx 1.2: past int32, an int64 column
