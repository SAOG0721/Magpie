"""Extract the production session interval application for CPU fault injection."""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
source = (repo / "src/Magpie.Core/GraphicsCaptureFrameSource.cpp").read_text(encoding="utf-8-sig")
start = source.index("void GraphicsCaptureFrameSource::_ApplyMinUpdateInterval() {")
end = source.index("\nbool GraphicsCaptureFrameSource::_StartCapture(", start)
output = Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)
(output / "WgcApplyMinUpdateInterval.inc").write_text(source[start:end], encoding="utf-8")
