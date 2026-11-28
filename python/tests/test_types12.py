"""fstx 1.2: int64 and binary columns from Python (DESIGN-1.2.md section 5, one block per row of its table).

Tests marked drop2 read such columns back through the engine's Arrow stream or filter them exactly; they wait for
the engine's drop 2 (conftest.py). The rest needs drop 1 only (writing, metadata, the Python side of filters).
Files go to pytest's tmp_path; golden/v12/ holds the cases R wrote with fstx 1.2 (make_golden.R --v12).
"""
import dataclasses
import hashlib
import json
import warnings

import pyarrow as pa
import pytest

import fstx
from conftest import CASES12, FORMAT_CORRUPT, FORMAT_READ_12, GOLDEN, ids
from fstx import _core, _filters, _read, _write
from fstx import _fxa1 as fxa
from fstx._file import ColumnMeta, FileMeta
from test_read import canon, check_values

drop2 = pytest.mark.drop2
BIT_INT64, BIT_BINARY = 1 << 16, 1 << 17
I64_MAX, I64_MIN = 2**63 - 1, -(2**63)
CLASS64 = fxa.encode({"class": fxa.character(["integer64"])})
BIN = [b"\x00\x01\x02", b"", None, b"text", bytes(range(256)), b"\xc3\x28", b"\x00"]


def write(data, path, **kw):
    """write_fstx with every warning an error; the file's metadata."""
    with warnings.catch_warnings():
        warnings.simplefilter("error", UserWarning)
        fstx.write_fstx(data, path, **kw)
    return fstx.metadata(path)


def attrs(c):
    return fxa.parse(c.attrs) if c.attrs else {}


def r_of(field):
    return json.loads(field.metadata[b"fstx.r"])


# ---------------------------------------------------------------- the build
def test_build_knows_the_new_bits():
    f = fstx.build_info()["features"]
    assert f.get("int64") == 16 and f.get("binary") == 17


def test_auto_wide_is_decided_in_one_place():
    # what int64="auto" does with values past int32 is one constant (DESIGN-1.2.md open decision 1)
    assert _write._AUTO_WIDE in ("native", "integer64")


# ---------------------------------------------------------------- write_fstx(int64=)
SMALL = [1, -2, None, 2**31 - 1]
BIG = [1, 2**40, None, -(2**52)]


@pytest.mark.parametrize("mode,small,big,bits", [
    ("auto", ("integer", None), ("int64", None), BIT_INT64),
    ("native", ("int64", None), ("int64", None), BIT_INT64),
    ("integer64", ("double", ["integer64"]), ("double", ["integer64"]), 0),
    ("double", ("double", None), ("double", None), 0),
])
def test_int64_modes(mode, small, big, bits, tmp_path):
    if mode == "auto" and _write._AUTO_WIDE == "integer64":
        big, bits = ("double", ["integer64"]), 0
    t = pa.table({"small": pa.array(SMALL, pa.int64()), "big": pa.array(BIG, pa.int64())})
    m = write(t, tmp_path / "a.fstx", int64=mode)
    for c, (phys, cls) in zip(m.columns, (small, big)):
        assert (c.phys, c.logical) == (phys, "none"), c.name
        assert (attrs(c)["class"].values if c.attrs else None) == cls, c.name
    assert (m.required_features, m.optional_features) == (bits, 0)


def test_int64_integer64_is_the_1_0_layout(tmp_path):
    # back-compat: int64="integer64" writes what fstx 1.0 writes (a double column with the bits, class in the
    # blob) and sets no feature bit, so fstx 1.0 reads the file
    m = write(pa.table({"x": pa.array([1, 2**40, None], pa.int64())}), tmp_path / "a.fstx", int64="integer64")
    assert m.columns[0].phys == "double" and m.columns[0].attrs == CLASS64 and m.required_features == 0


def test_int64_mode_refused(tmp_path):
    with pytest.raises(ValueError, match="'int64' should be one of \"auto\", \"native\", \"integer64\", \"double\""):
        fstx.write_fstx(pa.table({"x": pa.array([1], pa.int64())}), tmp_path / "a.fstx", int64="float")


@drop2
@pytest.mark.parametrize("mode", ["auto", "native", "integer64"])
def test_int64_values_read_back(mode, tmp_path):
    vals = [I64_MAX, -I64_MAX, None, 0, 2**53 + 1, -(2**53) - 1, -(2**31), 2**31, -1]
    p = tmp_path / "a.fstx"
    write(pa.table({"x": pa.array(vals, pa.int64())}), p, int64=mode)
    t = fstx.read_fstx(p)
    assert t.schema.field("x").type == pa.int64() and t.column("x").to_pylist() == vals
    native = mode == "native" or (mode == "auto" and _write._AUTO_WIDE == "native")
    assert r_of(t.schema.field("x")) == {"phys": "int64" if native else "double", "logical": "none",
                                         "class": ["integer64"]}
    if native:  # missing rows are INT64_MIN in the file and null here; nothing else is null
        assert t.column("x").null_count == 1


@drop2
def test_int64_all_missing_and_empty(tmp_path):
    for vals in ([None, None, None], []):
        p = tmp_path / "a.fstx"
        write(pa.table({"x": pa.array(vals, pa.int64())}), p, int64="native")
        t = fstx.read_fstx(p)
        assert t.schema.field("x").type == pa.int64() and t.column("x").to_pylist() == vals


# ---------------------------------------------------------------- int64 / uint64 / uint32
@pytest.mark.parametrize("mode", ["auto", "native", "integer64"])
def test_int64_min_is_refused(mode, tmp_path):
    p = tmp_path / "a.fstx"
    with pytest.raises(ValueError, match="-9223372036854775808") as e:
        fstx.write_fstx(pa.table({"x": pa.array([5, I64_MIN], pa.int64())}), p, int64=mode)
    assert "'x'" in str(e.value) and "missing value" in str(e.value) and not p.exists()
    write(pa.table({"x": pa.array([5, None], pa.int64())}), p, int64=mode)  # missing is fine


@pytest.mark.parametrize("mode", ["auto", "native", "integer64"])
def test_uint64_past_int64_is_refused(mode, tmp_path):
    p = tmp_path / "a.fstx"
    with pytest.raises(ValueError, match="above 9223372036854775807") as e:
        fstx.write_fstx(pa.table({"u": pa.array([1, 2**63], pa.uint64())}), p, int64=mode)
    assert "'u'" in str(e.value) and 'int64="double"' in str(e.value) and "string" in str(e.value)
    assert not p.exists()


def test_unsigned_columns(tmp_path):
    t = pa.table({"u32_small": pa.array([0, 7, None], pa.uint32()), "u32_big": pa.array([0, 2**32 - 1, None], pa.uint32()),
                  "u64_small": pa.array([0, 5, None], pa.uint64()), "u64_big": pa.array([0, I64_MAX, None], pa.uint64())})
    wide = "int64" if _write._AUTO_WIDE == "native" else "double"
    m = write(t, tmp_path / "a.fstx")
    assert [c.phys for c in m.columns] == ["integer", wide, "integer", wide]
    m = write(t, tmp_path / "b.fstx", int64="native")
    assert [c.phys for c in m.columns] == ["int64"] * 4 and all(c.attrs is None for c in m.columns)


@drop2
def test_unsigned_columns_read_back(tmp_path):
    t = pa.table({"u32": pa.array([0, 2**32 - 1, None], pa.uint32()), "u64": pa.array([0, I64_MAX, None], pa.uint64())})
    p = tmp_path / "a.fstx"
    write(t, p, int64="native")
    got = fstx.read_fstx(p)
    assert got.schema.types == [pa.int64(), pa.int64()]
    assert got.column("u32").to_pylist() == [0, 2**32 - 1, None] and got.column("u64").to_pylist() == [0, I64_MAX, None]


def test_timestamps_unchanged(tmp_path):
    # Arrow timestamps stay float64 seconds (POSIXct); no int64 column, no feature bit (DESIGN-1.2.md section 1)
    t = pa.table({"t": pa.array([0, 1700000000123456789, None], pa.timestamp("ns", "UTC"))})
    m = write(t, tmp_path / "a.fstx", int64="native")
    assert (m.columns[0].phys, m.columns[0].logical, m.required_features) == ("double", "POSIXct", 0)


# ---------------------------------------------------------------- metadata-driven writes
def test_1_0_layout_column_keeps_its_layout(tmp_path):
    src = fstx.read_fstx(GOLDEN / "integer64.fstx")  # R's bit64 integer64 in the 1.0 layout
    was = fstx.metadata(GOLDEN / "integer64.fstx").columns[0]
    assert r_of(src.schema.field(0))["phys"] == "double"
    for mode in ("auto", "integer64", "double"):
        m = write(src, tmp_path / f"{mode}.fstx", int64=mode)
        c = m.columns[0]
        assert (c.phys, c.attrs, m.required_features) == ("double", was.attrs, 0), mode
    m = write(src, tmp_path / "native.fstx", int64="native")
    c = m.columns[0]
    assert (c.phys, m.required_features) == ("int64", BIT_INT64)
    assert "class" not in attrs(c) and c.attrs == _write._blob_without_class64(was.attrs)
    assert c.r_class == ["integer64"]


def test_class_blob_helpers():
    other = fxa.encode({"label": fxa.character(["ids"]), "class": fxa.character(["integer64"])})
    assert _write._blob_without_class64(other) == fxa.encode({"label": fxa.character(["ids"])})
    assert _write._blob_without_class64(CLASS64) is None and _write._blob_without_class64(None) is None
    assert _write._blob_with_class64("x", None) == CLASS64
    assert _write._blob_with_class64("x", CLASS64) == CLASS64
    lab = fxa.encode({"label": fxa.character(["ids"])})
    assert fxa.parse(_write._blob_with_class64("x", lab)) == {"label": fxa.character(["ids"]),
                                                               "class": fxa.character(["integer64"])}


@drop2
def test_1_0_layout_column_read_back_both_ways(tmp_path):
    src = fstx.read_fstx(GOLDEN / "integer64.fstx")
    p = tmp_path / "n.fstx"
    write(src, p, int64="native")
    assert fstx.read_fstx(p).column(0).to_pylist() == src.column(0).to_pylist()


@drop2
def test_native_column_keeps_its_layout(tmp_path):
    vals = [1, 2**40, None, -7]
    p = tmp_path / "a.fstx"
    write(pa.table({"x": pa.array(vals, pa.int64())}), p, int64="native")
    t = fstx.read_fstx(p)
    for mode in ("auto", "native", "double"):  # an int64 column from a file stays one
        m = write(t, tmp_path / f"{mode}.fstx", int64=mode)
        assert (m.columns[0].phys, m.columns[0].attrs) == ("int64", None), mode
        assert fstx.read_fstx(tmp_path / f"{mode}.fstx").column(0).to_pylist() == vals
    m = write(t, tmp_path / "old.fstx", int64="integer64")  # unless the 1.0 layout is asked for
    assert (m.columns[0].phys, m.columns[0].attrs, m.required_features) == ("double", CLASS64, 0)
    assert fstx.read_fstx(tmp_path / "old.fstx").column(0).to_pylist() == vals
    small = pa.table({"x": pa.array([1, 2, None], pa.int64())})  # values that fit int32 stay int64 too
    p2 = tmp_path / "small.fstx"
    write(small, p2, int64="native")
    m = write(fstx.read_fstx(p2), tmp_path / "small2.fstx")
    assert m.columns[0].phys == "int64"


def test_schema_names_the_new_types(tmp_path):
    t = pa.table({"i": pa.array([1, 2**40], pa.int64()), "b": pa.array([b"a", None])})
    m = write(t, tmp_path / "a.fstx", int64="native")
    assert [c.phys for c in m.columns] == ["int64", "binary"] and [c.logical for c in m.columns] == ["none", "none"]
    assert m.columns[0].r_class == ["integer64"] and m.columns[1].r_class == []
    assert m.required_features == BIT_INT64 | BIT_BINARY
    s = m.schema  # what read_fstx returns, without reading
    assert s.types == [pa.int64(), pa.binary()]
    assert r_of(s.field("i")) == {"phys": "int64", "logical": "none", "class": ["integer64"]}
    assert r_of(s.field("b")) == {"phys": "binary", "logical": "none"}


def test_feature_bits_set_exactly_when_used(tmp_path):
    p = tmp_path / "a.fstx"
    plain = pa.table({"i": pa.array([1, 2], pa.int64()), "s": ["a", "b"], "d": [1.5, 2.5]})
    assert write(plain, p).required_features == 0
    assert write(pa.table({"i": pa.array([1, 2**40], pa.int64())}), p, int64="integer64").required_features == 0
    assert write(pa.table({"i": pa.array([1, 2], pa.int64())}), p, int64="native").required_features == BIT_INT64
    assert write(pa.table({"b": pa.array([b"x"])}), p).required_features == BIT_BINARY
    m = write(pa.table({"b": pa.array([b"x"]), "i": pa.array([1], pa.int64())}), p, int64="native")
    assert (m.required_features, m.optional_features) == (BIT_INT64 | BIT_BINARY, 0)


# ---------------------------------------------------------------- binary
def _binary_array(kind):
    if kind == "binary_view":
        if not hasattr(pa, "binary_view"):
            pytest.skip("this pyarrow has no binary_view")
        return pa.array(BIN, pa.binary()).cast(pa.binary_view())
    if kind == "fixed_size_binary":
        return pa.array([b"ab", b"\x00\x00", None, b"\xff\x00"], pa.binary(2))
    return pa.array(BIN, {"binary": pa.binary(), "large_binary": pa.large_binary()}[kind])


@pytest.mark.parametrize("kind", ["binary", "large_binary", "binary_view", "fixed_size_binary"])
def test_binary_types_are_stored_as_binary(kind, tmp_path):
    m = write(pa.table({"b": _binary_array(kind)}), tmp_path / "a.fstx")
    c = m.columns[0]
    assert (c.phys, c.logical, c.attrs, m.required_features) == ("binary", "none", None, BIT_BINARY)


@drop2
@pytest.mark.parametrize("kind", ["binary", "large_binary", "binary_view", "fixed_size_binary"])
def test_binary_types_read_back(kind, tmp_path):
    a = _binary_array(kind)
    p = tmp_path / "a.fstx"
    write(pa.table({"b": a}), p)
    t = fstx.read_fstx(p)
    assert t.schema.field("b").type == pa.binary()  # fixed_size_binary: the width is not kept
    assert t.column("b").to_pylist() == a.to_pylist()


def test_binary_with_metadata(tmp_path):
    a = pa.array([b"a\xff", None])
    # text fstx handed out as bytes (read_fstx(invalid_utf8="binary")) goes back as text, as in 1.0
    f = pa.field("b", pa.binary(), metadata={b"fstx.r": b'{"phys":"character","logical":"none"}'})
    assert write(pa.Table.from_arrays([a], schema=pa.schema([f])), tmp_path / "a.fstx").columns[0].phys == "character"
    # metadata of another type: ignored with a warning, the bytes are written as binary
    f = pa.field("b", pa.binary(), metadata={b"fstx.r": b'{"phys":"integer","logical":"none"}'})
    with pytest.warns(UserWarning, match="metadata is ignored"):
        fstx.write_fstx(pa.Table.from_arrays([a], schema=pa.schema([f])), tmp_path / "b.fstx")
    assert fstx.metadata(tmp_path / "b.fstx").columns[0].phys == "binary"
    # a binary column's own attributes travel (R's blob class, say)
    blob = fxa.encode({"class": fxa.character(["blob", "vctrs_list_of", "vctrs_vctr", "list"])})
    f = pa.field("b", pa.binary(), metadata={b"fstx.r": b'{"phys":"binary","logical":"none","class":["blob"]}',
                                              b"fstx.attrs": blob})
    m = write(pa.Table.from_arrays([a], schema=pa.schema([f])), tmp_path / "c.fstx")
    assert (m.columns[0].phys, m.columns[0].attrs) == ("binary", blob)
    assert m.columns[0].r_class == ["blob", "vctrs_list_of", "vctrs_vctr", "list"]


def test_binary_with_nul_and_odd_bytes_is_written(tmp_path):
    # none of the string checks apply: NUL, bytes that are not UTF-8, empty values
    vals = [b"\x00" * 10, b"\xff\xfe", b"", None, b"a\x00b"] * 20000  # repeats: dictionary-coded on the way
    m = write(pa.table({"b": pa.array(vals, pa.large_binary())}), tmp_path / "a.fstx")
    assert m.columns[0].phys == "binary" and m.nrow == 100000


@drop2
def test_binary_read_back(tmp_path):
    big = bytes([0x41, 0x00, 0xFF]) * 30000  # 90 KB, past 64 KB
    vals = BIN + [big]
    p = tmp_path / "a.fstx"
    write(pa.table({"b": pa.array(vals)}), p)
    t = fstx.read_fstx(p)
    assert t.column("b").to_pylist() == vals and t.column("b").null_count == 1
    # few distinct values (dictionary pages) and all distinct, over several batches
    n = 5000
    rep = [None if i % 11 == 0 else bytes([i % 7]) * (i % 5) for i in range(n)]
    uniq = [i.to_bytes(4, "little") for i in range(n)]
    q = tmp_path / "b.fstx"
    write(pa.table({"id": pa.array(range(n), pa.int32()), "rep": pa.array(rep), "uniq": pa.array(uniq)}), q)
    t = fstx.read_fstx(q)
    assert t.schema.types == [pa.int32(), pa.binary(), pa.binary()]
    assert t.column("rep").to_pylist() == rep and t.column("uniq").to_pylist() == uniq
    # read(): plain binary also when factors are asked for (as_factor is for character columns)
    assert fstx.read_fstx(q, as_factor=True).schema.field("rep").type == pa.binary()
    # rows, filters on another column, batches, the C stream protocol: plain binary every time
    assert fstx.read_fstx(q, rows=[3, 1, 1]).column("rep").to_pylist() == [rep[3], rep[1], rep[1]]
    got = fstx.read_fstx(q, filters=[("id", ">=", 4990)])
    assert got.column("rep").to_pylist() == rep[4990:]
    with fstx.open(q) as f:
        batches = list(f.to_batches(batch_rows=700))
        assert all(b.schema.field("rep").type == pa.binary() for b in batches)
        assert sum((b.column(1).to_pylist() for b in batches), []) == rep
        assert pa.table(f).column("rep").to_pylist() == rep
    # invalid_utf8 does not apply to binary
    t = fstx.read_fstx(p, invalid_utf8="error")
    assert t.column("b").to_pylist() == vals


def test_large_binary_past_the_string_threshold():
    c = ColumnMeta(index=0, name="b", phys="binary", logical="none", ordered=False, levels=None, tzone=None,
                   attrs=None, compressed_bytes=1, raw_bytes=_read._MAX_STRING + 1, pages=1)
    assert _read._column(c, "arrow", False, True).field.type == pa.large_binary()
    assert _read._column(c, "arrow", True, True).field.type == pa.large_binary()  # invalid_utf8="binary": the same
    small = dataclasses.replace(c, raw_bytes=_read._MAX_STRING)
    assert _read._column(small, "arrow", False, True).field.type == pa.binary()
    i64 = dataclasses.replace(c, phys="int64")
    assert _read._column(i64, "float", False, True).field.type == pa.int64()


def test_as_factor_refuses_a_binary_column(tmp_path):
    p = tmp_path / "a.fstx"
    write(pa.table({"b": pa.array([b"x"]), "s": ["y"]}), p)
    with pytest.raises(ValueError, match="not a character column"):
        fstx.read_fstx(p, as_factor=["b"])


# ---------------------------------------------------------------- filters: Python's side (no engine)
def _meta12():
    def col(i, name, phys, attrs=None):
        return ColumnMeta(index=i, name=name, phys=phys, logical="none", ordered=False, levels=None, tzone=None,
                          attrs=attrs, compressed_bytes=0, raw_bytes=0, pages=1)

    cols = [col(0, "i", "int64"), col(1, "b", "binary"), col(2, "old", "double", CLASS64)]
    return FileMeta(path="x.fstx", nrow=10, ncol=3, row_groups=1, pages=3, profile="balanced", format_version=3,
                    file_size=0, row_group_size=1024, table_attrs=None, columns=cols)


def preds(*terms):
    return _filters.to_preds(_meta12(), list(terms))


def test_int64_filter_values():
    EQ, NE, LT, GE, IN = _core.OP_EQ, _core.OP_NE, _core.OP_LT, _core.OP_GE, _core.OP_IN
    NONE, NOTNA, ISNA = _core.OP_NONE, _core.OP_NOTNA, _core.OP_ISNA
    assert preds(("i", "==", 5)) == [{"col": 0, "op": EQ, "i64": 5}]
    assert preds(("i", "==", 2**53 + 1)) == [{"col": 0, "op": EQ, "i64": 2**53 + 1}]  # exact, no double
    assert preds(("i", "<", 2.5)) == [{"col": 0, "op": LT, "value": 2.5}]  # a float: compared as a real number
    assert preds(("i", "==", I64_MIN)) == [{"col": 0, "op": EQ, "i64": I64_MIN}]
    # ints outside the int64 range are decided here
    assert preds(("i", "==", 2**63)) == [{"col": 0, "op": NONE}]
    assert preds(("i", "!=", 2**63)) == [{"col": 0, "op": NOTNA}]
    assert preds(("i", "<", 2**63)) == [{"col": 0, "op": NOTNA}]
    assert preds(("i", ">=", 2**63)) == [{"col": 0, "op": NONE}]
    assert preds(("i", ">", I64_MIN - 1)) == [{"col": 0, "op": NOTNA}]
    assert preds(("i", "<=", I64_MIN - 1)) == [{"col": 0, "op": NONE}]
    assert preds(("i", "!=", I64_MIN - 1)) == [{"col": 0, "op": NOTNA}]
    # NA and NaN match nothing; in: whole numbers in range, the rest can never be equal
    assert preds(("i", "==", None)) == [{"col": 0, "op": NONE}]
    assert preds(("i", "==", float("nan"))) == [{"col": 0, "op": NONE}]
    assert preds(("i", "in", [1, 2.0, 2.5, 2**64, None, float("nan"), float("inf"), -3])) == \
        [{"col": 0, "op": IN, "i64_set": [1, 2, -3], "set_na": True}]
    assert preds(("i", "between", (-1, 2**60))) == [{"col": 0, "op": GE, "i64": -1},
                                                    {"col": 0, "op": _core.OP_LE, "i64": 2**60}]
    assert preds(("i", "is_null")) == [{"col": 0, "op": ISNA}]
    assert preds(("i", "!=", 3)) == [{"col": 0, "op": NE, "i64": 3}]
    np = pytest.importorskip("numpy")
    assert preds(("i", "==", np.int64(2**62)), ("i", "<", np.uint64(2**63))) == \
        [{"col": 0, "op": EQ, "i64": 2**62}, {"col": 0, "op": NOTNA}]
    assert preds(("i", "<", np.float64(1.5))) == [{"col": 0, "op": LT, "value": 1.5}]
    for bad in (True, "5", b"5", np.bool_(True)):
        with pytest.raises(ValueError, match="int64 column"):
            preds(("i", "==", bad))
    with pytest.raises(ValueError, match="int64 column"):
        preds(("i", "in", [1, True]))


def test_binary_filter_values():
    EQ, NE, IN = _core.OP_EQ, _core.OP_NE, _core.OP_IN
    assert preds(("b", "==", b"a\x00b")) == [{"col": 1, "op": EQ, "strs": [b"a\x00b"]}]
    assert preds(("b", "!=", bytearray(b"\xff"))) == [{"col": 1, "op": NE, "strs": [b"\xff"]}]
    assert preds(("b", "==", memoryview(b"xy"))) == [{"col": 1, "op": EQ, "strs": [b"xy"]}]
    assert preds(("b", "in", [b"x", None, b""])) == [{"col": 1, "op": IN, "strs": [b"x", b""], "set_na": True}]
    assert preds(("b", "==", None)) == [{"col": 1, "op": _core.OP_NONE}]
    assert preds(("b", "not_null")) == [{"col": 1, "op": _core.OP_NOTNA}]
    with pytest.raises(ValueError, match="bytes values \\(not str\\)"):
        preds(("b", "==", "x"))
    with pytest.raises(ValueError, match="bytes values"):
        preds(("b", "in", [b"x", "y"]))
    with pytest.raises(ValueError, match="binary columns support ==, !=, in, is_null and not_null only"):
        preds(("b", "<", b"x"))


def test_1_0_layout_integer64_stays_unfilterable():
    with pytest.raises(ValueError, match="class 'integer64' cannot be filtered"):
        preds(("old", "==", 1))
    with pytest.raises(ValueError, match="class 'integer64' cannot be filtered"):
        fstx.read_fstx(GOLDEN / "integer64.fstx", filters=[("i64", "==", 1)])


# ---------------------------------------------------------------- filters in the engine
I64_VALS = ([0, 1, 2, 3, -1, 2**53, 2**53 + 1, -(2**53) - 1, I64_MAX, -I64_MAX, None, 2**31] * 250)


@pytest.fixture
def i64_file(tmp_path):
    p = tmp_path / "f.fstx"
    write(pa.table({"x": pa.array(I64_VALS, pa.int64()), "n": pa.array(range(len(I64_VALS)), pa.int32())}), p,
          int64="native")
    return p


def _expect(fn):
    return [v for v in I64_VALS if v is not None and fn(v)]


@drop2
@pytest.mark.parametrize("term,fn", [
    (("x", "==", 2**53), lambda v: v == 2**53),             # not 2^53 + 1, which the double 2^53 also is
    (("x", "==", 2**53 + 1), lambda v: v == 2**53 + 1),
    (("x", "<", 2.5), lambda v: v < 2.5),                   # a float: exact real compare
    (("x", ">", float(2**53)), lambda v: v > 2**53),
    (("x", ">=", -0.5), lambda v: v >= 0),
    (("x", "==", I64_MAX), lambda v: v == I64_MAX),         # in pages whose rounded max is above it
    (("x", "<=", -I64_MAX), lambda v: v <= -I64_MAX),
    (("x", "!=", 2**63), lambda v: True),                   # out of range: every row that is not missing
    (("x", "<", 2**63), lambda v: True),
    (("x", ">", 2**63), lambda v: False),
    (("x", ">", I64_MIN - 1), lambda v: True),
    (("x", "in", [1, 2**53 + 1, 2.0, 2.5]), lambda v: v in (1, 2, 2**53 + 1)),
    (("x", "between", (-1, 2**53)), lambda v: -1 <= v <= 2**53),
    (("x", "!=", 3), lambda v: v != 3),
], ids=lambda x: repr(x) if isinstance(x, tuple) else "")
def test_int64_filters(i64_file, term, fn):
    assert fstx.read_fstx(i64_file, columns=["x"], filters=[term]).column("x").to_pylist() == _expect(fn)


@drop2
def test_int64_filters_missing_and_numpy(i64_file):
    got = fstx.read_fstx(i64_file, columns=["n"], filters=[("x", "is_null")]).column("n").to_pylist()
    assert got == [i for i, v in enumerate(I64_VALS) if v is None]
    got = fstx.read_fstx(i64_file, columns=["x"], filters=[("x", "in", [None, 1])]).column("x").to_pylist()
    assert got == [v for v in I64_VALS if v in (None, 1)]
    np = pytest.importorskip("numpy")
    got = fstx.read_fstx(i64_file, columns=["x"], filters=[("x", "==", np.int64(2**53 + 1))]).column("x").to_pylist()
    assert got == _expect(lambda v: v == 2**53 + 1)


def test_binary_filters(tmp_path):
    # the matching rows are read from the int column, so no binary column goes through the Arrow stream
    vals = BIN * 300
    p = tmp_path / "b.fstx"
    write(pa.table({"n": pa.array(range(len(vals)), pa.int32()), "b": pa.array(vals)}), p)

    def rows(*terms):
        return fstx.read_fstx(p, columns=["n"], filters=list(terms)).column("n").to_pylist()

    def want(fn):
        return [i for i, v in enumerate(vals) if fn(v)]

    assert rows(("b", "==", b"\x00\x01\x02")) == want(lambda v: v == b"\x00\x01\x02")
    assert rows(("b", "==", bytearray(b"\x00"))) == want(lambda v: v == b"\x00")  # NUL is a byte like any other
    assert rows(("b", "!=", b"")) == want(lambda v: v is not None and v != b"")
    assert rows(("b", "in", [memoryview(b"text"), None])) == want(lambda v: v in (b"text", None))
    assert rows(("b", "is_null")) == want(lambda v: v is None)
    assert rows(("b", "not_null"), ("b", "!=", b"\x00")) == want(lambda v: v is not None and v != b"\x00")
    assert rows(("b", "==", b"\xc3")) == []


@drop2
def test_binary_filters_read_back(tmp_path):
    vals = BIN * 300
    p = tmp_path / "b.fstx"
    write(pa.table({"b": pa.array(vals)}), p)
    got = fstx.read_fstx(p, filters=[("b", "in", [b"text", None])]).column("b").to_pylist()
    assert got == [v for v in vals if v in (b"text", None)]


# ---------------------------------------------------------------- polars and pandas
@drop2
def test_polars_round_trip(tmp_path):
    pl = pytest.importorskip("polars")
    df = pl.DataFrame({
        "i": pl.Series([2**62, None, -(2**63) + 1], dtype=pl.Int64),
        "u": pl.Series([I64_MAX, None, 0], dtype=pl.UInt64),
        "b": pl.Series([b"\x00a", None, b""], dtype=pl.Binary),
        "small": pl.Series([1, None, -1], dtype=pl.Int64),
    })
    p = tmp_path / "a.fstx"
    write(df, p)
    back = fstx.to_polars(fstx.read_fstx(p))
    assert back["i"].dtype == pl.Int64 and back["i"].to_list() == df["i"].to_list()
    assert back["u"].dtype == pl.Int64 and back["u"].to_list() == df["u"].to_list()
    assert back["b"].dtype == pl.Binary and back["b"].to_list() == df["b"].to_list()
    assert back["small"].to_list() == df["small"].to_list()  # Int32: "auto" stores values that fit as R integers
    with pytest.raises(ValueError, match="above 9223372036854775807"):
        fstx.write_fstx(pl.DataFrame({"u": pl.Series([2**64 - 1], dtype=pl.UInt64)}), tmp_path / "x.fstx")


@drop2
def test_pandas_round_trip(tmp_path):
    pd = pytest.importorskip("pandas")
    t = pa.table({"i": pa.array([2**53 + 1, None, -5], pa.int64()), "b": pa.array([b"\x00", None, b"xy"])})
    p = tmp_path / "a.fstx"
    write(t, p, int64="native")
    got = fstx.read_fstx(p)
    df = fstx.to_pandas(got)
    assert str(df["i"].dtype) == "Int64" and df["i"][0] == 2**53 + 1 and pd.isna(df["i"][1])
    assert df["b"].tolist() == [b"\x00", None, b"xy"]
    # pyarrow's own to_pandas turns int64 with nulls into float64, which loses precision past 2^53 (pyarrow's rule;
    # DESIGN-1.2.md section 5); pandas' Arrow dtypes keep it
    plain = got.to_pandas()
    assert plain["i"].dtype == "float64" and plain["i"][0] != 2**53 + 1
    assert got.to_pandas(types_mapper=pd.ArrowDtype)["i"][0] == 2**53 + 1
    # back to fstx: df.attrs["fstx"] keeps the types
    m = write(df, tmp_path / "b.fstx")
    assert [c.phys for c in m.columns] == ["int64", "binary"]
    back = fstx.read_fstx(tmp_path / "b.fstx")
    assert back.column("i").to_pylist() == t.column("i").to_pylist()
    assert back.column("b").to_pylist() == t.column("b").to_pylist()


def test_pandas_bytes_are_binary(tmp_path):
    pd = pytest.importorskip("pandas")
    m = write(pd.DataFrame({"raw": [b"a", None, b"\x00"], "i": pd.array([1, None, 2**40], dtype="Int64")}),
              tmp_path / "a.fstx")
    assert [c.phys for c in m.columns] == ["binary", "int64" if _write._AUTO_WIDE == "native" else "double"]


# ---------------------------------------------------------------- appends, threads
def test_append_follows_the_file(tmp_path):
    p = tmp_path / "n.fstx"
    write(pa.table({"x": pa.array([1, 2**40], pa.int64())}), p, int64="native")
    fstx.write_fstx(pa.table({"x": pa.array([3, None], pa.int64())}), p, append=True, int64="integer64")
    m = fstx.metadata(p)
    assert (m.nrow, m.columns[0].phys, m.columns[0].attrs, m.required_features) == (4, "int64", None, BIT_INT64)
    q = tmp_path / "o.fstx"
    write(pa.table({"x": pa.array([1, 2**40], pa.int64())}), q, int64="integer64")
    fstx.write_fstx(pa.table({"x": pa.array([3], pa.int64())}), q, append=True, int64="native")
    m = fstx.metadata(q)
    assert (m.nrow, m.columns[0].phys, m.columns[0].attrs, m.required_features) == (3, "double", CLASS64, 0)
    b = tmp_path / "b.fstx"
    write(pa.table({"b": pa.array([b"\x00"])}), b)
    fstx.write_fstx(pa.table({"b": pa.array([b"x", None], pa.large_binary())}), b, append=True)
    m = fstx.metadata(b)
    assert (m.nrow, m.columns[0].phys, m.required_features) == (3, "binary", BIT_BINARY)


@drop2
def test_append_read_back(tmp_path):
    p = tmp_path / "n.fstx"
    write(pa.table({"x": pa.array([1, 2**40], pa.int64()), "b": pa.array([b"\x00", None])}), p, int64="native")
    fstx.write_fstx(pa.table({"x": pa.array([None, -(2**60)], pa.int64()), "b": pa.array([b"", b"z"])}), p, append=True)
    t = fstx.read_fstx(p)
    assert t.column("x").to_pylist() == [1, 2**40, None, -(2**60)]
    assert t.column("b").to_pylist() == [b"\x00", None, b"", b"z"]


def test_same_bytes_on_every_thread_count(tmp_path):
    n = 150000
    t = pa.table({
        "i": pa.array([None if k % 97 == 0 else k * 3000000001 for k in range(n)], pa.int64()),
        "r": pa.array([(k % 13) * 5000000000 for k in range(n)], pa.int64()),
        "b": pa.array([None if k % 89 == 0 else k.to_bytes(3, "little") for k in range(n)]),
        "d": pa.array([None if k % 7 == 0 else bytes([k % 5, 0]) for k in range(n)]),
    })
    files = []
    for th in (1, 2, 8):
        p = tmp_path / f"t{th}.fstx"
        write(t, p, threads=th, int64="native")
        files.append(p.read_bytes())
    assert files[0] == files[1] == files[2]


# ---------------------------------------------------------------- the 1.0 format files under 1.2
@pytest.mark.parametrize("path,want", FORMAT_READ_12, ids=ids(FORMAT_READ_12))
def test_format_files_with_a_known_bit_now_read(path, want):
    m = fstx.metadata(path)
    assert m.required_features & (BIT_INT64 if "int64" in path.name else BIT_BINARY)
    assert all(c.phys not in ("int64", "binary") for c in m.columns)
    assert fstx.read_fstx(path).num_rows == m.nrow


@pytest.mark.parametrize("path,want", FORMAT_CORRUPT, ids=ids(FORMAT_CORRUPT))
def test_format_files_now_corrupt(path, want):
    with pytest.raises(fstx.FstxError) as e:
        fstx.read_fstx(path)
    assert "Update fstx" not in str(e.value), str(e.value)
    if "kind7" in path.name:  # found when the page is decoded: the stream carries the message, not the code
        assert e.value.code in (None, fstx.ERR_CORRUPT), (e.value.code, str(e.value))
    else:  # a type without its feature bit: refused at open
        with pytest.raises(fstx.FstxError) as e2:
            fstx.open(path)
        assert e2.value.code == fstx.ERR_CORRUPT, (e2.value.code, str(e2.value))


# ---------------------------------------------------------------- golden files R wrote with fstx 1.2
@drop2
@pytest.mark.parametrize("path,want", CASES12, ids=ids(CASES12))
def test_golden12_values(path, want):
    t = fstx.read_fstx(path)
    assert (t.num_rows, t.num_columns) == (want["nrow"], want["ncol"])
    for j, w in enumerate(want["columns"]):
        col = t.column(j)
        assert str(col.type) == w["arrow_type"], w["name"]
        assert col.null_count == w["nulls"], w["name"]
        check_values(col, w)
        assert hashlib.md5(canon(col, w)).hexdigest() == w["md5"], w["name"]
    t.validate(full=True)


@pytest.mark.parametrize("path,want", CASES12, ids=ids(CASES12))
def test_golden12_metadata(path, want):
    m = fstx.metadata(path)
    bits = 0
    for c, w in zip(m.columns, want["columns"]):
        assert (c.phys, c.logical) == (w["stored"]["phys"], w["stored"]["logical"]), c.name
        assert c.r_class == w["r_class"], c.name
        bits |= BIT_INT64 if c.phys == "int64" else BIT_BINARY if c.phys == "binary" else 0
    assert m.required_features == bits


@drop2
@pytest.mark.parametrize("path,want", CASES12, ids=ids(CASES12))
def test_golden12_written_back_same_bytes(path, want, tmp_path):
    # read in Python, written again with R's settings: the bytes R wrote (DESIGN-1.2.md section 6, rule 3)
    if want["fstx"] != fstx.build_info()["engine_version"]:
        pytest.skip(f"R wrote this file with fstx {want['fstx']}, this engine is {fstx.build_info()['engine_version']}"
                    " (created_by differs)")
    q = tmp_path / path.name
    write(fstx.read_fstx(path), q, threads=1, **want.get("write_args") or {})
    assert q.read_bytes() == path.read_bytes()
