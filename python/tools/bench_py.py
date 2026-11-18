"""Speed check of the Python package: fstx against pyarrow's Parquet and Feather on one table (README "Speed").

    python bench_py.py <out dir> [rows]

Writes <out dir>/bench.fstx (for bench_r.R), times each operation `REPS` times (median and min reported, page
cache warm: every file is read once before the timed reads) and prints one line per result, plus bench_py.json.
"""
import json
import os
import platform
import statistics
import sys
import time

import numpy as np
import pyarrow as pa
import pyarrow.feather as feather
import pyarrow.parquet as pq

import fstx

out = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 10_000_000
REPS = 5
rng = np.random.default_rng(20261118)


def make(n):
    cats = np.array([f"category-{i:02d}" for i in range(20)])
    ids = rng.integers(0, 2**31 - 1, n)
    return pa.table({
        "id": pa.array(np.arange(n, dtype=np.int32)),
        "qty": pa.array(rng.integers(0, 100, n, dtype=np.int32)),
        "price": pa.array(np.round(rng.gamma(2.0, 500.0, n), 2)),
        "score": pa.array(rng.random(n)),
        "flag": pa.array(rng.random(n) < 0.3),
        "city": pa.array(cats[rng.integers(0, 20, n)]),
        "sku": pa.array(np.char.add("SKU-", ids.astype(str))),
        "day": pa.array((19000 + rng.integers(0, 1500, n)).astype(np.int32), pa.int32()).view(pa.date32()),
        "ts": pa.array(1.7e15 + rng.integers(0, 10**13, n), pa.int64()).view(pa.timestamp("us", "Asia/Kolkata")),
        "seg": pa.array(cats[rng.integers(0, 5, n)]).dictionary_encode(),
    })


def timed(fn):
    ts = []
    for _ in range(REPS):
        t0 = time.perf_counter()
        fn()
        ts.append(time.perf_counter() - t0)
    return statistics.median(ts), min(ts)


t = make(n)
mem = t.nbytes
print(f"table: {n:,} rows, {t.num_columns} columns, {mem / 2**20:.0f} MiB in Arrow memory")
paths = {k: os.path.join(out, "bench." + k) for k in ("fstx", "parquet", "feather_lz4", "feather_none", "fstx_fast")}
writes = {
    "fstx (balanced)": lambda: fstx.write_fstx(t, paths["fstx"]),
    "fstx (fast, no codec)": lambda: fstx.write_fstx(t, paths["fstx_fast"], profile="fast"),
    "parquet (snappy)": lambda: pq.write_table(t, paths["parquet"]),
    "feather (lz4)": lambda: feather.write_feather(t, paths["feather_lz4"]),
    "feather (none)": lambda: feather.write_feather(t, paths["feather_none"], compression="uncompressed"),
}
files = {"fstx (balanced)": "fstx", "fstx (fast, no codec)": "fstx_fast", "parquet (snappy)": "parquet",
         "feather (lz4)": "feather_lz4", "feather (none)": "feather_none"}
reads = {
    "fstx (balanced)": lambda: fstx.read_fstx(paths["fstx"]),
    "fstx (fast, no codec)": lambda: fstx.read_fstx(paths["fstx_fast"]),
    "parquet (snappy)": lambda: pq.read_table(paths["parquet"]),
    "feather (lz4)": lambda: feather.read_table(paths["feather_lz4"]),
    "feather (none)": lambda: feather.read_table(paths["feather_none"], memory_map=False),
}
two = ["price", "city"]
reads2 = {
    "fstx (balanced)": lambda: fstx.read_fstx(paths["fstx"], columns=two),
    "fstx (balanced, as_factor=True)": lambda: fstx.read_fstx(paths["fstx"], columns=two, as_factor=True),
    "parquet (snappy)": lambda: pq.read_table(paths["parquet"], columns=two),
    "feather (lz4)": lambda: feather.read_table(paths["feather_lz4"], columns=two),
}
res = {"rows": n, "columns": t.num_columns, "arrow_mib": round(mem / 2**20), "reps": REPS,
       "python": platform.python_version(), "pyarrow": pa.__version__, "fstx": fstx.build_info(),
       "cpu_count": os.cpu_count(), "write": {}, "read": {}, "read_2_columns": {}, "read_to_pandas": {},
       "size_mib": {}}
for k, fn in writes.items():
    res["write"][k] = timed(fn)
    res["size_mib"][k] = round(os.path.getsize(paths[files[k]]) / 2**20, 1)
for k, fn in reads.items():
    fn()  # warm the page cache
    got = fn()
    assert got.num_rows == n
    res["read"][k] = timed(fn)
for k, fn in reads2.items():
    res["read_2_columns"][k] = timed(fn)
res["read_to_pandas"]["fstx: read_fstx + fstx.to_pandas"] = timed(lambda: fstx.to_pandas(fstx.read_fstx(paths["fstx"])))
res["read_to_pandas"]["parquet: read_table + to_pandas"] = timed(lambda: pq.read_table(paths["parquet"]).to_pandas())
# the same reads on one thread
res["read_1_thread"] = {
    "fstx (balanced)": timed(lambda: fstx.read_fstx(paths["fstx"], threads=1)),
    "parquet (snappy)": timed(lambda: pq.read_table(paths["parquet"], use_threads=False)),
    "feather (lz4)": timed(lambda: feather.read_table(paths["feather_lz4"], use_threads=False)),
}
res["write_1_thread"] = {
    "fstx (balanced)": timed(lambda: fstx.write_fstx(t, paths["fstx"] + ".1t", threads=1)),
    "parquet (snappy)": timed(lambda: pq.write_table(t, paths["parquet"] + ".1t")),
}
for sec in ("write", "write_1_thread", "read", "read_1_thread", "read_2_columns", "read_to_pandas"):
    for k, (med, mn) in res[sec].items():
        extra = f"  {res['size_mib'][k]:7.1f} MiB" if sec == "write" else ""
        print(f"{sec:15s} {k:38s} median {med:7.3f} s  min {mn:7.3f} s{extra}")
with open(os.path.join(out, "bench_py.json"), "w") as fh:
    json.dump(res, fh, indent=1, default=str)
for p in list(paths.values()) + [paths["fstx"] + ".1t", paths["parquet"] + ".1t"]:
    if os.path.exists(p) and not p.endswith("bench.fstx"):
        os.remove(p)
