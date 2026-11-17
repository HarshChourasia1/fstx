"""Opening files and reading metadata, against the R package's golden files."""
import re
import shutil

import pytest

import fstx
from conftest import GOLDEN_COLUMNS, GOLDEN_FILES

READ = [r for r in GOLDEN_FILES if r["expect"] == "read"]
REFUSE = [r for r in GOLDEN_FILES if r["expect"] == "refuse"]

# R class in expected.csv -> (physical types allowed, logical type)
R_CLASS = {
    "integer": ({"int32"}, "none"),
    "numeric": ({"float64"}, "none"),
    "logical": ({"logical"}, "none"),
    "character": ({"string"}, "none"),
    "factor": ({"int32"}, "factor"),
    "ordered/factor": ({"int32"}, "factor"),
    "Date": ({"int32", "float64"}, "date"),
    "POSIXct/POSIXt": ({"float64"}, "posixct"),
}


def test_golden_set_is_there(golden):
    assert len(READ) >= 12 and len(REFUSE) >= 10


@pytest.mark.parametrize("row", READ, ids=[r["file"] for r in READ])
def test_shape_and_file_info(golden, row):
    path = golden / row["file"]
    with fstx.open(path) as f:
        assert f.num_rows == int(row["nrow"])
        assert f.num_columns == int(row["ncol"])
        assert f.shape == (int(row["nrow"]), int(row["ncol"]))
        info = f.info
        assert info["format_version"] == int(row["format_version"])
        assert info["profile"] == row["profile"]
        assert info["file_size"] == int(row["bytes"]) == path.stat().st_size
        assert len(f.schema) == f.num_columns
        assert sum(c.num_pages for c in f.schema) == info["num_pages"]


@pytest.mark.parametrize("row", READ, ids=[r["file"] for r in READ])
def test_column_names_and_types(golden, row):
    want = sorted((r for r in GOLDEN_COLUMNS if r["file"] == row["file"]), key=lambda r: int(r["index"]))
    with fstx.open(golden / row["file"]) as f:
        assert f.column_names == [r["column"] for r in want]
        for col, r in zip(f.schema, want):
            assert col.index == int(r["index"]) - 1
            if r["class"] in R_CLASS:  # other classes are plain columns with an R class attribute
                phys, logical = R_CLASS[r["class"]]
                assert col.physical in phys, col
                assert col.logical == logical, col
                assert col.ordered == (r["class"] == "ordered/factor")
            else:
                assert col.attrs  # the R class lives in the attribute blob


def test_types_file_in_detail(golden):
    with fstx.open(golden / "types.fstx") as f:
        assert f.column_names == [
            "int", "dbl", "lgl", "chr", "fac", "ord", "date", "date_dbl", "ct", "ct_notz", "labelled",
        ]
        assert [c.type for c in f.schema][:10] == [
            "int32", "float64", "logical", "string", "factor", "factor", "date", "date", "posixct", "posixct",
        ]
        assert f.column("date").physical == "int32"
        assert f.column("date_dbl").physical == "float64"
        assert f.levels("fac") == ["lo", "mid", "hi"]
        assert f.levels("ord") == ["s", "m", "l"]
        assert f.column("fac").num_levels == 3 and not f.column("fac").ordered
        assert f.column("ord").ordered
        assert f.column("ct").tzone == "UTC"
        assert f.column("ct_notz").tzone in (None, "")
        assert f.levels("int") == []
        assert f.column(-1).name == "labelled"
        with pytest.raises(KeyError):
            f.column("nope")
        assert "200 rows x 11 columns" in repr(f)


def test_non_ascii_names_and_levels(golden):
    with fstx.open(golden / "nonascii.fstx") as f:
        assert f.column_names == ["café", "日本", "Ω"]
        assert f.column("日本").logical == "factor"
        levels = f.levels("日本")
        assert len(levels) == f.column("日本").num_levels
        assert all(isinstance(s, str) for s in levels)


def test_non_ascii_path(golden, tmp_path):
    p = tmp_path / "dépôt 日本" / "tâble Ω.fstx"
    p.parent.mkdir()
    shutil.copyfile(golden / "types.fstx", p)
    with fstx.open(p) as f:  # a pathlib.Path
        assert f.num_rows == 200
    assert fstx.metadata(str(p))["num_columns"] == 11


def test_metadata_dict(golden):
    m = fstx.metadata(golden / "types.fstx")
    assert m["num_rows"] == 200 and m["num_columns"] == 11
    assert [c["name"] for c in m["columns"]][:3] == ["int", "dbl", "lgl"]
    assert m["columns"][4]["type"] == "factor"
    assert m["num_row_groups"] >= 1 and m["row_group_size"] % 1024 == 0


def test_close_and_reuse(golden):
    f = fstx.open(golden / "types.fstx")
    assert not f.closed
    f.close()
    f.close()
    assert f.closed
    assert f.num_rows == 200  # metadata stays
    with pytest.raises(ValueError):
        f.levels("fac")
    # many opens and closes: no handle is left behind (Windows would refuse the delete below)


def test_closed_file_can_be_deleted(golden, tmp_path):
    p = tmp_path / "x.fstx"
    shutil.copyfile(golden / "types.fstx", p)
    for _ in range(50):
        with fstx.open(p) as f:
            assert f.num_columns == 11
    p.unlink()
    assert not p.exists()


@pytest.mark.parametrize("row", REFUSE, ids=[r["file"] for r in REFUSE])
def test_newer_files_are_refused(golden, row):
    # files.csv holds the message R's read_fstx gives; a file this engine cannot read is refused when it
    # is opened or, for page-level numbers, only when its pages are read (not in this step).
    try:
        f = fstx.open(golden / row["file"])
    except fstx.FstxError as e:
        assert e.code == fstx.ERR_FORMAT
        assert re.search(row["message"], str(e)), str(e)
    else:
        f.close()
        pytest.skip("refused at read time, not at open (reading is not in this step)")


def test_missing_file(tmp_path):
    with pytest.raises(fstx.FstxError) as ei:
        fstx.open(tmp_path / "nope.fstx")
    assert ei.value.code == fstx.ERR_IO
    assert ei.value.column is None
    assert "nope.fstx" in str(ei.value)


def test_not_an_fstx_file(tmp_path):
    p = tmp_path / "junk.fstx"
    p.write_bytes(b"this is not an fstx file" * 40)
    with pytest.raises(fstx.FstxError) as ei:
        fstx.open(p)
    assert ei.value.code in (fstx.ERR_FORMAT, fstx.ERR_CORRUPT)


def test_truncated_file(golden, tmp_path):
    data = (golden / "types.fstx").read_bytes()
    p = tmp_path / "cut.fstx"
    p.write_bytes(data[: len(data) // 2])
    with pytest.raises(fstx.FstxError) as ei:
        fstx.open(p)
    assert ei.value.code in (fstx.ERR_FORMAT, fstx.ERR_CORRUPT, fstx.ERR_CHECKSUM)


def test_flipped_footer_byte(golden, tmp_path):
    data = bytearray((golden / "types.fstx").read_bytes())
    data[-200] ^= 0x40
    p = tmp_path / "flip.fstx"
    p.write_bytes(bytes(data))
    with pytest.raises(fstx.FstxError):
        fstx.open(p)
