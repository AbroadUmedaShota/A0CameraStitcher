"""Command line: python -m a0seam.estimate <request> [--out <plan>]

Exit codes:
  0  a plan was written and it is to be applied (state "usable")
  3  a plan was written but it must not be applied (state "declined")
  2  the request or its files are not acceptable; no plan is written
  1  anything else; no plan is written

A run that writes no plan leaves any file already at the output path as it was.
Output never contains a file system path: the status line goes to stdout, error
text to stderr, and both survive characters the console's code page cannot show.
"""
import argparse
import os
import re
import sys

from . import jsonio, record, rowfield, windows
from .errors import InputError
from .request import load_request

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_BAD_INPUT = 2
EXIT_DECLINED = 3

DEFAULT_PLAN_NAME = "seam-color-plan.json"

_PATH_MARK = "<path>"
_WINDOWS_PATH = re.compile(r"(?:[A-Za-z]:[\\/]|\\\\)[^\s'\"<>|*?]*")
_POSIX_PATH = re.compile(r"(?<![\w.:/-])/(?:[^\s'\"<>|/]+/)+[^\s'\"<>|/]*")


def estimate_plan(req):
    """Run the estimate for a loaded request; returns the plan dict."""
    design = req.design
    c0, c1 = req.seam - design.reach, req.seam + design.reach + 1
    strip = req.read_strip_columns(c0, c1)
    found = windows.collect_flat_windows(strip, c0, req.seam, design)
    knots = rowfield.fit_knots(found, req.seam, req.global_gain, req.height, design)
    return record.build_plan(req, found, knots)


def _harden_streams():
    """Make stdout and stderr replace characters their encoding cannot show instead of raising."""
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            try:
                reconfigure(errors="backslashreplace")
            except (ValueError, OSError):
                pass


def _without_paths(text, known):
    """text with the given paths, and anything shaped like an absolute path, replaced by a mark."""
    for path in sorted(filter(None, known), key=len, reverse=True):
        for variant in {path, path.replace("\\", "/"), path.replace("/", "\\")}:
            text = text.replace(variant, _PATH_MARK)
    return _POSIX_PATH.sub(_PATH_MARK, _WINDOWS_PATH.sub(_PATH_MARK, text))


def _failure_text(error, known):
    """One line about an unexpected failure: its type, and its message without paths."""
    if isinstance(error, OSError):
        return f"{type(error).__name__}: {error.strerror or 'I/O error'}"
    return f"{type(error).__name__}: {_without_paths(str(error), known)}"


def main(argv=None):
    _harden_streams()
    parser = argparse.ArgumentParser(prog="a0seam.estimate", description="Estimate the seam colour plan.")
    parser.add_argument("request", help="path of the request JSON")
    parser.add_argument("--out", help=f"plan path (default: {DEFAULT_PLAN_NAME} next to the request)")
    args = parser.parse_args(argv)
    request_path = os.path.abspath(args.request)
    out = args.out or os.path.join(os.path.dirname(request_path), DEFAULT_PLAN_NAME)
    known = (request_path, os.path.abspath(out), os.path.dirname(request_path),
             os.path.dirname(os.path.abspath(out)), os.getcwd(), os.path.expanduser("~"))
    try:
        req = load_request(request_path)
        plan = estimate_plan(req)
        jsonio.write_atomic(out, record.dumps(plan))
    except InputError as e:
        print(f"input error: {_without_paths(str(e), known)}", file=sys.stderr)
        return EXIT_BAD_INPUT
    except Exception as e:  # noqa: BLE001 - last resort, mapped to exit code 1
        print(f"error: {_failure_text(e, known)}", file=sys.stderr)
        return EXIT_ERROR
    diagnostics = plan["diagnostics"]
    print(f"state={plan['state']} knots={diagnostics['knotCount']} used={diagnostics['usedKnots']} "
          f"unused={diagnostics['unusedKnots']}")
    return EXIT_OK if plan["state"] == record.STATE_USABLE else EXIT_DECLINED
