"""The R package's fstx 1.2 golden files (tests/testthat/fixtures/golden-1.2, made by make_golden_v10.R and
make_golden.R there), read from Python. Among them is v10_integer64.fstx, written by fstx 1.0.2 itself with a bit64
integer64 column in the 1.0 layout: 1.2 must read what 1.0.x wrote (compatibility is checked one way). Files 1.2
writes are read as well, and the reserved numbers are refused as in R.

Each column is checked against R's expected.csv by the same canonical bytes R's canon.R uses, for the types whose
Arrow form holds R's stored values directly (int32, logical, double, int64, string, binary); dictionary, date and
timestamp columns are covered by the R test and the Python cases of golden/. FSTX_GOLDEN12_DIR overrides the folder
(an installed wheel tested away from the git tree). Skipped until the files exist.
"""
import csv
import hashlib
import math
import os
import struct
from pathlib import Path

import pyarrow as pa
import pytest

import fstx

HERE = Path(__file__).resolve().parent
G12 = Path(os.environ.get("FSTX_GOLDEN12_DIR") or HERE.parents[1] / "tests" / "testthat" / "fixtures" / "golden-1.2")


def _rows(name):
    p = G12 / name
    if not p.is_file():
        return []
    with open(p, encoding="utf-8", newline="") as fh:
        return list(csv.DictReader(fh))


FILES = _rows("files.csv")
EXPECTED = _rows("expected.csv")
READ = [f for f in FILES if f["expect"] == "read"]
REFUSE = [f for f in FILES if f["expect"] != "read"]
NA_INT32 = struct.pack("<i", -(2**31))
NA_REAL = struct.pack("<Q", 0x7FF00000000007A2)   # R's NA_real_
NA_LEN = b"\xff\xff\xff\xff"


def canon(col: pa.Array):
    """(canonical bytes as R's canon.R makes them, R's NA count), or None for a type not compared here."""
    t = col.type
    vals = col.to_pylist()
    out = bytearray()
    nas = 0
    if pa.types.is_int32(t) or pa.types.is_boolean(t):
        for v in vals:
            out += NA_INT32 if v is None else struct.pack("<i", int(v))
            nas += v is None
    elif pa.types.is_float64(t):
        bits = pa.array(col.fill_null(0.0).to_numpy(zero_copy_only=False).view("<u8")).to_pylist()
        for v, b in zip(vals, bits):
            out += NA_REAL if v is None else struct.pack("<Q", b)
            nas += v is None or math.isnan(v)          # R's is.na() counts NaN too
    elif pa.types.is_int64(t):
        for v in vals:
            out += struct.pack("<q", -(2**63) if v is None else v)
            nas += v is None
    elif pa.types.is_string(t) or pa.types.is_large_string(t) or pa.types.is_binary(t) or pa.types.is_large_binary(t):
        for v in vals:
            if v is None:
                out += NA_LEN
                nas += 1
            else:
                b = v.encode("utf-8") if isinstance(v, str) else v
                out += struct.pack("<I", len(b)) + b
    else:
        return None
    return bytes(out), nas


def ids(rows):
    return [r["file"] for r in rows]


@pytest.mark.parametrize("row", READ, ids=ids(READ))
def test_golden12_reads_as_recorded(row):
    p = G12 / row["file"]
    assert hashlib.md5(p.read_bytes()).hexdigest() == row["md5"]
    m = fstx.metadata(p)
    assert (m.nrow, m.ncol) == (int(row["nrow"]), int(row["ncol"]))
    assert [c.phys for c in m.columns] == row["types"].split(";")
    t = fstx.read_fstx(p)
    want = [e for e in EXPECTED if e["file"] == row["file"]]
    assert t.column_names == [e["column"] for e in want]
    compared = 0
    for e in want:
        got = canon(t.column(e["column"]).combine_chunks())
        if got is None:
            continue
        data, nas = got
        assert hashlib.md5(data).hexdigest() == e["data_md5"], e["column"]
        assert nas == int(e["nas"]), e["column"]
        compared += 1
    assert compared > 0 or not want


def test_golden12_has_the_file_fstx_102_wrote():
    if not FILES:
        pytest.skip("golden-1.2 files not generated yet (tests/testthat/fixtures/golden-1.2/make_golden.R)")
    v10 = [f for f in FILES if f["writer"] == "fstx 1.0.2"]
    assert [f["file"] for f in v10] == ["v10_integer64.fstx"]
    t = fstx.read_fstx(G12 / "v10_integer64.fstx")
    assert t.schema.field("big").type == pa.int64() and t.schema.field("small").type == pa.int64()
    big = t.column("big").to_pylist()
    assert big[:13] == [0, 1, -1, 123456789, -987654321, 2**40, -(2**40), 2**53, None, 2**53 + 1, -(2**53 + 1),
                        2**63 - 1, -(2**63) + 1]


@pytest.mark.parametrize("row", REFUSE, ids=ids(REFUSE))
def test_golden12_refusals(row):
    p = G12 / row["file"]
    assert hashlib.md5(p.read_bytes()).hexdigest() == row["md5"]
    with pytest.raises(fstx.FstxError, match=row["message"]) as e:
        fstx.metadata(p)
    if row["expect"] == "corrupt":
        assert "Update fstx" not in str(e.value)
    with pytest.raises(fstx.FstxError, match=row["message"] if row["expect"] == "corrupt" else "Update fstx"):
        fstx.read_fstx(p)
