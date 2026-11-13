"""Estimator of the seam colour plan for the stitched page.

The package reads a request (a seam column, the mapped A and B' strips around
the seam, the global gain) and writes a plan (a0.seam-color-plan.v1) with the
scale B' needs at the seam along the page. It never writes an image; applying
the plan is the renderer's job.

Importing the package changes nothing in the process. A program that starts
the estimator calls pin_single_thread() before numpy is imported.
"""
import os

__version__ = "1.0.0"

_THREAD_VARIABLES = ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "NUMEXPR_NUM_THREADS")


def pin_single_thread():
    """Set the thread-pool variables to 1 for this process; call it before numpy is imported.

    The estimator is serial and gives the same bytes whatever the machine's
    thread count is. The launchers call this so that a library that starts
    helper threads cannot change that.
    """
    for name in _THREAD_VARIABLES:
        os.environ[name] = "1"
