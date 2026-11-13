"""The plan file: contract fields, names, the check against B' that already carries a colour scale."""
import json
import os

import numpy as np
import pytest

import plan_contract
import synth
from a0seam import jsonio, record

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "plan-synthetic.json")
GAIN = np.asarray(synth.GLOBAL_GAIN)


def test_usable_plan_has_exactly_the_contract_fields(run_scene):
    scene = synth.Scene()
    code, plan, folder = run_scene(scene)
    assert code == 0 and plan_contract.problems(plan) == []
    assert plan["source"] == "capture-estimate" and plan["space"] == "gamma8"
    assert plan["state"] == "usable" and plan["reason"] is None
    assert plan["bindings"] == synth.binding_list()
    assert plan["globalGainBgr"] == [float(v) for v in scene.global_gain]
    assert all(k["seamOffsetBgr"] == [0.0, 0.0, 0.0] for k in plan["knots"])
    gaps = np.diff([k["yMm"] for k in plan["knots"]])
    assert (gaps >= 0.001).all()


def test_declined_plan_is_well_formed_and_carries_no_knots(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False,
                        gain_fn=lambda y, u: synth.true_gain(y, u) * 2.0)
    code, plan, _ = run_scene(scene)
    assert code == 3 and plan_contract.problems(plan) == []
    assert plan["state"] == "declined" and plan["knots"] == []
    assert plan["bindings"] == synth.binding_list()          # still tied to the facts it was made from


def test_seam_scale_is_the_total_and_includes_the_global_gain(run_scene):
    scene = synth.Scene(bars=False, orange=False, dark=False)
    _, plan, _ = run_scene(scene)
    ys, scale = synth.knot_arrays(plan)
    truth = synth.true_gain(ys, 0.0)                    # what B' needs in total, G included
    assert np.abs(scale / truth - 1.0)[(ys > 128) & (ys < 512)].max() <= 0.005
    ratio = scale / GAIN                                 # the part beyond G stays inside the limit
    assert ((ratio > 1 / 1.25) & (ratio < 1.25)).all()
    assert np.abs(ratio - 1.0).max() > 0.05              # and it is not G alone


def test_b_samples_that_already_carry_the_global_gain_are_declined(run_scene):
    """B' multiplied by G looks like a valid estimate of about 1.0 inside the scale limit; the ratio check catches it."""
    corrected = synth.Scene(bars=False, orange=False, dark=False,
                            gain_fn=lambda y, u: synth.true_gain(y, u) / GAIN)
    code, plan, _ = run_scene(corrected)
    assert code == 3
    assert plan["state"] == "declined" and plan["reason"] == "strip-ratio-far-from-global"
    assert plan["knots"] == []
    ratio = np.array(plan["diagnostics"]["stripRatioToGlobalBgr"])
    assert np.allclose(ratio, 1.0 / GAIN, atol=0.02)

    code, plan, _ = run_scene(synth.Scene(bars=False, orange=False, dark=False))
    assert code == 0 and plan["state"] == "usable"
    ratio = np.array(plan["diagnostics"]["stripRatioToGlobalBgr"])
    assert np.abs(ratio - 1.0).max() < 0.03              # the median window sits near G


def test_the_scale_limit_cannot_be_raised_above_the_readers(run_scene):
    code, plan, _ = run_scene(synth.Scene(design={"maxScaleRatioToGlobal": 1.5}))
    assert code == 2 and plan is None


def test_synthetic_fixture_is_a_valid_plan_in_the_writers_format():
    with open(FIXTURE, "rb") as f:
        raw = f.read()
    plan = json.loads(raw.decode("utf-8"))
    assert plan_contract.problems(plan) == []
    assert plan["state"] == "usable" and len(plan["knots"]) >= 3
    assert jsonio.dumps(plan).encode("utf-8") == raw


@pytest.mark.parametrize("mutate", [
    lambda p: p.update(source="reference-capture"),
    lambda p: p.update(space="linear-affine-srgb"),
    lambda p: p["knots"][1].update(yMm=p["knots"][0]["yMm"]),
    lambda p: p["knots"][1].update(seamScaleBgr=[g * 1.3 for g in p["globalGainBgr"]]),
    lambda p: p["knots"][1].update(seamOffsetBgr=[0.01, 0.0, 0.0]),
    lambda p: p["bindings"].pop(),
    lambda p: p.update(reason="no"),
    lambda p: p.update(globalGainBgr=[5.0, 1.0, 1.0]),
])
def test_the_contract_check_refuses_what_a_reader_would_refuse(mutate):
    with open(FIXTURE, encoding="utf-8") as f:
        plan = json.load(f)
    mutate(plan)
    assert plan_contract.problems(plan) != []


def test_digest_depends_on_the_scale_values(run_scene):
    _, plan, _ = run_scene(synth.Scene(bars=False, orange=False, dark=False))
    changed = json.loads(json.dumps(plan))
    changed["knots"][0]["seamScaleBgr"][0] += 0.001
    assert record.digest_without_environment(plan) != record.digest_without_environment(changed)
