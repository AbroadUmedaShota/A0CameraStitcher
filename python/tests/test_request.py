"""Bad input: exit code 2, no plan written; other failures: exit code 1."""
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
    out = os.path.join(os.path.dirname(path), cli.DEFAULT_PLAN_NAME)
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


# name -> (mutation, text the error message must contain)
MUTATIONS = {
    "wrong schema": (lambda d: set_in(d, ["schema"], "a0.seam-estimate-request.v2"), "schema must be"),
    "unknown top key": (lambda d: set_in(d, ["extra"], 1), "unknown keys"),
    "old output block": (lambda d: set_in(d, ["output"], {"width": 520}), "unknown keys"),
    "old default row gain": (lambda d: set_in(d, ["defaultRowGain"], {"file": "g.f32", "sha256": "0" * 64}),
                             "unknown keys"),
    "missing strip": (lambda d: delete_in(d, ["strip"]), "strip is missing"),
    "missing raster": (lambda d: delete_in(d, ["raster"]), "raster is missing"),
    "missing bindings": (lambda d: delete_in(d, ["bindings"]), "bindings is missing"),
    "seam outside": (lambda d: set_in(d, ["raster", "seamColumn"], SCENE.width), "seamColumn"),
    "seam is a float": (lambda d: set_in(d, ["raster", "seamColumn"], 260.5), "seamColumn"),
    "seam is a bool": (lambda d: set_in(d, ["raster", "seamColumn"], True), "seamColumn"),
    "height zero": (lambda d: set_in(d, ["raster", "heightPx"], 0), "heightPx"),
    "height too small": (lambda d: set_in(d, ["raster", "heightPx"], 40), "too small"),
    "height above the limit": (lambda d: set_in(d, ["raster", "heightPx"], 32769), "heightPx"),
    "width zero": (lambda d: set_in(d, ["raster", "widthPx"], 0), "widthPx"),
    "dpi zero": (lambda d: set_in(d, ["raster", "dpi"], 0), "dpi"),
    "dpi is a float": (lambda d: set_in(d, ["raster", "dpi"], 180.5), "dpi"),
    "dpi as text": (lambda d: set_in(d, ["raster", "dpi"], "180"), "dpi"),
    "dpi above the limit": (lambda d: set_in(d, ["raster", "dpi"], 4801), "dpi"),
    "topUm is a float": (lambda d: set_in(d, ["raster", "topUm"], 12500.5), "topUm"),
    "topUm is huge": (lambda d: set_in(d, ["raster", "topUm"], 10**12), "topUm"),
    "unknown raster key": (lambda d: set_in(d, ["raster", "pixelPitchUm"], 141.1), "unknown keys"),
    "raster lacks dpi": (lambda d: delete_in(d, ["raster", "dpi"]), "raster is missing"),
    "gain negative": (lambda d: set_in(d, ["globalGainBgr"], [1.2, -1.0, 1.1]), "globalGainBgr"),
    "gain wrong length": (lambda d: set_in(d, ["globalGainBgr"], [1.2, 1.1]), "globalGainBgr"),
    "gain above the readers range": (lambda d: set_in(d, ["globalGainBgr"], [1.2, 4.5, 1.1]), "globalGainBgr"),
    "gain below the readers range": (lambda d: set_in(d, ["globalGainBgr"], [1.2, 0.2, 1.1]), "globalGainBgr"),
    "strip does not cover the seam": (lambda d: (set_in(d, ["strip", "firstColumn"], 200), set_in(d, ["strip", "width"], 320)), "must cover"),
    "strip wrong size": (lambda d: set_in(d, ["strip", "width"], SCENE.width - 1), "bytes"),
    "strip hash not hex": (lambda d: set_in(d, ["strip", "sha256"], "XYZ"), "sha256"),
    "strip hash wrong": (lambda d: set_in(d, ["strip", "sha256"], "0" * 64), "does not match"),
    "strip hash upper case": (lambda d: set_in(d, ["strip", "sha256"], d["strip"]["sha256"].upper()), "sha256"),
    "strip file missing": (lambda d: set_in(d, ["strip", "file"], "nothing.u16"), "does not exist"),
    "strip file leaves the folder": (lambda d: set_in(d, ["strip", "file"], "../strip.u16"), "leaves"),
    "strip file absolute": (lambda d: set_in(d, ["strip", "file"], os.path.abspath("strip.u16")), "relative"),
    "b samples missing": (lambda d: delete_in(d, ["strip", "bSamples"]), "bSamples"),
    "b samples already scaled": (lambda d: set_in(d, ["strip", "bSamples"], "mapped-after-color-gain"), "bSamples"),
    "bindings empty": (lambda d: set_in(d, ["bindings"], []), "bindings"),
    "bindings not a list": (lambda d: set_in(d, ["bindings"], {"kind": "condition-v3"}), "bindings"),
    "bindings unknown kind": (lambda d: set_in(d, ["bindings", 1, "kind"], "capture-original-c"), "known kind"),
    "bindings repeat a kind": (lambda d: set_in(d, ["bindings", 2, "kind"], "capture-original-a"), "repeats"),
    "bindings lack a kind": (lambda d: d["bindings"].pop(0), "lacks"),
    "bindings sha upper case": (lambda d: set_in(d, ["bindings", 0, "sha256"], "A" * 64), "sha256"),
    "bindings sha short": (lambda d: set_in(d, ["bindings", 0, "sha256"], "ab"), "sha256"),
    "bindings extra key": (lambda d: set_in(d, ["bindings", 0, "note"], "x"), "exactly"),
    "unknown design key": (lambda d: set_in(d, ["design"], {"stepsPx": 8}), "unknown key"),
    "old design name for the limit": (lambda d: set_in(d, ["design"], {"gainLimit": 1.2}), "unknown key"),
    "design not an object": (lambda d: set_in(d, ["design"], [1]), "design"),
    "design ratio not above 1": (lambda d: set_in(d, ["design"], {"maxScaleRatioToGlobal": 1.0}), "maxScaleRatio"),
    "design ratio above the readers": (lambda d: set_in(d, ["design"], {"maxScaleRatioToGlobal": 1.3}),
                                       "maxScaleRatio"),
    "design int given as float": (lambda d: set_in(d, ["design"], {"stepPx": 8.5}), "integer"),
    "design radii out of order": (lambda d: set_in(d, ["design"], {"meanRadiusPx": 30}), "radii"),
    "design min windows too small": (lambda d: set_in(d, ["design"], {"minWindows": 1}), "minWindows"),
    "too many knots": (lambda d: (set_in(d, ["raster", "heightPx"], 5000), set_in(d, ["design"], {"knotSpacingRows": 1})),
                       "more than 4096 knots"),
}


@pytest.mark.parametrize("name", sorted(MUTATIONS))
def test_bad_request_exits_2_and_writes_no_plan(tmp_path, capsys, name):
    mutate, expected = MUTATIONS[name]
    _, path, doc = make_request(tmp_path)
    mutate(doc)
    save(path, doc)
    code, out = run(path)
    assert code == 2
    assert not os.path.exists(out)
    assert expected in capsys.readouterr().err


def test_strip_wider_than_the_limit_is_refused(tmp_path, capsys):
    _, path, doc = make_request(tmp_path)
    doc["raster"]["widthPx"] = 2000
    doc["strip"]["width"] = 1025
    save(path, doc)
    code, out = run(path)
    assert code == 2 and not os.path.exists(out)
    assert "at most 1024" in capsys.readouterr().err


def test_tampered_strip_is_caught_by_its_hash(tmp_path):
    folder, path, _ = make_request(tmp_path)
    with open(os.path.join(folder, "strip.u16"), "r+b") as f:
        f.seek(1000)
        byte = f.read(1)
        f.seek(1000)
        f.write(bytes([byte[0] ^ 1]))
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


def test_unexpected_failure_exits_1_and_writes_no_plan(tmp_path, monkeypatch, capsys):
    _, path, _ = make_request(tmp_path)

    def boom(_req):
        raise RuntimeError("boom")

    monkeypatch.setattr(cli, "estimate", boom)
    code, out = run(path)
    assert code == 1 and not os.path.exists(out)
    assert "boom" in capsys.readouterr().err


def test_unwritable_output_exits_1(tmp_path):
    _, path, _ = make_request(tmp_path)
    code = cli.main([path, "--out", str(tmp_path / "no-such-folder" / "plan.json")])
    assert code == 1


def test_a_failed_run_does_not_leave_temporary_files(tmp_path):
    folder, path, doc = make_request(tmp_path)
    doc["raster"]["seamColumn"] = -1
    save(path, doc)
    run(path)
    assert not [n for n in os.listdir(folder) if n.endswith(".tmp")]


def test_default_plan_path_is_next_to_the_request(tmp_path):
    folder, path, _ = make_request(tmp_path)
    assert cli.main([path]) == 0
    assert os.path.isfile(os.path.join(folder, cli.DEFAULT_PLAN_NAME))
