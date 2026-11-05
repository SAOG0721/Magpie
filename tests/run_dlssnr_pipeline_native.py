"""Compile/run the current production NR backend against an existing native build.

This compiles a test executable only; it does not build or deploy Magpie.
The build root contains obj/ plus build/ (or Magpie-Experimental-x64/).
"""
from pathlib import Path
import argparse
import ctypes
import subprocess
import sys
import shutil
import json
import hashlib

# Driver logs mix UTF-8 and localized byte strings; preserve diagnostic output
# even when a Windows parent process gives Python a legacy GBK console encoding.
sys.stdout.reconfigure(errors="replace")

parser = argparse.ArgumentParser()
parser.add_argument("build_root", type=Path)
parser.add_argument("--timing", action="store_true")
parser.add_argument("--core-library", type=Path)
parser.add_argument("--output", type=Path, help="Isolated test directory; required for packaged builds without Core.lib")
parser.add_argument("--performance-only", action="store_true")
parser.add_argument("--alternating-only", action="store_true", help="Alternating intervals2/3/4, true age, redraw cadence and hard rebuild")
parser.add_argument("--pipeline-only", action="store_true", help="Pipeline trusted/rejected/reset/expiry/in-flight native regressions")
parser.add_argument("--hot-cache-only", action="store_true", help="Three models x 1/3 passes, Pipeline hot switch and return")
parser.add_argument("--beta3-only", action="store_true", help="Four-mode performance regressions without the inference suite")
parser.add_argument("--inference-only", action="store_true", help="SDR/HDR, multi-pass, partial-cache and reconstruction cases")
parser.add_argument("--post-output-only", action="store_true", help="Pipeline idle-owner wake-up and fenced pool retry")
parser.add_argument("--gpu-fusion-live-only", action="store_true", help="Four-mode live detail GPU constants, debug bypass and zero residual with anti-flicker")
parser.add_argument("--submission-worker-only", action="store_true", help="Pipeline/Sparse gated CPU Evaluate, live edits, bounded requests, stop and failure")
parser.add_argument("--sparse-paired-input-only", action="store_true", help="Sparse old native pair A with distinct pending B, changed current C and exact normal-control pixels")
parser.add_argument("--no-debug", action="store_true", help="Measure release-style resource/Create cost without validation-layer overhead")
parser.add_argument("--compile-only", action="store_true")
options = parser.parse_args()
if sum((options.performance_only, options.alternating_only, options.pipeline_only,
        options.hot_cache_only, options.beta3_only, options.inference_only, options.post_output_only,
        options.gpu_fusion_live_only, options.submission_worker_only, options.sparse_paired_input_only)) > 1:
    parser.error("Choose only one native test suite selector.")
repo = Path(__file__).resolve().parents[1]
root = options.build_root.resolve()
runtime = next((root / folder for folder in ("build", "Magpie-Experimental-x64", "runtime")
                if (root / folder / "nvngx_dlssnr.dll").is_file()), None)
if runtime is None:
    parser.error("The existing build must contain the DLSSNR runtime.")
core_library = options.core_library.resolve() if options.core_library else runtime / "Magpie.Core.lib"
if not core_library.is_file():
    parser.error("Core library is missing; use --core-library with the matching build's static library.")
output = options.output.resolve() if options.output else root / "diagnostics/dlssnr-pipeline"
if output == runtime or runtime in output.parents:
    parser.error("Test output must be outside the deployment/runtime directory.")
output.mkdir(parents=True, exist_ok=True)
# The signed NGX snippet resolves beside the test executable. Copy only runtime
# binaries, never the user's config/log/cache or the installed Magpie executable.
for source in runtime.glob("*.dll"):
    shutil.copy2(source, output / source.name)

shell32 = ctypes.windll.shell32
shell32.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
shell32.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]

def command_args(text):
    count = ctypes.c_int()
    pointer = shell32.CommandLineToArgvW("tool " + text, ctypes.byref(count))
    result = [pointer[i] for i in range(1, count.value)]
    ctypes.windll.kernel32.LocalFree(pointer)
    return result

def tlog(path):
    return "\n".join(line for line in path.read_text(encoding="utf-16").splitlines()
                     if not line.startswith("^"))

def project_log(project, name):
    relative = f"{project}/{project}.tlog/{name}"
    return next((root / folder / relative for folder in ("obj", "obj/x64/Release")
                 if (root / folder / relative).is_file()), root / "obj" / relative)

clargs = command_args(tlog(project_log("Magpie.Core", "CL.command.1.tlog")))
includes = list(dict.fromkeys(arg for arg in clargs if arg.lower().startswith("/i")))
definitions = list(dict.fromkeys(clargs[i + 1] for i, arg in enumerate(clargs)
                                 if arg == "/D" and clargs[i + 1] != "NDEBUG"))
defs = [item for value in definitions for item in ("/D", value)]
if options.timing:
    defs += ["/D", "MP_ENABLE_NATIVE_BACKEND_TIMING"]
linkargs = command_args(tlog(project_log("Magpie", "link.command.1.tlog")))
libs = list(dict.fromkeys(arg for arg in linkargs
                         if arg.lower().endswith(".lib") or arg.lower().startswith("/libpath:")))
libs = [arg for arg in libs if Path(arg).name.lower() not in ("magpie.lib", "magpie.core.lib")]
libs.append(str(core_library))

exe = output / "DLSSNRPipelineTests.exe"
command = ["cl.exe", "/nologo", "/std:c++20", "/EHsc", "/utf-8", "/MT", "/O2", "/bigobj",
           "/I" + str(repo / "src/Magpie.Core"), "/I" + str(repo / "src/Magpie.Core/include")]
command += includes + defs + [str(repo / "tests/DLSSNRPipelineTests.cpp"),
                              "/Fe:" + str(exe), "/Fo:" + str(exe.with_suffix(".obj")), "/link", "/LTCG"]
command += libs + ["d3d11.lib", "d3dcompiler.lib", "dxgi.lib", "d3d12.lib", "WindowsApp.lib"]
print("Compiling the native test against current production sources.", flush=True)
result = subprocess.run(command, cwd=repo / "src/Magpie.Core")
source_files = ["src/Magpie.Core/" + name for name in
                ("DLSSNRFilter.cpp", "DLSSNRFilter.h", "DLSSNRPerformance.inl", "DLSSNRPerformanceShader.h",
                 "DLSSNRMultiPass.h", "DLSSNRGpuTimer.h", "DLSSNRSparse.h", "DLSSNRSparse.inl",
                 "DLSSNRSparseShader.h", "DLSSNRColorShader.h", "DLSSNRDetailShader.h", "include/DLSSNREnhancementPool.h",
                 "include/DLSSNRPerformanceParameters.h", "include/DLSSNRPerformanceState.h",
                 "include/DLSSNRAdaptiveResolution.h")]
source_files += ["src/Magpie.Core/include/DLSSNRSubmissionWorker.h"]
test_files = ["tests/" + name for name in ("DLSSNRPipelineTests.cpp", "DLSSNRPerformanceNativeCases.inl",
                                           "DLSSNRBeta3NativeCases.inl", "DLSSNRGpuFusionNativeCases.inl",
                                           "DLSSNRSubmissionWorkerNativeCases.inl",
                                           "run_dlssnr_pipeline_native.py")]
report = {
    "compileExitCode": result.returncode, "compileOnly": options.compile_only,
    "performanceOnly": options.performance_only, "alternatingOnly": options.alternating_only,
    "pipelineOnly": options.pipeline_only, "hotCacheOnly": options.hot_cache_only, "beta3Only": options.beta3_only,
    "inferenceOnly": options.inference_only, "postOutputOnly": options.post_output_only,
    "gpuFusionLiveOnly": options.gpu_fusion_live_only,
    "submissionWorkerOnly": options.submission_worker_only,
    "sparsePairedInputOnly": options.sparse_paired_input_only,
    "debugLayerRequested": not options.no_debug, "timingEnabled": options.timing,
    "coreLibrary": str(core_library), "coreLibrarySha256": hashlib.sha256(core_library.read_bytes()).hexdigest(),
    "runtimeSource": str(runtime),
    "sourceCommit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
    "sourceSha256": {name: hashlib.sha256((repo / name).read_bytes()).hexdigest() for name in source_files},
    "testSha256": {name: hashlib.sha256((repo / name).read_bytes()).hexdigest() for name in test_files},
    "runtimeNrSha256": hashlib.sha256((output / "nvngx_dlssnr.dll").read_bytes()).hexdigest(),
}
if result.returncode == 0:
    report["executableSha256"] = hashlib.sha256(exe.read_bytes()).hexdigest()
(output / "DLSSNRPipelineTests.build.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
if result.returncode:
    sys.exit(result.returncode)
if options.compile_only:
    sys.exit(0)
arguments = [str(exe)] + (["--performance-only"] if options.performance_only else [])
arguments += (["--alternating-only"] if options.alternating_only else [])
arguments += (["--pipeline-only"] if options.pipeline_only else [])
arguments += (["--hot-cache-only"] if options.hot_cache_only else [])
arguments += (["--beta3-only"] if options.beta3_only else [])
arguments += (["--inference-only"] if options.inference_only else [])
arguments += (["--post-output-only"] if options.post_output_only else [])
arguments += (["--gpu-fusion-live-only"] if options.gpu_fusion_live_only else [])
arguments += (["--submission-worker-only"] if options.submission_worker_only else [])
arguments += (["--sparse-paired-input-only"] if options.sparse_paired_input_only else [])
arguments += (["--no-debug"] if options.no_debug else [])
with (output / "DLSSNRPipelineTests.stdout.log").open("w", encoding="utf-8") as log:
    process = subprocess.Popen(arguments, cwd=output, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding="utf-8", errors="replace")
    for line in process.stdout:
        print(line, end="", flush=True)
        log.write(line)
    result = process.wait()
report["exitCode"] = result
report["arguments"] = arguments[1:]
report["measurementArtifacts"] = {
    path.name: {"bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    for path in output.glob("DLSSNR*Costs.*") if path.suffix in (".csv", ".json")
}
(output / "DLSSNRPipelineTests.result.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
sys.exit(result)
