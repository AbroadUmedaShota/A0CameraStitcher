"""Bad input: exit code 2, no record written; other failures: exit code 1."""
import hashlib
import json
import os

import pytest

import synth
from a0seam import cli

SCENE = synth.Scene(bars=False, orange=False, dark=False)


def make_request(tmp_path):
    folder = str(tmp_path / "req")
    path = synth.write_request(folder, SCENE)
    with open(path, encoding="utf-8") as f:
        return folder, path, json.load(f)


def save(path, doc):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)


def run(path):
    out = os.path.join(os.path.dirname(path), "seam-row-gain.json")
    code = cli.main([path, "--out", out])
    return code, out


def set_in(doc, keys, value):
    for k in keys[:-1]:
        doc = doc[k]
    doc[keys[-1]] = value


def delete_in(doc, keys):
    for k in keys[:-1]:
        doc = doc[k]
    del doc[keys[-1]]


MUTATIONS = {
    "wrong schema": lambda d: set_in(d, ["schema"], "a0.seam-estimate-request.v2"),
    "unknown top key": lambda d: set_in(d, ["extra"], 1),
    "missing strip": lambda d: delete_in(d, ["strip"]),
    "missing output": lambda d: delete_in(d, ["output"]),
    "seam outside": lambda d: set_in(d, ["seamColumn"], SCENE.width),
    "seam is a float": lambda d: set_in(d, ["seamColumn"], 260.5),
    "seam is a bool": lambda d: set_in(d, ["seamColumn"], True),
    "height zero": lambda d: set_in(d, ["output", "height"], 0),
    "height too small": lambda d: set_in(d, ["output", "height"], 40),
    "pitch zero": lambda d: set_in(d, ["output", "pixelPitchUm"], 0),
    "pitch as text": lambda d: set_in(d, ["output", "pixelPitchUm"], "141"),
    "unknown output key": lambda d: set_in(d, ["output", "dpi"], 180),
    "gain negative": lambda d: set_in(d, ["globalGainBgr"], [1.2, -1.0, 1.1]),
    "gain wrong length": lambda d: set_in(d, ["globalGainBgr"], [1.2, 1.1]),
    "strip does not cover the seam": lambda d: set_in(d, ["strip", "firstColumn"], 200),
    "strip wrong size": lambda d: set_in(d, ["strip", "width"], SCENE.width - 1),
    "strip hash not hex": lambda d: set_in(d, ["strip", "sha256"], "XYZ"),
    "strip hash wrong": lambda d: set_in(d, ["strip", "sha256"], "0" * 64),
    "strip hash upper case": lambda d: set_in(d, ["strip", "sha256"], d["strip"]["sha256"].upper()),
    "strip file missing": lambda d: set_in(d, ["strip", "file"], "nothing.u16"),
    "strip file leaves the folder": lambda d: set_in(d, ["strip", "file"], "../strip.u16"),
    "strip file absolute": lambda d: set_in(d, ["strip", "file"], os.path.abspath("strip.u16")),
    "default gain hash wrong": lambda d: set_in(d, ["defaultRowGain", "sha256"], "1" * 64),
    "unknown design key": lambda d: set_in(d, ["design"], {"stepsPx": 8}),
    "design not an object": lambda d: set_in(d, ["design"], [1]),
    "design gain limit not above 1": lambda d: set_in(d, ["design"], {"gainLimit": 1.0}),
    "design int given as float": lambda d: set_in(d, ["design"], {"stepPx": 8.5}),
    "design radii out of order": lambda d: set_in(d, ["design"], {"meanRadiusPx": 30}),
    "design min windows too small": lambda d: set_in(d, ["design"], {"minWindows": 1}),
}


@pytest.mark.parametrize("name", sorted(MUTATIONS))
def test_bad_request_exits_2_and_writes_no_record(tmp_path, name):
    _, path, doc = make_request(tmp_path)
    MUTATIONS[name](doc)
    save(path, doc)
    code, out = run(path)
    assert code == 2
    assert not os.path.exists(out)


def test_tampered_strip_is_caught_by_its_hash(tmp_path):
    folder, path, _ = make_request(tmp_path)
    with open(os.path.join(folder, "strip.u16"), "r+b") as f:
        f.seek(1000)
        byte = f.read(1)
        f.seek(1000)
        f.write(bytes([byte[0] ^ 1]))
    code, out = run(path)
    assert code == 2 and not os.path.exists(out)


def test_non_positive_default_gain_exits_2(tmp_path):
    folder, path, doc = make_request(tmp_path)
    bad = SCENE.default_row_gain()
    bad[5, 1] = 0.0
    gain_path = os.path.join(folder, "default-gain.f32")
    bad.tofile(gain_path)
    doc["defaultRowGain"]["sha256"] = hashlib.sha256(open(gain_path, "rb").read()).hexdigest()
    save(path, doc)
    code, out = run(path)
    assert code == 2 and not os.path.exists(out)


@pytest.mark.parametrize("text", ["", "not json", "[1, 2]", "{\"schema\": NaN}", "{\"schema\": 1, \"schema\": 2}"])
def test_unparseable_request_exits_2(tmp_path, text):
    path = str(tmp_path / "request.json")
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    code, out = run(path)
    assert code == 2 and not os.path.exists(out)


def test_missing_request_file_exits_2(tmp_path):
    code, out = run(str(tmp_path / "absent.json"))
    assert code == 2 and not os.path.exists(out)


def test_request_without_arguments_is_a_usage_error():
    with pytest.raises(SystemExit) as e:
        cli.main([])
    assert e.value.code == 2


def test_unexpected_failure_exits_1_and_writes_no_record(tmp_path, monkeypatch, capsys):
    _, path, _ = make_request(tmp_path)

    def boom(_req):
        raise RuntimeError("boom")

    monkeypatch.setattr(cli, "estimate", boom)
    code, out = run(path)
    assert code == 1 and not os.path.exists(out)
    assert "boom" in capsys.readouterr().err


def test_unwritable_output_exits_1(tmp_path):
    _, path, _ = make_request(tmp_path)
    code = cli.main([path, "--out", str(tmp_path / "no-such-folder" / "record.json")])
    assert code == 1


def test_a_failed_run_does_not_leave_temporary_files(tmp_path):
    folder, path, doc = make_request(tmp_path)
    doc["seamColumn"] = -1
    save(path, doc)
    run(path)
    assert not [n for n in os.listdir(folder) if n.endswith(".tmp")]


def test_default_record_path_is_next_to_the_request(tmp_path):
    folder, path, _ = make_request(tmp_path)
    assert cli.main([path]) == 0
    assert os.path.isfile(os.path.join(folder, cli.DEFAULT_RECORD_NAME))
