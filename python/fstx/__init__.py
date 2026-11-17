"""fstx: fast, safe column-store files (the FST-X format).

Reading (design: DESIGN.md in the source tree):

    import fstx
    t = fstx.read_fstx("data.fstx")                       # pyarrow.Table
    t = fstx.read_fstx("data.fstx", columns=["a", "b"], rows=slice(0, 1000), filters=[("a", ">", 5)])
    df = fstx.to_pandas(t)                                 # or fstx.to_polars(t)

    m = fstx.metadata("data.fstx")      # FileMeta: nrow, ncol, schema, columns (ColumnMeta), ...
    with fstx.open("data.fstx") as f:   # the open file: f.schema, f.read(...), f.to_batches(...)
        for batch in f.to_batches(batch_rows=100_000):
            ...

Writing takes a pyarrow.Table or a pandas / polars DataFrame:

    fstx.write_fstx(table, "data.fstx")                 # profile=, compress=, level=, append=, int64=
"""
from __future__ import annotations

import atexit as _atexit

from . import _core
from ._file import ColumnMeta, File, FileMeta, metadata, open  # noqa: A004 (open is the package's own verb)
from ._write import write_fstx
from ._read import FstxWarning, read_fstx, set_options, to_pandas, to_polars

FstxError = _core.FstxError
FstxError.__doc__ = (
    "An fstx engine error. args = (message, code, column); .code is one of the fstx.ERR_* numbers, "
    ".column the column involved or None."
)
FstxError.message = property(lambda self: self.args[0] if self.args else "")
FstxError.code = property(lambda self: self.args[1] if len(self.args) > 1 else None)
FstxError.column = property(lambda self: self.args[2] if len(self.args) > 2 else None)
FstxError.__str__ = lambda self: self.args[0] if self.args else ""

ERR_IO = _core.ERR_IO
ERR_FORMAT = _core.ERR_FORMAT
ERR_CORRUPT = _core.ERR_CORRUPT
ERR_CHECKSUM = _core.ERR_CHECKSUM
ERR_ARG = _core.ERR_ARG
ERR_MEMORY = _core.ERR_MEMORY
ERR_HOST = _core.ERR_HOST
ERR_TRANSLATE = _core.ERR_TRANSLATE
ERR_INTERNAL = _core.ERR_INTERNAL

build_info = _core.build_info

try:
    from importlib.metadata import version as _version

    __version__ = _version("fstx")
except Exception:  # running from a source tree that was never installed
    __version__ = "0+unknown"

# The engine's worker threads must be stopped before the interpreter unloads the module.
_atexit.register(_core.shutdown)

__all__ = [
    "ColumnMeta",
    "File",
    "FileMeta",
    "FstxError",
    "FstxWarning",
    "build_info",
    "metadata",
    "open",
    "write_fstx",
    "read_fstx",
    "set_options",
    "to_pandas",
    "to_polars",
    "__version__",
]
