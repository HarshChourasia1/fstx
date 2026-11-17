"""Filters: ``[(column, op, value), ...]`` (all must hold) into the engine's predicates.

The values are converted as R's ``.term_pred()`` (R/m5.R) converts them, from the column as R sees it:
character, factor (level names), Date, POSIXct or a plain number / logical. Columns of any other class
(integer64, difftime, ...) cannot be filtered, as in R: their page statistics do not mean what the values mean.
"""
from __future__ import annotations

import datetime as _dt
import math
import numbers
from typing import Any, List, Optional

from . import _core

_OPS = {"==": _core.OP_EQ, "!=": _core.OP_NE, "<": _core.OP_LT, "<=": _core.OP_LE, ">": _core.OP_GT,
        ">=": _core.OP_GE, "in": _core.OP_IN, "is_null": _core.OP_ISNA, "not_null": _core.OP_NOTNA}
_CMP = ("<", "<=", ">", ">=")
_EPOCH = _dt.date(1970, 1, 1)


def _zone(tz: str):
    if tz in ("UTC", "GMT", "Etc/UTC"):
        return _dt.timezone.utc
    import zoneinfo

    try:
        return zoneinfo.ZoneInfo(tz)
    except Exception:
        raise ValueError("time zone '%s' is not known to Python (on Windows: pip install tzdata)" % tz) from None


def _kind(col) -> str:
    """character / factor / Date / POSIXct / plain, or raises for a column that cannot be filtered."""
    if col.phys == "character":
        return "character"
    rc = col.r_class
    if rc is None:
        raise ValueError("its attributes are stored as R serialization, so its class is unknown to Python")
    a = col.attributes or {}
    if "factor" in rc and col.phys == "integer" and (
            col.logical == "factor" or ("levels" in a and a["levels"].type == "character")):
        return "factor"
    if "Date" in rc:
        return "Date"
    if "POSIXct" in rc:
        return "POSIXct"
    if not rc:
        return "plain"
    raise ValueError("columns of class '%s' cannot be filtered" % "/".join(rc))


def _number(kind: str, col, v: Any, bad) -> Optional[float]:
    """One value as the double the column stores; None for NA (None)."""
    if v is None:
        return None
    if kind == "Date":
        if isinstance(v, str):
            try:
                v = _dt.date.fromisoformat(v)
            except ValueError:
                raise bad("'%s' is not a date (YYYY-MM-DD)" % v) from None
        if not isinstance(v, _dt.date) or isinstance(v, _dt.datetime):
            raise bad("compare a Date column with datetime.date values or date strings")
        return float((v - _EPOCH).days)
    if kind == "POSIXct":
        if isinstance(v, str):
            try:
                v = _dt.datetime.fromisoformat(v)
            except ValueError:
                raise bad("'%s' is not a date-time (ISO 8601)" % v) from None
        elif isinstance(v, _dt.date) and not isinstance(v, _dt.datetime):
            v = _dt.datetime(v.year, v.month, v.day)
        if not isinstance(v, _dt.datetime):
            raise bad("compare a POSIXct column with datetime values or date-time strings")
        if v.tzinfo is None:  # a clock time in the column's time zone; none: local time, as R
            tz = col.tzone
            if col.logical != "POSIXct":
                t = (col.attributes or {}).get("tzone")
                tz = t.values[0] if t is not None and t.type == "character" and t.values else None
            if tz:
                try:
                    v = v.replace(tzinfo=_zone(tz))
                except ValueError as e:
                    raise bad(str(e)) from None
        try:
            return v.timestamp()
        except (OSError, OverflowError):
            raise bad("this date-time cannot be converted in local time; give it a time zone") from None
    if isinstance(v, bool) or (isinstance(v, numbers.Real) and not isinstance(v, str)):
        return float(v)
    raise bad("compare a numeric or logical column with numbers")


def to_preds(meta, filters) -> List[dict]:
    """The engine predicates for ``filters`` on a file with metadata ``meta`` (see fstx.read_fstx)."""
    if isinstance(filters, tuple) and filters and isinstance(filters[0], str):
        filters = [filters]  # one bare term
    names = meta.names
    preds: List[dict] = []
    for term in filters:
        if not isinstance(term, (tuple, list)) or len(term) not in (2, 3) or not isinstance(term[0], str):
            raise ValueError("filters must be (column, op, value) tuples, got %r" % (term,))
        name, op = term[0], term[1]
        if name not in names:
            raise KeyError("filters: column '%s' is not in the file" % name)
        col = meta.columns[names.index(name)]

        def bad(why: str, name=name) -> ValueError:
            return ValueError("filters on column '%s': %s" % (name, why))

        if op == "between":
            if len(term) != 3 or not isinstance(term[2], (tuple, list)) or len(term[2]) != 2:
                raise bad("between needs (lower, upper)")
            terms = [(name, ">=", term[2][0]), (name, "<=", term[2][1])]
        else:
            terms = [tuple(term)]
        for t in terms:
            op = t[1]
            if op not in _OPS:
                raise bad("unknown operator %r (use == != < <= > >= in between is_null not_null)" % (op,))
            p = {"col": col.index, "op": _OPS[op]}
            if op in ("is_null", "not_null"):
                if len(t) == 3 and t[2] is not None:
                    raise bad("%s takes no value" % op)
                preds.append(p)
                continue
            if len(t) != 3:
                raise bad("%s needs a value" % op)
            v = t[2]
            if op == "in":
                if isinstance(v, (str, bytes)) or not hasattr(v, "__iter__"):
                    raise bad("'in' needs a list of values")
                v = list(v)
            elif isinstance(v, (list, tuple, set, frozenset, dict)):
                raise bad("the value of %s must be a single value" % op)
            none = {"col": col.index, "op": _core.OP_NONE}
            try:
                kind = _kind(col)
            except ValueError as e:
                raise bad(str(e)) from None
            if kind in ("character", "factor"):
                what = "strings" if kind == "character" else "level names"
                if any(x is not None and not isinstance(x, str) for x in (v if op == "in" else [v])):
                    raise bad("compare a %s column with %s" % ("character" if kind == "character" else "factor", what))
            if kind == "character":
                if op in _CMP:
                    raise bad("character columns support ==, !=, in, is_null and not_null only")
                if op == "in":
                    p["strs"] = [x.encode("utf-8") for x in v if x is not None]
                    p["set_na"] = any(x is None for x in v)
                elif v is None:
                    p = none
                else:
                    p["strs"] = [v.encode("utf-8")]
                preds.append(p)
                continue
            if kind == "factor":
                lv = col.levels if col.logical == "factor" else col.attributes["levels"].values
                code = {s: k + 1 for k, s in reversed(list(enumerate(lv))) if s is not None}  # first match
                if op in _CMP:
                    if "ordered" not in col.r_class:
                        raise bad("<, <=, > and >= need an ordered factor")
                    if v is None:
                        p = none
                    elif v not in code:
                        raise bad("'%s' is not one of its levels" % v)
                    else:
                        p["value"] = float(code[v])
                elif op == "in":
                    p["set"] = [float(code[x]) for x in v if x is not None and x in code]
                    p["set_na"] = any(x is None for x in v)
                elif v is None:
                    p = none
                elif v not in code:  # no row equals it: == matches nothing, != every row that is not NA
                    p = none if op == "==" else {"col": col.index, "op": _core.OP_NOTNA}
                else:
                    p["value"] = float(code[v])
                preds.append(p)
                continue
            if op == "in":
                nums = [_number(kind, col, x, bad) for x in v]
                p["set"] = [x for x in nums if x is not None and not math.isnan(x)]
                p["set_na"] = any(x is None for x in nums)
                p["set_nan"] = any(x is not None and math.isnan(x) for x in nums)
            else:
                x = _number(kind, col, v, bad)
                if x is None or math.isnan(x):  # a comparison with NA or NaN matches nothing
                    p = none
                else:
                    p["value"] = x
            preds.append(p)
    return preds
