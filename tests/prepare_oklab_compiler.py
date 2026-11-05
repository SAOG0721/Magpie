"""Compile the actual EffectCompiler and its support sources in isolation.

Uses only dependency include directories from a known build, never its product
objects, libraries, configuration, runtime directory or outputs.
"""
from pathlib import Path
import argparse
import ctypes
import shutil
import subprocess

p=argparse.ArgumentParser()
p.add_argument('cache',type=Path)
p.add_argument('cache_repo',type=Path)
args=p.parse_args()
repo=Path(__file__).resolve().parents[1]
out=repo/'obj/tests/oklab-compiler';out.mkdir(parents=True,exist_ok=True)
shutil.copytree(repo/'src/Effects/OKLab',out/'effects/OKLab',dirs_exist_ok=True)
shutil.copy2(repo/'src/Effects/OKLabImageAdjustment.hlsl',out/'effects/OKLabImageAdjustment.hlsl')
shell=ctypes.windll.shell32
shell.CommandLineToArgvW.argtypes=[ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_int)]
shell.CommandLineToArgvW.restype=ctypes.POINTER(ctypes.c_wchar_p)
ctypes.windll.kernel32.LocalFree.argtypes=[ctypes.c_void_p]
text=(args.cache/'x64/Release/Magpie.Core/Magpie.Core.tlog/CL.command.1.tlog').read_text(encoding='utf-16')
text=' '.join(line for line in text.splitlines() if line and not line.startswith('^'))
count=ctypes.c_int();ptr=shell.CommandLineToArgvW('tool '+text,ctypes.byref(count))
tokens=[ptr[i] for i in range(1,count.value)];ctypes.windll.kernel32.LocalFree(ptr)
includes=[str(repo/'src/Magpie.Core'),str(repo/'src/Magpie.Core/include'),str(repo/'src/Shared')]
defines=[]
for i,token in enumerate(tokens):
    if token.lower().startswith(('/i','/external:i')):
        path=Path(token.split(':',1)[1][1:] if token.lower().startswith('/external:i') else token[2:])
        if not path.is_absolute():path=args.cache_repo/'src/Magpie.Core'/path
        includes.append(str(path))
    elif token.upper()=='/D':defines.append(tokens[i+1])
defines=[d for d in dict.fromkeys(defines) if d not in ('NDEBUG','SPDLOG_COMPILED_LIB')]
cmd=['cl.exe','/nologo','/std:c++20','/EHsc','/utf-8','/MT','/O2','/Gy','/bigobj',
     '/DFMT_HEADER_ONLY']
cmd+=['/I'+i for i in dict.fromkeys(includes)]+['/D'+d for d in defines]
sources=['tests/OKLabCompilerTests.cpp','src/Magpie.Core/EffectCompiler.cpp',
         'src/Magpie.Core/EffectCacheManager.cpp','src/Magpie.Core/DirectXHelper.cpp',
         'src/Magpie.Core/Win32Helper.cpp','src/Shared/StrHelper.cpp','src/Shared/Logger.cpp',
         'src/Shared/SmallVector.cpp','src/Magpie.Core/NvidiaForegroundCompatibility.cpp']
minhook=next(lib for include in includes if (Path(include)/'MinHook.h').is_file() for lib in (Path(include).parent/'lib').glob('*.lib'))
cmd += [str(repo/s) for s in sources]+['/Fe:'+str(out/'OKLabCompilerTests.exe'),'/Fo:'+str(out)+'/',
    '/link','/OPT:REF','d3d11.lib','d3dcompiler.lib','dxgi.lib','WindowsApp.lib','user32.lib','gdi32.lib',
    'advapi32.lib','shell32.lib','ole32.lib','dwmapi.lib','dcomp.lib','shcore.lib','ntdll.lib',str(minhook)]
subprocess.run(cmd,cwd=out,check=True)
subprocess.run([str(out/'OKLabCompilerTests.exe')],cwd=out,check=True)
