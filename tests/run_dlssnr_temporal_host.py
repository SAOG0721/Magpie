"""Run the production Temporal host/HLSL on WARP using an existing Core build.

The build root supplies obj/ tlogs and a Core static library only. This runner
does not initialize NGX, select a hardware adapter, build or deploy Magpie.
Run from a VS x64 developer shell, supplying an isolated output directory.
"""
from pathlib import Path
import argparse
import ctypes
import hashlib
import json
import subprocess
import sys

sys.stdout.reconfigure(errors="replace")
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build_root", type=Path)
parser.add_argument("--core-library", type=Path)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
root = args.build_root.resolve()
core = args.core_library.resolve() if args.core_library else root / "runtime/Magpie.Core.lib"
output = args.output.resolve()
if not core.is_file():
    parser.error("Missing Core static library; supply --core-library.")
output.mkdir(parents=True, exist_ok=True)

shell = ctypes.windll.shell32
shell.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
shell.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]

def split_command(command):
    count = ctypes.c_int()
    pointer = shell.CommandLineToArgvW("tool " + command, ctypes.byref(count))
    result = [pointer[i] for i in range(1, count.value)]
    ctypes.windll.kernel32.LocalFree(pointer)
    return result

def tlog(project, filename):
    tail = f"{project}/{project}.tlog/{filename}"
    path = next((root / folder / tail for folder in ("obj", "obj/x64/Release")
                 if (root / folder / tail).is_file()), None)
    if path is None:
        parser.error(f"Missing {project} build tlog {filename}.")
    return split_command("\n".join(line for line in path.read_text(encoding="utf-16").splitlines()
                                   if not line.startswith("^")))

compiler = tlog("Magpie.Core", "CL.command.1.tlog")
includes = list(dict.fromkeys(arg for arg in compiler if arg.lower().startswith("/i")))
definitions = list(dict.fromkeys(compiler[i+1] for i, arg in enumerate(compiler)
                                if arg == "/D" and compiler[i+1] != "NDEBUG"))
linker = tlog("Magpie", "link.command.1.tlog")
libraries = list(dict.fromkeys(arg for arg in linker
                              if arg.lower().endswith(".lib") or arg.lower().startswith("/libpath:")))
libraries = [arg for arg in libraries if Path(arg).name.lower() not in ("magpie.lib", "magpie.core.lib")]
exe = output / "DLSSNRTemporalHostTests.exe"
command = ["cl.exe", "/nologo", "/std:c++20", "/EHsc", "/utf-8", "/MT", "/O2", "/W4", "/bigobj",
           "/I"+str(repo / "src/Magpie.Core"), "/I"+str(repo / "src/Magpie.Core/include")]
command += includes + [arg for definition in definitions for arg in ("/D", definition)]
command += [str(repo / "tests/DLSSNRTemporalHostTests.cpp"), "/Fe:"+str(exe), "/Fo:"+str(exe.with_suffix(".obj")),
            "/link", "/LTCG", str(core)] + libraries + ["d3d11.lib", "d3dcompiler.lib", "dxgi.lib", "WindowsApp.lib"]
build = subprocess.run(command, cwd=repo / "src/Magpie.Core", stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
print(build.stdout, end="", flush=True)
(output / "compile.log").write_text(build.stdout, encoding="utf-8")
files = ["src/Magpie.Core/DLSSNRTemporal.cpp", "src/Magpie.Core/DLSSNRTemporalShader.h",
         "src/Magpie.Core/include/DLSSNRTemporalState.h", "tests/DLSSNRTemporalHostTests.cpp",
         "tests/run_dlssnr_temporal_host.py"]
report = {"device": "D3D_DRIVER_TYPE_WARP", "productionHost": True, "compileExitCode": build.returncode,
          "coreLibrary": str(core), "coreLibrarySha256": hashlib.sha256(core.read_bytes()).hexdigest(),
          "sourceCommit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
          "sourceSha256": {file: hashlib.sha256((repo / file).read_bytes()).hexdigest() for file in files}}
if build.returncode == 0:
    run = subprocess.run([str(exe)], cwd=output, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, encoding="utf-8", errors="replace")
    print(run.stdout, end="", flush=True)
    (output / "stdout.log").write_text(run.stdout, encoding="utf-8")
    report["exitCode"] = run.returncode
    report["executableSha256"] = hashlib.sha256(exe.read_bytes()).hexdigest()
(output / "result.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
sys.exit(report.get("exitCode", build.returncode))
