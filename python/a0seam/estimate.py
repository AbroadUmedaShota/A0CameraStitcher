"""Entry point: python -m a0seam.estimate <request>."""
import sys

from .cli import main

if __name__ == "__main__":
    sys.exit(main())
