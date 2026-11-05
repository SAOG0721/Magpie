"""Build/run an isolated actual-provider probe using an existing Core library."""
from pathlib import Path
import argparse
import ctypes
import hashlib
import json
import os
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("build_root", type=Path)
parser.add_argument("--core-library", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
root, output = args.build_root.resolve(), args.output.resolve()
runtime = root / "Magpie-Experimental-x64"
if output == runtime or runtime in output.parents:
    parser.error("Probe output must remain outside the deployed runtime")
output.mkdir(parents=True, exist_ok=True)
shell = ctypes.windll.shell32
shell.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
shell.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]

def arguments(text):
    count = ctypes.c_int()
    pointer = shell.CommandLineToArgvW("tool " + text, ctypes.byref(count))
    result = [pointer[i] for i in range(1, count.value)]
    ctypes.windll.kernel32.LocalFree(pointer)
    return result

def log(project, name):
    path = root / f"obj/x64/Release/{project}/{project}.tlog/{name}"
    return arguments("\n".join(line for line in path.read_text(encoding="utf-16").splitlines()
                               if not line.startswith("^")))

cl = log("Magpie.Core", "CL.command.1.tlog")
includes = list(dict.fromkeys(arg for arg in cl if arg.lower().startswith("/i")))
definitions = list(dict.fromkeys(cl[i + 1] for i, arg in enumerate(cl)
                                 if arg == "/D" and cl[i + 1] != "NDEBUG"))
link = log("Magpie", "link.command.1.tlog")
libs = list(dict.fromkeys(arg for arg in link if arg.lower().endswith(".lib") or arg.lower().startswith("/libpath:")))
libs = [arg for arg in libs if Path(arg).name.lower() not in ("magpie.lib", "magpie.core.lib")]
exe = output / "FrameGuidanceScalingNativeTests.exe"
command = ["cl.exe", "/nologo", "/std:c++20", "/EHsc", "/utf-8", "/MT", "/O2", "/bigobj",
           "/I" + str(repo / "src/Magpie.Core"), "/I" + str(repo / "src/Magpie.Core/include")]
command += includes + [item for value in definitions for item in ("/D", value)]
command += ["/FI" + str(repo / "tests/FrameGuidanceScalingProbeHost.h"),
            str(repo / "tests/FrameGuidanceScalingNativeTests.cpp")]
command += [str(repo / "src/Magpie.Core" / name) for name in
            ("ZeroFrameGuidanceProvider.cpp", "AmdOpticalFlowProvider.cpp", "NvidiaOpticalFlowProvider.cpp")]
command += ["/Fe:" + str(exe), "/Fo:" + str(output) + os.sep, "/link", "/LTCG"]
command += libs + [str(args.core_library.resolve()), "d3d11.lib", "d3dcompiler.lib", "dxgi.lib", "d3d12.lib", "WindowsApp.lib"]
print("Compiling actual FrameGuidanceService/provider probe", flush=True)
result = subprocess.run(command, cwd=repo / "src/Magpie.Core")
if result.returncode:
    sys.exit(result.returncode)
env = dict(os.environ)
env["PATH"] = str(runtime) + os.pathsep + env.get("PATH", "")
cases = [(method, quality) for method, qualities in ((1, (0, 1)), (2, (1, 2, 3, 4, 5)))
         for quality in qualities] + [("mixed",)]
cases += [(1, 0, 127, 127), (1, 1, 64, 64), (1, 0, 126, 126), (1, 1, 63, 63)]
results = []
with (output / "FrameGuidanceScalingNativeTests.stdout.log").open("w", encoding="utf-8") as stdout:
    for case in cases:
        process = subprocess.Popen([str(exe)] + [str(value) for value in case], cwd=output, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
        lines = []
        for line in process.stdout:
            print(line, end="", flush=True)
            stdout.write(line)
            lines.append(line)
        code = process.wait()
        results.append({"case": case, "exitCode": code, "unsupported": any("UNSUPPORTED" in line for line in lines)})
        print("CASE_EXIT", case, code, flush=True)
        native_log = output / "FrameGuidanceScalingNativeTests.log"
        if native_log.exists():
            (output / ("provider-" + "-".join(map(str, case)) + ".log")).write_bytes(native_log.read_bytes())
result = int(any(case["exitCode"] for case in results))
paths = ["src/Magpie.Core/" + name for name in ("FrameGuidanceService.cpp", "FrameGuidanceService.h",
         "MotionVectorRequest.h", "AmdOpticalFlowProvider.cpp", "NvidiaOpticalFlowProvider.cpp")]
report = {"exitCode": result, "coreLibrary": str(args.core_library.resolve()), "runtimeSearchPath": str(runtime),
          "cases": results,
          "testHost": "Immutable default SDR ScalingOptions lookup; no XAML ResourceLoader or user configuration",
          "sourceCommit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
          "sourceDirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=repo, text=True).strip()),
          "probeSha256": {"tests/" + name: hashlib.sha256((repo / "tests" / name).read_bytes()).hexdigest()
                          for name in ("FrameGuidanceScalingNativeTests.cpp", "FrameGuidanceScalingProbeHost.h", "run_frame_guidance_scaling_native.py")},
          "sourceSha256": {path: hashlib.sha256((repo / path).read_bytes()).hexdigest() for path in paths}}
(output / "FrameGuidanceScalingNativeTests.result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
sys.exit(result)
