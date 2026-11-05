"""Audit production-probe evidence without treating GPU-ready as desktop Present."""
import argparse
import csv
import json
from pathlib import Path
from statistics import mean


def rows(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def percentile(values, fraction):
    values = sorted(values)
    return values[min(len(values) - 1, int((len(values) - 1) * fraction))] if values else None


def audit(path):
    prefix = str(path)[:-len(".production.json")]
    info = json.loads(path.read_text(encoding="utf-8-sig"))
    recovery = json.loads(Path(prefix + ".recovery.json").read_text(encoding="utf-8-sig"))
    frames = rows(Path(prefix + ".production-frames.csv"))
    counters = rows(Path(prefix + ".recovery-frames.csv"))
    samples = rows(Path(prefix + ".samples.csv"))
    failures = []
    def require(condition, message):
        if not condition:
            failures.append(message)
    require(bool(frames), "missing output records")
    require(len(frames) == len(counters), "counter/output record mismatch")
    require(info["tailReachedLatest"], "tail did not reach latest accepted input")
    no_lite = info["mode"] in (1, 4, 6) and recovery["policy"] in (0, 1)
    if no_lite:
        require(not recovery["lightweightAllocated"], "fixed-spec policy allocated light feature")
        require(recovery["actualLiteEvaluations"] == 0 and recovery["liteSubmissions"] == 0,
                "fixed-spec policy evaluated light NR")
    require(len(recovery["passes"]) == info["passes"], "missing full pass")
    for entry in recovery["passes"]:
        require(entry["evaluateCount"] > 0 and entry["evaluateCount"] == entry["evaluateSuccessCount"]
                and entry["evaluateFailureCount"] == 0, f"failed full pass {entry['pass']}")
        require(entry["inputPercent"] == info["fullPercent"] and entry["modelPreset"] == info["model"],
                f"changed full specification on pass {entry['pass']}")
        require(entry["evaluateCount"] == recovery["passes"][0]["evaluateCount"], "unequal full pass counts")
    previous_target = previous_time = previous_revision = 0
    advances = []
    for frame in frames:
        target, stamp = int(frame["targetFrame"]), int(frame["targetAdmission100ns"])
        require(target >= previous_target, "target frame regressed")
        require(stamp >= previous_time, "target admission time regressed")
        require(target <= int(frame["physicalFrame"]), "target is newer than physical input")
        if target == previous_target and previous_target:
            require(stamp == previous_time, "held target changed its timestamp")
        if int(frame["held"]) and previous_revision:
            require(int(frame["filterOutputVersion"]) == previous_revision, "held pixels changed revision")
        if target > previous_target:
            advances.append(frame)
        previous_target, previous_time = target, stamp
        previous_revision = int(frame["filterOutputVersion"])
    require(all(int(r["pending"]) in (0, 1) and int(r["waiting"]) in (0, 1) for r in counters),
            "queue bounds exceeded")
    for name in ("fullSubmissions", "liteSubmissions", "sameSpecRecoveries", "continuousReuseOutputs",
                 "actualFullEvaluations", "actualLiteEvaluations"):
        values = [int(row[name]) for row in counters]
        require(all(a <= b for a, b in zip(values, values[1:])), f"{name} regressed")
    captures = [r for r in frames if int(r["physicalNew"]) and int(r["physicalFrame"]) > info["warmup"]]
    measured_advances = [r for r in advances if int(r["physicalFrame"]) > info["warmup"]]
    intervals = []
    for a, b in zip(measured_advances, measured_advances[1:]):
        if a["outputFrequency"] == b["outputFrequency"] and int(b["outputFrequency"]) > 0:
            intervals.append((int(b["outputReadyGpuTick"]) - int(a["outputReadyGpuTick"])) * 1000 / int(b["outputFrequency"]))
    stages = {}
    for stage in ("full_ngx_chain", "lite_ngx_chain", "production_wrapper_gpu_span"):
        selected = [float(row["milliseconds"]) for row in samples if row["stage"] == stage]
        stages[stage] = {"count": len(selected), "totalMsIncludingWarmup": sum(selected),
                         "meanMs": mean(selected) if selected else None}
    return {"case": path.name[:-len(".production.json")], "mode": info["mode"], "policy": recovery["policy"],
            "flow": info["flow"], "scene": info["scene"], "model": info["model"], "percent": info["fullPercent"],
            "passes": info["passes"], "fps": info["fps"], "capturesMeasured": len(captures),
            "allCapturedInputs": info["warmup"] + info["samples"], "counts": recovery,
            "advancesIncludingTailMeasured": len(measured_advances),
            "targetAgeMean": mean(int(r["physicalFrame"]) - int(r["targetFrame"]) for r in captures) if captures else None,
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
        parser.error("no completed production evidence found")
    result = {"cases": cases, "passed": all(not case["failures"] for case in cases),
              "notes": ["Timing regions overlap; do not sum nested GPU spans.",
                        "GPU-ready content intervals are not desktop Present or input latency.",
                        "Pending/waiting fields audit the production one-job/one-mailbox representation."]}
    options.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for case in cases:
        counts = case["counts"]
        print(f"{case['case']}: full={counts['fullSubmissions']} recovery={counts['sameSpecRecoveries']} "
              f"lite={counts['actualLiteEvaluations']} reuse={counts['continuousReuseOutputs']} "
              f"failures={case['failures']}")
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
