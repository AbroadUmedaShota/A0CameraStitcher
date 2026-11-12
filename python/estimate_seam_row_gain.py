"""Launcher for hosts that start Python in isolated mode (python -I).

Isolated mode puts neither the current directory nor PYTHONPATH on sys.path, so
`-m a0seam.estimate` cannot find the package. This script adds its own folder
(the folder that holds a0seam/) and runs the same command line:

    python -I python/estimate_seam_row_gain.py <request> [--out <record>]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from a0seam.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
