"""Opening an fstx file and its metadata.

Names follow DESIGN.md section 2: ``fstx.metadata(path)`` gives a ``FileMeta`` whose ``columns`` are
``ColumnMeta``; ``fstx.open(path)`` gives the open ``File`` with ``schema``, ``read`` and ``to_batches``
(the reading itself is in ``_read.py``).
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from functools import cached_property
from typing import Any, Dict, List, Optional, Union

from . import _core, _fxa1

PathLike = Union[str, "os.PathLike[str]"]


@dataclass(frozen=True)
class ColumnMeta:
    """One column as the file stores it (spec 4.2).

    phys:    "integer" (int32), "double" (float64), "logical" (int32 0 / 1) or "character" (UTF-8); since fstx
             1.2 also "int64" (64-bit integers, R's bit64 integer64) and "binary" (bytes; R: a list of raw vectors).
    logical: "none", "factor" (1-based int32 codes + ``levels``), "Date" (days since 1970-01-01, stored as
             integer or double) or "POSIXct" (double seconds since 1970-01-01 UTC).
    tzone:   POSIXct only: the time zone, "" when R stored an empty one, None when the column has none.
    attrs:   the column's attribute blob (spec 9.4: FXA1 or R serialization), raw, or None.
    """

    index: int
    name: str
    phys: str
    logical: str
    ordered: bool
    levels: Optional[List[str]]
    tzone: Optional[str]
    attrs: Optional[bytes] = field(repr=False)
    compressed_bytes: int
    raw_bytes: int
    pages: int

    @cached_property
    def attributes(self) -> Optional[Dict[str, "_fxa1.RVector"]]:
        """The column's R attributes other than those the schema stores, decoded from ``attrs``: {} when there
        is no blob, None when the blob is R serialization (never decoded in Python)."""
        return _decode(self.attrs, "column '%s'" % self.name, self.index)

    @cached_property
    def r_class(self) -> Optional[List[str]]:
        """The class vector the column has in R: ["factor"], ["ordered", "factor"], ["Date"],
        ["POSIXct", "POSIXt"], the class in the attribute blob (["integer64"], ["difftime"], ...), [] for a
        plain vector, None when the attributes are R serialization (the class is then unknown). An int64 column
        is ["integer64"] (R reads it as bit64 integer64; the class is not stored)."""
        if self.phys == "int64":
            a = self.attributes
            c = a.get("class") if a else None
            own = [v for v in c.values if v is not None] if c is not None and c.type == "character" else []
            return own if "integer64" in own else own + ["integer64"]
        if self.logical == "factor":
            return ["ordered", "factor"] if self.ordered else ["factor"]
        if self.logical == "Date":
            return ["Date"]
        if self.logical == "POSIXct":
            return ["POSIXct", "POSIXt"]
        a = self.attributes
        if a is None:
            return None
        c = a.get("class")
        return [v for v in c.values if v is not None] if c is not None and c.type == "character" else []


@dataclass(frozen=True)
class FileMeta:
    """A file's metadata. ``table_attrs`` is the table's attribute blob (spec 9.4), raw, or None."""

    path: str
    nrow: int
    ncol: int
    row_groups: int
    pages: int
    profile: str
    format_version: int
    file_size: int
    row_group_size: int
    table_attrs: Optional[bytes] = field(repr=False)
    columns: List[ColumnMeta] = field(repr=False)
    required_features: int = 0  # footer feature bits (spec 5): bit 16 int64 columns, bit 17 binary (fstx 1.2)
    optional_features: int = 0

    @property
    def names(self) -> List[str]:
        return [c.name for c in self.columns]

    @cached_property
    def attributes(self) -> Optional[Dict[str, "_fxa1.RVector"]]:
        """The table's R attributes (class, row names, data.table key, anything else), decoded from
        ``table_attrs`` and checked as R's reader checks them (spec 9.4): {"class": data.frame} when there is
        no blob, None when the blob is R serialization. Raises ``FstxError`` when they are corrupt."""
        if self.table_attrs is None:
            return {"class": _fxa1.RVector("character", ["data.frame"])}
        a = _decode(self.table_attrs, "table", None, self.path)
        if a is None:
            return None

        def bad(why: str) -> Exception:
            return _core.FstxError("'%s' is corrupt (table attributes: %s)" % (self.path, why), _core.ERR_CORRUPT, None)

        if any(k in a for k in ("names", "dim", "dimnames")):
            raise bad("names, dim or dimnames")
        cl = a.get("class")
        if cl is None or cl.type != "character" or "data.frame" not in cl.values:
            raise bad("no data.frame class")
        rn = a.get("row.names")
        if rn is not None:
            if rn.type == "integer" and len(rn.values) == 2 and rn.values[0] is None:  # R's compact c(NA, n)
                if rn.values[1] is None or abs(rn.values[1]) != self.nrow:
                    raise bad("row names do not match the row count")
            elif rn.type not in ("integer", "character") or len(rn.values) != self.nrow:
                raise bad("row names do not match the row count")
        return a

    @property
    def table_class(self) -> Optional[List[str]]:
        """The table's class in R (["data.frame"], ["data.table", "data.frame"], ...); None when unknown."""
        a = self.attributes
        return None if a is None else list(a["class"].values)

    @property
    def row_names(self) -> Optional[list]:
        """Stored row names, one per row (str or int); None when they are automatic (or unknown)."""
        a = self.attributes
        rn = None if a is None else a.get("row.names")
        if rn is None or (rn.type == "integer" and len(rn.values) == 2 and rn.values[0] is None):
            return None
        return list(rn.values)

    @cached_property
    def schema(self):
        """The pyarrow.Schema a default ``read_fstx()`` returns, with the metadata of DESIGN.md section 4."""
        from . import _read

        return _read.schema_of(self)


def _decode(blob: Optional[bytes], what: str, column: Optional[int], path: Optional[str] = None):
    k = _fxa1.kind(blob)
    if k == "none":
        return {}
    if k != "FXA1":
        return None
    try:
        return _fxa1.decode(blob)
    except _fxa1.Fxa1Error as e:
        msg = ("'%s' is corrupt (%s attributes: %s)" % (path, what, e) if path
               else "corrupt %s attributes: %s" % (what, e))
        raise _core.FstxError(msg, _core.ERR_CORRUPT, column) from None


def _meta(f: "_core.File") -> FileMeta:
    info = f.info()
    cols = []
    for c in f.columns():
        n_levels = c.pop("num_levels")
        levels = f.levels(c["index"]) if c["logical"] == "factor" else None
        assert levels is None or len(levels) == n_levels
        cols.append(ColumnMeta(levels=levels, **c))
    return FileMeta(columns=cols, **info)


class File:
    """An open fstx file. Use ``fstx.open(path)``; close it with ``close()`` or a ``with`` block.

    The metadata is read once at open and stays available after ``close()``.
    """

    def __init__(self, path: PathLike):
        p = os.fspath(path)
        if isinstance(p, bytes):
            p = os.fsdecode(p)
        self._f = _core.open(p)
        try:
            self._meta = _meta(self._f)
        except BaseException:
            self._f.close()
            raise

    # ---- lifetime
    def close(self) -> None:
        self._f.close()

    @property
    def closed(self) -> bool:
        return self._f.closed

    def __enter__(self) -> "File":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def __repr__(self) -> str:
        state = " (closed)" if self.closed else ""
        return f"<fstx.File {self.path!r}: {self.num_rows} rows x {self.num_columns} columns{state}>"

    # ---- metadata
    @property
    def path(self) -> str:
        return self._meta.path

    @property
    def metadata(self) -> FileMeta:
        return self._meta

    @property
    def num_rows(self) -> int:
        return self._meta.nrow

    @property
    def num_columns(self) -> int:
        return self._meta.ncol

    @property
    def shape(self) -> tuple:
        return (self._meta.nrow, self._meta.ncol)

    @property
    def column_names(self) -> List[str]:
        return self._meta.names

    @property
    def columns(self) -> List[ColumnMeta]:
        """The columns as stored, in file order."""
        return list(self._meta.columns)

    @property
    def schema(self):
        """The pyarrow.Schema a default ``read()`` returns (``metadata.schema``)."""
        return self._meta.schema

    # ---- reading (arguments: fstx.read_fstx)
    def read(self, columns=None, rows=None, filters=None, **options):
        """Reads the file, or part of it, into a pyarrow.Table. Arguments as ``fstx.read_fstx``."""
        from . import _read

        return _read.read(self, columns, rows, filters, **options)

    def to_batches(self, columns=None, batch_rows=None, filters=None, **options):
        """A pyarrow.RecordBatchReader over the file: one batch per row group, or of at most ``batch_rows``
        rows; each is decoded when it is pulled. Options: rows (a slice), threads, verify, attributes,
        datetimes, invalid_utf8 as in ``fstx.read_fstx`` (``invalid_utf8="binary"`` makes every character
        column binary here, as the schema cannot wait for the data). The reader opens the file itself, so it
        stays usable after this File is closed."""
        from . import _read

        return _read.to_batches(self, columns, batch_rows, filters, **options)

    def __arrow_c_stream__(self, requested_schema=None):
        """The Arrow PyCapsule stream protocol: polars, duckdb and pyarrow read the file a row group at a time."""
        return self.to_batches().__arrow_c_stream__(requested_schema)

    def column(self, which: Union[int, str]) -> ColumnMeta:
        """A column by 0-based position (negative counts from the end) or by name (first match)."""
        if isinstance(which, str):
            i = self._f.find_column(which) if not self.closed else next(
                (c.index for c in self._meta.columns if c.name == which), -1)
            if i < 0:
                raise KeyError(which)
            return self._meta.columns[i]
        return self._meta.columns[which]


def open(path: PathLike) -> File:  # noqa: A001
    """Opens an fstx file. Raises ``fstx.FstxError`` when it is missing, not fstx, newer or damaged."""
    return File(path)


def metadata(path: PathLike) -> FileMeta:
    """The file's metadata: rows, columns, row groups, pages, profile, format version and every column."""
    with File(path) as f:
        return f.metadata
