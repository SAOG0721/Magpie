"""Compare deterministic target-aligned snapshots; metrics are not a quality score."""
import argparse
import csv
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont


def read_rows(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    cases = {}
    for path in args.directory.glob("*.production.json"):
        name = path.name[:-len(".production.json")]
        info = json.loads(path.read_text())
        def load(file, bgra=False):
            data = np.fromfile(file, dtype=np.uint8).reshape(info["height"], info["width"], 4)
            return data[:, :, [2, 1, 0]] if bgra else data[:, :, :3]
        frames = {}
        for row in read_rows(args.directory / (name + ".production-images.csv")):
            frames[int(row["targetFrame"])] = {
                "input": load(row["targetBgra"], True), "output": load(row["outputRgba"])}
        cases[name] = {"info": info, "frames": frames}
    results = []
    for name, case in sorted(cases.items()):
        baseline = cases["base-" + case["info"]["scene"]]
        errors, magnitudes, baseline_magnitudes = [], [], []
        for target, data in case["frames"].items():
            if target not in baseline["frames"]:
                continue
            reference = baseline["frames"][target]
            if not np.array_equal(data["input"], reference["input"]):
                raise RuntimeError(f"Wrong retained target pixels: {name} target {target}")
            output, raw = data["output"].astype(np.float32), data["input"].astype(np.float32)
            full = reference["output"].astype(np.float32)
            # The tested NR DLL draws a diagnostic label at the bottom; its
            # apparent size changes with the inference resolution. Compare the
            # same scene-only region instead of counting that text as quality.
            roi_height = max(1, case["info"]["height"] - 256)
            output, raw, full = output[:roi_height], raw[:roi_height], full[:roi_height]
            errors.append(float(np.mean(np.abs(output - full))))
            magnitudes.append(float(np.mean(np.abs(output - raw))))
            baseline_magnitudes.append(float(np.mean(np.abs(full - raw))))
        results.append({"case": name, "alignedTargets": len(errors), "targetPixelsExact": True,
                        "meanAbsDifferenceFromEveryFrame8bit": float(np.mean(errors)),
                        "excludedBottomPixels": 256,
                        "meanCorrectionMagnitude8bit": float(np.mean(magnitudes)),
                        "everyFrameCorrectionMagnitude8bit": float(np.mean(baseline_magnitudes))})
    font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 20)
    for scene in ("translate", "cut"):
        selected = [(name, case) for name, case in sorted(cases.items()) if case["info"]["scene"] == scene]
        targets = set.intersection(*(set(case["frames"]) for _, case in selected))
        target = max(targets)
        raw = cases["base-" + scene]["frames"][target]["input"]
        panels = [("Current target input", raw)] + [(name, case["frames"][target]["output"]) for name, case in selected]
        columns = 3
        sheet = Image.new("RGB", (columns * 640, ((len(panels) + columns - 1) // columns) * 400), "#172431")
        draw = ImageDraw.Draw(sheet)
        for index, (label, pixels) in enumerate(panels):
            x, y = index % columns * 640, index // columns * 400
            sheet.paste(Image.fromarray(pixels).resize((640, 360), Image.Resampling.LANCZOS), (x, y + 40))
            draw.text((x + 10, y + 8), f"{label} / target {target}", fill="white", font=font)
        sheet.save(args.output / f"{scene}-target-{target}.png")
    report = {"cases": results, "notes": [
        "Retained target RGB is compared exactly with the corresponding every-frame input, not the physical latest frame.",
        "MAE from every-frame output measures difference, not subjective quality. Lower correction can also lower this metric.",
        "Scene metrics exclude the bottom 256 pixels containing the SDK diagnostic watermark; target-identity comparison uses all pixels.",
        "Only the recorded short deterministic scenes are covered; no game-wide flicker or ghosting claim.",
        "Quality snapshots allocate GPU textures and are excluded from performance claims."]}
    (args.output / "image-analysis.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for result in results:
        print(f"{result['case']}: targets={result['alignedTargets']} exactInput=true "
              f"MAE={result['meanAbsDifferenceFromEveryFrame8bit']:.4f} correction={result['meanCorrectionMagnitude8bit']:.4f}")


if __name__ == "__main__":
    main()
