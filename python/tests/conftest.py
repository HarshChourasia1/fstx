"""Shared test data: the format 1.0 golden files the R package ships (tests/testthat/fixtures/golden-1.0).

They were written by the R package 1.0.0, so every test here also checks R -> Python compatibility.
FSTX_GOLDEN_DIR overrides the folder (for an installed wheel tested away from the git tree).
"""
import csv
import os
from pathlib import Path

import pytest


def _golden_dir() -> Path:
    env = os.environ.get("FSTX_GOLDEN_DIR")
    if env:
        return Path(env)
    return Path(__file__).resolve().parents[2] / "tests" / "testthat" / "fixtures" / "golden-1.0"


GOLDEN = _golden_dir()


def _rows(name):
    path = GOLDEN / name
    if not path.exists():
        return []
    with open(path, newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


GOLDEN_FILES = _rows("files.csv")
GOLDEN_COLUMNS = _rows("expected.csv")


@pytest.fixture(scope="session")
def golden() -> Path:
    if not GOLDEN_FILES:
        pytest.skip(f"golden files not found in {GOLDEN}")
    return GOLDEN
