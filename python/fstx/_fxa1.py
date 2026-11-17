"""FXA1 attribute blobs (format spec 9.4): the decoder and the encoder.

A blob is the 4 bytes ``46 58 41 01`` ("FXA", version 1) and one attribute list. ``decode(blob)`` returns it
as ``{name: RVector}`` in stored order; ``encode(attrs)`` is its inverse, byte for byte. An ``RVector`` is one
plain R vector (logical, integer, double or character) with its own attributes, nested at most three levels.
Every count and length is checked against the bytes left; a blob that does not follow the spec raises
``Fxa1Error`` (a ValueError). R serialization (the other blob encoding) is never decoded: ``kind(blob)`` and
``is_fxa1(blob)`` tell the two apart.

    encode({"class": character(["data.frame"])})      # the 43 bytes of the spec's worked example

Values:

- logical: True / False / None (NA); integer: int / None (NA).
- double: float; None is R's NA_real_ (bits 0x7FF00000000007A2). Every other NaN, the quiet NA
  0x7FF80000000007A2 included, stays a float nan with its payload, and is written back with it.
- character: str / None (NA). Bytes that are not valid UTF-8 (allowed for values, spec 9 rule 7) are decoded
  with ``errors="surrogateescape"`` and encoded the same way, so they come back as the stored bytes.
"""
from __future__ import annotations

import struct
from typing import Dict, Iterable, List, Optional

MAGIC = b"FXA\x01"
TYPES = {1: "logical", 2: "integer", 3: "double", 4: "character"}
_CODES = {v: k for k, v in TYPES.items()}
_NA_INT = -(2**31)
_MAX_LEN = 2**31 - 2  # values in one vector, and bytes of one vector's strings
#: R's NA_real_ (a NaN with payload 1954); a ``None`` in a double vector is written as this
NA_REAL_BITS = 0x7FF00000000007A2
_NA_REAL = struct.pack("<Q", NA_REAL_BITS)


class Fxa1Error(ValueError):
    """An FXA1 blob that does not follow spec 9.4 (the file is corrupt)."""


class RVector:
    """One R vector from an attribute list: its type, its values and its own attributes."""

    __slots__ = ("type", "values", "attrs")

    def __init__(self, type: str, values: Iterable, attrs: Optional[Dict[str, "RVector"]] = None):
        if type not in _CODES:
            raise ValueError(f"FXA1 cannot hold a value of type {type!r}")
        self.type = type  # "logical", "integer", "double" or "character"
        self.values = list(values)
        self.attrs: Dict[str, RVector] = dict(attrs) if attrs else {}

    @property
    def r_class(self) -> Optional[List[str]]:
        """The vector's own class attribute, or None."""
        c = self.attrs.get("class")
        return list(c.values) if c is not None and c.type == "character" else None

    def __eq__(self, other):
        # by the bytes they are stored as: NaN equals the same NaN, 1 does not equal True
        return (isinstance(other, RVector) and self.type == other.type and self.attrs == other.attrs
                and _body(self) == _body(other))

    __hash__ = None  # type: ignore[assignment]

    def __repr__(self):
        a = f", attrs={self.attrs!r}" if self.attrs else ""
        return f"RVector({self.type!r}, {self.values!r}{a})"


Vec = RVector  # the writer's name for it


def logical(values: Iterable, attrs=None) -> RVector:
    return RVector("logical", values, attrs)


def integer(values: Iterable, attrs=None) -> RVector:
    return RVector("integer", values, attrs)


def double(values: Iterable, attrs=None) -> RVector:
    return RVector("double", values, attrs)


def character(values: Iterable, attrs=None) -> RVector:
    return RVector("character", values, attrs)


def kind(blob: Optional[bytes]) -> str:
    """"none", "FXA1" or "R-serialization" (anything that does not start with "FXA")."""
    if blob is None:
        return "none"
    return "FXA1" if bytes(blob[:3]) == MAGIC[:3] else "R-serialization"


def is_fxa1(blob: Optional[bytes]) -> bool:
    """True for an FXA1 blob, False for R serialization (or no blob)."""
    return blob is not None and bytes(blob[:3]) == MAGIC[:3]


# ---------------------------------------------------------------- decode
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


parse = decode  # the writer's name for it


# ---------------------------------------------------------------- encode
def _i32(*v: int) -> bytes:
    return struct.pack(f"<{len(v)}i", *v)


def _body(v: RVector) -> bytes:
    """The value bytes of one vector (after its type and length fields)."""
    n = len(v.values)
    if n > _MAX_LEN:
        raise ValueError("FXA1 cannot hold a vector this long")
    if v.type == "logical":
        out = []
        for e in v.values:
            if e is None:
                out.append(_NA_INT)
            elif e is True or e is False or (isinstance(e, int) and e in (0, 1)):
                out.append(int(e))
            else:
                raise ValueError(f"a logical value must be True, False or None, not {e!r}")
        return _i32(*out)
    if v.type == "integer":
        out = []
        for e in v.values:
            if e is None:
                out.append(_NA_INT)
            elif isinstance(e, bool) or not isinstance(e, int) or not -(2**31) < e < 2**31:
                raise ValueError(f"an integer value must be a whole number inside int32, not {e!r}")
            else:
                out.append(e)
        return _i32(*out)
    if v.type == "double":
        parts = []
        for e in v.values:
            if e is None:
                parts.append(_NA_REAL)
            elif isinstance(e, bool) or not isinstance(e, (int, float)):
                raise ValueError(f"a double value must be a number, not {e!r}")
            else:
                parts.append(struct.pack("<d", e))  # keeps a NaN's payload
        return b"".join(parts)
    lens, data = [], []
    for e in v.values:
        if e is None:
            lens.append(-1)
            continue
        if not isinstance(e, str):
            raise ValueError(f"a character value must be a str or None, not {e!r}")
        s = e.encode("utf-8", "surrogateescape")  # bytes decode() escaped go back as they were
        if b"\x00" in s:
            raise ValueError("a string with a NUL character cannot be stored")
        lens.append(len(s))
        data.append(s)
    blob = b"".join(data)
    if len(blob) > _MAX_LEN:
        raise ValueError("FXA1 cannot hold this many bytes of strings in one vector")
    return _i32(*lens) + blob


def _list(attrs: Dict[str, RVector], level: int, out: List[bytes]) -> None:
    if attrs and level > 3:
        raise ValueError("FXA1 holds attributes three levels deep at most")
    out.append(_i32(len(attrs)))
    for name, v in attrs.items():
        if not isinstance(name, str) or not name:
            raise ValueError("an attribute name must be a non-empty str")
        if not isinstance(v, RVector):
            raise ValueError(f"attribute {name!r} must be an RVector")
        nb = name.encode("utf-8")
        if b"\x00" in nb:
            raise ValueError("an attribute name cannot hold a NUL character")
        out.append(_i32(len(nb)))
        out.append(nb)
        out.append(_i32(_CODES[v.type], len(v.values)))
        out.append(_body(v))
        _list(v.attrs, level + 1, out)


def encode(attrs: Dict[str, RVector]) -> Optional[bytes]:
    """An attribute list as an FXA1 blob; ``None`` for an empty list (fstx stores no blob then)."""
    if not attrs:
        return None
    out = [MAGIC]
    _list(attrs, 1, out)  # dict keys are distinct, as the names of one list must be
    return b"".join(out)
