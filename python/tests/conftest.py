"""Shared test data. No R is needed: every expectation is in a JSON sidecar written by R 4.5 + fstx 1.0.0.

golden/<case>.fstx + <case>.json   the Python cases of DESIGN.md section 3 (golden/make_golden.R)
golden/format-1.0.json             expectations for the R package's own format files, which stay in
                                   tests/testthat/fixtures/golden-1.0 (FSTX_FORMAT_GOLDEN_DIR overrides
                                   the folder, for an installed wheel tested away from the git tree)
"""
import json
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
GOLDEN = HERE / "golden"


def _json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def _format_dir() -> Path:
    env = os.environ.get("FSTX_FORMAT_GOLDEN_DIR")
    if env:
        return Path(env)
    return HERE.parents[1] / "tests" / "testthat" / "fixtures" / "golden-1.0"


# [(path, sidecar dict)] of the Python cases
CASES = [(GOLDEN / c["file"], _json(GOLDEN / (Path(c["file"]).stem + ".json")))
         for c in _json(GOLDEN / "manifest.json")["cases"]]

FORMAT_DIR = _format_dir()
_fmt = _json(GOLDEN / "format-1.0.json")["files"] if FORMAT_DIR.is_dir() else []
# [(path, expectation dict)]: files that read, files that must be refused
FORMAT_READ = [(FORMAT_DIR / f["file"], f) for f in _fmt if f["expect"] == "read"]
FORMAT_REFUSE = [(FORMAT_DIR / f["file"], f) for f in _fmt if f["expect"] != "read"]


def ids(pairs):
    return [p.name for p, _ in pairs]


def quiet(path, **kw):
    """read_fstx without the invalid-UTF-8 warning (one golden file has such strings on purpose)."""
    import warnings

    import fstx

    with warnings.catch_warnings():
        warnings.simplefilter("ignore", fstx.FstxWarning)
        return fstx.read_fstx(path, **kw)


def one(col):
    import pyarrow as pa

    return col.combine_chunks() if isinstance(col, pa.ChunkedArray) else col


def same(a, b, schema_metadata=False):
    """Table equality that also holds for NaN (Table.equals says NaN != NaN): doubles are compared by bits.
    Field metadata must match; schema-level metadata only when asked (a partial read changes the table blob)."""
    import pyarrow as pa
    import pyarrow.compute as pc

    if a.num_columns == 0 or b.num_columns == 0:  # nothing but the row count (and the schema metadata)
        return a.num_columns == b.num_columns and a.num_rows == b.num_rows and \
            (not schema_metadata or a.schema.metadata == b.schema.metadata)
    if not schema_metadata:
        a, b = a.replace_schema_metadata(None), b.replace_schema_metadata(None)
    if not a.schema.equals(b.schema, check_metadata=True) or a.num_rows != b.num_rows:
        return False
    for x, y in zip(a.columns, b.columns):
        if pa.types.is_floating(x.type):
            x, y = one(x), one(y)
            if x.is_null().to_pylist() != y.is_null().to_pylist():
                return False
            x, y = pc.fill_null(x, 0.0).view(pa.int64()), pc.fill_null(y, 0.0).view(pa.int64())
        if not x.equals(y):
            return False
    return True
