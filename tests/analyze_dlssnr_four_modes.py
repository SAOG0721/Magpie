"""Check current-output identity, complete NR calls and four-mode GPU evidence."""
import argparse
import csv
import json
from pathlib import Path
from statistics import mean


def read_rows(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def percentile(values, fraction):
    values = sorted(values)
    return values[int((len(values) - 1) * fraction)] if values else None


def audit(path):
    prefix = str(path).removesuffix(".production.json")
    info = json.loads(path.read_text(encoding="utf-8-sig"))
    counts = json.loads(Path(prefix + ".counts.json").read_text(encoding="utf-8-sig"))
    frames = read_rows(Path(prefix + ".production-frames.csv"))
    counters = read_rows(Path(prefix + ".counters.csv"))
    samples = read_rows(Path(prefix + ".samples.csv"))
    failures = []

    def require(condition, message):
        if not condition:
            failures.append(message)

    require(info["modeVersion"] == 1 and info["mode"] in range(4), "obsolete mode identity")
    require(bool(frames) and len(frames) == len(counters), "missing output/counter rows")
    require(info["tailReachedLatest"] and info["bufferDelayEffective"] == 0, "buffered final target")
    require(counts["lightweightBranchPresent"] is False, "lightweight engine present")
    require(len(counts["passes"]) == info["passes"], "missing complete NR pass")
    for entry in counts["passes"]:
        require(entry["evaluateCount"] > 0 and entry["evaluateCount"] == entry["evaluateSuccessCount"]
                and entry["evaluateFailureCount"] == 0, f"failed pass {entry['pass']}")
        require(entry["inputPercent"] == info["fullPercent"] and entry["modelPreset"] == info["model"],
                f"changed specification on pass {entry['pass']}")
        require(entry["evaluateCount"] == counts["passes"][0]["evaluateCount"], "unequal pass counts")
        require(entry["width"] == int(info["width"] * info["fullPercent"] / 100 + .5)
                and entry["height"] == int(info["height"] * info["fullPercent"] / 100 + .5),
                "manual integer percentage lost")
    previous_target = previous_stamp = 0
    previous_observations = {}
    for frame in frames:
        target, stamp = int(frame["targetFrame"]), int(frame["targetAdmission100ns"])
        require(target >= previous_target and stamp >= previous_stamp, "output identity regressed")
        require(target == int(frame["physicalFrame"]) and stamp == int(frame["physicalAdmission100ns"]),
                "output delayed behind current capture")
        if target == previous_target:
            require(stamp == previous_stamp, "repeat capture changed timestamp")
        if int(frame["oldestNrSourceSequence"]):
            require(int(frame["oldestNrSourceSequence"]) <= int(frame["targetSequence"]), "future residual used")
        epoch = int(frame["epoch"])
        observation = int(frame["observationRevision"])
        require(observation >= previous_observations.get(epoch, 0), "NR observation clock regressed")
        previous_observations[epoch] = observation
        previous_target, previous_stamp = target, stamp
    for name in ("fullSubmissions", "sameSpecRecoveries", "continuousReuseOutputs", "actualFullEvaluations"):
        values = [int(row[name]) for row in counters]
        require(all(a <= b for a, b in zip(values, values[1:])), f"{name} regressed")
    require(all(int(r["pending"]) in (0, 1) and int(r["waiting"]) in (0, 1) for r in counters),
            "queue bounds exceeded")
    captures = [r for r in frames if int(r["physicalNew"]) and int(r["physicalFrame"]) > info["warmup"]]
    intervals = [(int(b["outputReadyGpuTick"]) - int(a["outputReadyGpuTick"])) * 1000 / int(b["outputFrequency"])
                 for a, b in zip(captures, captures[1:])
                 if a["outputFrequency"] == b["outputFrequency"] and int(b["outputFrequency"]) > 0]
    stages = {}
    for stage in ("full_ngx_chain", "production_wrapper_gpu_span", "motion_history_shader", "sparse_compose_residual"):
        selected = [float(row["milliseconds"]) for row in samples if row["stage"] == stage]
        stages[stage] = {"count": len(selected), "totalMsIncludingWarmup": sum(selected),
                         "meanMs": mean(selected) if selected else None}
    require(not any(row["stage"].startswith("lite_") for row in samples), "lightweight GPU stage executed")
    return {"case": path.name.removesuffix(".production.json"), "mode": info["mode"],
            "interval": info["intervalEffective"], "flow": info["flow"], "scene": info["scene"],
            "model": info["model"], "percent": info["fullPercent"], "passes": info["passes"], "fps": info["fps"],
            "capturesMeasured": len(captures), "allCapturedInputs": info["warmup"] + info["samples"], "counts": counts,
            "gpuReadyContentIntervalP50Ms": percentile(intervals, .5),
            "gpuReadyContentIntervalP95Ms": percentile(intervals, .95),
            "gpuReadyContentIntervalMaxMs": max(intervals) if intervals else None,
            "stages": stages, "failures": sorted(set(failures)),
            "measurementClass": info["measurementClass"], "desktopPresentMeasured": False}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    cases = [audit(path) for path in sorted(options.directory.glob("*.production.json"))]
    if not cases:
        parser.error("no completed production evidence")
    result = {"cases": cases, "passed": all(not case["failures"] for case in cases),
              "notes": ["Nested GPU spans overlap and cannot be summed.",
                        "GPU-ready intervals are not desktop Present, power or input latency.",
                        "Exact ordinary cadence is separately checked with deterministic native guidance."]}
    options.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for case in cases:
        print(f"{case['case']}: full={case['counts']['fullSubmissions']} "
              f"sameSpec={case['counts']['sameSpecRecoveries']} failures={case['failures']}")
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
