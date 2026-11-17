"""FXA1 attribute blobs (format spec 9.4): the decoder.

A blob is the 4 bytes ``46 58 41 01`` ("FXA", version 1) and one attribute list. ``decode(blob)`` returns it
as ``{name: RVector}``; an ``RVector`` is one plain R vector (logical, integer, double or character) with its
own attributes, nested at most three levels. Every count and length is checked against the bytes left; any
problem raises ``Fxa1Error``. R serialization (the other blob encoding) is never decoded: ``kind(blob)`` tells
the two apart.

The decoded form loses nothing an encoder needs:

- logical: True / False / None (NA); integer: int / None (NA).
- double: float; None only for R's NA_real_ (bits 0x7FF00000000007A2). Every other NaN, the quiet NA
  0x7FF80000000007A2 included, stays a float nan with its payload (``struct.pack("<d", x)`` gives the bits back).
- character: str / None (NA). Bytes that are not valid UTF-8 (allowed for values, spec 9 rule 7) are decoded
  with ``errors="surrogateescape"``, so ``s.encode("utf-8", "surrogateescape")`` gives the stored bytes back.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional

MAGIC = b"FXA\x01"
TYPES = {1: "logical", 2: "integer", 3: "double", 4: "character"}
_NA_INT = -(2**31)
_NA_REAL = b"\xa2\x07\x00\x00\x00\x00\xf0\x7f"  # 0x7FF00000000007A2, little-endian


class Fxa1Error(ValueError):
    """An FXA1 blob that does not follow spec 9.4 (the file is corrupt)."""


@dataclass
class RVector:
    """One R vector from an attribute blob: its type, its values and its own attributes."""

    type: str  # "logical", "integer", "double" or "character"
    values: list
    attrs: Dict[str, "RVector"] = field(default_factory=dict)

    @property
    def r_class(self) -> Optional[List[str]]:
        """The vector's own class attribute, or None."""
        c = self.attrs.get("class")
        return list(c.values) if c is not None and c.type == "character" else None


def kind(blob: Optional[bytes]) -> str:
    """"none", "FXA1" or "R-serialization" (anything that does not start with "FXA")."""
    if blob is None:
        return "none"
    return "FXA1" if bytes(blob[:3]) == MAGIC[:3] else "R-serialization"


def decode(blob: bytes) -> Dict[str, RVector]:
    """An FXA1 blob as {attribute name: RVector}, in stored order. Raises Fxa1Error."""
    b = bytes(blob)
    if b[:3] != MAGIC[:3]:
        raise Fxa1Error("not an FXA1 blob")
    if len(b) < 4 or b[3] != MAGIC[3]:
        raise Fxa1Error("attribute encoding version %d" % (b[3] if len(b) > 3 else 0))
    n = len(b)
    pos = 4

    def need(k: int) -> int:
        nonlocal pos
        if k < 0 or k > n - pos:
            raise Fxa1Error("truncated or wrong length")
        at = pos
        pos += k
        return at

    def i32() -> int:
        return struct.unpack_from("<i", b, need(4))[0]

    def one_list(level: int) -> Dict[str, RVector]:
        nonlocal pos
        cnt = i32()
        # an entry takes at least 17 bytes: name length, a 1-byte name, type, length, nested count
        if cnt < 0 or cnt > (n - pos) / 17:
            raise Fxa1Error("attribute count")
        if cnt > 0 and level > 3:
            raise Fxa1Error("attributes nested too deep")
        out: Dict[str, RVector] = {}
        for _ in range(cnt):
            nl = i32()
            if nl < 1:
                raise Fxa1Error("name length")
            at = need(nl)
            nb = b[at:at + nl]
            if b"\x00" in nb:
                raise Fxa1Error("NUL in a name")
            try:
                name = nb.decode("utf-8")
            except UnicodeDecodeError:
                raise Fxa1Error("a name is not valid UTF-8") from None
            ty = i32()
            ln = i32()
            if ln < 0:
                raise Fxa1Error("value count")
            if ty not in TYPES:
                raise Fxa1Error("value type %d" % ty)
            if ty == 3:
                if ln > (n - pos) / 8:
                    raise Fxa1Error("value count")
                at = need(8 * ln)
                vals: list = list(struct.unpack_from("<%dd" % ln, b, at))
                for k in range(ln):  # NA_real_ itself -> None; other NaN keep their bits as floats
                    if vals[k] != vals[k] and b[at + 8 * k:at + 8 * k + 8] == _NA_REAL:
                        vals[k] = None
            else:
                if ln > (n - pos) / 4:
                    raise Fxa1Error("value count")
                x = struct.unpack_from("<%di" % ln, b, need(4 * ln))
                if ty == 1:
                    if any(v not in (0, 1, _NA_INT) for v in x):
                        raise Fxa1Error("logical value other than 0, 1, NA")
                    vals = [None if v == _NA_INT else bool(v) for v in x]
                elif ty == 2:
                    vals = [None if v == _NA_INT else v for v in x]
                else:
                    if any(v < -1 for v in x):
                        raise Fxa1Error("string length")
                    at = need(sum(v for v in x if v > 0))
                    vals = []
                    for v in x:
                        if v < 0:
                            vals.append(None)
                            continue
                        s = b[at:at + v]
                        at += v
                        if b"\x00" in s:
                            raise Fxa1Error("NUL in a string")
                        vals.append(s.decode("utf-8", "surrogateescape"))
            sub = one_list(level + 1)
            if name in out:
                raise Fxa1Error("duplicate names")
            out[name] = RVector(TYPES[ty], vals, sub)
        return out

    res = one_list(1)
    if pos != n:
        raise Fxa1Error("bytes after the end")
    return res
