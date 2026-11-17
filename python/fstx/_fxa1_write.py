"""FXA1 attribute blobs (spec section 9.4): the encoder, and the small parser the writer needs.

A blob holds a named list of R attributes. Here one R vector is a ``Vec``: its R type ("logical", "integer",
"double" or "character"), its values (``None`` = NA) and its own attributes, a ``dict`` of name -> ``Vec`` in
order. A blob is such a dict. FXA1 allows three levels: the blob's list, a value's own attributes and theirs.

    encode({"class": character(["data.frame"])})      # the 43 bytes of the spec's worked example

The reader's decoder lives in ``_fxa1.py``; ``parse`` here exists because the writer has to change blobs it is
handed (row names, a data.table key or index, factor levels) and must not depend on how the reader presents
them. R-serialization blobs are never decoded: ``is_fxa1`` tells the two apart.
"""
from __future__ import annotations

import struct
from typing import Dict, Iterable, List, Optional

MAGIC = b"FXA\x01"
_TYPES = {"logical": 1, "integer": 2, "double": 3, "character": 4}
_NAMES = {v: k for k, v in _TYPES.items()}
_NA_INT = -(2**31)
_MAX_LEN = 2**31 - 2  # values in one vector, and bytes of one vector's strings
#: R's NA_real_ (a NaN with payload 1954); a ``None`` in a double vector is written as this
NA_REAL_BITS = 0x7FF00000000007A2


class Vec:
    """One R vector inside an attribute list."""

    __slots__ = ("type", "values", "attrs")

    def __init__(self, type: str, values: Iterable, attrs: Optional[Dict[str, "Vec"]] = None):
        if type not in _TYPES:
            raise ValueError(f"FXA1 cannot hold a value of type {type!r}")
        self.type = type
        self.values = list(values)
        self.attrs: Dict[str, Vec] = dict(attrs) if attrs else {}

    def __eq__(self, other):
        return (isinstance(other, Vec) and self.type == other.type and self.attrs == other.attrs
                and _body(self) == _body(other))

    def __repr__(self):
        a = f", attrs={self.attrs!r}" if self.attrs else ""
        return f"Vec({self.type!r}, {self.values!r}{a})"


def logical(values: Iterable, attrs=None) -> Vec:
    return Vec("logical", values, attrs)


def integer(values: Iterable, attrs=None) -> Vec:
    return Vec("integer", values, attrs)


def double(values: Iterable, attrs=None) -> Vec:
    return Vec("double", values, attrs)


def character(values: Iterable, attrs=None) -> Vec:
    return Vec("character", values, attrs)


def is_fxa1(blob: Optional[bytes]) -> bool:
    """True for an FXA1 blob, False for R serialization (or no blob)."""
    return blob is not None and bytes(blob[:3]) == MAGIC[:3]


def _i32(*v: int) -> bytes:
    return struct.pack(f"<{len(v)}i", *v)


def _body(v: Vec) -> bytes:
    """The value bytes of one vector (after its type and length fields)."""
    n = len(v.values)
    if n > _MAX_LEN:
        raise ValueError("FXA1 cannot hold a vector this long")
    if v.type == "logical":
        out = []
        for e in v.values:
            if e is None:
                out.append(_NA_INT)
            elif e is True or e is False or e in (0, 1):
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
                parts.append(struct.pack("<Q", NA_REAL_BITS))
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
        b = e.encode("utf-8")  # lone surrogates cannot be UTF-8: UnicodeEncodeError is a ValueError
        if b"\x00" in b:
            raise ValueError("a string with a NUL character cannot be stored")
        lens.append(len(b))
        data.append(b)
    data = b"".join(data)
    if len(data) > _MAX_LEN:
        raise ValueError("FXA1 cannot hold this many bytes of strings in one vector")
    return _i32(*lens) + data


def _list(attrs: Dict[str, Vec], level: int, out: List[bytes]) -> None:
    if attrs and level > 3:
        raise ValueError("FXA1 holds attributes three levels deep at most")
    out.append(_i32(len(attrs)))
    for name, v in attrs.items():
        if not isinstance(name, str) or not name:
            raise ValueError("an attribute name must be a non-empty str")
        if not isinstance(v, Vec):
            raise ValueError(f"attribute {name!r} must be a Vec")
        nb = name.encode("utf-8")
        if b"\x00" in nb:
            raise ValueError("an attribute name cannot hold a NUL character")
        out.append(_i32(len(nb)))
        out.append(nb)
        out.append(_i32(_TYPES[v.type], len(v.values)))
        out.append(_body(v))
        _list(v.attrs, level + 1, out)


def encode(attrs: Dict[str, Vec]) -> Optional[bytes]:
    """An attribute list as an FXA1 blob; ``None`` for an empty list (fstx stores no blob then)."""
    if not attrs:
        return None
    out = [MAGIC]
    _list(attrs, 1, out)  # dict keys are distinct, as the names of one list must be
    return b"".join(out)


class _Reader:
    def __init__(self, b: bytes):
        self.b = b
        self.pos = 4

    def left(self) -> int:
        return len(self.b) - self.pos

    def take(self, k: int) -> bytes:
        if k < 0 or k > self.left():
            raise ValueError("truncated or wrong length")
        self.pos += k
        return self.b[self.pos - k:self.pos]

    def i32(self, k: int = 1):
        return struct.unpack(f"<{k}i", self.take(4 * k))


def _parse_list(r: _Reader, level: int) -> Dict[str, Vec]:
    (cnt,) = r.i32()
    if cnt < 0 or cnt > r.left() // 17:  # an entry takes at least 17 bytes
        raise ValueError("attribute count")
    if cnt > 0 and level > 3:
        raise ValueError("attributes nested too deep")
    out: Dict[str, Vec] = {}
    for _ in range(cnt):
        (nl,) = r.i32()
        if nl < 1:
            raise ValueError("name length")
        nb = r.take(nl)
        if b"\x00" in nb:
            raise ValueError("NUL in a name")
        name = nb.decode("utf-8")  # UnicodeDecodeError is a ValueError
        ty, n = r.i32(2)
        if n < 0:
            raise ValueError("value count")
        if ty not in _NAMES:
            raise ValueError(f"value type {ty}")
        if ty == 3:
            if n > r.left() // 8:
                raise ValueError("value count")
            vals: list = list(struct.unpack(f"<{n}d", r.take(8 * n)))
        else:
            if n > r.left() // 4:
                raise ValueError("value count")
            x = r.i32(n)
            if ty == 1:
                if any(e not in (0, 1, _NA_INT) for e in x):
                    raise ValueError("logical value other than 0, 1, NA")
                vals = [None if e == _NA_INT else bool(e) for e in x]
            elif ty == 2:
                vals = [None if e == _NA_INT else e for e in x]
            else:
                if any(e < -1 for e in x):
                    raise ValueError("string length")
                data = r.take(sum(e for e in x if e > 0))
                if b"\x00" in data:
                    raise ValueError("NUL in a string")
                vals, p = [], 0
                for e in x:
                    if e < 0:
                        vals.append(None)
                    else:
                        # a string value that is not valid UTF-8 is not corrupt (spec 9 rule 7), but Python
                        # cannot hold it as str: the caller then keeps the blob's bytes as they are
                        vals.append(data[p:p + e].decode("utf-8"))
                        p += e
        if name in out:
            raise ValueError("duplicate names")
        out[name] = Vec(_NAMES[ty], vals, _parse_list(r, level + 1))
    return out


def parse(blob: bytes) -> Dict[str, Vec]:
    """An FXA1 blob as a dict of name -> ``Vec``. ``ValueError`` when it is not a sound FXA1 blob."""
    blob = bytes(blob)
    if not is_fxa1(blob):
        raise ValueError("not an FXA1 blob")
    if len(blob) < 4 or blob[3] != MAGIC[3]:
        raise ValueError(f"attribute encoding version {blob[3] if len(blob) > 3 else 0}")
    r = _Reader(blob)
    try:
        out = _parse_list(r, 1)
    except struct.error as e:  # cannot happen (take() checks lengths); kept so only ValueError leaves
        raise ValueError(str(e)) from None
    if r.left():
        raise ValueError("bytes after the end")
    return out
