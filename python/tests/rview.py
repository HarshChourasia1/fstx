"""R's view of a table, built in Python (the same JSON data as golden/rmatch/rview.R builds in R).

view_arrow(table)  what R shows for a pyarrow.Table: the table read from an R file, or the table about to be
                   written (then: what R will show after reading the file), using the column's fstx.r metadata
                   when it has some and write_fstx's rules for plain data otherwise (DESIGN.md sections 3 and 4)
view_pandas(df)    the same for a pandas DataFrame, with the values taken from pandas' own objects
view_polars(df)    the same for a polars DataFrame: the types from df.to_arrow() (what write_fstx stores), the
                   clock times from polars' own time zone code

Value strings are described at the top of rview.R. Times follow R's arithmetic on the stored double seconds,
so a time that a double cannot hold to the microsecond shows the microsecond R will show.
"""
import datetime as dt
import hashlib
import json
import math
from typing import Optional
from zoneinfo import ZoneInfo

import pyarrow as pa
import pyarrow.types as pt

INT32_MIN = -(2**31)
_UNITS = {"s": 1.0, "ms": 1e3, "us": 1e6, "ns": 1e9}
_EPOCH = dt.datetime(1970, 1, 1, tzinfo=dt.timezone.utc)
POSIXCT = ["POSIXct", "POSIXt"]


# ---------------------------------------------------------------- one value
def civil(days: int):
    """(year, month, day) of a day count from 1970-01-01, any year (H. Hinnant's algorithm)."""
    z = days + 719468
    era = z // 146097
    doe = z - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    y = yoe + era * 400
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    d = doy - (153 * mp + 2) // 5 + 1
    m = mp + 3 if mp < 10 else mp - 9
    return (y + 1 if m <= 2 else y), m, d


def r_fixed(tz: Optional[str]) -> Optional[str]:
    """R's tzone for a fixed offset as pyarrow writes it ("+05:30" -> "<+0530>-05:30", POSIX's inverted sign)."""
    if tz and tz[:1] in "+-" and len(tz) == 6 and tz[3] == ":":
        return "<%s%s%s>%s%s" % (tz[0], tz[1:3], tz[4:6], "-" if tz[0] == "+" else "+", tz[1:])
    return tz


def zone(tz: str):
    if tz[:1] == "<" and len(tz) == 13 and tz[6] == ">":  # R's POSIX form of a fixed offset: <+0530>-05:30
        tz = tz[1] + tz[2:4] + ":" + tz[4:6]
    if tz[:1] in "+-" and len(tz) == 6 and tz[3] == ":":  # a fixed offset, as pyarrow writes it
        sign = -1 if tz[0] == "-" else 1
        return dt.timezone(sign * dt.timedelta(hours=int(tz[1:3]), minutes=int(tz[4:6])))
    return ZoneInfo(tz)


def offset(whole: int, tz: str) -> int:
    """UTC offset in seconds at the instant `whole` (seconds since the epoch) in zone tz."""
    when = _EPOCH + dt.timedelta(seconds=min(max(whole, -62135596800 + 2 * 86400), 253402300799 - 2 * 86400))
    return int(when.astimezone(zone(tz)).utcoffset().total_seconds())


def clock(secs: Optional[float], tz: Optional[str]) -> Optional[str]:
    """R's view of a POSIXct value: the stored double seconds shown in zone tz ("" / None: UTC)."""
    if secs is None or not math.isfinite(secs):
        return None
    whole = math.floor(secs)
    frac = round((secs - whole) * 1e6)
    if frac >= 1000000:
        whole, frac = whole + 1, 0
    off = offset(whole, tz) if tz else 0
    days, sod = divmod(whole + off, 86400)
    y, m, d = civil(days)
    sign = "-" if off < 0 else "+"
    return "%04d-%02d-%02d %02d:%02d:%02d.%06d %s%02d:%02d" % (
        y, m, d, sod // 3600, sod % 3600 // 60, sod % 60, frac, sign, abs(off) // 3600, abs(off) % 3600 // 60)


def date(days) -> Optional[str]:
    if days is None or (isinstance(days, float) and not math.isfinite(days)):
        return None
    y, m, d = civil(math.floor(days))
    return "%04d-%02d-%02d" % (y, m, d)


def dbl(v) -> Optional[str]:
    if v is None:
        return None
    if math.isnan(v):
        return "NaN"
    if math.isinf(v):
        return "Inf" if v > 0 else "-Inf"
    return format(v, ".17g")


def chr_(v) -> Optional[str]:
    if v is None:
        return None
    b = v.encode("utf-8")
    return "%d:%s" % (len(b), hashlib.md5(b).hexdigest()) if len(b) > 2000 else v


def col(name, typeof, cls, values, tzone=None, levels=None):
    return {"name": name, "typeof": typeof, "class": list(cls), "tzone": tzone, "levels": levels,
            "values": list(values)}


def table(names, nrow, columns):
    return {"names": list(names), "nrow": nrow, "columns": columns}


# ---------------------------------------------------------------- pyarrow
def _r(field: pa.Field) -> Optional[dict]:
    md = field.metadata or {}
    return json.loads(md[b"fstx.r"]) if b"fstx.r" in md else None


def _fits(vals) -> bool:
    v = [x for x in vals if x is not None]
    return not v or (min(v) > INT32_MIN and max(v) < 2**31)


def _tzone(r: Optional[dict], tz: Optional[str]):
    if r is not None and r.get("logical") == "POSIXct":
        return r.get("tzone")  # None = no attribute
    return r_fixed(tz) or "UTC"  # data that never came from R: naive times are shown as UTC


def view_arrow_column(name: str, field: pa.Field, arr, int64: str = "auto") -> dict:
    t = field.type
    r = _r(field)
    cls = (r or {}).get("class")
    if pt.is_dictionary(t):
        a = arr
        if isinstance(a, pa.ChunkedArray):
            a = a.unify_dictionaries()
            a = a.chunk(0) if a.num_chunks == 1 else (pa.concat_arrays(a.chunks) if a.num_chunks else
                                                       pa.array([], t))
        levels = a.dictionary.cast(pa.string()).to_pylist()
        codes = [None if i is None else i + 1 for i in a.indices.to_pylist()]
        default = ["ordered", "factor"] if t.ordered else ["factor"]
        return col(name, "integer", cls or default, codes, levels=levels)
    vals = arr.to_pylist() if not pt.is_timestamp(t) else arr.cast(pa.int64()).to_pylist()
    if pt.is_boolean(t) or pt.is_null(t):
        return col(name, "logical", [], [None if v is None else ("TRUE" if v else "FALSE") for v in vals])
    if pt.is_integer(t):
        if t.bit_width < 32 or t == pa.int32():
            return col(name, "integer", cls or [], [None if v is None else str(v) for v in vals])
        is64 = cls is not None and "integer64" in cls
        if is64 or int64 == "integer64" or (int64 == "auto" and not _fits(vals)):
            return col(name, "double", ["integer64"], [None if v is None else str(v) for v in vals])
        if int64 == "auto":
            return col(name, "integer", [], [None if v is None else str(v) for v in vals])
        return col(name, "double", [], [dbl(None if v is None else float(v)) for v in vals])
    if pt.is_floating(t):
        return col(name, "double", cls or [], [dbl(v) for v in vals])
    if pt.is_string(t) or pt.is_large_string(t) or getattr(pt, "is_string_view", lambda _: False)(t):
        return col(name, "character", [], [chr_(v) for v in vals])
    if pt.is_date32(t):
        typeof = "integer" if (r or {}).get("phys") == "integer" else "double"
        days = arr.cast(pa.int32()).to_pylist()
        return col(name, typeof, cls or ["Date"], [date(v) for v in days])
    if pt.is_date64(t):
        ms = arr.cast(pa.int64()).to_pylist()
        return col(name, "double", ["Date"], [None if v is None else date(float(v) / 86400000.0) for v in ms])
    if pt.is_timestamp(t):
        tz = _tzone(r, t.tz)
        div = _UNITS[t.unit]
        secs = [None if v is None else (float(v) if div == 1.0 else float(v) / div) for v in vals]
        return col(name, "double", cls or POSIXCT, [clock(s, tz) for s in secs], tzone=tz)
    raise TypeError(f"rview: no view for column '{name}' of type {t}")


def view_arrow(t: pa.Table, int64: str = "auto") -> dict:
    return table(t.column_names, t.num_rows,
                 [view_arrow_column(f.name, f, t.column(i), int64) for i, f in enumerate(t.schema)])


# ---------------------------------------------------------------- pandas
def view_pandas(df, int64: str = "auto") -> dict:
    """R's view of a pandas DataFrame written by write_fstx (or made by fstx.to_pandas), from pandas' values."""
    import pandas as pd

    saved = {c["name"]: c.get("r") for c in (df.attrs.get("fstx") or {}).get("columns", [])}
    cols = []
    for j, name in enumerate(df.columns):
        s = df.iloc[:, j]
        r = saved.get(name)
        cls = (r or {}).get("class")
        na = s.isna().tolist()
        d = s.dtype
        if isinstance(d, pd.CategoricalDtype):
            levels = [None if (not isinstance(x, str) and pd.isna(x)) else str(x) for x in d.categories]
            codes = [None if c < 0 else int(c) + 1 for c in s.cat.codes.tolist()]
            cols.append(col(name, "integer", cls or (["ordered", "factor"] if d.ordered else ["factor"]), codes,
                            levels=levels))
        elif isinstance(d, pd.DatetimeTZDtype) or str(d).startswith("datetime64"):
            tz = getattr(d, "tz", None)
            if isinstance(tz, dt.timezone) and tz is not dt.timezone.utc:  # a fixed offset: pyarrow's "+05:30"
                o = int(tz.utcoffset(None).total_seconds())
                tz = "%s%02d:%02d" % ("-" if o < 0 else "+", abs(o) // 3600, abs(o) % 3600 // 60)
            tz = None if tz is None else str(tz)
            tzone = _tzone(r, tz)
            vals = []
            for ts, m in zip(s.tolist(), na):  # (whole microseconds only: pandas' fields cut nanoseconds off)
                if m:
                    vals.append(None)
                    continue
                if tz is not None and not tzone:  # an aware time R shows in UTC
                    ts = ts.tz_convert("UTC")
                # (the offset from the wall time: utcoffset() fails past the year 9999)
                off = 0 if ts.tzinfo is None else int((ts.tz_localize(None) - ts.tz_convert(None)).total_seconds())
                shown = ts
                sign = "-" if off < 0 else "+"
                vals.append("%04d-%02d-%02d %02d:%02d:%02d.%06d %s%02d:%02d" % (
                    shown.year, shown.month, shown.day, shown.hour, shown.minute, shown.second, shown.microsecond,
                    sign, abs(off) // 3600, abs(off) % 3600 // 60))
            cols.append(col(name, "double", cls or POSIXCT, vals, tzone=tzone))
        elif str(d) in ("bool", "boolean"):
            cols.append(col(name, "logical", [], [None if m else ("TRUE" if v else "FALSE")
                                                  for v, m in zip(s.tolist(), na)]))
        elif str(d) in ("int8", "int16", "int32", "uint8", "uint16", "Int8", "Int16", "Int32", "UInt8", "UInt16"):
            cols.append(col(name, "integer", cls or [], [None if m else str(int(v)) for v, m in zip(s.tolist(), na)]))
        elif str(d) in ("int64", "uint32", "uint64", "Int64", "UInt32", "UInt64"):
            vals = [None if m else int(v) for v, m in zip(s.tolist(), na)]
            is64 = cls is not None and "integer64" in cls
            if is64 or int64 == "integer64" or (int64 == "auto" and not _fits(vals)):
                cols.append(col(name, "double", ["integer64"], [None if v is None else str(v) for v in vals]))
            elif int64 == "auto":
                cols.append(col(name, "integer", [], [None if v is None else str(v) for v in vals]))
            else:
                cols.append(col(name, "double", [], [None if v is None else dbl(float(v)) for v in vals]))
        elif str(d).lower().startswith("float"):
            # pandas holds one missing value for NaN and NA: both are written as R's NA
            cols.append(col(name, "double", cls or [], [None if m else dbl(float(v)) for v, m in zip(s.tolist(), na)]))
        else:  # strings (str / object), or datetime.date objects
            vals = s.tolist()
            rlog = (r or {}).get("logical")
            if all(na) and d == object and r is None:  # pyarrow sees no type: the writer's all-null column, logical NA
                cols.append(col(name, "logical", [], [None] * len(vals)))
            elif any(isinstance(v, dt.date) for v in vals) or (all(na) and rlog == "Date"):
                typeof = "integer" if (r or {}).get("phys") == "integer" else "double"
                cols.append(col(name, typeof, cls or ["Date"],
                                [None if m else "%04d-%02d-%02d" % (v.year, v.month, v.day) for v, m in zip(vals, na)]))
            else:
                cols.append(col(name, "character", [], [None if m else chr_(v) for v, m in zip(vals, na)]))
    return table([str(c) for c in df.columns], len(df), cols)


# ---------------------------------------------------------------- polars
def view_polars(df, int64: str = "auto") -> dict:
    """R's view of a polars DataFrame written by write_fstx: types from to_arrow() (what the writer stores), the
    clock times recomputed with polars' own time zone code."""
    import polars as pl

    v = view_arrow(df.to_arrow(), int64)
    for c in v["columns"]:
        s = df[c["name"]]
        if isinstance(s.dtype, pl.Datetime) and s.dtype.time_zone and c["tzone"] != s.dtype.time_zone:
            continue
        if isinstance(s.dtype, pl.Datetime) and s.dtype.time_zone:
            off = (s.dt.base_utc_offset() + s.dt.dst_offset()).dt.total_seconds().to_list()
            parts = zip(s.dt.year().to_list(), s.dt.month().to_list(), s.dt.day().to_list(), s.dt.hour().to_list(),
                        s.dt.minute().to_list(), s.dt.second().to_list(), s.dt.microsecond().to_list(), off)
            got = []
            for y, mo, d, h, mi, se, us, o in parts:
                if y is None:
                    got.append(None)
                    continue
                o = int(o)
                got.append("%04d-%02d-%02d %02d:%02d:%02d.%06d %s%02d:%02d" % (
                    y, mo, d, h, mi, se, us, "-" if o < 0 else "+", abs(o) // 3600, abs(o) % 3600 // 60))
            c["values"] = got
    return v
