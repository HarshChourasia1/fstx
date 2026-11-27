"""write_fstx: a pyarrow.Table (or a pandas / polars DataFrame) into an fstx file.

Everything about types is decided here with pyarrow (DESIGN.md sections 3 to 5); ``_core.write`` receives, per
column, the stored definition (spec 4.2) and Arrow arrays that the binding hands to the engine: the chunks of a
number column as they are (``_Num``: the binding widens them and writes R's missing values, a window of rows at a
time, so no joined copy of the column is made), or the string chunks (dictionary-coded when the column repeats
its values, so that equal strings share one pointer as in R: ``_text_chunks``).

Metadata the reader left on a table says how R held a column and is used when it still fits the data
(keys and helpers in ``_meta.py``; a column without any of it is written as the plain data it is):

    field  b"fstx.r"            JSON: {"phys": "double", "logical": "Date", "class": ["Date"], "tzone": ""}
                                (phys integer / double / logical / character; logical none / factor / Date /
                                POSIXct; class = R's class vector, absent for none; tzone absent = no attribute)
    field  b"fstx.attrs"        the column's attribute blob (spec 9.4), base64 (raw bytes are taken too)
    schema b"fstx.table_attrs"  the table's attribute blob, base64 (raw bytes are taken too)
    schema b"fstx.nrow"         rows of the table when it was read (decimal text); only looked at for a table
                                blob in R serialization, which may hold row names nobody here can see

A pandas DataFrame carries the same in ``df.attrs["fstx"]`` (``fstx.to_pandas`` puts it there):
``{"table_attrs": blob, "nrow": n, "columns": [{"name": str, "r": dict or None, "attrs": blob}, ...]}``
with blobs as base64 text (bytes are taken too).
"""
from __future__ import annotations

import json
import os
import threading
import warnings
from concurrent.futures import ThreadPoolExecutor
from typing import Any, Dict, List, Optional

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.types as pt

from . import _core
from . import _fxa1 as fxa
from ._engine import LOCK as _ENGINE_LOCK  # one job at a time on the engine's worker pool
from ._file import metadata as _metadata
from ._meta import KEY_ATTRS, KEY_NROW, KEY_R, KEY_TABLE
from ._meta import arrow_tz as _arrow_tz
from ._meta import blob as _b
from ._meta import r_tzone as _r_tzone

_INT32, _FLOAT64, _LOGICAL, _STRING = 0, 1, 2, 3
_NONE, _FACTOR, _DATE, _POSIXCT = 0, 1, 2, 3
_PHYS = {"integer": _INT32, "double": _FLOAT64, "logical": _LOGICAL, "character": _STRING}
_PHYS_NAME = {v: k for k, v in _PHYS.items()}
_LT = {"none": _NONE, "factor": _FACTOR, "Date": _DATE, "POSIXct": _POSIXCT}
_PROFILES = {"fast": 0, "balanced": 1, "small": 2}
_CODECS = {"none": 0, "lz4": 1, "zstd": 2}
_BALANCED_LEVEL, _SMALL_LEVEL = 3, 9  # as R/fstx.R

_NA_INT = -(2**31)
_NA_INT64 = -(2**63)  # bit64's NA
_UNITS = {"s": 1.0, "ms": 1e3, "us": 1e6, "ns": 1e9}
_DATA_FRAME = {"class": fxa.character(["data.frame"])}


class _Col:
    __slots__ = ("name", "phys", "logical", "ordered", "levels", "tzone", "attrs", "data")

    def __init__(self, name, phys, data, logical=_NONE, ordered=False, levels=None, tzone=None, attrs=None):
        self.name = name
        self.phys = phys
        self.logical = logical
        self.ordered = ordered
        self.levels = levels
        self.tzone = tzone
        self.attrs = attrs
        self.data = data  # numbers: a _Num; strings: list of pyarrow arrays

    def r_class(self) -> Optional[List[str]]:
        """R's class vector for the column ([] = none), or None when an R-serialization blob hides it."""
        if self.logical == _FACTOR:
            return ["ordered", "factor"] if self.ordered else ["factor"]
        if self.logical == _DATE:
            return ["Date"]
        if self.logical == _POSIXCT:
            return ["POSIXct", "POSIXt"]
        return _blob_class(self.attrs)

    def describe(self) -> str:
        cl = self.r_class()
        return "/".join((cl or []) + [_PHYS_NAME[self.phys]])


def _blob_class(blob: Optional[bytes]) -> Optional[List[str]]:
    if blob is None:
        return []
    if not fxa.is_fxa1(blob):
        return None
    try:
        v = fxa.parse(blob).get("class")
    except ValueError:
        return None
    return [] if v is None else list(v.values)


def _warn(msg: str) -> None:
    warnings.warn("write_fstx: " + msg, UserWarning, stacklevel=4)


# ---------------------------------------------------------------- values -> what the engine takes
def _one(col) -> pa.Array:
    if isinstance(col, pa.Array):
        return col
    if col.num_chunks == 1:
        return col.chunk(0)
    if col.num_chunks == 0:
        return pa.array([], col.type)
    return pa.concat_arrays(col.chunks)


class _Num:
    """A number column as the engine's binding fills it, a window of rows at a time: the Arrow chunks as they are
    (int8, int16, int32, uint8, uint16, int64, bool, date32, float32 or float64; no joined copy of the column),
    missing values left as nulls (the binding writes R's NA in their place). ``add`` is added to every value
    (factor codes from 0-based indices); ``bits``: int64 values kept bit for bit in a double (integer64)."""
    __slots__ = ("chunks", "add", "bits")

    def __init__(self, col, add: int = 0, bits: bool = False):
        self.chunks = list(col.chunks) if isinstance(col, pa.ChunkedArray) else [col]
        self.add = add
        self.bits = bits


def _valid(arr) -> int:
    return len(arr) - arr.null_count


_DICT_SAMPLE = 65536  # rows after which the engine gives up its string dictionary at more than rows / 2 values


_POOL: Optional[ThreadPoolExecutor] = None
_POOL_LOCK = threading.Lock()


def _pool() -> ThreadPoolExecutor:
    global _POOL
    with _POOL_LOCK:
        if _POOL is None:
            _POOL = ThreadPoolExecutor(os.cpu_count() or 1, thread_name_prefix="fstx-write")
        return _POOL


def _text_chunks(col: pa.ChunkedArray, threads: int = 0) -> List[pa.Array]:
    """The chunks of a string column as the engine takes them. The engine's string dictionary keys on pointers
    (R's string cache gives equal strings one pointer), so a column that repeats its values goes as int32 codes
    into one dictionary shared by every chunk: equal strings then reach it as one pointer and are dictionary-coded
    as R codes them. A column of mostly distinct values (more than half of its first 65,536 rows, where the
    engine would give up) goes as it is. Long columns are coded in `threads` slices at once (0 = all cores)."""
    n = len(col)
    if n < 2 or col.null_count >= n - 1:
        return list(col.chunks)
    head = col.slice(0, _DICT_SAMPLE)
    if n > _DICT_SAMPLE and pc.count_distinct(head).as_py() * 2 > len(head):
        return list(col.chunks)
    parts = min(threads or os.cpu_count() or 1, n // _DICT_SAMPLE)
    if parts > 1:  # pyarrow codes on one thread: code slices in parallel, then give them one dictionary
        step = -(-n // parts)
        coded = pa.chunked_array([c for part in _pool().map(lambda s: s.dictionary_encode(),
                                                             [col.slice(i, step) for i in range(0, n, step)])
                                  for c in part.chunks], pa.dictionary(pa.int32(), col.type))
    else:
        coded = col.dictionary_encode()
    if len({c.dictionary.buffers()[1].address for c in coded.chunks}) > 1:
        coded = coded.unify_dictionaries()
    return list(coded.chunks)


# Arrow types the binding turns into int32 (with `add`) or float64 itself; anything else is cast first
_AS_INT32 = (pt.is_int8, pt.is_int16, pt.is_int32, pt.is_uint8, pt.is_uint16, pt.is_boolean, pt.is_date32)
_AS_DOUBLE = (pt.is_float64, pt.is_float32, pt.is_int32, pt.is_date32, pt.is_int64)


def _int32(arr, name: str, add: int = 0) -> _Num:
    """int32 with NA_integer_ for nulls. A value that is itself R's NA cannot be stored: the binding looks for one
    in int32 / date32 columns (on all threads, before the file is touched) and refuses the write."""
    if not any(f(arr.type) for f in _AS_INT32):
        arr = arr.cast(pa.int32())
    return _Num(arr, add)


def _double(arr) -> _Num:
    """float64 with NA_real_ for nulls; every other value keeps its bits (NaN stays NaN)."""
    if not any(f(arr.type) for f in _AS_DOUBLE):
        arr = arr.cast(pa.float64(), safe=False)
    return _Num(arr)


def _int64_bits(arr, name: str) -> _Num:
    """bit64 integer64: the int64 bit patterns in a float64 column, NA = INT64_MIN."""
    if arr.type != pa.int64():
        try:
            arr = arr.cast(pa.int64())
        except pa.ArrowInvalid:
            raise ValueError(f"write_fstx: column '{name}' holds values above 9223372036854775807, which "
                             "integer64 cannot store") from None
    if _valid(arr) and pc.min(arr).as_py() == _NA_INT64:
        raise ValueError(f"write_fstx: column '{name}' holds -9223372036854775808, which is integer64's "
                         "missing value and cannot be stored as a value")
    return _Num(arr, bits=True)


def _is_view(t) -> bool:
    f = getattr(pt, "is_string_view", None)
    return bool(f and f(t))


def _is_text(t) -> bool:
    return pt.is_string(t) or pt.is_large_string(t) or _is_view(t)


def _refuse(name: str, t, hint: str = "") -> TypeError:
    return TypeError(f"write_fstx: column '{name}' has type '{t}', which fstx cannot store" + hint)


# ---------------------------------------------------------------- one column
def _column(name: str, col: pa.ChunkedArray, r: Optional[dict], blob: Optional[bytes], int64: str,
            quiet: bool = False, base_levels: Optional[List[str]] = None, threads: int = 0) -> _Col:
    t = col.type
    cls = (r or {}).get("class") or []
    rphys = (r or {}).get("phys")
    rlog = (r or {}).get("logical")

    def natural(what: str) -> _Col:  # the metadata does not describe this data: write the data as it is
        if not quiet:
            _warn(f"column '{name}' is {t} but its fstx metadata describes {what}; the metadata is ignored")
        return _column(name, col, None, None, int64, True, base_levels, threads)

    def described() -> str:
        return "/".join(list(cls) + [str(rphys)]) if r is not None else "attributes only"

    if isinstance(t, pa.BaseExtensionType):
        raise _refuse(name, t, " (extension types are not supported; store the plain values)")

    if pt.is_dictionary(t):
        if not (_is_text(t.value_type) or pt.is_null(t.value_type)):  # a dictionary of numbers, dates, ...: written as its values
            return _column(name, col.cast(t.value_type), None, None, int64, True, None, threads)
        native = r is None or (rphys == "integer" and rlog == "factor") or (rphys == "character" and rlog == "none")
        in_blob = r is not None and rphys == "integer" and rlog == "none" and "factor" in cls
        if r is None and blob is not None:
            return natural("attributes only")
        if not (native or in_blob):
            return natural(described())
        u = col.unify_dictionaries() if col.num_chunks > 1 else col
        dic = u.chunk(0).dictionary if u.num_chunks else pa.array([], t.value_type)
        levels = dic.cast(pa.string()).to_pylist()
        if len(set(levels)) != len(levels):
            raise ValueError(f"write_fstx: column '{name}' has a dictionary with repeated values, which cannot "
                             "be factor levels")
        if any(lv is not None and "\x00" in lv for lv in levels):
            raise ValueError(f"write_fstx: column '{name}' has a level with a NUL character")
        codes = pa.chunked_array([c.indices for c in u.chunks], t.index_type)
        if base_levels is not None and None not in levels and native and levels != base_levels:
            if t.ordered:
                raise ValueError(f"write_fstx: ordered factor '{name}' must keep the file's levels")
            levels = base_levels + [lv for lv in levels if lv not in set(base_levels)]
            codes = pc.index_in(u.cast(pa.string()), value_set=pa.array(levels, pa.string()))
        codes = _int32(codes, name, add=1)  # R's codes count from 1
        if native and None not in levels:
            return _Col(name, _INT32, codes, _FACTOR, bool(t.ordered), levels, attrs=blob)
        # a class of its own or NA as a level: plain integer codes, levels and class in the blob, as R writes it
        attrs: Dict[str, fxa.Vec] = {}
        if blob is not None:
            if not fxa.is_fxa1(blob):
                _warn(f"column '{name}': its R-serialized attributes cannot be combined with an NA level and "
                      "are dropped")
            else:
                attrs = fxa.parse(blob)
        if in_blob and "class" not in attrs:
            return natural(described())
        attrs["levels"] = fxa.character(levels)
        if not in_blob:
            attrs["class"] = fxa.character(["ordered", "factor"] if t.ordered else ["factor"])
        return _Col(name, _INT32, codes, attrs=fxa.encode(attrs))

    if pt.is_date(t):
        native = r is None or (rlog == "Date" and rphys in ("double", "integer"))
        in_blob = r is not None and rlog == "none" and "Date" in cls and rphys in ("double", "integer")
        if r is None and blob is not None:
            return natural("attributes only")
        if not (native or in_blob):
            return natural(described())
        if pt.is_date32(t):  # days
            data, phys = (_int32(col, name), _INT32) if rphys == "integer" else (_double(col), _FLOAT64)
        else:  # date64: milliseconds; a time of day becomes a fraction of a day
            ms = col.cast(pa.int64()).cast(pa.float64(), safe=False)
            data, phys = _double(pc.divide(ms, pa.scalar(86400000.0))), _FLOAT64
        return _Col(name, phys, data, _DATE if native else _NONE, attrs=blob)

    if pt.is_timestamp(t):
        tz = t.tz or None
        if r is None and blob is not None:
            return natural("attributes only")
        # data that never came from R: naive times are shown as UTC; a fixed offset "+05:30" is stored in the
        # POSIX TZ form R reads, "<+0530>-05:30" (_meta.r_tzone)
        tzone: Optional[str] = _r_tzone(tz) or "UTC"
        logical, phys = _POSIXCT, _FLOAT64
        if r is not None:
            if rlog == "POSIXct" and rphys == "double":
                rtz = r.get("tzone")
            elif rlog == "none" and "POSIXct" in cls and rphys in ("double", "integer") and fxa.is_fxa1(blob):
                v = fxa.parse(blob).get("tzone")
                rtz = v.values[0] if v is not None and v.type == "character" and v.values else None
                logical = _NONE
            else:
                return natural(described())
            # (pandas turns a zero offset "+00:00" into UTC: the same clock, so the stored zone is kept)
            zero = tz == "UTC" and _arrow_tz(rtz) in ("+00:00", "-00:00")
            if not ((tz and (rtz in (tz, _r_tzone(tz)) or zero)) or (not tz and rtz in (None, ""))):
                return natural(f"time zone {rtz!r}")
            tzone = rtz if logical == _POSIXCT else None
        secs = col.cast(pa.int64()).cast(pa.float64(), safe=False)
        if t.unit != "s":
            secs = pc.divide(secs, pa.scalar(_UNITS[t.unit]))
        if rphys == "integer" and _valid(secs):  # R held whole seconds in an integer vector: keep that if it fits
            mm = pc.min_max(secs).as_py()
            if pc.all(pc.equal(secs, pc.floor(secs))).as_py() and mm["min"] > _NA_INT and mm["max"] < 2**31:
                return _Col(name, _INT32, _int32(secs.cast(pa.int32()), name), attrs=blob)
        elif rphys == "integer":
            return _Col(name, _INT32, _int32(secs.cast(pa.int32()), name), attrs=blob)
        return _Col(name, phys, _double(secs), logical, tzone=tzone, attrs=blob)

    if pt.is_time(t) or pt.is_duration(t):
        if r is not None or blob is not None:
            return natural(described())
        secs = col.cast(pa.int32() if pt.is_time32(t) else pa.int64()).cast(pa.float64(), safe=False)
        if t.unit != "s":
            secs = pc.divide(secs, pa.scalar(_UNITS[t.unit]))
        if pt.is_time(t):  # as hms::hms()
            attrs = {"units": fxa.character(["secs"]), "class": fxa.character(["hms", "difftime"])}
        else:
            attrs = {"class": fxa.character(["difftime"]), "units": fxa.character(["secs"])}
        return _Col(name, _FLOAT64, _double(secs), attrs=fxa.encode(attrs))

    if pt.is_boolean(t):
        if r is not None and not (rphys == "logical" and rlog == "none"):
            return natural(described())
        return _Col(name, _LOGICAL, _int32(col, name), attrs=blob)

    if pt.is_integer(t):
        if t.bit_width < 32 or t == pa.int32():
            if r is not None and not (rphys == "integer" and rlog in ("none", "Date")):
                return natural(described())
            return _Col(name, _INT32, _int32(col, name), _DATE if rlog == "Date" else _NONE, attrs=blob)
        is64 = r is not None and rphys == "double" and rlog == "none" and "integer64" in cls
        if (r is not None or blob is not None) and not is64:
            return natural(described())
        arr = col
        mm = pc.min_max(arr).as_py() if _valid(arr) else {"min": 0, "max": 0}
        fits = mm["min"] > _NA_INT and mm["max"] < 2**31
        if is64 or int64 == "integer64" or (int64 == "auto" and not fits):
            return _Col(name, _FLOAT64, _int64_bits(arr, name),
                        attrs=blob if is64 else fxa.encode({"class": fxa.character(["integer64"])}))
        if int64 == "auto":
            # every value fits: int64 is narrowed by the binding, the rest cast here
            return _Col(name, _INT32, _Num(arr) if pt.is_int64(t) else _int32(arr, name))
        if mm["min"] < -(2**53) or mm["max"] > 2**53:
            raise ValueError(f"write_fstx: column '{name}' holds whole numbers past 2^53, which a double cannot "
                             "store exactly (use int64='integer64' or 'auto')")
        return _Col(name, _FLOAT64, _double(arr))

    if pt.is_floating(t):
        if r is not None and not (rphys == "double" and "integer64" not in cls and rlog in ("none", "Date", "POSIXct")):
            return natural(described())
        try:
            data = _double(col)
        except pa.ArrowNotImplementedError:
            raise _refuse(name, t, " (cast it to float64 first)") from None
        return _Col(name, _FLOAT64, data, _LT.get(rlog, _NONE),
                    tzone=r.get("tzone") if rlog == "POSIXct" else None, attrs=blob)

    if _is_text(t):
        if r is not None and not (rphys == "character" and rlog == "none"):
            return natural(described())
        if _is_view(t):
            col = col.cast(pa.large_string())
        return _Col(name, _STRING, _text_chunks(col, threads), attrs=blob)

    if pt.is_binary(t) or pt.is_large_binary(t):
        # fstx has no binary type; the one binary column it takes is text it handed out itself as bytes
        # (read_fstx(invalid_utf8="binary")), which goes back as the same bytes
        if r is not None and rphys == "character" and rlog == "none":
            return _Col(name, _STRING, _text_chunks(col, threads), attrs=blob)
        raise _refuse(name, t, " (fstx 1.0 has no binary type)")

    if pt.is_null(t) and r is not None:
        # nothing but missing values, so the data has no type of its own (pandas hands over an all-None date
        # column like this): the metadata says what the column is
        if rlog == "factor":
            as_type = pa.dictionary(pa.int32(), pa.string(), False)
        elif rlog == "Date":
            as_type = pa.date32()
        elif rlog == "POSIXct":
            as_type = pa.timestamp("us", _arrow_tz(r.get("tzone")) or None)
        else:
            as_type = {"integer": pa.int32(), "double": pa.float64(), "logical": pa.bool_(),
                       "character": pa.string()}[rphys]
        typed = pa.chunked_array([pa.nulls(len(col), as_type)])
        return _column(name, typed, r, blob, int64, quiet, base_levels, threads)
    if pt.is_null(t):  # an all-null column: logical NA, R's default
        return _Col(name, _LOGICAL, _int32(pa.nulls(len(col), pa.int32()), name))

    if pt.is_decimal(t):
        raise _refuse(name, t, " (cast it to float64 or string first)")
    raise _refuse(name, t)


# ---------------------------------------------------------------- table attributes
def _still_sorted(table: pa.Table, keys: List[Any]) -> bool:
    """Whether the rows are still in data.table key order (ascending, missing values first)."""
    names = table.schema.names
    cols = []
    for k in keys:
        if not isinstance(k, str) or names.count(k) != 1:
            return False
        c = table.column(names.index(k))
        t = c.type
        if pt.is_dictionary(t):  # a factor sorts by its codes
            c = _one(c.unify_dictionaries() if c.num_chunks > 1 else c).indices
        elif pt.is_floating(t):
            if pc.any(pc.is_nan(c)).as_py():  # data.table and Arrow place NaN differently
                return False
        elif not (pt.is_integer(t) or pt.is_boolean(t) or pt.is_string(t) or pt.is_large_string(t)
                  or pt.is_date(t) or pt.is_timestamp(t)):
            return False
        cols.append(c)
    if table.num_rows < 2 or not cols:
        return True
    tab = pa.table(cols, names=[str(i) for i in range(len(cols))])
    try:
        try:  # pyarrow 25 and later: where nulls go is said per key
            idx = pc.sort_indices(tab, sort_keys=[(str(i), "ascending", "at_start") for i in range(len(cols))])
        except (TypeError, ValueError):
            idx = pc.sort_indices(tab, sort_keys=[(str(i), "ascending") for i in range(len(cols))],
                                  null_placement="at_start")
    except (pa.ArrowNotImplementedError, pa.ArrowInvalid):
        return False
    # the sort is stable: the rows are in order exactly when it leaves every row where it is
    return bool(pc.all(pc.greater(idx.slice(1), idx.slice(0, len(idx) - 1))).as_py())


_NO_INDEX = object()  # "the data has no pandas index": the blob's own row names are looked at


def _has_row_names(v: Optional[fxa.Vec]) -> bool:
    """Stored row names (not R's compact c(NA, n) form of automatic ones)."""
    if v is None:
        return False
    return not (v.type == "integer" and len(v.values) == 2 and v.values[0] is None)


def _table_blob(table: pa.Table, row_names) -> Optional[bytes]:
    """The table's attribute blob: the one in the metadata when it still fits, re-encoded when something had to go."""
    meta = table.schema.metadata or {}
    try:
        blob = _b(meta.get(KEY_TABLE))
    except ValueError:
        raise ValueError("write_fstx: the table's fstx.table_attrs metadata is damaged (not base64)") from None
    n = table.num_rows
    if blob is not None and not fxa.is_fxa1(blob):
        # R serialization is never decoded: it goes back as it is unless it could hold row names for another
        # row count
        try:
            same_rows = int(meta[KEY_NROW]) == n
        except (KeyError, ValueError):
            same_rows = False
        if same_rows or b"row.names" not in blob:
            if row_names is not _NO_INDEX and row_names is not None:
                _warn("the index is not stored as row names: the table attributes are in R serialization")
            return blob
        _warn("the table attributes are in R serialization and may hold row names for another row count; "
              "they are dropped")
        blob = None
    if blob is None:
        attrs = dict(_DATA_FRAME)
        changed = True
    else:
        try:
            attrs = fxa.parse(blob)
        except ValueError as e:
            raise ValueError(f"write_fstx: the table's fstx.table_attrs metadata is damaged ({e})") from None
        cl = attrs.get("class")
        if cl is None or cl.type != "character" or "data.frame" not in cl.values or \
                any(k in attrs for k in ("names", "dim", "dimnames")):
            raise ValueError("write_fstx: the table's fstx.table_attrs metadata does not describe a data frame")
        changed = False
        rn = attrs.get("row.names")
        if rn is not None and row_names is not _NO_INDEX and row_names is not None:
            if rn != row_names:  # the pandas index decides; same place in the list, so unchanged names keep the bytes
                attrs["row.names"] = row_names
                changed = True
            row_names = None
        elif rn is not None and not (_has_row_names(rn) and rn.type in ("integer", "character")
                                     and len(rn.values) == n and row_names is _NO_INDEX):
            del attrs["row.names"]  # rows were added or dropped, or the pandas index says there are none
            changed = True
        key = attrs.get("sorted")
        if key is not None and not (key.type == "character" and _still_sorted(table, key.values)):
            del attrs["sorted"]
            changed = True
        if "index" in attrs:  # data.table's secondary index: nobody can tell whether rows moved
            del attrs["index"]
            changed = True
    if row_names is not _NO_INDEX and row_names is not None:
        attrs["row.names"] = row_names
        changed = True
    return fxa.encode(attrs) if changed else bytes(blob)


# ---------------------------------------------------------------- what write_fstx accepts
def _no_columns(n: int, smeta: Optional[dict]) -> pa.Table:
    """A table of n rows and no columns (an empty struct array keeps the row count)."""
    batch = pa.RecordBatch.from_struct_array(pa.array([{}] * n, pa.struct([])))
    return pa.Table.from_batches([batch.replace_schema_metadata(smeta or None)])


def _from_pandas(df):
    """A pandas DataFrame as a table with the fstx metadata of ``df.attrs["fstx"]``, and its index as row names."""
    import pandas as pd

    n = len(df)
    idx = df.index
    row_names = None
    plain = isinstance(idx, pd.RangeIndex) and idx.start == 0 and idx.step == 1
    if n == 0:  # an index without rows says nothing: row names the table attributes hold stay
        row_names = _NO_INDEX
    elif not plain:
        vals = None if isinstance(idx, pd.MultiIndex) else idx.tolist()
        if vals is not None and all(isinstance(v, str) for v in vals) and len(set(vals)) == n:
            row_names = fxa.character(vals)
        elif vals is not None and all(isinstance(v, int) and not isinstance(v, bool) and _NA_INT < v < 2**31
                                      for v in vals) and len(set(vals)) == n:
            row_names = None if vals == list(range(n)) else fxa.integer(vals)
        else:
            raise TypeError("write_fstx: the DataFrame's index cannot be stored as R row names (they are distinct "
                            "strings or distinct whole numbers); call reset_index() first, with drop=True to "
                            "leave the index out")
    saved = df.attrs.get("fstx") if isinstance(df.attrs, dict) else None
    saved = saved if isinstance(saved, dict) else {}
    cmeta = saved.get("columns") if isinstance(saved.get("columns"), list) else []
    by_name: Dict[str, List[dict]] = {}
    for c in cmeta:
        if isinstance(c, dict):
            by_name.setdefault(c.get("name"), []).append(c)
    names = [c if isinstance(c, str) else str(c) for c in df.columns]
    arrays, fields = [], []
    for i, nm in enumerate(names):
        a = pa.Array.from_pandas(df.iloc[:, i])
        a = a if isinstance(a, pa.ChunkedArray) else pa.chunked_array([a])
        c = cmeta[i] if i < len(cmeta) and isinstance(cmeta[i], dict) and cmeta[i].get("name") == nm else None
        if c is None and len(by_name.get(nm, [])) == 1 and names.count(nm) == 1:
            c = by_name[nm][0]  # columns were dropped or moved
        md = {}
        if c is not None and isinstance(c.get("r"), dict):
            md[KEY_R] = json.dumps(c["r"]).encode()
        if c is not None and c.get("attrs") is not None:
            md[KEY_ATTRS] = _b(c["attrs"])
        arrays.append(a)
        fields.append(pa.field(nm, a.type, metadata=md or None))
    smeta = {}
    if saved.get("table_attrs") is not None:
        smeta[KEY_TABLE] = _b(saved["table_attrs"])
    if saved.get("nrow") is not None:
        smeta[KEY_NROW] = str(saved["nrow"]).encode()
    schema = pa.schema(fields, metadata=smeta or None)
    if not arrays:  # no columns: keep the row count
        return _no_columns(n, smeta), row_names
    return pa.Table.from_arrays(arrays, schema=schema), row_names


def _to_table(data):
    if isinstance(data, pa.Table):
        return data, _NO_INDEX
    if isinstance(data, pa.RecordBatch):
        return pa.Table.from_batches([data]), _NO_INDEX
    if isinstance(data, pa.RecordBatchReader):
        return data.read_all(), _NO_INDEX
    mod = type(data).__module__.split(".")[0]
    kind = type(data).__name__
    if mod == "pandas" and kind == "DataFrame":
        return _from_pandas(data)
    if mod == "polars":
        if kind != "DataFrame":
            raise TypeError(f"write_fstx: cannot write a polars {kind}; collect() it into a DataFrame first")
        return data.to_arrow(), _NO_INDEX
    if hasattr(data, "__arrow_c_stream__"):
        return pa.table(data), _NO_INDEX
    if hasattr(data, "__arrow_c_array__"):
        return pa.Table.from_batches([pa.record_batch(data)]), _NO_INDEX
    raise TypeError("write_fstx: 'data' must be a pyarrow.Table, RecordBatch or RecordBatchReader, a pandas or "
                    f"polars DataFrame, or an object with __arrow_c_stream__, not {type(data).__name__}")


def _field_meta(field: pa.Field):
    md = field.metadata or {}
    blob = _b(md.get(KEY_ATTRS))
    r = None
    if KEY_R in md:
        try:
            r = json.loads(md[KEY_R])
        except ValueError:
            r = None
        ok = isinstance(r, dict) and r.get("phys") in _PHYS and r.get("logical") in _LT and \
            (r.get("class") is None or (isinstance(r["class"], list) and all(isinstance(c, str) for c in r["class"]))) and \
            (r.get("tzone") is None or isinstance(r["tzone"], str))
        if not ok:
            _warn(f"column '{field.name}': its fstx.r metadata cannot be read and is ignored")
            return None, None
    return r, blob


# ---------------------------------------------------------------- append
def _append_plan(path: str, table: pa.Table, row_names) -> dict:
    """R's .append_prepare(): what the file holds, checked against the new rows."""
    m = _metadata(path)  # opened and closed again: the writer needs the file to itself
    tab = m.table_attrs
    if tab is not None and fxa.is_fxa1(tab):
        try:
            ta = fxa.parse(tab)
        except ValueError as e:
            raise ValueError(f"write_fstx: '{path}' is corrupt (table attributes: {e})") from None
        rn = ta.get("row.names")
        if _has_row_names(rn) and rn.type == "character":
            raise ValueError("write_fstx: cannot append to a file with stored row names")
        drop = ["row.names"] + (["sorted", "index"] if table.num_rows > 0 else [])
        if any(k in ta for k in drop):  # integer row names are positions; new rows can break a key or index
            for k in drop:
                ta.pop(k, None)
            tab = fxa.encode(ta)
    elif tab is not None and any(k in tab for k in (b"row.names", b"sorted", b"index")):
        raise ValueError("write_fstx: cannot append to this file from Python: its table attributes are in R "
                         "serialization and may hold row names, a key or an index that the new rows would break")
    new_rn = row_names
    if new_rn is _NO_INDEX:
        try:
            nb = _b((table.schema.metadata or {}).get(KEY_TABLE))
            new_rn = fxa.parse(nb).get("row.names") if fxa.is_fxa1(nb) else None
        except ValueError:
            new_rn = None
        if new_rn is not None and len(new_rn.values) != table.num_rows:
            new_rn = None
    if new_rn is not None and new_rn.type == "character":
        raise ValueError("write_fstx: cannot append rows with row names (for pandas: reset_index(drop=True))")
    if table.schema.names != m.names:
        raise ValueError(f"write_fstx: the new rows have columns ({', '.join(table.schema.names)}) but the file "
                         f"has ({', '.join(m.names)})")
    return {"meta": m, "table_attrs": tab}


def _file_hint(c) -> tuple:
    """A file column as the metadata the reader would have put on it."""
    cl = {"factor": ["ordered", "factor"] if c.ordered else ["factor"], "Date": ["Date"],
          "POSIXct": ["POSIXct", "POSIXt"]}.get(c.logical)
    if cl is None:
        cl = _blob_class(c.attrs) or []
    r = {"phys": c.phys, "logical": c.logical, "class": cl}
    if c.logical == "POSIXct" and c.tzone is not None:
        r["tzone"] = c.tzone
    return r, c.attrs


def _check_append(col: _Col, c) -> None:
    """The new rows' column against the file's (R: same type and class, same time zone)."""
    was = _Col(c.name, _PHYS[c.phys], None, _LT[c.logical], c.ordered, c.levels, c.tzone, c.attrs)
    a, b = was.r_class(), col.r_class()
    same = was.phys == col.phys and was.logical == col.logical and was.ordered == col.ordered and \
        (a == b if a is not None and b is not None else was.attrs == col.attrs)
    if not same:
        raise ValueError(f"write_fstx: column '{c.name}' is {was.describe()} in the file but {col.describe()} in "
                         "the new rows")
    if col.logical == _POSIXCT and was.tzone != col.tzone:
        raise ValueError(f"write_fstx: POSIXct column '{c.name}' has time zone {was.tzone!r} in the file but "
                         f"{col.tzone!r} in the new rows")
    if col.logical == _NONE and fxa.is_fxa1(was.attrs) and fxa.is_fxa1(col.attrs):
        la, lb = fxa.parse(was.attrs).get("levels"), fxa.parse(col.attrs).get("levels")
        if la != lb:
            raise ValueError(f"write_fstx: column '{c.name}' (levels kept as an attribute) must keep the file's levels")


# ---------------------------------------------------------------- write_fstx
def write_fstx(data, path, *, profile: Optional[str] = None, compress: Optional[str] = None,
               level: Optional[int] = None, threads: Optional[int] = None, append: bool = False,
               int64: str = "auto") -> None:
    """Write a table to an fstx file.

    data      a pyarrow.Table, RecordBatch or RecordBatchReader, a pandas or polars DataFrame, or any object
              with ``__arrow_c_stream__``.
    profile   "balanced" (the default: Zstandard level 3, kept on a page only where it saves 10%), "fast" (no
              compression) or "small" (Zstandard level 9). An append uses the file's profile by default.
    compress  "zstd", "lz4" or "none" on every page instead of the profile's choice.
    level     the Zstandard level.
    threads   worker threads; None = all cores.
    append    True adds the rows to an existing file (same columns, types and classes; factor levels may grow
              at the end; same time zone; no row names). A file that does not exist is simply written.
    int64     how 64-bit integers (and uint32 / uint64) are stored: "auto" = R integer when every value fits
              int32, else bit64 integer64; "integer64" = always integer64; "double" = double (refused past 2^53).

    Types fstx 1.0 cannot store (binary, decimal, list, struct, map, extension types) raise TypeError naming
    the column; values it cannot store (INT32_MIN in an integer column, a NUL inside a string) raise ValueError.
    """
    if profile is not None and profile not in _PROFILES:
        raise ValueError("write_fstx: 'profile' should be one of \"balanced\", \"fast\", \"small\"")
    if compress is not None and compress not in _CODECS:
        raise ValueError("write_fstx: 'compress' should be one of \"zstd\", \"lz4\", \"none\"")
    if int64 not in ("auto", "integer64", "double"):
        raise ValueError("write_fstx: 'int64' should be one of \"auto\", \"integer64\", \"double\"")
    if level is not None and (isinstance(level, bool) or not isinstance(level, int)):
        raise ValueError("write_fstx: 'level' must be a single whole number")
    if threads is not None and (isinstance(threads, bool) or not isinstance(threads, int) or threads < 1):
        raise ValueError("write_fstx: 'threads' must be a single whole number >= 1")
    if not isinstance(append, bool):
        raise ValueError("write_fstx: 'append' must be True or False")
    p = os.fspath(path)
    if isinstance(p, bytes):
        p = os.fsdecode(p)
    if not p:
        raise ValueError("write_fstx: 'path' must be a single non-empty string")
    p = os.path.abspath(os.path.expanduser(p))

    table, row_names = _to_table(data)
    if not append:
        _write(p, table, row_names, profile, compress, level, threads, int64, False)
        return
    # an append plans on what the file holds now (columns, factor levels, profile) and must write before anyone
    # else changes the file: planning and writing are one step under the engine lock (re-entrant)
    with _ENGINE_LOCK:
        _write(p, table, row_names, profile, compress, level, threads, int64, os.path.exists(p))


def _write(p: str, table: pa.Table, row_names, profile, compress, level, threads, int64: str, append: bool) -> None:
    plan = None
    if append:
        plan = _append_plan(p, table, row_names)
        if profile is None:  # a profile from a newer fstx: encode as balanced; the file keeps its stored value
            profile = plan["meta"].profile if plan["meta"].profile in _PROFILES else "balanced"
    profile = profile or "balanced"
    keep_pct = 0
    if compress is None:
        compress = "none" if profile == "fast" else "zstd"
        keep_pct = 10 if profile == "balanced" else 0
    if level is None:
        level = _SMALL_LEVEL if profile == "small" else _BALANCED_LEVEL

    cols: List[_Col] = []
    for i, field in enumerate(table.schema):
        if not isinstance(field.name, str):
            raise TypeError("write_fstx: column names must be strings")
        r, blob = _field_meta(field)
        quiet, base = False, None
        if plan is not None:
            fc = plan["meta"].columns[i]
            if r is None and blob is None:  # plain data takes the file's own description of the column
                r, blob = _file_hint(fc)
                quiet = True
            if fc.logical == "factor":
                base = list(fc.levels)
        col = _column(field.name, table.column(i), r, blob, int64, quiet, base, threads or 0)
        if plan is not None:
            _check_append(col, plan["meta"].columns[i])
        cols.append(col)

    table_attrs = plan["table_attrs"] if plan is not None else _table_blob(table, row_names)
    spec = []
    for c in cols:
        parts, add, bits = (c.data.chunks, c.data.add, c.data.bits) if isinstance(c.data, _Num) else (c.data, 0, False)
        spec.append((c.name, c.phys, c.logical, c.ordered, c.levels, c.tzone, c.attrs,
                     [a.__arrow_c_array__() for a in parts], add, bits))
    with _ENGINE_LOCK:
        _core.write(p, spec, table_attrs, table.num_rows, _CODECS[compress], level, threads or 0,
                    _PROFILES[profile], keep_pct, plan is not None)
