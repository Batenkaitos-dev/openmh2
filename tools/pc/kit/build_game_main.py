"""Starts the build of the game's program. It is kit/start/__main__.py in the release folder: the launcher runs `python kit/start`."""
import runpy
import sys
from pathlib import Path

p = Path(__file__).resolve().parent.parent / "src" / "tools" / "pc" / "kitbuild.py"
sys.argv[0] = str(p)
runpy.run_path(str(p), run_name="__main__")
