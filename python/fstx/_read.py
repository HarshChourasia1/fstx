"""Reading fstx files into Arrow (DESIGN.md sections 2 to 6).

The engine's Arrow C stream (src/r_arrow.cpp) decodes the pages; pyarrow imports it and pulls the batches with
the GIL released. This module chooses columns and rows, runs filters, and finishes each batch where the stream
cannot know R's meaning: integer64 (the bits of a double column), factors, Dates and POSIXct that R stored as
plain numbers with their class in the attribute blob, dictionary-coded strings (always returned as plain
strings), the UTF-8 check, and the metadata that lets a writer store the table again the way R wrote it.

Arrow metadata (DESIGN.md section 4):

- field ``fstx.r``: JSON ``{"phys", "logical", "class"?, "tzone"?}``, on every column that is not a plain
  logical / integer / double / character vector without attributes. ``tzone`` is there only for a POSIXct
  column stored natively with a time zone (``""`` = stored empty; key absent = no tzone attribute).
- field ``fstx.attrs``: the column's attribute blob, raw (FXA1 or R serialization).
- schema ``fstx.table_attrs``: the table's attribute blob, raw.
- schema ``fstx.row_names``: JSON list, only when the read is not the whole file in file order and the file
  stores row names: the row names of the rows read, in the order read. It replaces ``row.names`` of the blob.
"""
from __future__ import annotations

import json
import warnings
from typing import Any, Dict, Iterator, List, Optional, Sequence

import pyarrow as pa
import pyarrow.compute as pc

from . import _core, _filters
from ._file import ColumnMeta, File, FileMeta, PathLike

_INT64_MIN = -(2**63)
_MAX_STRING = (2**31 - 1) // 8  # the stream's rule: text that could pass 2 GB in a batch gets 64-bit offsets
_OPTIONS = {"verify_nulls": False}


class FstxWarning(UserWarning):
    """A read that had to change data to fit Arrow (strings that are not valid UTF-8, ...)."""


def set_options(*, verify_nulls: Optional[bool] = None) -> dict:
    """Package options; returns them. ``verify_nulls=True`` (R: ``options(fstx.verify_nulls = TRUE)``) counts
    the missing values of every decoded page against the page's stored count and refuses a file that differs."""
    if verify_nulls is not None:
        _OPTIONS["verify_nulls"] = bool(verify_nulls)
    return dict(_OPTIONS)


def _error(message: str, code: Optional[int] = None, column: Optional[int] = None) -> Exception:
    return _core.FstxError(message, code, column)


# ---------------------------------------------------------------- what each column becomes
class _Col:
    """One file column in a read: ``kind`` says how the stream's array becomes the Arrow column ``field``."""

    __slots__ = ("meta", "kind", "field", "levels", "ordered")

    def __init__(self, meta: ColumnMeta, kind: str, typ: pa.DataType, md: Dict[str, bytes], levels=None, ordered=False):
        self.meta = meta
        self.kind = kind
        self.field = pa.field(meta.name, typ, nullable=True, metadata=md or None)
        self.levels = levels
        self.ordered = ordered


def _blob_tz(a) -> Optional[str]:
    t = a.get("tzone") if a else None
    if t is not None and t.type == "character" and t.values and t.values[0]:
        return t.values[0]
    return None


def _column(c: ColumnMeta, datetimes: str, binary: bool, attributes: bool) -> _Col:
    rc = c.r_class  # raises FstxError when the blob is corrupt
    a = c.attributes
    r: Dict[str, Any] = {"phys": c.phys, "logical": c.logical}
    if rc:
        r["class"] = rc
    if c.logical == "POSIXct" and c.tzone is not None:
        r["tzone"] = c.tzone
    md: Dict[str, bytes] = {}
    if c.logical != "none" or c.attrs is not None:
        md["fstx.r"] = json.dumps(r, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if c.attrs is not None and (attributes or a is not None):  # attributes=False drops R serialization only
        md["fstx.attrs"] = c.attrs
    arrow = datetimes == "arrow"
    stored = pa.float64() if c.phys == "double" else pa.int32()
    rc = rc or []
    if c.phys == "character":
        big = c.raw_bytes > _MAX_STRING
        if binary:
            md.setdefault("fstx.r", json.dumps(r, separators=(",", ":")).encode("utf-8"))
            return _Col(c, "string", pa.large_binary() if big else pa.binary(), md)
        return _Col(c, "string", pa.large_string() if big else pa.string(), md)
    if c.phys == "logical":
        return _Col(c, "same", pa.bool_(), md)
    if c.logical == "factor":
        return _Col(c, "same", pa.dictionary(pa.int32(), pa.string(), bool(c.ordered)), md)
    if c.logical == "Date":
        return _Col(c, "same", pa.date32() if arrow else stored, md)
    if c.logical == "POSIXct":
        return _Col(c, "same", pa.timestamp("us", c.tzone or None) if arrow else stored, md)
    # classes R keeps in the attribute blob
    if c.phys == "double" and "integer64" in rc:
        return _Col(c, "int64", pa.int64(), md)
    if c.phys == "integer" and "factor" in rc and a and "levels" in a and a["levels"].type == "character":
        ordered = "ordered" in rc
        return _Col(c, "factor", pa.dictionary(pa.int32(), pa.string(), ordered), md, a["levels"].values, ordered)
    if "Date" in rc and arrow:
        return _Col(c, "date", pa.date32(), md)
    if "POSIXct" in rc and arrow:
        return _Col(c, "ts", pa.timestamp("us", _blob_tz(a)), md)
    return _Col(c, "same", stored, md)


def _resolve_columns(meta: FileMeta, columns) -> List[int]:
    n = meta.ncol
    if columns is None:
        return list(range(n))
    if isinstance(columns, (str, int)):
        columns = [columns]
    names = meta.names
    out = []
    for c in columns:
        if isinstance(c, str):
            if c not in names:
                raise KeyError("column '%s' is not in the file" % c)
            out.append(names.index(c))  # first match, as R
        elif isinstance(c, bool) or not hasattr(c, "__index__"):
            raise TypeError("'columns' must be None, column names or 0-based column positions")
        else:
            i = c.__index__()
            if not -n <= i < n:
                raise IndexError("column position %d is outside 0 to %d" % (i, n - 1))
            out.append(i % n)
    return out


class _Plan:
    def __init__(self, meta: FileMeta, columns, datetimes: str, invalid_utf8: str, attributes: bool, binary: bool):
        if datetimes not in ("arrow", "float"):
            raise ValueError("'datetimes' must be \"arrow\" or \"float\"")
        if invalid_utf8 not in ("replace", "error", "binary"):
            raise ValueError("'invalid_utf8' must be \"replace\", \"error\" or \"binary\"")
        self.meta = meta
        self.index = _resolve_columns(meta, columns)
        self.cols = [_column(meta.columns[i], datetimes, binary, attributes) for i in self.index]
        self.raw = datetimes == "float"
        self.invalid_utf8 = invalid_utf8
        ta = meta.attributes  # checks the table blob (raises FstxError when corrupt)
        md = {}
        if meta.table_attrs is not None and (attributes or ta is not None):
            md["fstx.table_attrs"] = meta.table_attrs
        self.schema = pa.schema([c.field for c in self.cols], metadata=md or None)


def schema_of(meta: FileMeta) -> pa.Schema:
    return _Plan(meta, None, "arrow", "replace", True, False).schema


# ---------------------------------------------------------------- rows
class _Rows:
    """The rows of a read: the range [start, stop), or ``sel`` (uint64 bytes, ascending) with ``take`` to put
    them back in the order asked for."""

    def __init__(self, start=0, stop=0, sel: Optional[bytes] = None, take=None, n: Optional[int] = None):
        self.start, self.stop, self.sel, self.take = start, stop, sel, take
        self.n = (stop - start if sel is None else len(sel) // 8) if n is None else n

    def indices(self) -> List[int]:
        if self.sel is None:
            return list(range(self.start, self.stop))
        rows = pa.Array.from_buffers(pa.uint64(), len(self.sel) // 8, [None, pa.py_buffer(self.sel)])
        return (rows.take(self.take) if self.take is not None else rows).to_pylist()


def _resolve_rows(nrow: int, rows) -> _Rows:
    if rows is None:
        return _Rows(0, nrow)
    if isinstance(rows, slice):
        rows = range(*rows.indices(nrow))  # Python's slice rules: negative positions, clamped ends
    elif isinstance(rows, range) and len(rows) and not (0 <= rows[0] < nrow and 0 <= rows[-1] < nrow):
        raise IndexError("'rows' must be between 0 and %d" % (nrow - 1))
    if isinstance(rows, range):
        if not len(rows):
            return _Rows(0, 0)
        if rows.step == 1:
            return _Rows(rows.start, rows.stop)
    try:
        a = rows if isinstance(rows, (pa.Array, pa.ChunkedArray)) else pa.array(rows)
    except (pa.ArrowException, TypeError, ValueError):
        raise TypeError("'rows' must be None, a slice, a range or a sequence of 0-based row numbers") from None
    if isinstance(a, pa.ChunkedArray):
        a = a.combine_chunks()
    if len(a) == 0:
        return _Rows(0, 0)
    if not pa.types.is_integer(a.type) or a.null_count:
        raise TypeError("'rows' must be whole row numbers (0-based), without missing values")
    mm = pc.min_max(a).as_py()
    if mm["min"] < 0 or mm["max"] >= nrow:
        raise IndexError("'rows' must be between 0 and %d" % (nrow - 1))
    a = a.cast(pa.int64())
    n = len(a)
    step = pc.subtract(a.slice(1), a.slice(0, n - 1))
    if n == 1 or pc.all(pc.equal(step, 1)).as_py():
        return _Rows(mm["min"], mm["max"] + 1)  # one ascending run: the range read
    take = None
    if not pc.all(pc.greater_equal(step, 0)).as_py():  # the stream wants ascending rows: read those, then reorder
        u = pc.unique(a)
        u = u.take(pc.sort_indices(u))
        take = pc.index_in(a, value_set=u)
        a = u
    a = a.cast(pa.uint64())
    return _Rows(sel=a.buffers()[1].slice(a.offset * 8, len(a) * 8).to_pybytes(), take=take,
                 n=n)


def _threads(threads) -> int:
    if threads is None:
        return 0
    if isinstance(threads, bool) or not isinstance(threads, int) or threads < 1:
        raise ValueError("'threads' must be None (all cores) or a whole number >= 1")
    return threads


def _verify(verify) -> int:
    return (_core.VERIFY_HASH if verify else 0) | (_core.VERIFY_NULLS if _OPTIONS["verify_nulls"] else 0)


def _select(f: File, rows, filters, threads: int, verify: int) -> _Rows:
    """rows and filters into the rows to read."""
    meta = f.metadata
    if filters is None:
        return _resolve_rows(meta.nrow, rows)
    if rows is not None and not isinstance(rows, (slice, range)):
        raise ValueError("use either a list of 'rows' or 'filters', not both (a slice of rows may go with filters)")
    w = _resolve_rows(meta.nrow, rows)
    if w.sel is not None:
        raise ValueError("with 'filters', 'rows' must be a slice or range with step 1")
    preds = _filters.to_preds(meta, filters)
    sel, _ = _core.filter(f._f, preds, w.start, w.stop, threads, verify, False)
    return _Rows(sel=sel)


def _make_unique(names: list) -> List[str]:
    """R's make.unique(): repeats get .1, .2, ... (skipping names that exist)."""
    names = [str(s) for s in names]
    taken, seen, count, out = set(names), set(), {}, []
    for s in names:
        if s in seen:
            k = count.get(s, 0)
            while True:
                k += 1
                c = "%s.%d" % (s, k)
                if c not in taken and c not in seen:
                    break
            count[s] = k
            s = c
        seen.add(s)
        out.append(s)
    return out


def _with_row_names(plan: _Plan, sel: _Rows) -> pa.Schema:
    """The schema for this read: a partial or reordered read carries its own row names (module docstring)."""
    meta = plan.meta
    if sel.sel is None and sel.start == 0 and sel.stop == meta.nrow:
        return plan.schema
    rn = meta.row_names
    if rn is None:
        return plan.schema
    rn = [rn[i] for i in sel.indices()]
    if len(set(rn)) != len(rn):
        rn = _make_unique(rn)  # as x[rows, ] does in R
    md = dict(plan.schema.metadata or {})
    md[b"fstx.row_names"] = json.dumps(rn, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return plan.schema.with_metadata(md)


def row_names(schema: pa.Schema) -> Optional[list]:
    """The row names a table read by fstx carries (str or int, one per row as read), or None."""
    md = schema.metadata or {}
    if b"fstx.row_names" in md:
        return json.loads(md[b"fstx.row_names"].decode("utf-8"))
    blob = md.get(b"fstx.table_attrs")
    if blob is None:
        return None
    from . import _fxa1

    if _fxa1.kind(blob) != "FXA1":
        return None
    rn = _fxa1.decode(blob).get("row.names")
    if rn is None or (rn.type == "integer" and len(rn.values) == 2 and rn.values[0] is None):
        return None
    return list(rn.values)


# ---------------------------------------------------------------- finishing a batch
class _State:
    def __init__(self):
        self.warned = set()   # columns already warned about (invalid UTF-8)
        self.bad = set()      # columns that hold invalid UTF-8
        self.plain = set()    # character columns that had a batch that was not dictionary-coded


def _utf8_ok(a: pa.Array) -> bool:
    try:
        a.validate(full=True)
        return True
    except pa.ArrowInvalid:
        return False


def _replace_utf8(a: pa.Array) -> pa.Array:
    raw = a.view(pa.large_binary() if pa.types.is_large_string(a.type) else pa.binary())
    return pa.array([None if b is None else b.decode("utf-8", "replace") for b in raw.to_pylist()], a.type)


def _strings(arr: pa.Array, j: int, col: _Col, mode: str, st: _State) -> pa.Array:
    is_dict = pa.types.is_dictionary(arr.type)
    if not is_dict:
        st.plain.add(j)
    vals = arr.dictionary if is_dict else arr
    want = col.field.type
    name = col.meta.name
    binary = pa.types.is_binary(want) or pa.types.is_large_binary(want)
    if not _utf8_ok(vals):
        st.bad.add(j)
        if mode == "error":
            raise ValueError("column '%s' holds strings that are not valid UTF-8 "
                             "(invalid_utf8=\"replace\" or \"binary\" reads them)" % name)
        if mode == "replace":
            if j not in st.warned:
                st.warned.add(j)
                warnings.warn("fstx: column '%s' holds strings that are not valid UTF-8; the bad bytes were "
                              "replaced with U+FFFD (invalid_utf8=\"binary\" keeps them)" % name, FstxWarning,
                              stacklevel=4)
            vals = _replace_utf8(vals)
            if is_dict:
                arr = pa.DictionaryArray.from_arrays(arr.indices, vals)
            else:
                arr = vals
    if is_dict:
        big = pa.types.is_large_string(want) or pa.types.is_large_binary(want)
        try:
            arr = (arr.cast(pa.dictionary(pa.int32(), pa.large_string())) if big else arr).dictionary_decode()
        except (pa.ArrowInvalid, pa.ArrowCapacityError):
            raise _error("column '%s': a batch holds more than 2 GB of text; read it with to_batches() and a "
                         "smaller batch_rows" % name, _core.ERR_ARG, col.meta.index) from None
    if binary:
        return arr.view(want)
    return arr if arr.type == want else arr.cast(want)


def _to_int64(arr: pa.Array) -> pa.Array:
    # bit64's integer64 is a double column holding int64 bit patterns: take the 8 bytes as they are. The
    # validity the stream made (from the page's missing-value marks and from NaN payloads) means nothing here:
    # the one missing value is INT64_MIN.
    v = pa.Array.from_buffers(pa.int64(), len(arr), [None, arr.buffers()[1]], offset=arr.offset)
    na = pc.equal(v, _INT64_MIN)
    if pc.any(na).as_py():
        v = pc.if_else(na, pa.scalar(None, pa.int64()), v)
    return v


def _to_factor(arr: pa.Array, col: _Col, path: str) -> pa.Array:
    n = len(col.levels)
    mm = pc.min_max(arr).as_py()
    if mm["min"] is not None and (mm["min"] < 1 or mm["max"] > n):
        raise _error("'%s' is corrupt (column '%s': a factor code is outside its %d levels)" % (path, col.meta.name, n),
                     _core.ERR_CORRUPT, col.meta.index)
    try:
        levels = pa.array(col.levels, pa.string())  # an NA level is a null dictionary value
    except (pa.ArrowException, UnicodeError):
        raise _error("'%s' is corrupt (column '%s': a factor level is not valid UTF-8)" % (path, col.meta.name),
                     _core.ERR_CORRUPT, col.meta.index) from None
    idx = pc.subtract(arr, pa.scalar(1, pa.int32()))
    return pa.DictionaryArray.from_arrays(idx, levels, ordered=col.ordered)


def _to_date(arr: pa.Array, col: _Col) -> pa.Array:
    if pa.types.is_int32(arr.type):
        return arr.view(pa.date32())
    d = pc.floor(arr)  # whole days, rounded down (as R prints a fractional Date, as the stream does)
    d = pc.if_else(pc.is_finite(d), d, pa.scalar(None, pa.float64()))
    try:
        return d.cast(pa.int32()).view(pa.date32())
    except pa.ArrowInvalid:
        raise _error("column '%s' has a Date outside the range of Arrow date32" % col.meta.name, _core.ERR_ARG,
                     col.meta.index) from None


def _to_timestamp(arr: pa.Array, col: _Col) -> pa.Array:
    if pa.types.is_int32(arr.type):
        return pc.multiply(arr.cast(pa.int64()), pa.scalar(1000000, pa.int64())).view(col.field.type)
    us = pc.round(pc.multiply(arr, 1e6), round_mode="half_to_even")  # nearest microsecond, as the stream
    us = pc.if_else(pc.is_finite(us), us, pa.scalar(None, pa.float64()))
    mm = pc.min_max(us).as_py()
    if mm["min"] is not None and (mm["min"] < -9.2e18 or mm["max"] > 9.2e18):
        raise _error("column '%s' has a time outside the range of Arrow microsecond timestamps" % col.meta.name,
                     _core.ERR_ARG, col.meta.index)
    return us.cast(pa.int64()).view(col.field.type)


def _finish(batch: pa.RecordBatch, plan: _Plan, schema: pa.Schema, st: _State) -> pa.RecordBatch:
    out = []
    for j, col in enumerate(plan.cols):
        a = batch.column(j)
        k = col.kind
        if k == "string":
            a = _strings(a, j, col, plan.invalid_utf8, st)
        elif k == "int64":
            a = _to_int64(a)
        elif k == "factor":
            a = _to_factor(a, col, plan.meta.path)
        elif k == "date":
            a = _to_date(a, col)
        elif k == "ts":
            a = _to_timestamp(a, col)
        out.append(a)
    return pa.RecordBatch.from_arrays(out, schema=schema)


class _Stream:
    def __init__(self, capsule):
        self._capsule = capsule

    def __arrow_c_stream__(self, requested_schema=None):
        return self._capsule


def _batches(plan: _Plan, schema: pa.Schema, sel: _Rows, threads: int, batch_rows: int, verify: int,
             st: _State) -> Iterator[pa.RecordBatch]:
    """The finished batches of a read (not called for an empty selection or for no columns)."""
    capsule = _core.stream(plan.meta.path, plan.index, [c.meta.name for c in plan.cols], sel.start, sel.stop,
                           sel.sel, threads, batch_rows, verify, plan.raw)
    reader = pa.RecordBatchReader.from_stream(_Stream(capsule))
    try:
        while True:
            try:
                batch = reader.read_next_batch()  # decodes with the GIL released
            except StopIteration:
                return
            except (pa.ArrowException, OSError) as e:  # the engine's message; the stream carries no error code
                raise _error(str(e)) from None
            yield _finish(batch, plan, schema, st)
    finally:
        reader.close()  # releases the stream and with it the file


def _empty(plan: _Plan, schema: pa.Schema) -> pa.RecordBatch:
    out = []
    for col, fld in zip(plan.cols, schema):
        if pa.types.is_dictionary(fld.type):
            lv = col.levels if col.kind == "factor" else col.meta.levels
            out.append(pa.DictionaryArray.from_arrays(pa.array([], pa.int32()), pa.array(lv or [], pa.string()),
                                                      ordered=fld.type.ordered))
        else:
            out.append(pa.array([], fld.type))
    return pa.RecordBatch.from_arrays(out, schema=schema)


def _no_columns(n: int, schema: pa.Schema) -> pa.Table:
    rb = pa.RecordBatch.from_struct_array(pa.StructArray.from_buffers(pa.struct([]), n, [None]))
    return pa.Table.from_batches([rb], schema=schema)


# ---------------------------------------------------------------- the public calls
def read(f: File, columns=None, rows=None, filters=None, *, threads=None, verify=True, as_factor=False,
         attributes=True, datetimes="arrow", invalid_utf8="replace") -> pa.Table:
    if f.closed:
        raise ValueError("the fstx file is closed")
    plan = _Plan(f.metadata, columns, datetimes, invalid_utf8, attributes, False)
    names = [c.meta.name for c in plan.cols]
    if as_factor is True or as_factor is False:
        fac = [j for j, c in enumerate(plan.cols) if c.kind == "string"] if as_factor else []
    elif isinstance(as_factor, (list, tuple)) and all(isinstance(s, str) for s in as_factor):
        fac = []
        for s in as_factor:
            if s not in names:
                raise ValueError("'as_factor' names column '%s', which is not among the columns read" % s)
            j = names.index(s)
            if plan.cols[j].kind != "string":
                raise ValueError("'as_factor' names column '%s', which is not a character column" % s)
            fac.append(j)
    else:
        raise TypeError("'as_factor' must be True, False or a list of column names")
    nt, vf = _threads(threads), _verify(verify)
    sel = _select(f, rows, filters, nt, vf)
    schema = _with_row_names(plan, sel)
    if not plan.cols:
        return _no_columns(sel.n, schema)
    st = _State()
    stream_n = sel.n if sel.take is None else len(sel.sel) // 8
    batches = list(_batches(plan, schema, sel, nt, 0, vf, st)) if stream_n else []
    if not batches:  # no rows: one empty batch, so that factors keep their levels
        batches = [_empty(plan, schema)]
    table = pa.Table.from_batches(batches, schema=schema)
    for j, col in enumerate(plan.cols):
        if col.kind != "string":
            continue
        chunks = table.column(j).chunks
        typ = col.field.type
        if j in st.bad and invalid_utf8 == "binary":
            # a column with bad bytes comes back as binary, marked as a character column for the writer
            typ = pa.large_binary() if pa.types.is_large_string(typ) else pa.binary()
            md = dict(col.field.metadata or {})
            md.setdefault(b"fstx.r", b'{"phys":"character","logical":"none"}')
            table = table.set_column(j, pa.field(col.meta.name, typ, metadata=md),
                                     pa.chunked_array([c.view(typ) for c in chunks], typ))
            continue
        if j in fac and (as_factor is not True or (chunks and j not in st.plain)):
            # levels in code-point order (R sorts them in the session's locale)
            u = pc.unique(pa.chunked_array(chunks, typ)).drop_null()
            u = u.take(pc.sort_indices(u))
            levels = u.cast(pa.string())
            dt = pa.dictionary(pa.int32(), pa.string())
            chunks = [pa.DictionaryArray.from_arrays(pc.index_in(c, value_set=u), levels) for c in chunks]
            table = table.set_column(j, pa.field(col.meta.name, dt, metadata=table.schema.field(j).metadata),
                                     pa.chunked_array(chunks, dt))
    if sel.take is not None:
        table = table.take(sel.take)
    return table


def to_batches(f: File, columns=None, batch_rows=None, filters=None, *, rows=None, threads=None, verify=True,
               attributes=True, datetimes="arrow", invalid_utf8="replace") -> pa.RecordBatchReader:
    if f.closed:
        raise ValueError("the fstx file is closed")
    if batch_rows is None:
        batch_rows = 0
    elif isinstance(batch_rows, bool) or not isinstance(batch_rows, int) or batch_rows < 1:
        raise ValueError("'batch_rows' must be None (a batch per row group) or a whole number >= 1")
    if rows is not None and not isinstance(rows, (slice, range)):
        raise TypeError("to_batches: 'rows' must be a slice or a range")
    plan = _Plan(f.metadata, columns, datetimes, invalid_utf8, attributes, invalid_utf8 == "binary")
    nt, vf = _threads(threads), _verify(verify)
    sel = _select(f, rows, filters, nt, vf)
    if sel.sel is not None and sel.take is not None:
        raise ValueError("to_batches: 'rows' must be ascending")
    schema = _with_row_names(plan, sel)
    if not plan.cols:
        return pa.RecordBatchReader.from_batches(schema, iter(_no_columns(sel.n, schema).to_batches()))
    gen = _batches(plan, schema, sel, nt, batch_rows, vf, _State()) if sel.n else iter(())
    return pa.RecordBatchReader.from_batches(schema, gen)


def read_fstx(path: PathLike, columns=None, rows=None, filters=None, *, threads=None, verify=True,
              as_factor=False, attributes=True, datetimes="arrow", invalid_utf8="replace") -> pa.Table:
    """Reads an fstx file into a pyarrow.Table.

    columns: None (all), or names / 0-based positions, in the order wanted (a repeated name: its first match).
    rows:    None (all); a slice (Python's rules) or a range; or a sequence of 0-based row numbers in any
             order, repeats allowed, returned in the order given.
    filters: ``[(column, op, value), ...]``, all must hold. op is one of ``== != < <= > >= in between is_null
             not_null``. Values are what the column holds in R: strings for character columns, level names for
             factors (``<`` needs an ordered factor), ``datetime.date`` or ISO strings for Dates, ``datetime``
             or ISO strings for POSIXct; ``None`` in a comparison matches nothing; ``in`` may hold ``None``
             (missing values) and ``nan``. ``is_null`` / ``not_null`` follow R's ``is.na()``: in a double
             column NaN counts as missing too (``("x", "in", [None])`` matches NA alone). Pages are skipped by their min / max. With filters, ``rows`` may
             only be a slice (the window that is filtered).
    threads: None = all cores.
    verify:  check every page's checksum (the default).
    as_factor: True returns character columns whose pages are all dictionary-coded as
             ``dictionary<int32, string>``; a list of names returns those columns so. Levels are sorted.
    attributes: False drops attribute blobs in R serialization from the metadata (FXA1 blobs stay).
    datetimes: "arrow" gives ``date32`` and ``timestamp[us, tz]`` (fractional days floored, seconds rounded to
             the microsecond, Inf / NaN null); "float" gives Date / POSIXct columns exactly as stored
             (``double`` or ``int32``).
    invalid_utf8: strings that are not valid UTF-8: "replace" the bad bytes with U+FFFD and warn (default),
             "error", or "binary" (such a column comes back as ``binary`` with its bytes).

    The metadata the table carries (``fstx.r``, ``fstx.attrs``, ``fstx.table_attrs``, ``fstx.row_names``) lets
    ``write_fstx`` store it again the way R wrote it.
    """
    with File(path) as f:
        return read(f, columns, rows, filters, threads=threads, verify=verify, as_factor=as_factor,
                    attributes=attributes, datetimes=datetimes, invalid_utf8=invalid_utf8)


# ---------------------------------------------------------------- pandas and polars
def _without_na_levels(table: pa.Table, lib: str) -> pa.Table:
    for j, fld in enumerate(table.schema):
        if not pa.types.is_dictionary(fld.type):
            continue
        col = table.column(j)
        if any(c.dictionary.null_count for c in col.chunks):
            warnings.warn("fstx: column '%s' is a factor with NA as a level, which %s cannot hold; it is "
                          "returned as strings" % (fld.name, lib), FstxWarning, stacklevel=3)
            vt = fld.type.value_type
            table = table.set_column(j, pa.field(fld.name, vt, metadata=fld.metadata), col.cast(vt))
    return table


def to_pandas(table: pa.Table):
    """A pandas DataFrame from a table read by fstx, keeping what R means: logical, integer, integer64 and
    double columns get pandas' nullable dtypes (missing values stay missing, integers stay integers; NaN stays
    apart from NA only where pandas keeps them apart: pandas 3 makes NaN missing too unless its option
    ``future.distinguish_nan_and_na`` is on), stored row names become the index, and the table's Arrow schema with its fstx metadata is
    kept in ``df.attrs["fstx_schema"]`` (serialized; ``pyarrow.ipc.read_schema`` reads it) for the writer.
    A factor with NA as a level is returned as strings, with a warning."""
    import pandas as pd

    t = _without_na_levels(table, "pandas")
    nullable = {pa.bool_(): pd.BooleanDtype(), pa.int32(): pd.Int32Dtype(), pa.int64(): pd.Int64Dtype(),
                pa.float64(): pd.Float64Dtype()}
    df = t.to_pandas(types_mapper=nullable.get)
    rn = row_names(table.schema)
    if rn is not None and len(rn) == table.num_rows:
        df.index = pd.Index(rn)
    df.attrs["fstx_schema"] = table.schema.serialize().to_pybytes()
    return df


def to_polars(table: pa.Table):
    """A polars DataFrame from a table read by fstx. polars has no index and no metadata: row names, classes
    and attribute blobs do not travel (write from the pyarrow.Table to keep them). A factor with NA as a level
    is returned as strings, with a warning. Column names must be unique (polars' rule): a table with repeated
    names raises ValueError."""
    import polars as pl

    names = table.column_names
    if len(set(names)) != len(names):
        dup = sorted(set(n for n in names if names.count(n) > 1))
        raise ValueError("polars needs unique column names; repeated: %s (rename them with "
                         "table.rename_columns() first)" % ", ".join(repr(n) for n in dup))
    t = _without_na_levels(table, "polars")
    # the attribute blobs are raw bytes, which polars cannot take as metadata: all metadata is dropped
    t = t.cast(pa.schema([pa.field(f.name, f.type) for f in t.schema]))
    return pl.from_arrow(t)
