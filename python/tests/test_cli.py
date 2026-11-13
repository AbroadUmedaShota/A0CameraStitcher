"""The command line as a program: what the plan says about its request, what a failed run leaves behind,
what the output shows, and what importing the package does to the process."""
import hashlib
import io
import json
import os
import subprocess
import sys

import pytest

import synth
from a0seam import cli

PYTHON_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCHER = os.path.join(PYTHON_DIR, "estimate_seam_row_gain.py")
SCENE = synth.Scene(bars=False, orange=False, dark=False)
SENTINEL = b'{"sentinel": "an earlier plan"}\n'


def make_request(tmp_path, name="req"):
    folder = str(tmp_path / name)
    path = synth.write_request(folder, SCENE)
    return folder, path


def sha256_of(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def read_text(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def test_plan_names_the_request_file_it_was_made_from(tmp_path):
    folder, path = make_request(tmp_path)
    out = os.path.join(folder, "plan.json")
    assert cli.main([path, "--out", out]) == 0
    assert json.loads(read_text(out))["requestSha256"] == sha256_of(path)
    # A request that differs by one byte is a different request.
    with open(path, "a", encoding="utf-8") as f:
        f.write("\n")
    assert cli.main([path, "--out", out]) == 0
    assert json.loads(read_text(out))["requestSha256"] == sha256_of(path)


def test_a_declined_plan_replaces_an_earlier_one(tmp_path):
    folder, path = make_request(tmp_path)
    out = os.path.join(folder, "plan.json")
    with open(out, "wb") as f:
        f.write(SENTINEL)
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    doc["design"] = {"minWindows": 1000000, "maxUnusedFraction": 1.0}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    assert cli.main([path, "--out", out]) == 3
    assert json.loads(read_text(out))["state"] == "declined"


def test_a_run_that_fails_keeps_the_plan_that_was_there(tmp_path, monkeypatch):
    folder, path = make_request(tmp_path)
    out = os.path.join(folder, "plan.json")

    def put_sentinel():
        with open(out, "wb") as f:
            f.write(SENTINEL)

    put_sentinel()
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    doc["raster"]["dpi"] = 0                                    # not acceptable: exit 2
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    assert cli.main([path, "--out", out]) == 2
    with open(out, "rb") as f:
        assert f.read() == SENTINEL

    _, path = make_request(tmp_path, "second")                   # acceptable, but the estimate fails: exit 1
    out = os.path.join(os.path.dirname(path), "plan.json")
    put_sentinel()

    def boom(_req):
        raise RuntimeError("boom")

    monkeypatch.setattr(cli, "estimate_plan", boom)
    assert cli.main([path, "--out", out]) == 1
    with open(out, "rb") as f:
        assert f.read() == SENTINEL


def test_status_line_and_errors_do_not_show_paths(tmp_path, capsys):
    folder, path = make_request(tmp_path)
    assert cli.main([path, "--out", os.path.join(folder, "plan.json")]) == 0
    shown = capsys.readouterr()
    assert shown.out.startswith("state=usable knots=") and str(tmp_path) not in shown.out + shown.err

    missing = os.path.join(str(tmp_path), "no-such-folder", "plan.json")
    assert cli.main([path, "--out", missing]) == 1                # the folder of the plan does not exist
    shown = capsys.readouterr()
    assert shown.err.startswith("error: ") and str(tmp_path) not in shown.out + shown.err

    assert cli.main([os.path.join(folder, "absent.json")]) == 2
    shown = capsys.readouterr()
    assert shown.err.startswith("input error: ") and str(tmp_path) not in shown.out + shown.err


@pytest.mark.parametrize("path_text", [
    "C:\\Users\\someone\\Desktop\\file.txt",
    "C:/Users/someone/Desktop/file.txt",
    "\\\\server\\share\\file.txt",
    "/home/someone/file.txt",
])
def test_paths_in_an_unexpected_failure_message_are_replaced(tmp_path, monkeypatch, capsys, path_text):
    _, path = make_request(tmp_path)

    def boom(_req):
        raise RuntimeError(f"cannot open {path_text} for reading")

    monkeypatch.setattr(cli, "estimate_plan", boom)
    assert cli.main([path]) == 1
    err = capsys.readouterr().err
    assert "someone" not in err and "RuntimeError: cannot open <path> for reading" in err


def test_characters_the_console_cannot_show_do_not_turn_a_good_run_into_a_failure(tmp_path, monkeypatch):
    folder, path = make_request(tmp_path)
    out_folder = tmp_path / "snow \u2603"
    out_folder.mkdir()
    out = str(out_folder / "plan.json")
    raw_out, raw_err = io.BytesIO(), io.BytesIO()
    monkeypatch.setattr(sys, "stdout", io.TextIOWrapper(raw_out, encoding="cp932", errors="strict"))
    monkeypatch.setattr(sys, "stderr", io.TextIOWrapper(raw_err, encoding="cp932", errors="strict"))
    assert cli.main([path, "--out", out]) == 0
    assert os.path.isfile(out)

    def boom(_req):
        raise RuntimeError("snow \u2603")

    monkeypatch.setattr(cli, "estimate_plan", boom)
    assert cli.main([path, "--out", out]) == 1                    # reported, not a UnicodeEncodeError
    sys.stderr.flush()
    assert b"snow \\u2603" in raw_err.getvalue()


def run_isolated(code):
    done = subprocess.run([sys.executable, "-I", "-c", code], capture_output=True, text=True, timeout=120)
    assert done.returncode == 0, done.stderr
    return done.stdout.strip()


def test_importing_the_package_leaves_the_environment_alone():
    # Only the entry modules (the launcher and a0seam.estimate) pin the thread pools, not the library modules.
    code = ("import os, sys; sys.path.insert(0, %r); os.environ['OMP_NUM_THREADS'] = '7'; "
            "import a0seam, a0seam.cli, a0seam.request, a0seam.record; print(os.environ['OMP_NUM_THREADS'])" % PYTHON_DIR)
    assert run_isolated(code) == "7"


def test_the_launchers_pin_the_thread_pools():
    code = ("import os, sys; sys.path.insert(0, %r); os.environ['OMP_NUM_THREADS'] = '7'; import a0seam; "
            "a0seam.pin_single_thread(); print(os.environ['OMP_NUM_THREADS'], os.environ['OPENBLAS_NUM_THREADS'])"
            % PYTHON_DIR)
    assert run_isolated(code) == "1 1"
    for source in (LAUNCHER, os.path.join(PYTHON_DIR, "a0seam", "estimate.py")):
        text = read_text(source)
        assert text.index("pin_single_thread()") < text.index("import main")


def test_launcher_runs_with_the_flags_the_host_should_use(tmp_path):
    folder, path = make_request(tmp_path)
    out = os.path.join(folder, "plan.json")
    done = subprocess.run([sys.executable, "-I", "-X", "utf8", LAUNCHER, "--out", out, "--", path],
                          capture_output=True, text=True, timeout=120)
    assert done.returncode == 0, done.stderr
    assert str(tmp_path) not in done.stdout + done.stderr
    assert json.loads(read_text(out))["state"] == "usable"
