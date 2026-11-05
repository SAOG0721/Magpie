"""Extract production original-reference selection/publication; no GPU work."""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
source = (repo / "src/Magpie.Core/PassThroughFrames.cpp").read_text(encoding="utf-8-sig")
start = source.index("bool PassThroughFrames::SelectBackendFrame(")
end = source.index("bool PassThroughFrames::Consume(", start)
metadata_start = source.index("HdrFrameMetadata PassThroughFrames::_ReferenceMetadata(")
metadata_end = source.index("void PassThroughFrames::UpdateBackend(const HdrFrame&", metadata_start)
update_start = source.index("void PassThroughFrames::_UpdateBackend(")
(out / "PassThroughFrameSelectionUnderTest.h").write_text(source[update_start:metadata_start] + source[metadata_start:metadata_end] + source[start:end], encoding="utf-8")
print("Extracted production reference selection/publication for CPU identity tests.")
