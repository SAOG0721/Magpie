"""Build an isolated, instrumented copy of the current DLSSNR backend.

The generated copy adds timestamp scopes only. Runtime DLLs are copied from the
selected deployment; the installed program and its configuration are untouched.
"""
from pathlib import Path
import argparse
import ctypes
import hashlib
import json
import re
import shutil
import subprocess
import sys

sys.stdout.reconfigure(errors="replace")
parser = argparse.ArgumentParser()
parser.add_argument("build_root", type=Path)
parser.add_argument("--core-library", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--compile-only", action="store_true")
options, benchmark_args = parser.parse_known_args()
repo = Path(__file__).resolve().parents[1]
root = options.build_root.resolve()
output = options.output.resolve()
runtime = next((root / folder for folder in ("build", "Magpie-Experimental-x64", "runtime")
                if (root / folder / "nvngx_dlssnr.dll").is_file()), None)
if runtime is None:
    parser.error("Runtime DLL directory missing")
if output == runtime or runtime in output.parents:
    parser.error("Output must be outside deployment")
output.mkdir(parents=True, exist_ok=True)
for file in runtime.glob("*.dll"):
    shutil.copy2(file, output / file.name)

source = (repo / "src/Magpie.Core/DLSSNRFilter.cpp").read_text(encoding="utf-8-sig")
instrumented_functions = {
    "PrepareInput": "input_prepare",
    "PrepareReducedGuidance": "guidance_reduce",
    "CompositeResidual": "residual_control_upsample_composite",
}
for function, label in instrumented_functions.items():
    pattern = rf"(static bool {function}\([\s\S]*?\) noexcept \{{)"
    source, count = re.subn(pattern, lambda match: match.group(1) +
        f'\n\tCostBenchmark::Region costRegion("{label}", impl.context11, impl.width, impl.height);', source, count=1)
    if count != 1:
        raise RuntimeError(f"Instrumentation point missing: {function}")
dispatch_start = "\t\tauto* dc = composition.context11;\n"
assert source.count(dispatch_start) == 1
source = source.replace(dispatch_start, dispatch_start + '''\t\tconst char* costStage = shader == publishShader.get() ? "publish_residual_shader" :
            shader == composeShader.get() ? "compose_current_shader" :
            shader == checkShader.get() ? "check_reuse_shader" :
            shader == motionShader.get() ? "motion_history_shader" :
            shader == coverageShader.get() ? "coverage_reduce_shader" : "other_performance_shader";
        CostBenchmark::Region costDispatch(costStage, dc, values.width, values.height);
''')
source=source.replace('#include "DLSSNRPerformance.inl"','#include "DLSSNRCostBenchmarkPerformance.generated.inl"')
prepared_copy = 'impl.context11->CopyResource(impl.sharedInput11.get(), preparedInput);'
if source.count(prepared_copy) != 1:
    raise RuntimeError("Prepared input copy instrumentation point changed")
source = source.replace(prepared_copy, '{ CostBenchmark::Region costPreparedCopy("prepared_input_copy", impl.context11, impl.width, impl.height); ' + prepared_copy + ' }')
(output / "DLSSNRCostBenchmarkFilter.generated.cpp").write_text(source, encoding="utf-8")
performance=(repo / "src/Magpie.Core/DLSSNRPerformance.inl").read_text(encoding="utf-8-sig")
publication='bool DLSSNRFilter::PublishPerformanceResult() noexcept {'
assert performance.count(publication)==1
performance=performance.replace(publication,publication+'\n    CostBenchmark::Region costPublish("publish_result_span_including_wait", _impl->context11, _impl->width, _impl->height);\n    CostBenchmark::CpuRegion costPublishCpu("publish_result_cpu", _impl->width, _impl->height);')
# Publish flushes internally. Finish its GPU marker before that existing flush,
# otherwise its end marker would not be submitted until the next source frame
# and the apparent duration would include a CPU submission gap of one period.
start=performance.index(publication)
end=performance.index('bool DLSSNRFilter::SubmitPerformanceInput',start)
body=performance[start:end]
assert body.count('\tdc->Flush();')==1
body=body.replace('\tdc->Flush();','\tcostPublish.Stop();\n\tdc->Flush();')
performance=performance[:start]+body+performance[end:]
(output / "DLSSNRCostBenchmarkPerformance.generated.inl").write_text(performance,encoding="utf-8")

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
def project_log(project, name):
    relative = f"{project}/{project}.tlog/{name}"
    return next(root / folder / relative for folder in ("obj", "obj/x64/Release") if (root / folder / relative).is_file())
def tlog(path):
    return "\n".join(line for line in path.read_text(encoding="utf-16").splitlines() if not line.startswith("^"))
clargs = command_args(tlog(project_log("Magpie.Core", "CL.command.1.tlog")))
includes = list(dict.fromkeys(arg for arg in clargs if arg.lower().startswith("/i")))
definitions = list(dict.fromkeys(clargs[i + 1] for i, arg in enumerate(clargs) if arg == "/D" and clargs[i + 1] != "NDEBUG"))
defs = [item for value in definitions for item in ("/D", value)]
# D3D12 per-pass timestamps are already in the product backend. Enabling this
# switch only adds CPU bookkeeping/log branches and is deliberately avoided.
linkargs = command_args(tlog(project_log("Magpie", "link.command.1.tlog")))
libs = list(dict.fromkeys(arg for arg in linkargs if arg.lower().endswith(".lib") or arg.lower().startswith("/libpath:")))
libs = [arg for arg in libs if Path(arg).name.lower() not in ("magpie.lib", "magpie.core.lib")]
libs.append(str(options.core_library.resolve()))
exe = output / "DLSSNRCostBenchmark.exe"
command = ["cl.exe", "/nologo", "/std:c++20", "/EHsc", "/utf-8", "/MT", "/O2", "/bigobj",
           "/I" + str(output), "/I" + str(repo / "src/Magpie.Core"), "/I" + str(repo / "src/Magpie.Core/include")]
command += includes + defs + [str(repo / "tests/DLSSNRCostBenchmark.cpp"), "/Fe:" + str(exe),
    "/Fo:" + str(exe.with_suffix(".obj")), "/link", "/LTCG"]
command += libs + ["d3d11.lib", "d3dcompiler.lib", "dxgi.lib", "d3d12.lib", "WindowsApp.lib"]
result = subprocess.run(command, cwd=repo / "src/Magpie.Core")
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
report = {"compileExitCode": result.returncode, "compileOnly": options.compile_only,
    "debugLayerRequested": False, "runtimeSource": str(runtime),
    "coreLibrary": str(options.core_library.resolve()), "coreLibrarySha256": sha(options.core_library),
    "sourceCommit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
    "sourceSha256": {name:sha(repo / name) for name in ["src/Magpie.Core/DLSSNRFilter.cpp", "src/Magpie.Core/DLSSNRPerformance.inl",
        "tests/DLSSNRCostBenchmark.cpp", "tests/DLSSNRCostBenchmarkGpu.h", "tests/run_dlssnr_cost_benchmark.py"]},
    "generatedFilterSha256": sha(output / "DLSSNRCostBenchmarkFilter.generated.cpp"),
    "generatedPerformanceSha256": sha(output / "DLSSNRCostBenchmarkPerformance.generated.inl"),
    "runtimeNrSha256": sha(output / "nvngx_dlssnr.dll"),
    "instrumentation": list(instrumented_functions) + ["Performance::Dispatch","PublishPerformanceResult"],
    "notes": ["D3D11 timeline spans include scheduling/queue dependencies between timestamps.",
        "D3D12 timings are NGX command-list region timings, not CPU API duration.",
        "Cold creation, final texture readback, and fixture texture upload are outside per-module GPU regions.",
        "Fixed-FPS output is a GPU-ready publication boundary; no desktop Present is exercised."]}
if result.returncode == 0:
    report["executableSha256"] = sha(exe)
(output / "DLSSNRCostBenchmark.build.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
if result.returncode or options.compile_only:
    sys.exit(result.returncode)
arguments = benchmark_args
if arguments[:1] == ["--"]:
    arguments = arguments[1:]
sys.exit(subprocess.call([str(exe)] + arguments, cwd=output))
