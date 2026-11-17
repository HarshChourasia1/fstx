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
