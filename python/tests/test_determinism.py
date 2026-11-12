"""Same input, same output bytes."""
import os
import subprocess
import sys

import synth
from a0seam import cli, record
from a0seam.request import load_request

PYTHON_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# SHA-256 of the record of the default synthetic scene without its environment
# block (record.digest_without_environment). It pins the numeric result: when a
# change to the estimator or a numpy upgrade moves it, review the difference in
# the record and update this value in the same commit.
PINNED_DIGEST = "930b6d5326e597089f919908f3c6d8a74a00edbfe62188e82f3c691b5b5e535a"


def estimate_scene(folder, scene):
    path = synth.write_request(str(folder), scene)
    return load_request(path), path


def test_two_runs_write_identical_bytes(tmp_path):
    scene = synth.Scene()
    outputs = []
    for name in ("one", "two"):
        _, path = estimate_scene(tmp_path / name, scene)
        out = str(tmp_path / name / "record.json")
        assert cli.main([path, "--out", out]) == 0
        with open(out, "rb") as f:
            outputs.append(f.read())
    assert outputs[0] == outputs[1]


def test_record_digest_is_pinned(tmp_path):
    req, _ = estimate_scene(tmp_path, synth.Scene())
    assert record.digest_without_environment(cli.estimate(req)) == PINNED_DIGEST


def test_fresh_process_gives_the_same_bytes_whatever_the_thread_environment(tmp_path):
    scene = synth.Scene()
    req, path = estimate_scene(tmp_path / "in", scene)
    expected = record.dumps(cli.estimate(req)).encode("utf-8")
    launcher = os.path.join(PYTHON_DIR, "estimate_seam_row_gain.py")
    for threads in ("1", "4"):
        out = str(tmp_path / f"out-{threads}.json")
        env = dict(os.environ, OMP_NUM_THREADS=threads, OPENBLAS_NUM_THREADS=threads, MKL_NUM_THREADS=threads)
        done = subprocess.run([sys.executable, "-I", launcher, path, "--out", out], env=env,
                              capture_output=True, text=True, timeout=120)
        assert done.returncode == 0, done.stderr
        with open(out, "rb") as f:
            assert f.read() == expected


def test_module_entry_point_and_exit_codes(tmp_path):
    _, path = estimate_scene(tmp_path / "in", synth.Scene())
    out = str(tmp_path / "out.json")
    ok = subprocess.run([sys.executable, "-m", "a0seam.estimate", path, "--out", out],
                        cwd=PYTHON_DIR, capture_output=True, text=True, timeout=120)
    assert ok.returncode == 0 and os.path.isfile(out)
    bad = subprocess.run([sys.executable, "-m", "a0seam.estimate", str(tmp_path / "absent.json")],
                         cwd=PYTHON_DIR, capture_output=True, text=True, timeout=120)
    assert bad.returncode == 2 and "input error" in bad.stderr


def test_record_has_no_machine_specific_text_besides_the_environment_block(tmp_path):
    req, _ = estimate_scene(tmp_path, synth.Scene())
    text = record.dumps(cli.estimate(req))
    assert str(tmp_path) not in text
    assert "\r" not in text and text.endswith("}\n")
