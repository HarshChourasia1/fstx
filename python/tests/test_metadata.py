"""Opening files and reading metadata, against the golden files written by the R package."""
import re
import shutil

import pytest

import fstx
from conftest import CASES, FORMAT_READ, FORMAT_REFUSE, GOLDEN, ids

ALL_READ = CASES + FORMAT_READ


def test_golden_sets_are_there():
    assert len(CASES) >= 30
    if not FORMAT_READ:
        pytest.skip("the R package's format golden files are not next to this tree")
    assert len(FORMAT_READ) >= 12 and len(FORMAT_REFUSE) >= 10


@pytest.mark.parametrize("path,want", ALL_READ, ids=ids(ALL_READ))
def test_file_metadata(path, want):
    m = fstx.metadata(path)
    assert isinstance(m, fstx.FileMeta)
    assert (m.nrow, m.ncol) == (want["nrow"], want["ncol"])
    assert m.profile == want["profile"]
    assert m.row_groups == want["row_groups"]
    assert m.file_size == want["file_bytes"] == path.stat().st_size
    assert m.format_version == 3
    assert m.row_group_size > 0 and m.row_group_size % 1024 == 0
    assert len(m.columns) == m.ncol
    assert sum(c.pages for c in m.columns) == m.pages
    blob = want["table"].get("blob_hex")
    assert (m.table_attrs.hex() if m.table_attrs else None) == blob


@pytest.mark.parametrize("path,want", ALL_READ, ids=ids(ALL_READ))
def test_column_metadata(path, want):
    m = fstx.metadata(path)
    assert m.names == [c["name"] for c in want["columns"]]
    for i, (col, w) in enumerate(zip(m.columns, want["columns"])):
        s = w["stored"]
        assert isinstance(col, fstx.ColumnMeta)
        assert col.index == i
        assert (col.phys, col.logical) == (s["phys"], s["logical"]), col
        assert col.tzone == (s.get("tzone", "") if s["tzone_present"] else None), col
        if s["logical"] == "factor":
            assert col.levels == s["levels"], col
            assert col.ordered == s["ordered"], col
        else:
            assert col.levels is None and not col.ordered
        assert (col.attrs.hex() if col.attrs else None) == s.get("blob_hex"), col
        assert (col.attrs is None) == (s["blob"] == "none"), col
        assert col.pages == len(w["pages"]), col
        assert col.raw_bytes >= 0 and col.compressed_bytes >= 0


def test_open_file_object():
    with fstx.open(GOLDEN / "factors.fstx") as f:
        m = f.metadata
        assert f.shape == (f.num_rows, f.num_columns) == (m.nrow, m.ncol)
        assert f.column_names == m.names
        assert f.columns == m.columns
        assert f.column(0) is m.columns[0]
        assert f.column(-1) is m.columns[-1]
        assert f.column(m.names[1]) is m.columns[1]
        with pytest.raises(KeyError):
            f.column("no such column")
        with pytest.raises(IndexError):
            f.column(m.ncol)
        assert f"{m.nrow} rows x {m.ncol} columns" in repr(f)
        assert not f.closed
    assert f.closed and "(closed)" in repr(f)
    f.close()  # twice is fine
    assert f.num_rows == m.nrow and f.column(m.names[0]).index == 0  # metadata stays after close


def test_duplicate_and_empty_names():
    with fstx.open(GOLDEN / "names.fstx") as f:
        names = f.column_names
        assert len(names) != len(set(names)) or "" in names  # what the case is about
        for n in set(names):
            assert f.column(n).index == names.index(n)  # first match, as R


def test_non_ascii_path(tmp_path):
    p = tmp_path / "dépôt हिंदी 日本" / "tâble Ω.fstx"
    p.parent.mkdir()
    shutil.copyfile(GOLDEN / "basic.fstx", p)
    want = fstx.metadata(GOLDEN / "basic.fstx")
    assert fstx.metadata(p).names == want.names  # a pathlib.Path
    assert fstx.metadata(str(p)).nrow == want.nrow
    assert fstx.metadata(p).path == str(p)


def test_closed_file_can_be_deleted(tmp_path):
    # no handle or mapping is left behind (Windows refuses to delete a mapped file)
    p = tmp_path / "x.fstx"
    shutil.copyfile(GOLDEN / "basic.fstx", p)
    for _ in range(50):
        with fstx.open(p) as f:
            assert f.num_columns > 0
    fstx.metadata(p)
    p.unlink()
    assert not p.exists()


@pytest.mark.parametrize("path,want", FORMAT_REFUSE, ids=ids(FORMAT_REFUSE))
def test_newer_files_are_refused(path, want):
    # message_regex is what R's read_fstx() says. A number the engine does not know is refused when the
    # file is opened or, for page-level numbers, only when the page is read (reading is not in this step).
    try:
        f = fstx.open(path)
    except fstx.FstxError as e:
        assert e.code == fstx.ERR_FORMAT, (e.code, str(e))
        assert re.search(want["message_regex"], str(e)), str(e)
    else:
        f.close()
        pytest.skip("refused at read time, not at open")


def test_missing_file(tmp_path):
    with pytest.raises(fstx.FstxError) as ei:
        fstx.open(tmp_path / "nope.fstx")
    e = ei.value
    assert e.code == fstx.ERR_IO and e.column is None
    assert "nope.fstx" in str(e) and e.message == str(e)


def test_not_an_fstx_file(tmp_path):
    p = tmp_path / "junk.fstx"
    p.write_bytes(b"this is not an fstx file" * 40)
    with pytest.raises(fstx.FstxError) as ei:
        fstx.open(p)
    assert ei.value.code == fstx.ERR_FORMAT
    assert "not an FSTX file" in str(ei.value)


def test_truncated_file(tmp_path):
    data = (GOLDEN / "basic.fstx").read_bytes()
    p = tmp_path / "cut.fstx"
    p.write_bytes(data[: len(data) // 2])
    with pytest.raises(fstx.FstxError) as ei:
        fstx.open(p)
    assert ei.value.code in (fstx.ERR_FORMAT, fstx.ERR_CORRUPT, fstx.ERR_CHECKSUM)


def test_flipped_footer_byte(tmp_path):
    data = bytearray((GOLDEN / "basic.fstx").read_bytes())
    data[-100] ^= 0x40  # inside the footer, which is checksummed
    p = tmp_path / "flip.fstx"
    p.write_bytes(bytes(data))
    with pytest.raises(fstx.FstxError) as ei:
        fstx.open(p)
    assert ei.value.code in (fstx.ERR_CORRUPT, fstx.ERR_CHECKSUM)
