"""The table read_fstx() is to return for a golden file (DESIGN.md sections 3 and 4), built from the JSON sidecar.

The write tests run without the Python reader: this stands in for it. Values come from the sidecar (and from
golden/full/ for the columns a sidecar cuts short, checked against the sidecar's md5), the attribute blobs from
``fstx.metadata``. Field metadata b"fstx.r" / b"fstx.attrs" and schema metadata b"fstx.table_attrs" /
b"fstx.nrow" are what fstx/_write.py documents.
"""
import hashlib
import json
import struct

import pyarrow as pa

import fstx
from conftest import GOLDEN

NA_REAL = 0x7FF00000000007A2
_FULL = {"strings_many": "strings_many", **{c: "cd" for c in (
    "codec_none", "codec_lz4", "codec_zstd1", "codec_zstd19", "profile_fast", "profile_balanced", "profile_small")}}


def _json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def _is_na(bits: int) -> bool:  # NA_real_ in either form: a NaN whose low word is 1954
    return (bits >> 52) & 0x7FF == 0x7FF and bits & 0xFFFFFFFFFFFFF != 0 and bits & 0xFFFFFFFF == 1954


def _signed(bits: int) -> int:
    return bits - (1 << 64) if bits >> 63 else bits


def doubles(bits_hex):
    """float64 from exact bit patterns; NA_real_ (both forms) is null, any other NaN a value."""
    bits = [int(h, 16) for h in bits_hex]
    return pa.array([None if _is_na(b) else _signed(b) for b in bits], pa.int64()).view(pa.float64())


def _long_strings(values):
    hi = _json(GOLDEN / "strings_indic.json")["columns"][0]["values"][0]
    made = ["0123456789" * 7000, (hi + " ") * 300]  # make_golden.R: long_ascii, long_deva
    out = []
    for v in values:
        if isinstance(v, dict):
            s = next(m for m in made if len(m.encode()) == v["bytes"])
            assert hashlib.md5(s.encode()).hexdigest() == v["md5"]
            v = s
        out.append(v)
    return out


def _md5(c, values, bits=None):
    """The sidecar's canonical md5 for a plain logical / integer / double / character column."""
    h = hashlib.md5()
    if c["r_typeof"] in ("logical", "integer"):
        h.update(struct.pack(f"<{len(values)}i", *[-2**31 if v is None else int(v) for v in values]))
    elif c["r_typeof"] == "double":
        b = [int(x, 16) for x in bits]
        h.update(struct.pack(f"<{len(b)}Q", *[NA_REAL if _is_na(x) else x for x in b]))
    else:
        for v in values:
            if v is None:
                h.update(struct.pack("<i", -1))
            else:
                h.update(struct.pack("<i", len(v.encode())) + v.encode())
    return h.hexdigest()


def _column(c, full, invalid_utf8):
    t = c["arrow_type"]
    values, bits = c.get("values"), c.get("bits")
    if c["values_truncated"]:
        values, bits = full["values"], full.get("bits")
        assert len(values) == c["n"] and _md5(c, values, bits) == c["md5"], c["name"]
    if t == "bool":
        return pa.array(values, pa.bool_())
    if t == "int32":
        return pa.array(values, pa.int32())
    if t == "double":
        return doubles(bits)
    if t == "int64":
        return pa.array([None if v is None else int(v) for v in values], pa.int64())
    if t == "string":
        if "hex" in c:
            raw = [None if h is None else bytes.fromhex(h) for h in c["hex"]]
            if invalid_utf8 == "binary":
                return pa.array(raw, pa.binary())
            return pa.array([None if b is None else b.decode("utf-8", "replace") for b in raw], pa.string())
        return pa.array(_long_strings(values), pa.string())
    if t.startswith("dictionary"):
        idx = pa.array([None if k is None else k - 1 for k in c["codes"]], pa.int32())
        return pa.DictionaryArray.from_arrays(idx, pa.array(c["levels"], pa.string()), ordered="ordered=1" in t)
    if t == "date32[day]":
        return pa.array(c["date32"], pa.int32()).cast(pa.date32())
    if t.startswith("timestamp[us"):
        tz = t[len("timestamp[us, tz="):-1] if "tz=" in t else None
        return pa.array([None if v is None else int(v) for v in c["us"]], pa.int64()).cast(pa.timestamp("us", tz))
    raise AssertionError(f"no builder for {t}")


def field_r(c):
    s = c["stored"]
    r = {"phys": s["phys"], "logical": s["logical"]}
    if c["r_class"]:
        r["class"] = c["r_class"]
    if s["logical"] == "POSIXct" and s["tzone_present"]:
        r["tzone"] = s.get("tzone", "")
    return r


def golden_table(path, want, invalid_utf8="replace", metadata=True) -> pa.Table:
    """The pyarrow.Table for a golden file. ``metadata=False`` leaves the fstx metadata out (a table that was
    rebuilt in Python and lost it)."""
    m = fstx.metadata(path)
    full = None
    if path.stem in _FULL:
        full = {c["name"]: c for c in _json(GOLDEN / "full" / (_FULL[path.stem] + ".json"))["columns"]}
    arrays, fields = [], []
    for c, cm in zip(want["columns"], m.columns):
        a = _column(c, full and full[c["name"]], invalid_utf8)
        assert len(a) == c["n"]
        md = {b"fstx.r": json.dumps(field_r(c)).encode()}
        if cm.attrs is not None:
            md[b"fstx.attrs"] = cm.attrs
        arrays.append(a)
        fields.append(pa.field(c["name"], a.type, metadata=md if metadata else None))
    smeta = {b"fstx.nrow": str(m.nrow).encode()}
    if m.table_attrs is not None:
        smeta[b"fstx.table_attrs"] = m.table_attrs
    if not arrays:  # no columns: an empty struct array keeps the row count
        b = pa.RecordBatch.from_struct_array(pa.array([{}] * m.nrow, pa.struct([])))
        return pa.Table.from_batches([b.replace_schema_metadata(smeta) if metadata else b])
    return pa.Table.from_arrays(arrays, schema=pa.schema(fields, metadata=smeta if metadata else None))


def to_pandas(table: pa.Table):
    """What fstx.to_pandas() is to do (DESIGN.md section 2), as far as the writer cares: nullable integer and
    boolean dtypes, R row names as the index, and the fstx metadata in ``df.attrs["fstx"]``."""
    import base64

    import pandas as pd

    from fstx import _fxa1_write as fxa

    nullable = {pa.int32(): pd.Int32Dtype(), pa.int64(): pd.Int64Dtype(), pa.bool_(): pd.BooleanDtype()}
    df = table.to_pandas(types_mapper=nullable.get)
    smeta = table.schema.metadata or {}
    blob = smeta.get(b"fstx.table_attrs")
    if blob is not None and fxa.is_fxa1(blob):
        rn = fxa.parse(blob).get("row.names")
        if rn is not None and len(rn.values) == len(df):
            df.index = pd.Index(rn.values)

    def b64(b):
        return None if b is None else base64.b64encode(b).decode()

    cols = []
    for f in table.schema:
        md = f.metadata or {}
        cols.append({"name": f.name, "r": json.loads(md[b"fstx.r"]) if b"fstx.r" in md else None,
                     "attrs": b64(md.get(b"fstx.attrs"))})
    df.attrs["fstx"] = {"table_attrs": b64(blob), "nrow": table.num_rows, "columns": cols}
    return df
