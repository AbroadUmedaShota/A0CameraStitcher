"""The reader's rules for a0.seam-color-plan.v1, written out for the tests (not part of the package).

A plan must pass `problems(plan)` with an empty result. The rules come from the
format description: closed codes, ranges, strictly increasing yMm, S / G within
the ratio limit, offsets zero in gamma8 and so on.
"""
import math

TOP_KEYS = ["schema", "source", "state", "reason", "space", "bindings", "requestSha256",
            "globalGainBgr", "knots", "diagnostics"]
KNOT_KEYS = ["yMm", "seamScaleBgr", "seamOffsetBgr"]
SOURCES = {"capture-estimate"}
SPACES = {"gamma8"}
BINDING_KINDS = {"condition-v3", "capture-original-a", "capture-original-b"}
GLOBAL_GAIN_RANGE = (0.25, 4.0)
MAX_RATIO = 1.25
MIN_KNOT_GAP_MM = 0.001


def _is_hex64(v):
    return isinstance(v, str) and len(v) == 64 and all(c in "0123456789abcdef" for c in v)


def _three(v):
    return isinstance(v, list) and len(v) == 3 and all(isinstance(x, (int, float)) and math.isfinite(x) for x in v)


def problems(plan):
    out = []
    if list(plan) != TOP_KEYS:
        return [f"keys are {list(plan)}"]
    if plan["schema"] != "a0.seam-color-plan.v1":
        out.append("schema")
    if plan["source"] not in SOURCES:
        out.append("source")
    if plan["space"] not in SPACES:
        out.append("space")
    if plan["state"] not in ("usable", "declined"):
        out.append("state")
    reason = plan["reason"]
    if plan["state"] == "usable" and reason is not None:
        out.append("usable plan with a reason")
    if plan["state"] == "declined" and not (isinstance(reason, str) and 1 <= len(reason) <= 64
                                            and all(c in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in reason)):
        out.append("declined plan without a well-formed reason")
    kinds = []
    if not isinstance(plan["bindings"], list) or not 1 <= len(plan["bindings"]) <= 8:
        out.append("bindings size")
    else:
        for b in plan["bindings"]:
            if list(b) != ["kind", "sha256"] or b["kind"] not in BINDING_KINDS or not _is_hex64(b["sha256"]):
                out.append(f"binding {b}")
            kinds.append(b.get("kind"))
    if len(set(kinds)) != len(kinds):
        out.append("binding kinds repeat")
    if plan["source"] == "capture-estimate" and set(kinds) != BINDING_KINDS:
        out.append("capture-estimate needs all three bindings")
    if not _is_hex64(plan["requestSha256"]):
        out.append("requestSha256")
    g = plan["globalGainBgr"]
    low, high = GLOBAL_GAIN_RANGE
    if not _three(g) or not all(low <= x <= high for x in g):
        out.append("globalGainBgr")
        return out
    if not isinstance(plan["diagnostics"], dict):
        out.append("diagnostics")
    knots = plan["knots"]
    if plan["state"] == "declined":
        return out            # a declined plan is not applied; its knots are not read
    if not 2 <= len(knots) <= 4096:
        out.append("knot count")
    previous = None
    for k in knots:
        if list(k) != KNOT_KEYS:
            out.append(f"knot keys {list(k)}")
            continue
        if previous is not None and not k["yMm"] - previous >= MIN_KNOT_GAP_MM:
            out.append("yMm not strictly increasing")
        previous = k["yMm"]
        if not _three(k["seamScaleBgr"]) or not all(s > 0 for s in k["seamScaleBgr"]):
            out.append("seamScaleBgr")
        elif not all(1.0 / MAX_RATIO <= s / gain <= MAX_RATIO for s, gain in zip(k["seamScaleBgr"], g)):
            out.append("S / G beyond the ratio limit")
        if plan["space"] == "gamma8" and k["seamOffsetBgr"] != [0.0, 0.0, 0.0]:
            out.append("seamOffsetBgr must be zero in gamma8")
    return out
