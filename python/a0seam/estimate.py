"""Entry point: python -m a0seam.estimate <request>."""
import sys

from . import pin_single_thread

pin_single_thread()

from .cli import main  # noqa: E402 - numpy is imported by cli, after the thread pools are pinned

if __name__ == "__main__":
    sys.exit(main())
