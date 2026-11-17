"""Copy what the Python build needs from the R package into python/, so python/ builds on its own (sdist).

    python python/tools/make_core.py      # from anywhere in the git tree

1. python/core/  the engine: src/*.cpp and *.h without the R glue (r_*.cpp, r_lazy.h, cpp11.cpp; r_arrow.cpp
                 stays, CMakeLists.txt compiles it without R), src/vendor/ whole, and a DESCRIPTION holding
                 only the version. CMakeLists.txt uses core/ instead of ../src when core/ exists. core/ is in
                 .gitignore and goes into the sdist through sdist.include in pyproject.toml.
2. python/LICENSES/  refreshed from inst/COPYRIGHTS and src/vendor/<lib>/LICENSE. These copies are in git (the
                 wheel lists them as licence files, and a wheel built from the git tree has no core/);
                 `git diff python/LICENSES` after this script shows any drift, and the wheels workflow fails on it.
"""
import re
import shutil
import sys
from pathlib import Path

PY = Path(__file__).resolve().parents[1]
REPO = PY.parent
SRC = REPO / "src"
CORE = PY / "core"
LIBS = ["flatbuffers", "fsst", "highway", "lz4", "xxhash", "zstd"]


def r_glue(name: str) -> bool:
    return name == "cpp11.cpp" or (name.startswith("r_") and name != "r_arrow.cpp")


def main() -> int:
    if not (SRC / "fstx_core.h").exists():
        print(f"make_core: no engine in {SRC} (run it in the git tree)", file=sys.stderr)
        return 1
    m = re.search(r"^Version:\s*(\S+)", (REPO / "DESCRIPTION").read_text(encoding="utf-8"), re.M)
    if not m:
        print("make_core: no Version: line in DESCRIPTION", file=sys.stderr)
        return 1

    if CORE.exists():
        shutil.rmtree(CORE)
    CORE.mkdir()
    n = 0
    for f in sorted(SRC.iterdir()):
        if f.is_file() and f.suffix in (".cpp", ".h") and not r_glue(f.name):
            shutil.copy2(f, CORE / f.name)
            n += 1
    shutil.copytree(SRC / "vendor", CORE / "vendor")
    (CORE / "DESCRIPTION").write_text(f"Package: fstx\nVersion: {m.group(1)}\n", encoding="utf-8")

    lic = PY / "LICENSES"
    lic.mkdir(exist_ok=True)
    shutil.copyfile(REPO / "inst" / "COPYRIGHTS", lic / "COPYRIGHTS")
    for lib in LIBS:
        (lic / lib).mkdir(exist_ok=True)
        shutil.copyfile(SRC / "vendor" / lib / "LICENSE", lic / lib / "LICENSE")

    print(f"make_core: engine {m.group(1)}, {n} files + vendor/ -> {CORE}; licences -> {lic}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
