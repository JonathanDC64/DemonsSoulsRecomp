"""The setup engine's entry for the host (docs/88): `python -X utf8 -u run_setup.py <subcommand> ...`
does what `python -m des_setup <subcommand> ...` does from this folder.

The embeddable Python's ._pth file leaves the script's folder off sys.path
(docs/52 13.3), so this puts it there first.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from des_setup.__main__ import main  # noqa: E402
from des_setup.pipeline import die_with_parent  # noqa: E402

if __name__ == "__main__":
    die_with_parent()   # (POSIX: ends with the host that started it)
    sys.exit(main())
