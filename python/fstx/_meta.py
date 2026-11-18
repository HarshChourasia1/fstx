"""The Arrow metadata fstx puts on what it reads and looks for on what it writes (DESIGN.md section 4).

    field  b"fstx.r"            JSON {"phys", "logical", "class"?, "tzone"?}: how R holds the column
    field  b"fstx.attrs"        the column's attribute blob (spec 9.4), base64 text
    schema b"fstx.table_attrs"  the table's attribute blob, base64 text
    schema b"fstx.nrow"         decimal text: the row count the table blob belongs to. Only next to a table blob
                                in R serialization (which nobody here can look into) on a read of the whole file

Blobs are base64 because Arrow metadata is read as UTF-8 text by other libraries (polars refuses anything
else). ``blob()`` also takes raw bytes, which an FXA1 or R-serialization blob can always be told from base64.

Time zones: a fixed UTC offset, which Arrow writes "+05:30", is stored as R's tzone in POSIX TZ form,
"<+0530>-05:30" (POSIX counts the other way round, so the sign after the name is inverted); R reads "+05:30"
itself as UTC-05:30. ``r_tzone()`` and ``arrow_tz()`` turn one into the other (DESIGN.md section 3).
"""
from __future__ import annotations

import base64
import binascii
import re
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


_ARROW_FIXED = re.compile(r"([+-])(\d\d):(\d\d)")
_POSIX_FIXED = re.compile(r"<([+-])(\d\d)(\d\d)>([+-])(\d\d):(\d\d)")


def r_tzone(tz: Optional[str]) -> Optional[str]:
    """R's tzone for an Arrow time zone: a fixed offset "+05:30" becomes "<+0530>-05:30"; others are kept."""
    m = _ARROW_FIXED.fullmatch(tz) if tz else None
    if m is None:
        return tz
    s, hh, mm = m.groups()
    return "<%s%s%s>%s%s:%s" % (s, hh, mm, "-" if s == "+" else "+", hh, mm)


def arrow_tz(tzone: Optional[str]) -> Optional[str]:
    """The Arrow time zone for R's tzone: the exact form r_tzone() writes goes back to "+05:30"; others are kept."""
    m = _POSIX_FIXED.fullmatch(tzone) if tzone else None
    if m is None:
        return tzone
    s, hh, mm, s2, hh2, mm2 = m.groups()
    if (hh, mm) != (hh2, mm2) or s == s2:
        return tzone
    return "%s%s:%s" % (s, hh, mm)
