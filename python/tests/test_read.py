"""Reading: every golden file value by value against its sidecar, then rows, columns, filters, batches,
metadata, the pandas / polars helpers and the risky cases of DESIGN.md section 8."""
import datetime as dt
import hashlib
import json
import re
import shutil
import struct
import sys
import threading
import warnings

import pyarrow as pa
import pyarrow.compute as pc
import pytest

import fstx
from conftest import CASES, FORMAT_DIR, FORMAT_READ, FORMAT_REFUSE, GOLDEN, ids, one, quiet, same
from fstx import _fxa1, _meta
from fstx._read import row_names

ALL_READ = CASES + FORMAT_READ
INT_NA = -(2**31)
I64_NA = -(2**63)
NA_REAL = struct.pack("<Q", 0x7FF00000000007A2)


def str_raw(vals):
    out = bytearray()
    for v in vals:
        if v is None:
            out += struct.pack("<i", -1)
        else:
            b = v.encode("utf-8") if isinstance(v, str) else v
            out += struct.pack("<i", len(b)) + b
    return bytes(out)


def canon(col, w):
    """The canonical bytes of make_golden.R (its header), from a column read with datetimes="float" and
    invalid_utf8="binary": what R holds after its own read."""
    at = w["arrow_type"]
    if at == "int64":
        return b"".join(struct.pack("<q", I64_NA if v is None else v) for v in col.to_pylist())
    if at.startswith("dictionary"):
        a = one(col)
        codes = [INT_NA if v is None else v + 1 for v in a.indices.to_pylist()]
        return struct.pack("<%di" % len(codes), *codes) + str_raw(a.dictionary.to_pylist())
    ty = w["r_typeof"]
    vals = col.to_pylist()
    if at == "binary":  # fstx 1.2: per value the length (-1 = NULL), then the bytes
        return str_raw(vals)
    if ty in ("logical", "integer"):
        return struct.pack("<%di" % len(vals), *[INT_NA if v is None else int(v) for v in vals])
    if ty == "double":
        return b"".join(NA_REAL if v is None else struct.pack("<d", v) for v in vals)
    assert ty == "character"
    return str_raw(vals)


def want_double(w, key="values"):
    out = []
    for v in w[key]:
        out.append({"NaN": float("nan"), "Inf": float("inf"), "-Inf": float("-inf"), "-0": -0.0}.get(v, v)
                   if isinstance(v, str) else v)
    return out


def check_values(col, w):
    """The values the sidecar lists (all of them, or the first 12 of a long column)."""
    at = w["arrow_type"]
    k = len(w.get("values", w.get("codes", w.get("days", w.get("seconds", [])))))
    a = one(col).slice(0, k)
    if at == "int64":
        assert [None if v is None else str(v) for v in a.to_pylist()] == w["values"]
    elif at.startswith("dictionary"):
        assert [None if v is None else v + 1 for v in a.indices.to_pylist()] == w["codes"]
        assert one(col).dictionary.to_pylist() == w["levels"]
        assert col.type.ordered == at.endswith("ordered=1>")
    elif at == "date32[day]":
        assert a.cast(pa.int32()).to_pylist() == w["date32"]
    elif at.startswith("timestamp"):
        assert [None if v is None else str(v) for v in a.cast(pa.int64()).to_pylist()] == w["us"]
    elif at == "double":
        got = a.to_pylist()
        for g, v, bits in zip(got, w["values"], w["bits"]):
            if v is None:
                assert g is None  # NA_real_ and its quiet form are null
            else:
                assert g is not None and struct.pack(">d", g).hex() == bits  # exact bits: NaN, -0, subnormal
    elif at == "binary":  # fstx 1.2
        for g, v in zip(a.to_pylist(), w["values"]):
            if isinstance(v, dict):
                assert len(g) == v["bytes"] and hashlib.md5(g).hexdigest() == v["md5"]
            else:
                assert (None if g is None else g.hex()) == v
    elif at == "string":
        got = a.to_pylist()
        hexes = w.get("hex")
        for i, (g, v) in enumerate(zip(got, w["values"])):
            if isinstance(v, dict):  # a long string: byte count and md5
                b = g.encode("utf-8")
                assert len(b) == v["bytes"] and hashlib.md5(b).hexdigest() == v["md5"]
            elif v is None and hexes and hexes[i] is not None:  # not valid UTF-8: bad bytes replaced
                assert g == bytes.fromhex(hexes[i]).decode("utf-8", "replace") and "�" in g
            else:
                assert g == v
    else:
        assert at in ("bool", "int32")
        assert a.to_pylist() == w["values"]


# ---------------------------------------------------------------- every golden file
@pytest.mark.parametrize("path,want", ALL_READ, ids=ids(ALL_READ))
def test_read_golden_values(path, want):
    t = quiet(path)
    assert isinstance(t, pa.Table)
    assert (t.num_rows, t.num_columns) == (want["nrow"], want["ncol"])
    assert t.column_names == [c["name"] for c in want["columns"]]
    for j, w in enumerate(want["columns"]):
        col = t.column(j)
        assert str(col.type) == w["arrow_type"], w["name"]
        assert len(col) == w["n"]
        assert col.null_count == w["nulls"], w["name"]
        check_values(col, w)
    t.validate(full=True)


@pytest.mark.parametrize("path,want", ALL_READ, ids=ids(ALL_READ))
def test_read_golden_md5(path, want):
    # the exact stored values (Date / POSIXct as stored, bad UTF-8 as bytes): R's canonical md5 of every column
    t = fstx.read_fstx(path, datetimes="float", invalid_utf8="binary")
    for j, w in enumerate(want["columns"]):
        assert hashlib.md5(canon(t.column(j), w)).hexdigest() == w["md5"], w["name"]


@pytest.mark.parametrize("path,want", ALL_READ, ids=ids(ALL_READ))
def test_read_golden_metadata(path, want):
    m = fstx.metadata(path)
    t = quiet(path)
    assert m.schema == t.schema and m.schema.equals(t.schema, check_metadata=True)
    with fstx.open(path) as f:
        assert f.schema.equals(t.schema, check_metadata=True)
    tw = want["table"]
    smd = t.schema.metadata or {}
    if tw["blob"] == "none":
        assert b"fstx.table_attrs" not in smd
    else:
        assert _meta.blob(smd[b"fstx.table_attrs"]) == m.table_attrs  # base64 in the metadata
        smd[b"fstx.table_attrs"].decode("ascii")
    # the row count travels only with a table blob in R serialization
    assert smd.get(b"fstx.nrow") == (str(m.nrow).encode() if tw["blob"] == "R-serialization" else None)
    if tw["blob"] == "R-serialization":
        assert m.table_class is None and m.attributes is None
    else:
        assert m.table_class == tw["class"]
        rn = tw["row_names"]
        assert m.row_names == (None if rn == "automatic" else rn["values"])
        for k, v in tw.get("attrs", {}).items():
            if not v.get("opaque"):
                assert m.attributes[k].type == v["type"]
    for col, fld, w in zip(m.columns, t.schema, want["columns"]):
        s = w["stored"]
        md = fld.metadata or {}
        if s["blob"] == "R-serialization":
            assert col.r_class is None and col.attributes is None
        else:
            assert col.r_class == w["r_class"], col.name
            for k, v in w.get("attrs", {}).items():
                if not v.get("opaque"):
                    got = col.attributes[k]
                    assert got.type == v["type"] and len(got.values) == len(v["values"])
                    if v["type"] in ("character", "integer", "logical"):
                        assert got.values == v["values"]
        assert _meta.blob(md.get(b"fstx.attrs")) == col.attrs
        r = json.loads(md[b"fstx.r"])  # on every column
        assert (r["phys"], r["logical"]) == (s["phys"], s["logical"])
        assert r.get("class", []) == (col.r_class or [])
        assert ("tzone" in r) == (s["logical"] == "POSIXct" and s["tzone_present"])
        if s["logical"] == "POSIXct":
            assert r.get("tzone") == (s.get("tzone", "") if s["tzone_present"] else None)


@pytest.mark.parametrize("path,want", FORMAT_REFUSE, ids=ids(FORMAT_REFUSE))
def test_newer_files_are_refused_by_read(path, want):
    with pytest.raises(fstx.FstxError) as ei:
        fstx.read_fstx(path)
    assert re.search(want["message_regex"], str(ei.value)), str(ei.value)


def test_verify_nulls_option():
    assert fstx.set_options() == {"verify_nulls": False}
    try:
        assert fstx.set_options(verify_nulls=True) == {"verify_nulls": True}
        for path, want in ALL_READ:
            assert quiet(path).num_rows == want["nrow"]
    finally:
        fstx.set_options(verify_nulls=False)


# ---------------------------------------------------------------- the risky cases (DESIGN.md section 8)
def test_integer64_bits_not_validity():
    t = fstx.read_fstx(GOLDEN / "integer64.fstx")
    v = t.column("i64").to_pylist()
    assert t.schema.field("i64").type == pa.int64()
    # bits of NA_real_, quiet NA, Inf and NaN are numbers; only INT64_MIN is missing
    assert v[7] is None and v.count(None) == 1
    assert v[8:] == [0x7FF00000000007A2, 0x7FF80000000007A2, 0x7FF0000000000000, 0x7FF8000000000000]
    assert v[3:7] == [2**53 + 1, -(2**53) - 1, 2**63 - 1, -(2**63) + 1]
    # the same through a row list (the stream's other decode path) and in every order
    order = [11, 8, 7, 9, 0, 8, 10]
    assert fstx.read_fstx(GOLDEN / "integer64.fstx", rows=order).column(0).to_pylist() == [v[i] for i in order]
    assert fstx.read_fstx(GOLDEN / "integer64.fstx", rows=[7, 8, 9]).column(0).to_pylist() == v[7:10]
    assert json.loads(t.schema.field("i64").metadata[b"fstx.r"]) == {
        "phys": "double", "logical": "none", "class": ["integer64"]}


def test_na_and_nan_are_different():
    t = fstx.read_fstx(GOLDEN / "basic.fstx")
    d = t.column("dbl")
    assert d.is_null().to_pylist() == [False, True, False, True, False, False, False, False]
    assert pc.is_nan(d).to_pylist() == [False, None, True, None, False, False, False, False]
    assert t.column("chr").to_pylist()[1:3] == ["", None]  # "" is a value, NA is null


def test_dates_arrow_and_float():
    t = fstx.read_fstx(GOLDEN / "dates.fstx")
    assert t.column("dbl").to_pylist() == t.column("int").to_pylist() == [
        dt.date(1970, 1, 1), dt.date(2026, 11, 17), dt.date(1947, 8, 15), dt.date(1, 1, 1), dt.date(9999, 12, 31),
        None]
    assert json.loads(t.schema.field("dbl").metadata[b"fstx.r"])["phys"] == "double"
    assert json.loads(t.schema.field("int").metadata[b"fstx.r"])["phys"] == "integer"
    f = fstx.read_fstx(GOLDEN / "dates.fstx", datetimes="float")
    assert [str(x.type) for x in f.schema] == ["double", "int32"]
    assert f.column("int").to_pylist() == [0, 20774, -8175, -719162, 2932896, None]
    assert f.schema.field("dbl").metadata == t.schema.field("dbl").metadata  # still marked as a Date
    odd = fstx.read_fstx(GOLDEN / "dates_odd.fstx")
    assert odd.column("d").cast(pa.int32()).to_pylist() == [1, -1, None, None, None, None, 3]
    raw = fstx.read_fstx(GOLDEN / "dates_odd.fstx", datetimes="float").column("d").to_pylist()
    assert raw[:4] == [1.5, -0.5, float("inf"), float("-inf")] and raw[4] != raw[4] and raw[5] is None


def test_datetimes_time_zones_and_rounding():
    t = fstx.read_fstx(GOLDEN / "datetimes.fstx")
    assert [str(f.type) for f in t.schema] == [
        "timestamp[us, tz=UTC]", "timestamp[us, tz=Asia/Kolkata]", "timestamp[us, tz=America/New_York]",
        "timestamp[us]", "timestamp[us]"]
    us = [0, 1700000000000000, 1700000000500000, -86400250000, 1711929600000000, 1699999999123456, None,
          253402300799000000]
    for name in t.column_names:
        assert t.column(name).cast(pa.int64()).to_pylist() == us
    r = [json.loads(f.metadata[b"fstx.r"]) for f in t.schema]
    assert [x.get("tzone") for x in r] == ["UTC", "Asia/Kolkata", "America/New_York", "", None]
    assert "tzone" in r[3] and "tzone" not in r[4]  # tzone "" and no tzone are told apart
    sub = fstx.read_fstx(GOLDEN / "datetimes_subus.fstx")
    assert sub.column("t").cast(pa.int64()).to_pylist() == [1700000000123457, 1700000000333333, 0, 0]
    raw = fstx.read_fstx(GOLDEN / "datetimes_subus.fstx", datetimes="float").column("t").to_pylist()
    assert raw == [1700000000.1234567, 1700000000 + 1 / 3, 0.0000004, 1.5e-7]
    odd = fstx.read_fstx(GOLDEN / "datetimes_odd.fstx")
    assert odd.column("t").cast(pa.int64()).to_pylist() == [None, None, None, None, 0]
    assert odd.column("i").cast(pa.int64()).to_pylist() == [1000000, None, 1700000000000000, 0, -1000000]
    raw = fstx.read_fstx(GOLDEN / "datetimes_odd.fstx", datetimes="float")
    assert raw.column("i").type == pa.int32() and raw.column("i").to_pylist() == [1, None, 1700000000, 0, -1]
    assert raw.column("t").to_pylist()[:2] == [float("inf"), float("-inf")]


def test_classes_in_the_blob():
    t = fstx.read_fstx(GOLDEN / "classes_blob.fstx")
    assert t.column("f").to_pylist() == ["a", "b", "a"]
    assert t.column("d").to_pylist() == [dt.date(1970, 1, 2), dt.date(1970, 1, 3), None]
    assert str(t.schema.field("t").type) == "timestamp[us, tz=UTC]"
    assert json.loads(t.schema.field("f").metadata[b"fstx.r"])["class"] == ["myfac", "factor"]
    na = fstx.read_fstx(GOLDEN / "factor_na_level.fstx").column("f")
    assert na.null_count == 0 and na.to_pylist() == ["a", None, "b", "a"]  # the NA level is a value
    assert one(na).dictionary.to_pylist() == ["a", "b", None] and one(na).indices.to_pylist() == [0, 2, 1, 0]
    d = fstx.read_fstx(GOLDEN / "difftime.fstx")
    assert all(f.type == pa.float64() for f in d.schema)
    m = fstx.metadata(GOLDEN / "difftime.fstx")
    assert m.columns[1].r_class == ["difftime"] and m.columns[1].attributes["units"].values == ["mins"]
    assert m.columns[5].r_class == ["hms", "difftime"]


def test_invalid_utf8_modes():
    p = GOLDEN / "strings_invalid_utf8.fstx"
    want = [bytes.fromhex(h) if h else None for h in ["6f6b", "626164ff62797465", "e0a4", "636166c3a9", ""]]
    with pytest.warns(fstx.FstxWarning, match="column 's'.*not valid UTF-8") as rec:
        t = fstx.read_fstx(p)
    assert len(rec) == 1
    assert t.column("s").type == pa.string()
    assert t.column("s").to_pylist() == ["ok", "bad�byte", "�", "café", None]
    t.validate(full=True)
    with pytest.raises(ValueError, match="not valid UTF-8"):
        fstx.read_fstx(p, invalid_utf8="error")
    b = fstx.read_fstx(p, invalid_utf8="binary")
    assert b.column("s").type == pa.binary() and b.column("s").to_pylist() == want
    assert json.loads(b.schema.field("s").metadata[b"fstx.r"]) == {"phys": "character", "logical": "none"}
    # a file without bad strings keeps its string columns in binary mode, and "error" reads it
    ok = fstx.read_fstx(GOLDEN / "basic.fstx", invalid_utf8="binary")
    assert ok.column("chr").type == pa.string()
    assert json.loads(ok.schema.field("chr").metadata[b"fstx.r"]) == {"phys": "character", "logical": "none"}
    assert same(fstx.read_fstx(GOLDEN / "basic.fstx", invalid_utf8="error"), ok)
    with fstx.open(p) as f:
        with pytest.warns(fstx.FstxWarning):
            assert f.to_batches().read_all().column("s").to_pylist() == t.column("s").to_pylist()
        rb = f.to_batches(invalid_utf8="binary")
        assert rb.schema.field("s").type == pa.binary() and rb.read_all().column("s").to_pylist() == want
    with pytest.raises(ValueError):
        fstx.read_fstx(p, invalid_utf8="keep")


def test_strings_are_plain_and_as_factor():
    p = GOLDEN / "strings_many.fstx"
    t = fstx.read_fstx(p)
    assert [f.type for f in t.schema] == [pa.string(), pa.string()]  # dictionary pages decode to strings
    fa = fstx.read_fstx(p, as_factor=True)
    assert fa.column("many").type == pa.string()  # FSST pages: not dictionary-coded
    few = one(fa.column("few"))
    assert few.type == pa.dictionary(pa.int32(), pa.string())
    assert few.dictionary.to_pylist() == ["blue", "green", "red"]  # sorted
    assert fa.column("few").to_pylist() == t.column("few").to_pylist()
    named = fstx.read_fstx(p, as_factor=["many"])
    lev = one(named.column("many")).dictionary.to_pylist()
    assert lev == sorted(set(x for x in t.column("many").to_pylist() if x is not None))
    assert named.column("many").to_pylist() == t.column("many").to_pylist()
    assert named.column("few").type == pa.string()
    with pytest.raises(ValueError, match="not among the columns read"):
        fstx.read_fstx(p, as_factor=["nope"])
    with pytest.raises(ValueError, match="not a character column"):
        fstx.read_fstx(GOLDEN / "basic.fstx", as_factor=["int"])
    with pytest.raises(TypeError):
        fstx.read_fstx(p, as_factor="few")


def test_attributes_false_drops_r_serialization_only():
    p = GOLDEN / "attrs_serialized.fstx"
    t = fstx.read_fstx(p)
    assert _meta.blob(t.schema.metadata[b"fstx.table_attrs"])[:2] == b"B\n"
    assert _meta.blob(t.schema.field("v").metadata[b"fstx.attrs"])[:2] == b"B\n"
    t = fstx.read_fstx(p, attributes=False)
    assert not t.schema.metadata and b"fstx.attrs" not in t.schema.field("v").metadata
    a = fstx.read_fstx(GOLDEN / "attrs_fxa1.fstx", attributes=False)
    assert a.schema.equals(fstx.read_fstx(GOLDEN / "attrs_fxa1.fstx").schema, check_metadata=True)
    m = fstx.metadata(GOLDEN / "attrs_fxa1.fstx")
    assert m.attributes["note"].values == ["table note"] and m.attributes["version"].values == [3]
    lab = m.columns[1].attributes["labels"]
    assert lab.values == [1, 2, 3] and lab.attrs["names"].values == ["low", "mid", "high"]


def test_tables_without_columns_or_rows():
    assert fstx.read_fstx(GOLDEN / "empty_0x0.fstx").shape == (0, 0)
    t = fstx.read_fstx(GOLDEN / "zero_cols_5rows.fstx")
    assert t.shape == (5, 0) and b"fstx.table_attrs" in t.schema.metadata
    assert fstx.read_fstx(GOLDEN / "zero_cols_5rows.fstx", rows=[4, 0, 0]).num_rows == 3
    assert fstx.read_fstx(GOLDEN / "basic.fstx", columns=[]).shape == (8, 0)
    assert fstx.read_fstx(GOLDEN / "basic.fstx", columns=[], filters=[("int", ">", 0)]).shape == (4, 0)
    z = fstx.read_fstx(GOLDEN / "zero_rows.fstx")
    assert z.num_rows == 0 and z.schema.equals(fstx.metadata(GOLDEN / "zero_rows.fstx").schema)
    with fstx.open(GOLDEN / "zero_cols_5rows.fstx") as f:
        assert f.to_batches().read_all().num_rows == 5
    with fstx.open(GOLDEN / "zero_rows.fstx") as f:
        assert f.to_batches().read_all().schema.equals(z.schema)


# ---------------------------------------------------------------- columns and rows
BIG = GOLDEN / "codec_zstd1.fstx"  # 2,000 rows: int, double, two string columns, logical


def test_columns():
    full = fstx.read_fstx(BIG)
    t = fstx.read_fstx(BIG, columns=["s", "i"])
    assert t.column_names == ["s", "i"] and t.column("i").equals(full.column("i"))
    assert fstx.read_fstx(BIG, columns=[4, 0, -1]).column_names == ["b", "i", "b"]
    assert fstx.read_fstx(BIG, columns="v").column_names == ["v"]
    n = fstx.read_fstx(GOLDEN / "names.fstx", columns=["a", "a", "", "NA"])
    assert n.column_names == ["a", "a", "", "NA"]
    assert n.column(0).to_pylist() == n.column(1).to_pylist() == [1, 2]  # a repeated name: its first match
    assert fstx.read_fstx(GOLDEN / "names.fstx").column(1).to_pylist() == [3, 4]
    with pytest.raises(KeyError):
        fstx.read_fstx(BIG, columns=["nope"])
    with pytest.raises(IndexError):
        fstx.read_fstx(BIG, columns=[5])
    with pytest.raises(TypeError):
        fstx.read_fstx(BIG, columns=[1.5])


@pytest.mark.parametrize("rows", [
    slice(0, 10), slice(1990, None), slice(-5, None), slice(100, 50), slice(0, 5000), slice(3, 40, 7),
    range(10, 20), range(30, 10, -3), [5], [0, 1999], [7, 7, 7], [1999, 0, 500, 0, 1024, 1023],
    list(range(100, 200)), [3, 3, 9, 20], []])
def test_rows(rows):
    full = fstx.read_fstx(BIG)
    t = fstx.read_fstx(BIG, rows=rows)
    idx = list(range(2000))[rows] if isinstance(rows, slice) else list(rows)
    assert t.num_rows == len(idx)
    assert t.equals(full.take(pa.array(idx, pa.int64())))
    assert t.schema.equals(full.schema, check_metadata=True)


def test_rows_other_inputs_and_errors():
    np = pytest.importorskip("numpy")
    full = fstx.read_fstx(BIG)
    for r in (np.array([9, 2, 2], dtype=np.int64), np.array([9, 2, 2], dtype=np.uint32), pa.array([9, 2, 2])):
        assert fstx.read_fstx(BIG, rows=r).equals(full.take([9, 2, 2]))
    for bad in ([-1], [2000], range(1995, 2005)):
        with pytest.raises(IndexError):
            fstx.read_fstx(BIG, rows=bad)
    for bad in ([1.5], ["a"], [1, None], 5):
        with pytest.raises(TypeError):
            fstx.read_fstx(BIG, rows=bad)


def test_rows_on_every_type():
    for name in ("types.fstx",):
        if not FORMAT_READ:
            pytest.skip("no format golden files")
        p = FORMAT_DIR / name
        full = fstx.read_fstx(p)
        idx = [199, 0, 57, 57, 100, 3]
        assert same(fstx.read_fstx(p, rows=idx), full.take(idx))
        assert same(fstx.read_fstx(p, rows=slice(50, 150)), full.slice(50, 100))
    for path, want in CASES:
        full = quiet(path)
        n = full.num_rows
        if n and full.num_columns:
            idx = [n - 1, 0, n // 2, 0]
            assert same(quiet(path, rows=idx), full.take(idx)), path.name


def test_row_names_follow_the_rows():
    p = GOLDEN / "rownames_char.fstx"
    full = fstx.read_fstx(p)
    assert _meta.blob(full.schema.metadata[b"fstx.table_attrs"]) == fstx.metadata(p).table_attrs
    assert row_names(full.schema) == ["r1", "r2", "rन"]
    # a partial or reordered read: the table blob is rewritten with the row names of the rows read
    assert row_names(fstx.read_fstx(p, rows=[2, 0]).schema) == ["rन", "r1"]
    assert row_names(fstx.read_fstx(p, rows=slice(1, 3)).schema) == ["r2", "rन"]
    assert row_names(fstx.read_fstx(p, rows=[1, 1, 1]).schema) == ["r2", "r2.1", "r2.2"]  # as R's make.unique
    assert row_names(fstx.read_fstx(p, filters=[("v", ">", 10)]).schema) == ["r2", "rन"]
    assert row_names(fstx.read_fstx(p, rows=[]).schema) == []
    part = _fxa1.decode(_meta.blob(fstx.read_fstx(p, rows=[2, 0]).schema.metadata[b"fstx.table_attrs"]))
    assert part["class"].values == ["data.frame"] and part["row.names"] == _fxa1.character(["rन", "r1"])
    ints = fstx.read_fstx(GOLDEN / "rownames_int.fstx", rows=[1])
    assert row_names(ints.schema) == [2]
    # automatic row names stay automatic, for the new row count
    big = fstx.read_fstx(BIG, rows=[5, 1])
    assert row_names(big.schema) is None
    auto = _fxa1.decode(fstx.metadata(BIG).table_attrs).get("row.names")
    if auto is not None:
        got = _fxa1.decode(_meta.blob(big.schema.metadata[b"fstx.table_attrs"]))["row.names"]
        assert got.values == [None, 2 if auto.values[1] > 0 else -2]
    # R serialization cannot be looked into: the blob stays and the row count that went with it goes
    ser = fstx.read_fstx(GOLDEN / "attrs_serialized.fstx", rows=[0])
    assert b"fstx.table_attrs" in ser.schema.metadata and b"fstx.nrow" not in ser.schema.metadata


# ---------------------------------------------------------------- filters
def _mask(t, mask):
    return t.filter(pc.fill_null(mask, False))


def test_filters_numbers_and_strings():
    full = fstx.read_fstx(BIG)
    i, v, s, b = (full.column(k) for k in ("i", "v", "s", "b"))
    cases = [
        ([("i", ">", 1500)], pc.greater(i, 1500)),
        ([("i", "<=", 10)], pc.less_equal(i, 10)),
        ([("i", "==", i[777].as_py())], pc.equal(i, i[777])),
        ([("i", "!=", i[0].as_py())], pc.not_equal(i, i[0])),
        ([("v", ">=", 50.0), ("v", "<", 60)], pc.and_(pc.greater_equal(v, 50.0), pc.less(v, 60))),
        ([("v", "between", (10, 20.5))], pc.and_(pc.greater_equal(v, 10), pc.less_equal(v, 20.5))),
        ([("v", "is_null")], pc.fill_null(pc.is_nan(v), True)),
        ([("v", "not_null", None), ("s", "==", "beta")],
         pc.and_(pc.invert(pc.fill_null(pc.is_nan(v), True)), pc.equal(s, "beta"))),
        ([("s", "!=", "alpha")], pc.not_equal(s, "alpha")),
        ([("s", "in", ["alpha", "delta", "nope"])], pc.is_in(s, value_set=pa.array(["alpha", "delta"]))),
        ([("v", "in", [None])], v.is_null()),
        ([("v", "in", [v[1].as_py(), None])], pc.or_(v.is_null(), pc.fill_null(pc.equal(v, v[1]), False))),
        ([("b", "==", True)], b),
        ([("b", "==", False), ("i", ">", 100)], pc.and_(pc.invert(b), pc.greater(i, 100))),
        ([("i", "==", None)], pa.array([False] * 2000)),
        ([("v", ">", float("nan"))], pa.array([False] * 2000)),
        ([("s", "==", None)], pa.array([False] * 2000)),
        (("i", "<", 3), pc.less(i, 3)),  # one bare term
    ]
    for flt, mask in cases:
        got = fstx.read_fstx(BIG, filters=flt)
        assert got.equals(_mask(full, mask)), flt
    w = fstx.read_fstx(BIG, rows=slice(500, 1500), filters=[("i", ">", 1500)], columns=["i", "w"])
    assert w.equals(_mask(full.slice(500, 1000), pc.greater(full.slice(500, 1000).column("i"), 1500)).select(["i", "w"]))
    assert fstx.read_fstx(BIG, filters=[("i", "<", -5)]).num_rows == 0
    assert fstx.read_fstx(BIG, filters=[]).num_rows == 2000
    with fstx.open(BIG) as f:
        got = f.to_batches(filters=[("i", ">", 1500)], batch_rows=64).read_all()
        assert got.equals(_mask(full, pc.greater(i, 1500)))


def test_filters_nan_and_missing():
    full = fstx.read_fstx(GOLDEN / "basic.fstx")
    f = lambda flt: fstx.read_fstx(GOLDEN / "basic.fstx", columns=["int"], filters=flt).column(0).to_pylist()
    ints = full.column("int").to_pylist()
    assert f([("dbl", "in", [float("nan")])]) == [ints[2]]  # NaN rows only, not the NA rows
    assert f([("dbl", "is_null")]) == ints[1:4]  # as R's is.na(): NaN counts
    assert f([("dbl", "in", [None])]) == [ints[1], ints[3]]  # NA alone
    assert f([("dbl", "not_null")]) == [ints[0]] + ints[4:]
    assert f([("dbl", "in", [None, float("nan"), 0.1])]) == ints[:4]
    assert f([("chr", "in", ["", None])]) == [ints[1], ints[2]]
    assert f([("lgl", "is_null")]) == [ints[2], ints[5]]
    assert f([("dbl", "<", 0)]) == [ints[6]]  # -Inf; -0 is not below 0


def test_filters_factors_dates_times():
    p = GOLDEN / "factors.fstx"
    full = fstx.read_fstx(p)
    rows = lambda flt: fstx.read_fstx(p, filters=flt).column("plain").to_pylist()
    assert rows([("plain", "==", "a")]) == ["a", "a"]
    assert rows([("plain", "!=", "a")]) == ["b", "c", "b"]
    assert rows([("plain", "==", "zzz")]) == []
    assert rows([("plain", "!=", "zzz")]) == ["b", "a", "c", "a", "b"]  # every row that is not NA
    assert rows([("plain", "in", ["c", None, "zzz"])]) == [None, "c"]
    assert fstx.read_fstx(p, filters=[("ord", ">=", "mid")]).column("ord").to_pylist() == ["hi", "mid", "hi"]
    assert fstx.read_fstx(p, filters=[("ord", "<", "mid")]).column("ord").to_pylist() == ["lo", "lo"]
    with pytest.raises(ValueError, match="ordered factor"):
        rows([("plain", "<", "b")])
    with pytest.raises(ValueError, match="not one of its levels"):
        rows([("ord", "<", "zzz")])
    with pytest.raises(ValueError, match="level names"):
        rows([("plain", "==", 1)])
    blob = fstx.read_fstx(GOLDEN / "classes_blob.fstx", filters=[("f", "==", "b")])
    assert blob.column("f").to_pylist() == ["b"]

    d = GOLDEN / "dates.fstx"
    for col in ("dbl", "int"):
        got = fstx.read_fstx(d, filters=[(col, ">=", "1970-01-01")]).column(col).to_pylist()
        assert got == [dt.date(1970, 1, 1), dt.date(2026, 11, 17), dt.date(9999, 12, 31)]
        got = fstx.read_fstx(d, filters=[(col, "==", dt.date(1947, 8, 15))]).column(col).to_pylist()
        assert got == [dt.date(1947, 8, 15)]
        got = fstx.read_fstx(d, filters=[(col, "in", ["0001-01-01", None])]).column(col).to_pylist()
        assert got == [dt.date(1, 1, 1), None]
    with pytest.raises(ValueError, match="not a date"):
        fstx.read_fstx(d, filters=[("dbl", ">", "yesterday")])
    with pytest.raises(ValueError, match="Date column"):
        fstx.read_fstx(d, filters=[("dbl", ">", 5)])

    ts = GOLDEN / "datetimes.fstx"
    us = lambda col, flt: fstx.read_fstx(ts, filters=flt).column(col).cast(pa.int64()).to_pylist()
    utc = dt.timezone.utc
    assert us("utc", [("utc", "==", "2023-11-14T22:13:20")]) == [1700000000000000]
    assert us("utc", [("utc", "==", dt.datetime(2023, 11, 14, 22, 13, 20, tzinfo=utc))]) == [1700000000000000]
    assert us("utc", [("utc", "<", "1970-01-01")]) == [-86400250000]
    assert us("utc", [("utc", "between", ("2023-11-14T22:13:20", "2023-11-14T22:13:20.5"))]) == [
        1700000000000000, 1700000000500000]
    # an aware datetime means the same instant in every column, whatever its time zone
    for col in ("kolkata", "new_york", "tz_empty", "tz_none"):
        assert us(col, [(col, ">", dt.datetime(2024, 1, 1, tzinfo=utc))]) == [1711929600000000, 253402300799000000]
    odd = fstx.read_fstx(GOLDEN / "datetimes_odd.fstx", filters=[("i", ">", "2000-01-01T00:00:00")])
    assert odd.column("i").cast(pa.int64()).to_pylist() == [1700000000000000]


def test_filters_named_time_zone():
    pytest.importorskip("zoneinfo")
    try:
        import zoneinfo

        zoneinfo.ZoneInfo("Asia/Kolkata")
    except Exception:
        pytest.skip("no time zone database (pip install tzdata)")
    ts = GOLDEN / "datetimes.fstx"
    got = fstx.read_fstx(ts, filters=[("kolkata", "==", "2023-11-15T03:43:20")])  # 22:13:20 UTC + 05:30
    assert got.column("kolkata").cast(pa.int64()).to_pylist() == [1700000000000000]
    got = fstx.read_fstx(ts, filters=[("new_york", "==", "2023-11-14 17:13:20")])
    assert got.column("new_york").cast(pa.int64()).to_pylist() == [1700000000000000]


def test_filters_refused():
    with pytest.raises(ValueError, match="class 'integer64' cannot be filtered"):
        fstx.read_fstx(GOLDEN / "integer64.fstx", filters=[("i64", ">", 0)])
    with pytest.raises(ValueError, match="class 'difftime' cannot be filtered"):
        fstx.read_fstx(GOLDEN / "difftime.fstx", filters=[("secs", ">", 0)])
    with pytest.raises(ValueError, match="R serialization"):
        fstx.read_fstx(GOLDEN / "attrs_serialized.fstx", filters=[("v", ">", 0)])
    with pytest.raises(KeyError):
        fstx.read_fstx(BIG, filters=[("nope", ">", 0)])
    for bad in ([("i", "~", 1)], [("i", ">")], [("i", ">", [1, 2])], [("i", "in", 5)], [("s", "<", "a")],
                [("s", "==", 5)], [("i", "==", "5")], [("i", "between", 5)], ["i > 5"], [("i", "is_null", 3)]):
        with pytest.raises(ValueError):
            fstx.read_fstx(BIG, filters=bad)
    with pytest.raises(ValueError, match="either"):
        fstx.read_fstx(BIG, rows=[1, 2, 5], filters=[("i", ">", 0)])
    # a labelled integer column (attributes, no class) filters as a plain one
    assert fstx.read_fstx(GOLDEN / "attrs_fxa1.fstx", filters=[("code", ">=", 2)]).column("code").to_pylist() == [2, 3]


# ---------------------------------------------------------------- batches, the open file, threads
def test_to_batches():
    full = fstx.read_fstx(BIG)
    with fstx.open(BIG) as f:
        r = f.to_batches()
        assert isinstance(r, pa.RecordBatchReader) and r.schema.equals(full.schema, check_metadata=True)
        assert r.read_all().equals(full)
        sizes = [b.num_rows for b in f.to_batches(batch_rows=300)]
        assert sizes == [300] * 6 + [200]
        t = f.to_batches(columns=["w", "i"], batch_rows=999, rows=slice(100, 1900)).read_all()
        assert t.equals(full.slice(100, 1800).select(["w", "i"]))
        assert pa.table(f).equals(full)  # __arrow_c_stream__
        assert f.read(columns=["i"], rows=[5, 1]).equals(full.select(["i"]).take([5, 1]))
        for bad in (0, -1, 1.5, True):
            with pytest.raises(ValueError):
                f.to_batches(batch_rows=bad)
        with pytest.raises(TypeError):
            f.to_batches(rows=[1, 2])
        late = f.to_batches(batch_rows=500)
    assert late.read_all().equals(full)  # the reader has its own handle on the file
    with pytest.raises(ValueError, match="closed"):
        f.read()
    with pytest.raises(ValueError, match="closed"):
        f.to_batches()


def test_row_groups_make_the_batches():
    if not FORMAT_READ:
        pytest.skip("no format golden files")
    want = {p.name: w for p, w in FORMAT_READ}
    for name in ("mix_int.fstx", "appended.fstx"):
        with fstx.open(FORMAT_DIR / name) as f:
            sizes = [b.num_rows for b in f.to_batches()]
            assert len(sizes) == want[name]["row_groups"] and sum(sizes) == f.num_rows
            t = f.read()
            assert t.column(0).num_chunks == len(sizes)
            assert f.read(rows=slice(7, f.num_rows - 9)).equals(t.slice(7, f.num_rows - 16))


def _assert_no_open_handle(p):
    """On Windows: opening the file with no sharing at all fails (error 32) while any handle on it is open."""
    if sys.platform != "win32":
        return
    import ctypes
    from ctypes import wintypes

    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.restype = wintypes.HANDLE
    k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                              wintypes.DWORD, wintypes.HANDLE]
    h = k.CreateFileW(str(p), 0x80000000, 0, None, 3, 0x80, None)  # GENERIC_READ, no sharing, OPEN_EXISTING
    assert h != wintypes.HANDLE(-1).value, "the file is still open (Windows error %d)" % ctypes.get_last_error()
    k.CloseHandle(h)


def test_file_is_released_after_reading(tmp_path):
    p = tmp_path / "x.fstx"
    shutil.copyfile(BIG, p)
    t = fstx.read_fstx(p, filters=[("i", ">", 10)])
    with fstx.open(p) as f:
        r = f.to_batches(batch_rows=100)
        first = r.read_next_batch()
        r.close()
    # Windows Server 2022 refuses to delete a mapped file, but Windows 11 deletes it anyway (POSIX delete
    # semantics): check that no handle is left, not just that the delete works
    _assert_no_open_handle(p)
    p.unlink()
    assert t.num_rows > 0 and first.num_rows == 100 and t.column("w").to_pylist()  # the data owns its memory


def test_reads_from_many_threads():
    full = fstx.read_fstx(BIG)
    errors, done = [], []

    def work(k):
        try:
            for i in range(10):
                if k % 2:
                    assert fstx.read_fstx(BIG, threads=1 + k % 3).equals(full)
                else:
                    assert fstx.read_fstx(BIG, rows=[1999, 3, 3], filters=None, columns=["s"]).num_rows == 3
            done.append(k)
        except BaseException as e:  # noqa: BLE001
            errors.append(e)

    ts = [threading.Thread(target=work, args=(k,)) for k in range(8)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    assert not errors and len(done) == 8


def test_threads_and_verify_arguments():
    full = fstx.read_fstx(BIG)
    assert fstx.read_fstx(BIG, threads=1, verify=False).equals(full)
    assert fstx.read_fstx(BIG, threads=64).equals(full)
    for bad in (0, -2, 1.5, True):
        with pytest.raises(ValueError):
            fstx.read_fstx(BIG, threads=bad)
    with pytest.raises(ValueError):
        fstx.read_fstx(BIG, datetimes="numpy")


def test_damaged_page_is_refused(tmp_path):
    data = bytearray(BIG.read_bytes())
    data[200] ^= 0xFF  # inside the first pages
    p = tmp_path / "bad.fstx"
    p.write_bytes(data)
    with pytest.raises(fstx.FstxError):
        fstx.read_fstx(p)


# ---------------------------------------------------------------- the FXA1 decoder
def test_fxa1_decoder():
    blob = bytes.fromhex("465841010100000005000000636c61737304000000010000000a000000646174612e6672616d6500000000")
    a = _fxa1.decode(blob)
    assert list(a) == ["class"] and a["class"].type == "character" and a["class"].values == ["data.frame"]
    assert _fxa1.kind(blob) == "FXA1" and _fxa1.kind(None) == "none" and _fxa1.kind(b"B\n....") == "R-serialization"
    i32 = lambda *v: struct.pack("<%di" % len(v), *v)
    name = lambda s: i32(len(s)) + s
    dbl = (b"FXA\x01" + i32(1) + name(b"x") + i32(3, 4) + struct.pack("<Q", 0x7FF00000000007A2)
           + struct.pack("<Q", 0x7FF80000000007A2) + struct.pack("<d", 1.5) + struct.pack("<Q", 0x7FF8000000000000)
           + i32(0))
    v = _fxa1.decode(dbl)["x"].values
    assert v[0] is None and v[2] == 1.5 and v[1] != v[1] and v[3] != v[3]
    assert struct.pack("<d", v[1]) == struct.pack("<Q", 0x7FF80000000007A2)  # the quiet NA keeps its bits
    mixed = (b"FXA\x01" + i32(3) + name(b"l") + i32(1, 3, 1, 0, -2**31) + i32(0)
             + name(b"s") + i32(4, 3, 1, -1, 2) + b"a\xc3\xa9" + i32(0)
             + name(b"i") + i32(2, 2, 7, -2**31) + i32(1) + name(b"names") + i32(4, 2, 1, 1) + b"pq" + i32(0))
    m = _fxa1.decode(mixed)
    assert m["l"].values == [True, False, None] and m["s"].values == ["a", None, "é"]
    assert m["i"].values == [7, None] and m["i"].attrs["names"].values == ["p", "q"]
    bad_utf8 = b"FXA\x01" + i32(1) + name(b"s") + i32(4, 1, 2) + b"\xff\xfe" + i32(0)
    s = _fxa1.decode(bad_utf8)["s"].values[0]
    assert s.encode("utf-8", "surrogateescape") == b"\xff\xfe"  # value bytes survive (spec 9 rule 7)
    for broken, why in [
        (blob[:-1], "truncated"), (blob + b"\x00", "after the end"), (b"FXA\x02" + blob[4:], "version 2"),
        (b"FXA\x01" + i32(5), "attribute count"), (b"FXA\x01" + i32(-1), "attribute count"),
        (b"FXA\x01" + i32(1) + i32(0) + i32(1, 0, 0) + b"\x00" * 8, "name length"),
        (b"FXA\x01" + i32(1) + name(b"a\x00") + i32(2, 0, 0), "NUL in a name"),
        (b"FXA\x01" + i32(1) + name(b"\xff") + i32(2, 0, 0) + b"\x00", "not valid UTF-8"),
        (b"FXA\x01" + i32(1) + name(b"x") + i32(9, 0, 0) + b"\x00", "value type 9"),
        (b"FXA\x01" + i32(1) + name(b"x") + i32(2, 99, 0) + b"\x00", "value count"),
        (b"FXA\x01" + i32(1) + name(b"x") + i32(1, 1, 2) + i32(0), "logical value"),
        (b"FXA\x01" + i32(1) + name(b"x") + i32(4, 1, -5) + i32(0), "string length"),
        (b"FXA\x01" + i32(1) + name(b"x") + i32(4, 1, 1) + b"\x00" + i32(0), "NUL in a string"),
        (b"FXA\x01" + i32(2) + (name(b"x") + i32(2, 1, 1) + i32(0)) * 2, "duplicate names"),
    ]:
        with pytest.raises(_fxa1.Fxa1Error, match=why):
            _fxa1.decode(broken)
    # nesting: three levels are the limit
    leaf = name(b"d") + i32(2, 1, 1)
    deep = b"FXA\x01" + i32(1) + name(b"a") + i32(2, 1, 1) + i32(1) + name(b"b") + i32(2, 1, 1) + i32(1) + \
        name(b"c") + i32(2, 1, 1) + i32(1) + leaf + i32(0)
    with pytest.raises(_fxa1.Fxa1Error, match="nested too deep"):
        _fxa1.decode(deep)
    # the encoder is the decoder's inverse, also for string bytes that are not UTF-8
    odd = b"FXA\x01" + i32(1) + name(b"s") + i32(4, 1, 2) + b"\xff\xfe" + i32(0)
    assert _fxa1.encode(_fxa1.decode(odd)) == odd


def test_corrupt_attribute_blob_is_an_fstx_error(tmp_path):
    # the same file with the table blob's version byte changed would fail its footer checksum, so the check
    # is made on the metadata object directly
    m = fstx.metadata(GOLDEN / "basic.fstx")
    import dataclasses

    bad = dataclasses.replace(m, table_attrs=m.table_attrs[:-2])
    with pytest.raises(fstx.FstxError, match="corrupt \\(table attributes") as ei:
        bad.attributes
    assert ei.value.code == fstx.ERR_CORRUPT
    no_class = dataclasses.replace(m, table_attrs=b"FXA\x01" + struct.pack("<i", 0))
    with pytest.raises(fstx.FstxError, match="no data.frame class"):
        no_class.table_class
    col = dataclasses.replace(m.columns[0], attrs=b"FXA\x07")
    with pytest.raises(fstx.FstxError, match="version 7"):
        col.r_class


# ---------------------------------------------------------------- pandas and polars
def test_to_pandas():
    pd = pytest.importorskip("pandas")
    t = fstx.read_fstx(GOLDEN / "basic.fstx")
    df = fstx.to_pandas(t)
    assert [str(x) for x in df.dtypes[:4]] == ["boolean", "Int32", "Float64", "Float64"]
    assert df["int"].tolist()[:5] == [0, 1, -1, 2147483647, -2147483647] and df["int"][5] is pd.NA
    assert df["lgl"][2] is pd.NA and df["lgl"][0] is True or bool(df["lgl"][0]) is True
    assert df["dbl"].isna().tolist()[1] and df["dbl"][5] == float("inf")
    assert df["chr"][1] == "" and pd.isna(df["chr"][2])
    meta = df.attrs["fstx"]
    json.dumps(meta)  # plain JSON data
    assert list(meta) == ["table_attrs", "nrow", "columns"] and meta["nrow"] is None
    assert _meta.blob(meta["table_attrs"]) == fstx.metadata(GOLDEN / "basic.fstx").table_attrs
    assert [c["name"] for c in meta["columns"]] == t.column_names
    assert [c["r"] for c in meta["columns"]] == [json.loads(f.metadata[b"fstx.r"]) for f in t.schema]
    assert all(c["attrs"] is None for c in meta["columns"])
    ser = fstx.to_pandas(fstx.read_fstx(GOLDEN / "attrs_serialized.fstx")).attrs["fstx"]
    assert ser["nrow"] == fstx.metadata(GOLDEN / "attrs_serialized.fstx").nrow
    assert _meta.blob(ser["columns"][0]["attrs"]) == fstx.metadata(GOLDEN / "attrs_serialized.fstx").columns[0].attrs
    i64 = fstx.to_pandas(fstx.read_fstx(GOLDEN / "integer64.fstx"))["i64"]
    assert str(i64.dtype) == "Int64" and i64[5] == 2**63 - 1 and i64[3] == 2**53 + 1 and i64[7] is pd.NA
    rn = fstx.to_pandas(fstx.read_fstx(GOLDEN / "rownames_char.fstx"))
    assert rn.index.tolist() == ["r1", "r2", "rन"] and rn["v"].tolist() == [10, 20, 30]
    part = fstx.to_pandas(fstx.read_fstx(GOLDEN / "rownames_char.fstx", rows=[2, 0]))
    assert part.index.tolist() == ["rन", "r1"] and part["v"].tolist() == [30, 10]
    ri = fstx.to_pandas(fstx.read_fstx(GOLDEN / "rownames_int.fstx"))
    assert ri.index.tolist() == [4, 2]
    assert fstx.to_pandas(fstx.read_fstx(BIG)).index.tolist()[:3] == [0, 1, 2]
    fac = fstx.to_pandas(fstx.read_fstx(GOLDEN / "factors.fstx"))
    assert str(fac["ord"].dtype) == "category" and fac["ord"].cat.ordered
    assert fac["unused"].cat.categories.tolist() == ["w", "x", "y", "z"]
    with pytest.warns(fstx.FstxWarning, match="NA as a level"):
        na = fstx.to_pandas(fstx.read_fstx(GOLDEN / "factor_na_level.fstx"))
    assert na["f"].tolist()[0] == "a" and pd.isna(na["f"][1])
    d = fstx.to_pandas(fstx.read_fstx(GOLDEN / "dates.fstx"))
    assert d["dbl"][3] == dt.date(1, 1, 1) and d["int"][4] == dt.date(9999, 12, 31)
    assert fstx.to_pandas(fstx.read_fstx(GOLDEN / "zero_cols_5rows.fstx")).shape == (5, 0)


@pytest.mark.parametrize("path,want", CASES, ids=ids(CASES))
def test_to_pandas_every_case(path, want):
    pytest.importorskip("pandas")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", fstx.FstxWarning)
        df = fstx.to_pandas(quiet(path))
    assert df.shape == (want["nrow"], want["ncol"])
    assert list(df.columns) == [c["name"] for c in want["columns"]]


def test_pandas_nan_and_na():
    pd = pytest.importorskip("pandas")
    t = fstx.read_fstx(GOLDEN / "basic.fstx")
    df = fstx.to_pandas(t)
    assert df["dbl"].isna().tolist()[1] and df["dbl"].isna().tolist()[3]  # NA is missing, always
    assert df["dbl"][5] == float("inf") and df["dbl"][0] == 0.1
    # pandas 3 treats NaN as NA in its nullable dtypes unless future.distinguish_nan_and_na is on; then both survive
    try:
        ctx = pd.option_context("future.distinguish_nan_and_na", True)
        ctx.__enter__()
    except Exception:
        pytest.skip("this pandas has no future.distinguish_nan_and_na option")
    try:
        back = pa.array(fstx.to_pandas(t)["dbl"].array)
    finally:
        ctx.__exit__(None, None, None)
    assert back.is_null().to_pylist() == [False, True, False, True, False, False, False, False]
    assert pc.is_nan(back).to_pylist()[2] is True


@pytest.mark.parametrize("path,want", CASES, ids=ids(CASES))
def test_to_polars_every_case(path, want):
    pl = pytest.importorskip("polars")
    t = quiet(path)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", fstx.FstxWarning)
        if len(set(t.column_names)) != len(t.column_names):
            with pytest.raises(ValueError, match="unique column names"):
                fstx.to_polars(t)
            return
        df = fstx.to_polars(t)
    assert isinstance(df, pl.DataFrame)
    if want["ncol"]:
        assert df.shape == (want["nrow"], want["ncol"])
        assert df.columns == t.column_names


def test_to_polars_values_and_stream():
    pl = pytest.importorskip("polars")
    t = fstx.read_fstx(GOLDEN / "basic.fstx")
    df = fstx.to_polars(t)
    assert df["int"].to_list() == t.column("int").to_pylist()
    assert df["lgl"].to_list() == t.column("lgl").to_pylist()
    assert df["chr"].to_list() == t.column("chr").to_pylist()
    assert df["dbl"].null_count() == 2 and df["dbl"].is_nan().to_list()[2] is True
    i64 = fstx.to_polars(fstx.read_fstx(GOLDEN / "integer64.fstx"))["i64"]
    assert i64.dtype == pl.Int64 and i64.to_list() == fstx.read_fstx(GOLDEN / "integer64.fstx").column(0).to_pylist()
    with fstx.open(BIG) as f:  # polars reads the open file through the Arrow stream protocol
        df = pl.DataFrame(f)
    assert df.shape == (2000, 5) and df["i"].to_list() == fstx.read_fstx(BIG).column("i").to_pylist()
