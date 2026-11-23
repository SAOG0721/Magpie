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
present_start = source.index("void PassThroughFrames::OnPresented()")
present_end = source.index("\n}\n", present_start) + len("\n}\n")
header = (repo / "src/Magpie.Core/PassThroughFrames.h").read_text(encoding="utf-8-sig")
frontend_start = header.index("\tID3D11Texture2D* FrontendTexture(")
frontend_end = header.index("\n\t}", frontend_start) + len("\n\t}")
(out / "PassThroughFrameSelectionUnderTest.h").write_text(
    source[update_start:metadata_start] + source[metadata_start:metadata_end] + source[start:end] +
    source[present_start:present_end], encoding="utf-8")
(out / "PassThroughFrontendUnderTest.h").write_text(
    header[frontend_start:frontend_end].replace("ID3D11Texture2D", "CpuReferenceTexture"), encoding="utf-8")
print("Extracted production reference selection, publication and presentation for CPU identity tests.")
