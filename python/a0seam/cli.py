"""Command line: python -m a0seam.estimate <request> [--out <record>]

Exit codes:
  0  a record was written and it is to be applied
  3  a record was written but it must not be applied (state "declined")
  2  the request or its files are not acceptable; no record is written
  1  anything else; no record is written
"""
import argparse
import os
import sys
import tempfile

from . import record, rowfield, windows
from .errors import InputError
from .request import load_request

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_BAD_INPUT = 2
EXIT_DECLINED = 3

DEFAULT_RECORD_NAME = "seam-row-gain.json"


def estimate(req):
    """Run the estimate for a loaded request; returns the record dict."""
    design = req.design
    c0, c1 = req.seam - design.reach, req.seam + design.reach + 1
    strip = req.read_strip_columns(c0, c1)
    found = windows.collect_flat_windows(strip, c0, req.seam, design)
    knots = rowfield.fit_knots(found, req.seam, req.global_gain, req.height, design)
    return record.build_record(req, found, knots, req.load_default_row_gain())


def _write_atomic(path, text):
    folder = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(dir=folder, prefix=".seam-row-gain-", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(prog="a0seam.estimate", description="Estimate the seam row-gain record.")
    parser.add_argument("request", help="path of the request JSON")
    parser.add_argument("--out", help=f"record path (default: {DEFAULT_RECORD_NAME} next to the request)")
    args = parser.parse_args(argv)
    out = args.out or os.path.join(os.path.dirname(os.path.abspath(args.request)), DEFAULT_RECORD_NAME)
    try:
        req = load_request(args.request)
        rec = estimate(req)
        _write_atomic(out, record.dumps(rec))
    except InputError as e:
        print(f"input error: {e}", file=sys.stderr)
        return EXIT_BAD_INPUT
    except Exception as e:  # noqa: BLE001 - last resort, mapped to exit code 1
        print(f"error: {type(e).__name__}: {e}", file=sys.stderr)
        return EXIT_ERROR
    knots = rec["knots"]
    print(f"state={rec['state']} knots={len(knots)} unused={rec['unusedKnots']} record={out}")
    return EXIT_OK if rec["state"] == record.STATE_APPLIED else EXIT_DECLINED
