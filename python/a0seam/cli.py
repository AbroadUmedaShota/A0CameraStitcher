"""Command line: python -m a0seam.estimate <request> [--out <plan>]

Exit codes:
  0  a plan was written and it is to be applied (state "usable")
  3  a plan was written but it must not be applied (state "declined")
  2  the request or its files are not acceptable; no plan is written
  1  anything else; no plan is written
"""
import argparse
import os
import sys

from . import jsonio, record, rowfield, windows
from .errors import InputError
from .request import load_request

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_BAD_INPUT = 2
EXIT_DECLINED = 3

DEFAULT_PLAN_NAME = "seam-color-plan.json"


def estimate(req):
    """Run the estimate for a loaded request; returns the plan dict."""
    design = req.design
    c0, c1 = req.seam - design.reach, req.seam + design.reach + 1
    strip = req.read_strip_columns(c0, c1)
    found = windows.collect_flat_windows(strip, c0, req.seam, design)
    knots = rowfield.fit_knots(found, req.seam, req.global_gain, req.height, design)
    return record.build_plan(req, found, knots)


def main(argv=None):
    parser = argparse.ArgumentParser(prog="a0seam.estimate", description="Estimate the seam colour plan.")
    parser.add_argument("request", help="path of the request JSON")
    parser.add_argument("--out", help=f"plan path (default: {DEFAULT_PLAN_NAME} next to the request)")
    args = parser.parse_args(argv)
    out = args.out or os.path.join(os.path.dirname(os.path.abspath(args.request)), DEFAULT_PLAN_NAME)
    try:
        req = load_request(args.request)
        plan = estimate(req)
        jsonio.write_atomic(out, record.dumps(plan))
    except InputError as e:
        print(f"input error: {e}", file=sys.stderr)
        return EXIT_BAD_INPUT
    except Exception as e:  # noqa: BLE001 - last resort, mapped to exit code 1
        print(f"error: {type(e).__name__}: {e}", file=sys.stderr)
        return EXIT_ERROR
    diagnostics = plan["diagnostics"]
    print(f"state={plan['state']} knots={diagnostics['knotCount']} unused={diagnostics['unusedKnots']} plan={out}")
    return EXIT_OK if plan["state"] == record.STATE_USABLE else EXIT_DECLINED
