"""Exercise the shipped parameter descriptors, not duplicated test constants."""
from pathlib import Path
import re
import sys

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)

def descriptor(path, name, function):
    text = (repo / path).read_text(encoding="utf-8-sig")
    block = next(b for b in text.split("//!PARAMETER\n")[1:] if re.search(rf"^(int|float) {name};", b, re.M))
    kind = re.search(rf"^(int|float) {name};", block, re.M)[1]
    fields = dict(re.findall(r"//!([A-Z]+) ([^\n]+)", block))
    options = re.findall(r"//!OPTION (\d+) ([^\n]+)", block)
    values = [int(v) for v, _ in options]
    lo = fields.get("MIN", str(min(values)) if values else "0")
    hi = fields.get("MAX", str(max(values)) if values else "0")
    fields.setdefault("STEP", "1")
    numbers = [fields["DEFAULT"], lo, hi, fields["STEP"]]
    if kind == "float": numbers = [v + ".f" for v in numbers]
    result = f"inline Magpie::EffectParameterDesc {function}() {{ Magpie::EffectParameterDesc p; p.name=\"{name}\";\n"
    result += f"p.constant = Magpie::EffectConstant<{kind}>{{{','.join(numbers)}}};\n"
    if options: result += "p.choices={" + ",".join(f'{{{v},"{label}"}}' for v, label in options) + "};\n"
    return result + "return p; }\n"

code = descriptor("src/Effects/DLSS/DLSS_SR.hlsl", "renderPreset", "SrPresetDescriptor")
code += descriptor("src/Effects/DLSSNR/DLSSNR_AI_Filter.hlsl", "antiFlickerTimeMs", "NrTimeDescriptor")
(out / "dlss_parameter_descriptors.h").write_text(code, encoding="utf-8")

factory = (repo / "src/Magpie.Core/NativeEffectBackendFactory.cpp").read_text(encoding="utf-8-sig")
assert factory.count(".renderPreset = ParseDLSSSRPreset(option)") == 2  # SDR and HDR
assert ".renderPreset = 10 // Preserve the hidden legacy adapter's J contract." in factory
renderer = (repo / "src/Magpie.Core/Renderer.cpp").read_text(encoding="utf-8-sig")
assert "return DLSSNRParameterChangesCurrentOutput(name, activePasses);" in renderer
print("Shipped SR/NR descriptors and SDR/HDR/legacy/renderer wiring checked.")
