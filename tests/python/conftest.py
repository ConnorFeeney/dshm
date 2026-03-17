import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PYTHON_MODULE_DIR = ROOT / "build" / "debug" / "lib"

sys.path.insert(0, str(PYTHON_MODULE_DIR))
