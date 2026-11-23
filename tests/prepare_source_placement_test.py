"""Extract the real tracker sampling and foreground paths for a CPU fixture."""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
source = (repo / "src/Magpie.Core/SrcTracker.cpp").read_text(encoding="utf-8-sig")


def block(marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


(output / "SourcePlacementTracker.inc").write_text(
    "\n\n".join(block(marker) for marker in (
        "bool SrcTracker::UpdateState(",
        "bool SrcTracker::UpdateForeground(",
    )), encoding="utf-8")
print("Extracted production SrcTracker state sampling and foreground notification.")
