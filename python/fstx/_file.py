"""Opening an fstx file and reading its metadata (no column data yet).

Names follow DESIGN.md section 2: ``fstx.metadata(path)`` gives a ``FileMeta`` whose ``columns`` are
``ColumnMeta``; ``fstx.open(path)`` gives the open ``File`` that reading will hang its methods on.
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Any, List, Optional, Union

from . import _core

PathLike = Union[str, "os.PathLike[str]"]


@dataclass(frozen=True)
class ColumnMeta:
    """One column as the file stores it (spec 4.2).

    phys:    "integer" (int32), "double" (float64), "logical" (int32 0 / 1) or "character" (UTF-8).
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

    @property
    def names(self) -> List[str]:
        return [c.name for c in self.columns]


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
        """The columns as stored, in file order. (``schema``, the pyarrow.Schema, comes with reading.)"""
        return list(self._meta.columns)

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
