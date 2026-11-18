"""R-match: what Python shows is what R shows, both ways.

R -> Python: golden/rmatch/<case>.fstx were written by R (make_rmatch.R), with R's own view of every value in
<case>.view.json (rview.R). Each file is read with pyarrow, then turned into pandas and polars, and the view
of each (rview.py) must equal R's, apart from the differences DESIGN.md section 8 lists (KNOWN below).

Python -> R: every table read here, and tables built from plain Python data, are written into
golden/from_python/rmatch/<name>.fstx with <name>.view.json: what R is to show after reading the file
(rview.py's prediction, from the data written), plus the R case it came from and its known differences.
check_rmatch.R reads each file in R and compares. The same files are read back here too.
"""
import json
import warnings

import pyarrow as pa
import pytest

import fstx
import rview
from conftest import GOLDEN

RM = GOLDEN / "rmatch"
OUT = GOLDEN / "from_python" / "rmatch"
OUT.mkdir(parents=True, exist_ok=True)
CASES = sorted(p.stem for p in RM.glob("*.fstx"))

# Differences from R's view that DESIGN.md section 8 documents: (path, case, column) -> reason
KNOWN = {
    ("polars", "rmatch_dates", "d_int"): "polars carries no metadata: a Date stored as integer comes back double",
    ("polars", "rmatch_times", "tz_empty"): "polars carries no metadata: tzone \"\" comes back as \"UTC\"",
    ("polars", "rmatch_times", "tz_none"): "polars carries no metadata: no tzone comes back as \"UTC\"",
    ("polars", "rmatch_int64", "none"): "polars carries no metadata: an all-NA Int64 comes back integer (int64='auto')",
    # found by this file (18 Nov 2026), added to DESIGN.md section 8: the same rule for values that all fit int32
    ("polars", "rmatch_int64", "small"): "polars carries no metadata: an Int64 whose values all fit int32 comes back "
                                         "integer (int64='auto')",
}


def r_view(case):
    with open(RM / (case + ".view.json"), encoding="utf-8") as fh:
        v = json.load(fh)
    return {k: v[k] for k in ("names", "nrow", "columns")}


def diff(got, want):
    """{column: first difference} between two views."""
    assert got["names"] == want["names"] and got["nrow"] == want["nrow"]
    out = {}
    for g, w in zip(got["columns"], want["columns"]):
        for k in ("typeof", "class", "tzone", "levels", "values"):
            if g[k] != w[k]:
                if k == "values":
                    i = next(i for i, (a, b) in enumerate(zip(g[k], w[k])) if a != b)
                    out[g["name"]] = f"row {i}: {g[k][i]!r} vs R {w[k][i]!r}"
                else:
                    out[g["name"]] = f"{k}: {g[k]!r} vs R {w[k]!r}"
                break
    return out


def known(path, case):
    return {c: why for (p, k, c), why in KNOWN.items() if p == path and k == case}


def read(case):
    return fstx.read_fstx(RM / (case + ".fstx"))


def save(data, name, view, source=None, known_cols=None, open_cols=None, **kw):
    """Writes data to from_python/rmatch/<name>.fstx and what R is to show to <name>.view.json; reads the file
    back here and checks it against the same view."""
    p = OUT / (name + ".fstx")
    with warnings.catch_warnings():
        warnings.simplefilter("error", UserWarning)
        fstx.write_fstx(data, p, **kw)
    doc = {"file": p.name, "source": source, "known": known_cols or {}, "open": open_cols or {}, **view}
    with open(OUT / (name + ".view.json"), "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=1)
    assert diff(rview.view_arrow(fstx.read_fstx(p)), view) == {}
    return p


# ---------------------------------------------------------------- R -> Python
@pytest.mark.parametrize("case", CASES)
def test_r_to_pyarrow(case):
    assert diff(rview.view_arrow(read(case)), r_view(case)) == {}


@pytest.mark.parametrize("case", CASES)
def test_r_to_pandas(case):
    pytest.importorskip("pandas")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", fstx.FstxWarning)
        df = fstx.to_pandas(read(case))
    assert diff(rview.view_pandas(df), r_view(case)).keys() == known("pandas", case).keys()


@pytest.mark.parametrize("case", CASES)
def test_r_to_polars(case):
    pytest.importorskip("polars")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", fstx.FstxWarning)
        df = fstx.to_polars(read(case))
    assert diff(rview.view_polars(df), r_view(case)).keys() == known("polars", case).keys()


# ---------------------------------------------------------------- R -> Python -> R
@pytest.mark.parametrize("case", CASES)
@pytest.mark.parametrize("path", ["pyarrow", "pandas", "polars"])
def test_r_to_python_to_r(case, path):
    t = read(case)
    if path == "pyarrow":
        data, view = t, rview.view_arrow(t)
    elif path == "pandas":
        pytest.importorskip("pandas")
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", fstx.FstxWarning)
            data = fstx.to_pandas(t)
        view = rview.view_pandas(data)
    else:
        pytest.importorskip("polars")
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", fstx.FstxWarning)
            data = fstx.to_polars(t)
        view = rview.view_polars(data)
    save(data, f"{case}.{path}", view, source=case + ".fstx", known_cols=known(path, case))


# ---------------------------------------------------------------- Python data -> R: dates, times, integer64
US = 1000000
NS = 1000 * US
DST = [1710053999 * US, 1710054000 * US, 1730611800 * US, 1730615400 * US]  # New York: 01:59:59 EST, 03:00 EDT, 01:30 twice


def native_times_arrow():
    return pa.table({
        "kolkata": pa.array([0, 1700000000123456, -1, None, 253402300799 * US, -62135596800 * US],
                            pa.timestamp("us", "Asia/Kolkata")),
        "new_york_ns": pa.array([d * 1000 for d in DST] + [None, 1700000000123456789], pa.timestamp("ns", "America/New_York")),
        "naive_s": pa.array([0, -86400, 1700000000, None, 1, 2], pa.timestamp("s")),
        "utc_ms": pa.array([1500, -1, None, 0, 86399999, 1700000000001], pa.timestamp("ms", "UTC")),
        "kathmandu": pa.array([0, 504901800 * US, 504901799 * US, None, 1, 2], pa.timestamp("us", "Asia/Kathmandu")),
        "d32": pa.array([-1, 0, 19782, -719162, 2932896, None], pa.date32()),
        "d64": pa.array([0, 2 * 86400000 + 3600000, -1, None, 86400000, -86400000], pa.date64()),
    })


def test_native_times_pyarrow():
    t = native_times_arrow()
    v = rview.view_arrow(t)
    by = {c["name"]: c for c in v["columns"]}
    # the prediction, spelled out for a few values: what R prints
    assert by["kolkata"]["values"][:2] == ["1970-01-01 05:30:00.000000 +05:30", "2023-11-15 03:43:20.123456 +05:30"]
    assert by["new_york_ns"]["values"][:4] == ["2024-03-10 01:59:59.000000 -05:00", "2024-03-10 03:00:00.000000 -04:00",
                                               "2024-11-03 01:30:00.000000 -04:00", "2024-11-03 01:30:00.000000 -05:00"]
    assert by["new_york_ns"]["values"][5] == "2023-11-14 17:13:20.123457 -05:00"  # ns past a double: R's microsecond
    assert by["naive_s"]["tzone"] == "UTC" and by["d64"]["values"][1] == "1970-01-03"  # 2 days 1 hour: floor
    assert by["kathmandu"]["values"][1:3] == ["1986-01-01 00:15:00.000000 +05:45", "1985-12-31 23:59:59.000000 +05:30"]
    save(t, "native_times.pyarrow", v)


def test_native_times_fixed_offset():
    # pyarrow and pandas give a fixed UTC offset as the time zone "+05:30". The writer stores it as R's tzone,
    # but R reads "+05:30" as a POSIX TZ string, where the sign means the opposite (UTC-05:30), and warns that
    # the zone is unknown: R shows these times 11 hours off. Open (not in DESIGN.md); check_rmatch.R shows it.
    t = pa.table({"fixed": pa.array([0, 1700000000 * US, None], pa.timestamp("us", "+05:30"))})
    v = rview.view_arrow(t)
    assert v["columns"][0]["values"][0] == "1970-01-01 05:30:00.000000 +05:30"
    save(t, "native_fixed_offset.pyarrow", v, open_cols={
        "fixed": "fixed-offset time zone '+05:30': R reads it as a POSIX TZ string (UTC-05:30) and shows the time 11 h off"})


def test_native_times_pandas():
    pd = pytest.importorskip("pandas")
    import datetime as dt

    ny = pd.DatetimeIndex(["2024-11-03 01:30:00", "2024-11-03 01:30:00", "2024-03-10 03:00:00", None])
    df = pd.DataFrame({
        "kolkata": pd.to_datetime(["2024-01-01 10:00:00.5", None, "1969-12-31 23:59:59.999999", "2000-02-29"],
                                  format="ISO8601").tz_localize("Asia/Kolkata").as_unit("us"),
        "new_york": ny.tz_localize("America/New_York", ambiguous=[True, False, False, False]),
        "naive_ns": pd.to_datetime(["2024-01-01 10:00:00.123456", "1960-06-01", None, "2262-04-11"], format="ISO8601"),
        "utc": pd.to_datetime(["1970-01-01", None, "2024-02-29 12:00", "1900-01-01"], format="ISO8601").tz_localize("UTC"),
        "dates": [dt.date(2024, 2, 29), None, dt.date(1, 1, 1), dt.date(9999, 12, 31)],
    })
    v = rview.view_pandas(df)
    by = {c["name"]: c for c in v["columns"]}
    assert by["new_york"]["values"][:2] == ["2024-11-03 01:30:00.000000 -04:00", "2024-11-03 01:30:00.000000 -05:00"]
    assert by["naive_ns"]["tzone"] == "UTC" and by["dates"]["class"] == ["Date"]
    assert diff(rview.view_arrow(pa.Table.from_pandas(df, preserve_index=False)), v) == {}  # the two views agree
    save(df, "native_times.pandas", v)


def test_native_times_polars():
    pl = pytest.importorskip("polars")
    import datetime as dt

    df = pl.DataFrame({
        "kolkata": pl.Series([dt.datetime(2024, 1, 1, 10, 0, 0, 500000), None, dt.datetime(1969, 12, 31, 23, 59, 59)],
                             dtype=pl.Datetime("us")).dt.replace_time_zone("Asia/Kolkata"),
        "new_york": pl.Series([d for d in DST[2:]] + [None], dtype=pl.Int64).cast(pl.Datetime("us"))
                      .dt.replace_time_zone("UTC").dt.convert_time_zone("America/New_York"),
        "naive_ms": pl.Series([dt.datetime(2024, 1, 1, 10, 0, 0, 123000), dt.datetime(1950, 1, 1), None],
                              dtype=pl.Datetime("ms")),
        "dates": pl.Series([dt.date(2024, 2, 29), None, dt.date(1, 1, 1)], dtype=pl.Date),
    })
    v = rview.view_polars(df)
    by = {c["name"]: c for c in v["columns"]}
    assert by["new_york"]["values"][:2] == ["2024-11-03 01:30:00.000000 -04:00", "2024-11-03 01:30:00.000000 -05:00"]
    assert diff(rview.view_arrow(df.to_arrow()), v) == {}  # polars' clock = zoneinfo's clock
    save(df, "native_times.polars", v)


def native_int64_arrow():
    big = 2**63 - 1
    return pa.table({
        "i64": pa.array([big, -big, None, 0, 2**53 + 1, -(2**31)], pa.int64()),
        "i64_small": pa.array([1, -1, None, 2**31 - 1, -(2**31) + 1, 0], pa.int64()),
        "i64_none": pa.array([None] * 6, pa.int64()),
        "u64": pa.array([0, big, None, 1, 2, 3], pa.uint64()),
        "u32": pa.array([0, 2**32 - 1, None, 1, 2, 3], pa.uint32()),
        "u32_small": pa.array([0, 7, None, 1, 2, 3], pa.uint32()),
    })


@pytest.mark.parametrize("mode", ["auto", "integer64", "double"])
def test_native_int64_pyarrow(mode):
    t = native_int64_arrow()
    if mode == "double":  # past 2^53: refused, so only the columns a double holds exactly
        t = t.select(["i64_small", "i64_none", "u32", "u32_small"])
    v = rview.view_arrow(t, int64=mode)
    kinds = {c["name"]: (c["typeof"], c["class"]) for c in v["columns"]}
    if mode == "auto":
        assert kinds["i64"] == ("double", ["integer64"]) and kinds["i64_small"] == ("integer", [])
        assert kinds["u32"] == ("double", ["integer64"]) and kinds["u32_small"] == ("integer", [])
    save(t, f"native_int64.pyarrow.{mode}", v, int64=mode)


def test_native_int64_refused(tmp_path):
    with pytest.raises(ValueError, match="-9223372036854775808"):
        fstx.write_fstx(pa.table({"x": pa.array([-(2**63), 1], pa.int64())}), tmp_path / "a.fstx")
    with pytest.raises(ValueError, match="above 9223372036854775807"):
        fstx.write_fstx(pa.table({"x": pa.array([2**63], pa.uint64())}), tmp_path / "a.fstx")
    with pytest.raises(ValueError, match="past 2"):
        fstx.write_fstx(pa.table({"x": pa.array([2**53 + 1], pa.int64())}), tmp_path / "a.fstx", int64="double")


def test_native_int64_pandas():
    pd = pytest.importorskip("pandas")
    df = pd.DataFrame({
        "nullable": pd.array([2**63 - 1, None, -5, -(2**63) + 1], dtype="Int64"),
        "numpy_small": pd.array([1, 2, 3, -4], dtype="int64"),
        "numpy_big": pd.array([2**40, 2, 3, -4], dtype="int64"),
        "nullable_small": pd.array([1, None, 3, 4], dtype="Int64"),
        "u64": pd.array([2**63 - 1, 0, None, 1], dtype="UInt64"),
    })
    v = rview.view_pandas(df)
    kinds = {c["name"]: (c["typeof"], c["class"]) for c in v["columns"]}
    assert kinds["numpy_small"] == ("integer", []) and kinds["numpy_big"] == ("double", ["integer64"])
    assert diff(rview.view_arrow(pa.Table.from_pandas(df, preserve_index=False)), v) == {}
    save(df, "native_int64.pandas", v)


def test_native_int64_polars():
    pl = pytest.importorskip("polars")
    df = pl.DataFrame({
        "big": pl.Series([2**62, None, -(2**63) + 1], dtype=pl.Int64),
        "small": pl.Series([1, None, -1], dtype=pl.Int64),
        "u32": pl.Series([2**32 - 1, None, 0], dtype=pl.UInt32),
        "u64": pl.Series([2**63 - 1, None, 0], dtype=pl.UInt64),
    })
    save(df, "native_int64.polars", rview.view_polars(df))
