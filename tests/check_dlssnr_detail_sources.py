"""Syntax-check changed native/UI sources using existing build dependencies.

No product object, executable, package or deployment is generated.
"""
from pathlib import Path
import ctypes
import subprocess
import sys

repo = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
shell = ctypes.windll.shell32
shell.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
shell.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]
for project, sources in {
    "Magpie.Core": ["DLSSNRFilter.cpp", "DLSSNRTemporal.cpp", "NativeEffectBackendFactory.cpp", "Renderer.cpp", "StepTimer.cpp"],
    "Magpie": ["ScalingModeItem.cpp", "ScalingModesService.cpp"],
}.items():
    relative = f"{project}/{project}.tlog/CL.command.1.tlog"
    path = next((build / folder / relative for folder in ("obj", "obj/x64/Release")
                 if (build / folder / relative).is_file()), build / "obj" / relative)
    raw = " ".join(line for line in path.read_text(encoding="utf-16").splitlines()
                   if not line.startswith("^"))
    count = ctypes.c_int()
    pointer = shell.CommandLineToArgvW("tool " + raw, ctypes.byref(count))
    args = [pointer[i] for i in range(1, count.value)]
    ctypes.windll.kernel32.LocalFree(pointer)
    includes = list(dict.fromkeys(arg for arg in args if arg.lower().startswith("/i")))
    definitions = list(dict.fromkeys(args[i+1] for i,arg in enumerate(args) if arg == "/D"))
    for source in sources:
        command = ["cl.exe", "/nologo", "/Zs", "/std:c++20", "/EHsc", "/utf-8", "/W4", "/WX", "/bigobj",
                   "/I"+str(repo/"src/Magpie.Core/include"), "/I"+str(repo/"src/Magpie.Core")]
        command += includes + [item for value in definitions for item in ("/D",value)]
        command += [str(repo/f"src/{project}/{source}")]
        subprocess.run(command, cwd=repo/f"src/{project}", check=True)
print("Current production native/UI sources passed MSVC syntax checks using existing dependencies.")
