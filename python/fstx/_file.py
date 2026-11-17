"""Opening an fstx file and reading its metadata (no column data yet)."""
from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Any, Optional, Union

from . import _core

PathLike = Union[str, "os.PathLike[str]"]


@dataclass(frozen=True)
class Column:
    """One column of an fstx file.

    physical: how the values are stored: "int32", "float64", "logical" or "string".
    logical:  what they mean: "none", "factor" (int32 codes + levels), "date" (days since 1970-01-01)
              or "posixct" (seconds since 1970-01-01 UTC, shown in ``tzone``).
    """

    index: int
    name: str
    physical: str
    logical: str
    ordered: bool
    num_levels: int
    tzone: Optional[str]
    attrs: Optional[bytes]
    compressed_bytes: int
    raw_bytes: int
    num_pages: int

    @property
    def type(self) -> str:
        """One word for the column: the logical type when there is one, else the physical type."""
        return self.physical if self.logical == "none" else self.logical


class File:
    """An open fstx file. Use ``fstx.open(path)``; close it with ``close()`` or a ``with`` block."""

    def __init__(self, path: PathLike):
        p = os.fspath(path)
        if isinstance(p, bytes):
            p = os.fsdecode(p)
        self._f = _core.open(p)
        self._info = self._f.info()
        self._schema = tuple(Column(**c) for c in self._f.columns())

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

    # ---- metadata (kept after close)
    @property
    def path(self) -> str:
        return self._info["path"]

    @property
    def num_rows(self) -> int:
        return self._info["num_rows"]

    @property
    def num_columns(self) -> int:
        return self._info["num_columns"]

    @property
    def shape(self) -> tuple:
        return (self.num_rows, self.num_columns)

    @property
    def column_names(self) -> list:
        return [c.name for c in self._schema]

    @property
    def schema(self) -> tuple:
        """The columns, in file order (a tuple of ``fstx.Column``)."""
        return self._schema

    @property
    def info(self) -> dict:
        """File-level details: rows, columns, row groups, pages, sizes, format version, profile."""
        return dict(self._info)

    def column(self, which: Union[int, str]) -> Column:
        """A column by position (negative counts from the end) or by name."""
        if isinstance(which, str):
            for c in self._schema:
                if c.name == which:
                    return c
            raise KeyError(which)
        return self._schema[which]

    def levels(self, which: Union[int, str]) -> list:
        """The levels of a factor column, in order (an empty list for other columns)."""
        return self._f.levels(self.column(which).index)


def open(path: PathLike) -> File:  # noqa: A001
    """Opens an fstx file. Raises ``fstx.FstxError`` when it is missing, not fstx, newer or damaged."""
    return File(path)


def metadata(path: PathLike) -> dict:
    """The file's metadata as one plain dict: the ``File.info`` keys plus ``columns`` (a list of dicts)."""
    with File(path) as f:
        out = f.info
        out["columns"] = [
            {
                "name": c.name,
                "type": c.type,
                "physical": c.physical,
                "logical": c.logical,
                "ordered": c.ordered,
                "num_levels": c.num_levels,
                "tzone": c.tzone,
                "compressed_bytes": c.compressed_bytes,
                "raw_bytes": c.raw_bytes,
                "num_pages": c.num_pages,
            }
            for c in f.schema
        ]
        return out
