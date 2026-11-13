"""Estimator of the seam colour plan for the stitched page.

The package reads a request (a seam column, the mapped A and B' strips around
the seam, the global gain) and writes a plan (a0.seam-color-plan.v1) with the
scale B' needs at the seam along the page. It never writes an image; applying
the plan is the renderer's job.
"""
import os

# The estimator is serial and must give the same bytes whatever the machine's
# thread count is. Pin the thread pools before numpy is imported.
for _name in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "NUMEXPR_NUM_THREADS"):
    os.environ[_name] = "1"
del _name

__version__ = "1.0.0"
