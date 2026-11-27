"""R-match: what Python shows is what R shows, both ways.

R -> Python: golden/rmatch/<case>.fstx were written by R (make_rmatch.R), with R's own view of every value in
<case>.view.json (rview.R). Each file is read with pyarrow, then turned into pandas and polars, and the view
of each (rview.py) must equal R's, apart from the differences DESIGN.md section 8 lists (KNOWN below).

Python -> R: every table read here, and tables built from plain Python data, are written into
golden/from_python/rmatch/<name>.fstx with <name>.view.json: what R is to show after reading the file
(rview.py's prediction, from the data written), plus the R case it came from and its known differences.
check_rmatch.R reads each file in R and compares. The same files are read back here too.

fstx 1.2 (int64 and binary columns): make_rmatch.R --v12 writes rmatch_int64_native and rmatch_binary with an R
built from the 1.2 branch; besides the views, each is written back from Python with R's settings and must give the
same bytes (DESIGN-1.2.md section 6, rule 3). Their tests, and every test here that reads an int64 or binary
column, wait for the engine's drop 2 (conftest.py).
"""
import json
import warnings

import pyarrow as pa
import pytest

import fstx
import rview
from conftest import DROP2, GOLDEN

RM = GOLDEN / "rmatch"
OUT = GOLDEN / "from_python" / "rmatch"
OUT.mkdir(parents=True, exist_ok=True)
CASES = sorted(p.stem for p in RM.glob("*.fstx"))
V12 = {"rmatch_int64_native", "rmatch_binary"}  # written by R with fstx 1.2 (make_rmatch.R --v12)
CASE_PARAMS = [pytest.param(c, marks=pytest.mark.drop2) if c in V12 else c for c in CASES]
drop2 = pytest.mark.drop2

# Differences from R's view that DESIGN.md section 8 documents: (path, case, column) -> reason
KNOWN = {
    ("polars", "rmatch_dates", "d_int"): "polars carries no metadata: a Date stored as integer comes back double",
    ("polars", "rmatch_times", "tz_empty"): "polars carries no metadata: tzone \"\" comes back as \"UTC\"",
    ("polars", "rmatch_times", "tz_none"): "polars carries no metadata: no tzone comes back as \"UTC\"",
    ("polars", "rmatch_int64", "none"): "polars carries no metadata: an all-NA Int64 comes back integer (int64='auto')",
    # found by this file (18 Nov 2026), added to DESIGN.md section 8: the same rule for values that all fit int32
    ("polars", "rmatch_int64", "small"): "polars carries no metadata: an Int64 whose values all fit int32 comes back "
                                         "integer (int64='auto')",
    # part B (factors, NA, strings)
    ("pandas", "rmatch_factors", "na_level"): "a factor with an NA level cannot be a Categorical: strings (warning)",
    ("pandas", "rmatch_na", "dbl"): "pandas: R's NaN becomes NA in a double column",
    ("pandas", "rmatch_na", "dbl_nan_only"): "pandas: R's NaN becomes NA in a double column",
    **{("polars", "rmatch_factors", c): "polars: a Categorical keeps only the levels in use, in order of first "
                                         "appearance, and no order; an all-null one has no levels"
       for c in ("plain", "unused", "ordered", "odd_levels", "all_na", "ordered_unused")},
    # found by this file (18 Nov 2026), added to DESIGN.md section 8
    ("polars", "rmatch_factors", "na_level"): "a factor with an NA level comes back as strings through polars too",
    # fstx 1.2: the same rules for int64 columns; binary columns lose their R class through polars
    ("polars", "rmatch_int64_native", "none"): "polars carries no metadata: an all-NA Int64 comes back integer "
                                               "(int64='auto')",
    ("polars", "rmatch_int64_native", "small"): "polars carries no metadata: an Int64 whose values all fit int32 "
                                                "comes back integer (int64='auto')",
    ("polars", "rmatch_binary", "asis"): "polars carries no metadata: the AsIs class is lost",
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
@pytest.mark.parametrize("case", CASE_PARAMS)
def test_r_to_pyarrow(case):
    assert diff(rview.view_arrow(read(case)), r_view(case)) == {}


@pytest.mark.parametrize("case", CASE_PARAMS)
def test_r_to_pandas(case):
    pytest.importorskip("pandas")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", fstx.FstxWarning)
        df = fstx.to_pandas(read(case))
    got = diff(rview.view_pandas(df), r_view(case))
    pd = pytest.importorskip("pandas")
    if int(pd.__version__.split(".")[0]) >= 3:
        assert got.keys() == known("pandas", case).keys(), got
    else:  # pandas 2 keeps NaN and NA apart, so the pandas 3 differences may not show
        assert set(got) <= set(known("pandas", case)), got


@pytest.mark.parametrize("case", CASE_PARAMS)
def test_r_to_polars(case):
    pytest.importorskip("polars")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", fstx.FstxWarning)
        df = fstx.to_polars(read(case))
    got = diff(rview.view_polars(df), r_view(case))
    assert got.keys() == known("polars", case).keys(), got


# ---------------------------------------------------------------- R -> Python -> R
@pytest.mark.parametrize("case", CASE_PARAMS)
@pytest.mark.parametrize("path", ["pyarrow", "pandas", "polars"])
def test_r_to_python_to_r(case, path):
    if path == "polars" and case in ("rmatch_int64", "rmatch_na") and not DROP2:
        pytest.skip("waits for engine drop 2: through polars (no metadata) the values past int32 become an int64 column")
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


FIXED = ["+05:30", "-03:00", "+00:00", "-00:00", "+05:45", "-09:30", "+14:00", "-12:00"]
FIXED_R = ["<+0530>-05:30", "<-0300>+03:00", "<+0000>-00:00", "<-0000>+00:00", "<+0545>-05:45", "<-0930>+09:30",
           "<+1400>-14:00", "<-1200>+12:00"]


def fixed_offsets_arrow():
    vals = [0, 1700000000 * US + 123456, None, -2208988800 * US, 253402300799 * US, -1]
    return pa.table({tz: pa.array(vals, pa.timestamp("us", tz)) for tz in FIXED})


def check_fixed_file(p, tzs):
    """The file holds R's POSIX form of each offset; read back, each column is the Arrow fixed offset again."""
    m = fstx.metadata(p)
    assert [c.tzone for c in m.columns] == [FIXED_R[FIXED.index(tz)] for tz in tzs]
    got = fstx.read_fstx(p)
    assert [f.type for f in got.schema] == [pa.timestamp("us", tz) for tz in tzs]
    return got


def test_native_times_fixed_offset(tmp_path):
    # pyarrow and pandas give a fixed UTC offset as the time zone "+05:30". R reads "+05:30" as a POSIX TZ string
    # (UTC-05:30), so the writer stores R's POSIX form "<+0530>-05:30" (sign inverted) and the reader maps it
    # back (DESIGN.md section 3). R 4.5.1 and 4.6.1 show the right wall time for it; they warn "unknown timezone".
    t = fixed_offsets_arrow()
    v = rview.view_arrow(t)
    by = {c["name"]: c for c in v["columns"]}
    assert [by[tz]["tzone"] for tz in FIXED] == FIXED_R
    assert by["+05:30"]["values"][:2] == ["1970-01-01 05:30:00.000000 +05:30", "2023-11-15 03:43:20.123456 +05:30"]
    assert by["-03:00"]["values"][0] == "1969-12-31 21:00:00.000000 -03:00"
    assert by["-09:30"]["values"][3] == "1899-12-31 14:30:00.000000 -09:30"
    assert by["+14:00"]["values"][4] == "10000-01-01 13:59:59.000000 +14:00"
    assert by["+00:00"]["values"][5] == "1969-12-31 23:59:59.999999 +00:00"
    p = save(t, "native_fixed_offset.pyarrow", v)
    got = check_fixed_file(p, FIXED)
    assert got.equals(t)  # the same instants, the same time zones
    assert json.loads(got.schema.field("+05:30").metadata[b"fstx.r"])["tzone"] == "<+0530>-05:30"
    # read -> write -> read keeps the file's tzone; an append of plain "+05:30" rows matches it
    q = tmp_path / "again.fstx"
    fstx.write_fstx(got, q)
    assert check_fixed_file(q, FIXED).equals(t)
    fstx.write_fstx(t, q, append=True)
    assert check_fixed_file(q, FIXED).equals(pa.concat_tables([t, t]))
    # a naive filter value is a clock time in the column's offset
    f = fstx.read_fstx(p, columns=["+05:30"], filters=[("+05:30", "==", "1970-01-01T05:30:00")])
    assert f.column(0).cast(pa.int64()).to_pylist() == [0]


def test_fixed_offset_mapping():
    from fstx import _meta

    assert [_meta.r_tzone(tz) for tz in FIXED] == FIXED_R
    assert [_meta.arrow_tz(tz) for tz in FIXED_R] == FIXED
    for tz in ("Asia/Kolkata", "UTC", "", None, "+0530", "EST5EDT", "<+0530>+05:30", "<+0530>-05:45"):
        assert _meta.r_tzone(tz) == tz and _meta.arrow_tz(tz) == tz  # names and other POSIX strings are kept


def test_fixed_offset_from_r_style_tzone(tmp_path):
    # a file whose tzone is already the POSIX form (as R would write it) reads as the Arrow offset; one that holds
    # "+05:30" itself (an older Python write, or R) reads as it is
    t = pa.table({"x": pa.array([0, None], pa.timestamp("us", "+05:30"))})
    t2 = pa.table([t.column(0)], schema=pa.schema([pa.field("x", t.schema.field("x").type, metadata={
        b"fstx.r": b'{"phys":"double","logical":"POSIXct","class":["POSIXct","POSIXt"],"tzone":"+05:30"}'})]))
    fstx.write_fstx(t2, tmp_path / "old.fstx")
    assert fstx.metadata(tmp_path / "old.fstx").columns[0].tzone == "+05:30"
    assert fstx.read_fstx(tmp_path / "old.fstx").schema.field("x").type == pa.timestamp("us", "+05:30")
    fstx.write_fstx(t, tmp_path / "new.fstx")
    assert fstx.metadata(tmp_path / "new.fstx").columns[0].tzone == "<+0530>-05:30"


def test_fixed_offset_pandas():
    pd = pytest.importorskip("pandas")
    import datetime as dt

    # pandas from pyarrow: datetime.timezone offsets; through fstx.to_pandas and back, nothing changes
    got = fstx.read_fstx(save(fixed_offsets_arrow(), "native_fixed_offset.pyarrow2", rview.view_arrow(fixed_offsets_arrow())))
    df = fstx.to_pandas(got)
    assert str(df["+05:30"].dtype).startswith("datetime64[us, ")
    assert df["+05:30"].dt.tz.utcoffset(None) == dt.timedelta(hours=5, minutes=30)  # pandas 2 shows pytz.FixedOffset(330)
    assert diff(rview.view_pandas(df), rview.view_arrow(got)) == {}
    p = save(df, "native_fixed_offset.pandas_rt", rview.view_pandas(df))
    assert check_fixed_file(p, FIXED).equals(got)
    # plain pandas data with fixed offsets
    tz = [dt.timezone(dt.timedelta(hours=5, minutes=30)), dt.timezone(-dt.timedelta(hours=3))]
    naive = pd.to_datetime(["2024-01-01 10:00:00.5", None, "1969-12-31 23:59:59"], format="ISO8601")
    df2 = pd.DataFrame({"a": naive.tz_localize(tz[0]).as_unit("us"), "b": naive.tz_localize(tz[1]).as_unit("us")})
    v = rview.view_pandas(df2)
    assert [c["tzone"] for c in v["columns"]] == ["<+0530>-05:30", "<-0300>+03:00"]
    assert v["columns"][0]["values"][0] == "2024-01-01 10:00:00.500000 +05:30"
    p = save(df2, "native_fixed_offset.pandas", v)
    back = fstx.to_pandas(check_fixed_file(p, ["+05:30", "-03:00"]))
    if int(pd.__version__.split(".")[0]) >= 3:
        pd.testing.assert_frame_equal(back, df2, check_dtype=True)
    else:  # pandas 2 names a fixed offset pytz.FixedOffset(330) where df2 says UTC+05:30: same times, same offsets
        assert list(back.columns) == list(df2.columns)
        for c in df2.columns:
            assert back[c].dt.tz.utcoffset(None) == df2[c].dt.tz.utcoffset(None)
            pd.testing.assert_series_equal(back[c].dt.tz_convert("UTC"), df2[c].dt.tz_convert("UTC"))


def test_fixed_offset_polars():
    pl = pytest.importorskip("polars")

    # polars knows named time zones only: to_polars gives UTC (same instants) with a warning; written from polars
    # the column is a UTC one (DESIGN.md section 8)
    got = fstx.read_fstx(save(fixed_offsets_arrow(), "native_fixed_offset.pyarrow3", rview.view_arrow(fixed_offsets_arrow())))
    with pytest.warns(fstx.FstxWarning, match="fixed UTC offset"):
        df = fstx.to_polars(got)
    assert all(df[tz].dtype == pl.Datetime("us", "UTC") for tz in FIXED)
    assert df.to_arrow().column("+05:30").cast(pa.int64()).equals(got.column("+05:30").cast(pa.int64()))
    p = save(df, "native_fixed_offset.polars", rview.view_polars(df))
    m = fstx.metadata(p)
    assert all(c.tzone == "UTC" for c in m.columns)


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


@pytest.mark.parametrize("mode", [pytest.param("auto", marks=drop2), pytest.param("native", marks=drop2),
                                  "integer64", "double"])
def test_native_int64_pyarrow(mode):
    # "auto" and "native" store values past int32 as int64 columns (fstx 1.2), which R reads as integer64 too
    t = native_int64_arrow()
    if mode == "double":  # past 2^53: refused, so only the columns a double holds exactly
        t = t.select(["i64_small", "i64_none", "u32", "u32_small"])
    v = rview.view_arrow(t, int64=mode)
    kinds = {c["name"]: (c["typeof"], c["class"]) for c in v["columns"]}
    if mode == "auto":
        assert kinds["i64"] == ("double", ["integer64"]) and kinds["i64_small"] == ("integer", [])
        assert kinds["u32"] == ("double", ["integer64"]) and kinds["u32_small"] == ("integer", [])
    if mode == "native":
        assert all(k == ("double", ["integer64"]) for k in kinds.values())
    save(t, f"native_int64.pyarrow.{mode}", v, int64=mode)


def test_native_int64_refused(tmp_path):
    with pytest.raises(ValueError, match="-9223372036854775808"):
        fstx.write_fstx(pa.table({"x": pa.array([-(2**63), 1], pa.int64())}), tmp_path / "a.fstx")
    with pytest.raises(ValueError, match="above 9223372036854775807"):
        fstx.write_fstx(pa.table({"x": pa.array([2**63], pa.uint64())}), tmp_path / "a.fstx")
    with pytest.raises(ValueError, match="past 2"):
        fstx.write_fstx(pa.table({"x": pa.array([2**53 + 1], pa.int64())}), tmp_path / "a.fstx", int64="double")


@drop2
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


@drop2
def test_native_int64_polars():
    pl = pytest.importorskip("polars")
    df = pl.DataFrame({
        "big": pl.Series([2**62, None, -(2**63) + 1], dtype=pl.Int64),
        "small": pl.Series([1, None, -1], dtype=pl.Int64),
        "u32": pl.Series([2**32 - 1, None, 0], dtype=pl.UInt32),
        "u64": pl.Series([2**63 - 1, None, 0], dtype=pl.UInt64),
    })
    save(df, "native_int64.polars", rview.view_polars(df))


# ---------------------------------------------------------------- Python data -> R: factors, NA, strings
LONG = "abcdefghij" * 10000                                          # 100,000 bytes
TEXTS = ["plain", "", None, "NA", "हिन्दी", "தமிழ்", "😀 4-byte", "é combining", "日本語", "tab\tline\nbreak", LONG]


def native_factors_arrow():
    two = pa.chunked_array([pa.array(["b", "a", None]).dictionary_encode(), pa.array(["c", "a", "b"]).dictionary_encode()])
    return pa.table({
        "encoded": pa.array(["b", "a", None, "c", "a", "b"]).dictionary_encode(),
        "unused": pa.DictionaryArray.from_arrays(pa.array([1, 1, 3, None, 1, 3], pa.int32()), ["w", "x", "y", "z", "v"]),
        "ordered": pa.DictionaryArray.from_arrays(pa.array([0, 2, 1, None, 2, 0], pa.int8()), ["lo", "mid", "hi"],
                                                  ordered=True),
        "large": pa.DictionaryArray.from_arrays(pa.array([0, 1, 2, 2, None, 0], pa.int16()),
                                                pa.array(["हिन्दी", "", "NA"], pa.large_string())),
        "two_chunks": two,  # two dictionaries: one set of levels, in order of first appearance
        "all_null": pa.DictionaryArray.from_arrays(pa.array([None] * 6, pa.int32()), ["p", "q"]),
    })


def test_native_factors_pyarrow():
    t = native_factors_arrow()
    v = rview.view_arrow(t)
    by = {c["name"]: c for c in v["columns"]}
    assert by["two_chunks"]["levels"] == ["b", "a", "c"] and by["two_chunks"]["values"] == [1, 2, None, 3, 2, 1]
    assert by["ordered"]["class"] == ["ordered", "factor"] and by["unused"]["levels"] == ["w", "x", "y", "z", "v"]
    save(t, "native_factors.pyarrow", v)


def test_native_factors_pandas():
    pd = pytest.importorskip("pandas")
    df = pd.DataFrame({
        "plain": pd.Categorical(["b", "a", None, "c", "a", "b"]),
        "unused": pd.Categorical(["x", "x", "z", None, "x", "z"], categories=["w", "x", "y", "z", "v"]),
        "ordered": pd.Categorical(["lo", "hi", "mid", None, "hi", "lo"], categories=["lo", "mid", "hi"], ordered=True),
        "indic": pd.Categorical(["हिन्दी", "", "NA", "NA", None, "தமிழ்"]),
        "all_na": pd.Categorical([None] * 6, categories=["p", "q"]),
    })
    v = rview.view_pandas(df)
    by = {c["name"]: c for c in v["columns"]}
    assert by["plain"]["levels"] == ["a", "b", "c"] and by["ordered"]["class"] == ["ordered", "factor"]
    assert diff(rview.view_arrow(pa.Table.from_pandas(df, preserve_index=False)), v) == {}
    save(df, "native_factors.pandas", v)


def test_native_factors_polars():
    pl = pytest.importorskip("polars")
    df = pl.DataFrame({
        "categorical": pl.Series(["b", "a", None, "c", "a", "b"], dtype=pl.Categorical),
        # an Enum keeps its categories, unused ones too, in their order
        "enum": pl.Series(["x", "x", "z", None, "x", "z"], dtype=pl.Enum(["w", "x", "y", "z", "v"])),
    })
    v = rview.view_polars(df)
    by = {c["name"]: c for c in v["columns"]}
    assert by["enum"]["levels"] == ["w", "x", "y", "z", "v"] and by["enum"]["values"][:3] == [2, 2, 4]
    save(df, "native_factors.polars", v)


def native_na_arrow():
    return pa.table({
        "lgl": pa.array([None, True, False, None, True, None]),
        "int": pa.array([None, 1, -(2**31) + 1, 2**31 - 1, None, 0], pa.int32()),
        "dbl": pa.array([None, float("nan"), -0.0, float("inf"), float("-inf"), 0.1]),
        "chr": pa.array([None, "", "NA", " ", "a", None]),
        "date": pa.array([None, 19723, None, 0, None, None], pa.date32()),
        "time": pa.array([None, 0, None, 1000000, None, 2000000], pa.timestamp("us", "Asia/Kolkata")),
        "i64": pa.array([None, 5000000000, None, -1, None, 0], pa.int64()),
        "null_type": pa.nulls(6),
        "int_all": pa.nulls(6, pa.int32()),
        "dbl_all": pa.nulls(6, pa.float64()),
        "chr_all": pa.nulls(6, pa.string()),
        "date_all": pa.nulls(6, pa.date32()),
        "time_all": pa.nulls(6, pa.timestamp("us", "UTC")),
    })


@drop2  # its int64 column is stored as one (fstx 1.2)
def test_native_na_pyarrow():
    t = native_na_arrow()
    v = rview.view_arrow(t)
    by = {c["name"]: c for c in v["columns"]}
    assert by["dbl"]["values"][:3] == [None, "NaN", "-0"] and by["chr"]["values"][:3] == [None, "", "NA"]
    assert by["null_type"]["typeof"] == "logical" and by["i64"]["class"] == ["integer64"]
    save(t, "native_na.pyarrow", v)


def test_native_na_pandas():
    pd = pytest.importorskip("pandas")
    import numpy as np

    df = pd.DataFrame({
        "boolean": pd.array([None, True, False, None, True, None], dtype="boolean"),
        "Int32": pd.array([None, 1, -(2**31) + 1, 2**31 - 1, None, 0], dtype="Int32"),
        "Float64": pd.array([None, 1.5, -0.0, float("inf"), None, 0.1], dtype="Float64"),
        "float64_nan": np.array([np.nan, 1.5, np.nan, -np.inf, 0.0, 2.0]),  # NaN is pandas' missing value: R's NA
        "object": pd.Series([None, "", "NA", " ", "a", np.nan], dtype=object),
        "str": pd.Series([None, "", "NA", " ", "a", None], dtype="str"),
        "all_none": pd.Series([None] * 6, dtype=object),
    })
    v = rview.view_pandas(df)
    by = {c["name"]: c for c in v["columns"]}
    assert by["float64_nan"]["values"][:3] == [None, "1.5", None] and by["object"]["values"][:3] == [None, "", "NA"]
    assert diff(rview.view_arrow(pa.Table.from_pandas(df, preserve_index=False)), v) == {}
    save(df, "native_na.pandas", v)


def test_native_na_polars():
    pl = pytest.importorskip("polars")
    df = pl.DataFrame({
        "lgl": pl.Series([None, True, False, None, True, None], dtype=pl.Boolean),
        "int": pl.Series([None, 1, -(2**31) + 1, 2**31 - 1, None, 0], dtype=pl.Int32),
        "dbl": pl.Series([None, float("nan"), -0.0, float("inf"), float("-inf"), 0.1], dtype=pl.Float64),  # NaN stays NaN
        "chr": pl.Series([None, "", "NA", " ", "a", None], dtype=pl.String),
        "null": pl.Series([None] * 6, dtype=pl.Null),
        "date_all": pl.Series([None] * 6, dtype=pl.Date),
    })
    v = rview.view_polars(df)
    by = {c["name"]: c for c in v["columns"]}
    assert by["dbl"]["values"][:2] == [None, "NaN"] and by["null"]["typeof"] == "logical"
    save(df, "native_na.polars", v)


def test_native_strings_pyarrow():
    t = pa.table({
        "string": pa.array(TEXTS, pa.string()),
        "large_string": pa.array(TEXTS, pa.large_string()),
        **({"string_view": pa.array(TEXTS, pa.string_view())} if hasattr(pa, "string_view") else {}),
    })
    v = rview.view_arrow(t)
    assert v["columns"][0]["values"][-1].startswith("100000:")
    save(t, "native_strings.pyarrow", v)


def test_native_strings_pandas():
    pd = pytest.importorskip("pandas")
    df = pd.DataFrame({"str": pd.Series(TEXTS, dtype="str"), "object": pd.Series(TEXTS, dtype=object)})
    v = rview.view_pandas(df)
    assert diff(rview.view_arrow(pa.Table.from_pandas(df, preserve_index=False)), v) == {}
    save(df, "native_strings.pandas", v)


def test_native_strings_polars():
    pl = pytest.importorskip("polars")
    df = pl.DataFrame({"s": pl.Series(TEXTS, dtype=pl.String)})
    save(df, "native_strings.polars", rview.view_polars(df))


# ---------------------------------------------------------------- fstx 1.2: binary columns, same bytes as R
BINS = [b"\x00\x01\x02", b"", None, b"text", bytes(range(256)), b"\xff\x00", bytes([0x41, 0x00, 0xFF]) * 30000,
        b"\x00", None, b"\xc3\x28"]


@drop2
def test_native_binary_pyarrow():
    t = pa.table({
        "binary": pa.array(BINS, pa.binary()),
        "large_binary": pa.array(BINS, pa.large_binary()),
        "fixed": pa.array([b"ab", b"\x00\x00", None, b"\xff\x00"] * 2 + [b"zz", None], pa.binary(2)),
        "repeats": pa.array([b"\x01\x02", b"\x00", None, b"\x01\x02", b""] * 2),
        **({"binary_view": pa.array(BINS, pa.binary()).cast(pa.binary_view())} if hasattr(pa, "binary_view") else {}),
    })
    v = rview.view_arrow(t)
    by = {c["name"]: c for c in v["columns"]}
    assert by["binary"]["typeof"] == "list" and by["binary"]["class"] == [] and by["binary"]["values"][:3] == ["000102", "", None]
    assert by["binary"]["values"][6].startswith("90000:")
    save(t, "native_binary.pyarrow", v)


@drop2
def test_native_binary_pandas():
    pd = pytest.importorskip("pandas")
    df = pd.DataFrame({"raw": pd.Series(BINS, dtype=object), "none": pd.Series([None] * len(BINS), dtype=object)})
    df.attrs["fstx"] = {"table_attrs": None, "nrow": None, "columns": [
        {"name": "raw", "r": None, "attrs": None},
        {"name": "none", "r": {"phys": "binary", "logical": "none"}, "attrs": None}]}  # all missing: the metadata types it
    v = rview.view_pandas(df)
    by = {c["name"]: c for c in v["columns"]}
    assert by["raw"]["typeof"] == by["none"]["typeof"] == "list"
    save(df, "native_binary.pandas", v)


@drop2
def test_native_binary_polars():
    pl = pytest.importorskip("polars")
    df = pl.DataFrame({"b": pl.Series(BINS, dtype=pl.Binary), "i": pl.Series([2**62, None] * 5, dtype=pl.Int64)})
    save(df, "native_binary.polars", rview.view_polars(df))


@drop2
@pytest.mark.parametrize("case", sorted(V12 & set(CASES)))
def test_same_bytes_as_r(case, tmp_path):
    """R wrote the file (fstx 1.2, threads = 1, its defaults); Python reads it and writes it again with the same
    settings: the bytes must be R's (DESIGN-1.2.md section 6, rule 3)."""
    with open(RM / (case + ".view.json"), encoding="utf-8") as fh:
        made = json.load(fh)["made_with"]
    eng = fstx.build_info()["engine_version"]
    if not made.startswith(f"fstx {eng},"):
        pytest.skip(f"R's file was made with {made.split(',')[0]}, this engine is fstx {eng} (created_by differs)")
    q = tmp_path / (case + ".fstx")
    with warnings.catch_warnings():
        warnings.simplefilter("error", UserWarning)
        fstx.write_fstx(read(case), q, threads=1)
    assert q.read_bytes() == (RM / (case + ".fstx")).read_bytes()
