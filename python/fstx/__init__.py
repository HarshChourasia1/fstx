"""fstx: fast, safe column-store files (the FST-X format).

This first step opens a file and reports its metadata:

    import fstx
    with fstx.open("data.fstx") as f:
        f.num_rows, f.num_columns, f.column_names
        f.schema          # tuple of fstx.Column
        f.info            # file-level details

    fstx.metadata("data.fstx")   # the same as one dict

Reading columns and writing files come next (see DEV.md in the source tree).
"""
from __future__ import annotations

import atexit as _atexit

from . import _core
from ._file import Column, File, metadata, open  # noqa: A004 (open is the package's own verb)

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
    "Column",
    "File",
    "FstxError",
    "build_info",
    "metadata",
    "open",
    "__version__",
]
