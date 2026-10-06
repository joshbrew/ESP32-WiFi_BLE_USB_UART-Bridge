import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from bridge import VERSION
from bridge.release import build_bundle

parser = argparse.ArgumentParser(description="Build a checksummed Pi application update (no OS or dependency changes)")
parser.add_argument("--output", default=str(ROOT / "pi-controller-update.zip"))
arguments = parser.parse_args()
target = Path(arguments.output)
target.write_bytes(build_bundle(ROOT, VERSION))
print(f"Created {target} ({target.stat().st_size} bytes)")
