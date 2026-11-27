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
# fstx 1.2 changes five expectations of the 1.0 format files, and only these (DESIGN-1.2.md section 6, rule 1): the
# feature bits 16 / 17 are known, so a file that sets one without having such a column reads; physical types 4 / 5
# without their bit, and page kind 7 under an int32 column, are corrupt. The 1.0 files and format-1.0.json stay as
# they are; the overrides live here.
FORMAT_12_READ = {"reserved_int64.fstx", "reserved_binary.fstx"}
FORMAT_12_CORRUPT = {"reserved_phys4.fstx", "reserved_phys5.fstx", "reserved_kind7.fstx"}
# [(path, expectation dict)]: files that read, files that must be refused (as newer), files 1.2 finds corrupt,
# files that only 1.2 reads (no value expectations: format-1.0.json holds a refusal for them)
FORMAT_READ = [(FORMAT_DIR / f["file"], f) for f in _fmt if f["expect"] == "read"]
FORMAT_REFUSE = [(FORMAT_DIR / f["file"], f) for f in _fmt
                 if f["expect"] != "read" and f["file"] not in FORMAT_12_READ | FORMAT_12_CORRUPT]
FORMAT_CORRUPT = [(FORMAT_DIR / f["file"], f) for f in _fmt if f["file"] in FORMAT_12_CORRUPT]
FORMAT_READ_12 = [(FORMAT_DIR / f["file"], f) for f in _fmt if f["file"] in FORMAT_12_READ]

# Golden files written by R with fstx 1.2 (golden/v12/, make_golden.R --v12): int64 and binary columns.
GOLDEN12 = GOLDEN / "v12"
_m12 = GOLDEN12 / "manifest.json"
CASES12 = [(GOLDEN12 / c["file"], _json(GOLDEN12 / (Path(c["file"]).stem + ".json")))
           for c in _json(_m12)["cases"]] if _m12.is_file() else []


def _engine_has_drop2() -> bool:
    """Whether the engine this tree builds reads int64 / binary columns through its Arrow stream and filters int64
    exactly (agent E's drop 2 of fstx 1.2). Until then the Python reader must not open a stream on such a column.
    FSTX_TEST_DROP2=1 / 0 overrides; otherwise the engine sources next to these tests are looked at. To be removed
    (with the marker) once drop 2 is merged."""
    env = os.environ.get("FSTX_TEST_DROP2")
    if env in ("0", "1"):
        return env == "1"
    src = HERE.parents[1] / "src"
    try:
        arrow = (src / "r_arrow.cpp").read_text(encoding="utf-8", errors="replace")
        filt = "".join(p.read_text(encoding="utf-8", errors="replace") for p in src.glob("core_filter*.cpp"))
    except OSError:
        return False
    return "FSTX_BINARY" in arrow and "has_i64" in filt


DROP2 = _engine_has_drop2()


def pytest_configure(config):
    config.addinivalue_line("markers", "drop2: needs the fstx 1.2 engine's int64 / binary Arrow stream and exact "
                                       "int64 filters (agent E's drop 2)")


def pytest_collection_modifyitems(config, items):
    import pytest

    if DROP2:
        return
    skip = pytest.mark.skip(reason="waits for engine drop 2 (int64 / binary in the Arrow stream, exact int64 filters)")
    for item in items:
        if "drop2" in item.keywords:
            item.add_marker(skip)


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
