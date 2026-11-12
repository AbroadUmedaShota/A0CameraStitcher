"""Estimator of the seam row-gain field for the stitched page.

The package reads a request (a seam column, the mapped A and B' strips around
the seam, the global gain) and writes a record of per-row gains at the seam.
It never writes an image; applying the gains is the renderer's job.
"""
import os

# The estimator is serial and must give the same bytes whatever the machine's
# thread count is. Pin the thread pools before numpy is imported.
for _name in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "NUMEXPR_NUM_THREADS"):
    os.environ[_name] = "1"
del _name

__version__ = "1.0.0"
