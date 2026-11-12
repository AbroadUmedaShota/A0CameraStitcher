import json
import os

import pytest

import synth
from a0seam import cli


@pytest.fixture
def run_scene(tmp_path):
    """Write a scene as a request in its own folder, run the CLI in-process, return (exit code, record or None, folder)."""
    counter = [0]

    def run(scene, **kw):
        counter[0] += 1
        folder = str(tmp_path / f"case{counter[0]}")
        request = synth.write_request(folder, scene)
        out = os.path.join(folder, "seam-row-gain.json")
        code = cli.main([request, "--out", out])
        record = None
        if os.path.exists(out):
            with open(out, encoding="utf-8") as f:
                record = json.load(f)
        return code, record, folder

    return run
