"""The build itself: engine version, SIMD dispatch and the engine's kernel self-test under every target."""
import pytest

import fstx
from fstx import _core


def test_build_info():
    b = fstx.build_info()
    assert b["engine_version"].count(".") == 2
    assert b["compiler"]
    assert b["hardware_threads"] >= 1
    assert b["simd_compiled"], "no SIMD target compiled in"
    assert set(b["simd_cpu"]) <= set(b["simd_compiled"])
    print(b)


def _targets():
    return ["off"] + [t.lower() for t in _core.build_info()["simd_cpu"]]


@pytest.mark.parametrize("target", _targets())
def test_kernel_selftest(target):
    """Every SIMD kernel gives the plain loop's bits, under each target this CPU runs."""
    try:
        used = _core.set_simd(target)
        assert used.lower() == ("scalar" if target == "off" else target)
        if target != "off":
            assert _core.build_info()["simd_dispatch"].lower() == target
        res = _core.kernel_selftest(seed=20261117, iters=10)
        assert res, "the self-test ran no kernel"
        bad = [r for r in res if r[2] != 0 or r[1] == 0]
        assert not bad, bad
    finally:
        _core.set_simd("auto")


def test_set_simd_rejects_unknown():
    with pytest.raises(ValueError):
        _core.set_simd("avx9000")
