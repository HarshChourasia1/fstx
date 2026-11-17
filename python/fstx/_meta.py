"""The Arrow metadata fstx puts on what it reads and looks for on what it writes (DESIGN.md section 4).

    field  b"fstx.r"            JSON {"phys", "logical", "class"?, "tzone"?}: how R holds the column
    field  b"fstx.attrs"        the column's attribute blob (spec 9.4), base64 text
    schema b"fstx.table_attrs"  the table's attribute blob, base64 text
    schema b"fstx.nrow"         decimal text: the row count the table blob belongs to. Only next to a table blob
                                in R serialization (which nobody here can look into) on a read of the whole file

Blobs are base64 because Arrow metadata is read as UTF-8 text by other libraries (polars refuses anything
else). ``blob()`` also takes raw bytes, which an FXA1 or R-serialization blob can always be told from base64.
"""
from __future__ import annotations

import base64
import binascii
from typing import Optional

KEY_R = b"fstx.r"
KEY_ATTRS = b"fstx.attrs"
KEY_TABLE = b"fstx.table_attrs"
KEY_NROW = b"fstx.nrow"


def b64(blob: Optional[bytes]) -> Optional[bytes]:
    """A blob as it is put in Arrow metadata."""
    return None if blob is None else base64.b64encode(bytes(blob))


def blob(value) -> Optional[bytes]:
    """The blob behind a metadata value: base64 (bytes or str), or the raw blob itself. Raw blobs are never
    valid base64: FXA1 has the byte 0x01 at offset 3, R serialization a line feed at offset 1."""
    if value is None:
        return None
    if isinstance(value, str):
        return base64.b64decode(value, validate=True)  # binascii.Error is a ValueError
    raw = bytes(value)
    try:
        return base64.b64decode(raw, validate=True)
    except (binascii.Error, ValueError):
        return raw
