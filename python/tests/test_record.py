"""Record serialization."""
import math

import numpy as np
import pytest

from a0seam import record


def test_numbers_use_six_decimals_and_no_negative_zero():
    text = record.dumps({"a": 1.0, "b": -0.0000001, "c": 1 / 3, "d": 12, "e": np.float64(2.5), "f": np.int64(7)})
    assert text == '{\n  "a": 1.000000,\n  "b": 0.000000,\n  "c": 0.333333,\n  "d": 12,\n  "e": 2.500000,\n  "f": 7\n}\n'


def test_layout_is_fixed():
    doc = {"x": [1.5, 2.5, 3.5], "k": [{"u": True, "v": None}, {"u": False, "v": "s"}], "e": {}}
    assert record.dumps(doc) == (
        '{\n'
        '  "x": [1.500000, 2.500000, 3.500000],\n'
        '  "k": [\n'
        '    {\n      "u": true,\n      "v": null\n    },\n'
        '    {\n      "u": false,\n      "v": "s"\n    }\n'
        '  ],\n'
        '  "e": {}\n'
        '}\n')


@pytest.mark.parametrize("bad", [math.nan, math.inf, -math.inf])
def test_non_finite_numbers_are_refused(bad):
    with pytest.raises(ValueError):
        record.dumps({"x": bad})


def test_digest_ignores_only_the_environment_block():
    base = {"schema": "s", "environment": {"python": "3.12.10"}, "v": 1.0}
    same = {"schema": "s", "environment": {"python": "3.13.0"}, "v": 1.0}
    other = {"schema": "s", "environment": {"python": "3.12.10"}, "v": 1.000001}
    assert record.digest_without_environment(base) == record.digest_without_environment(same)
    assert record.digest_without_environment(base) != record.digest_without_environment(other)
