"""Isolated native SR/FG wrapper probe using candidate objects and tlogs.
Run inside a VS x64 developer shell. A new output directory snapshots a build;
subsequent calls reuse that snapshot and compile the helper, never edit the app.
"""
from pathlib import Path
import argparse,ctypes,subprocess,shutil,json,hashlib
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--build-root',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--case',nargs=3,required=True,metavar=('KIND','PRESET','MULTIPLIER'));p.add_argument('--skip-build',action='store_true');a=p.parse_args()
a.source=a.source.resolve();a.build_root=a.build_root.resolve();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
runtime=a.output/'runtime';objs=a.output/'objects';tlogs=a.output/'tlogs'
if not runtime.exists():
 runtime.mkdir();objs.mkdir()
 for t in ['Magpie.Core','Magpie']:
  (tlogs/t).mkdir(parents=True)
  for f in (a.build_root/'obj/x64/Release'/t/(t+'.tlog')).glob('*.command.1.tlog'):shutil.copy2(f,tlogs/t/f.name)
 for f in (a.build_root/'obj/x64/Release/Magpie.Core').glob('*.obj'):shutil.copy2(f,objs/f.name)
 package=a.build_root/'Magpie-Experimental-x64'
 for name in ['nvngx_dlss.dll','nvngx_dlssnr.dll','nvngxruntime.dll','libxell.dll','libxess_fg.dll','libxess.dll','ffx_fsr2_api_dx11_x64.dll','ffx_fsr2_api_x64.dll','build-manifest.json']:shutil.copy2(package/name,runtime/name)
 shutil.copytree(package/'runtimes/dlssg_sm86',runtime/'runtimes/dlssg_sm86')
shell=ctypes.windll.shell32;shell.CommandLineToArgvW.argtypes=[ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_int)];shell.CommandLineToArgvW.restype=ctypes.POINTER(ctypes.c_wchar_p);ctypes.windll.kernel32.LocalFree.argtypes=[ctypes.c_void_p]
def args(path):
 text='\n'.join(x for x in path.read_text(encoding='utf-16').splitlines() if not x.startswith('^'))
 n=ctypes.c_int();v=shell.CommandLineToArgvW('tool '+text,ctypes.byref(n));r=[v[i] for i in range(1,n.value)];ctypes.windll.kernel32.LocalFree(v);return r
exe=runtime/'DLSSPresetSmoke.exe'
if not a.skip_build:
 if not (objs/'pch.obj').is_file(): raise SystemExit('Candidate object snapshot is incomplete: pch.obj missing; wait for build completion and use a fresh output directory.')
 archive=a.output/'Magpie.Core.lib'
 rsp=a.output/'objects.rsp';rsp.write_text('\n'.join('"'+str(f)+'"' for f in objs.glob('*.obj')),encoding='utf-8')
 subprocess.run(['lib.exe','/NOLOGO','/OUT:'+str(archive),'@'+str(rsp)],check=True)
 cl=args(tlogs/'Magpie.Core/CL.command.1.tlog');link=args(tlogs/'Magpie/link.command.1.tlog')
 includes=list(dict.fromkeys(x for x in cl if x.lower().startswith('/i')))
 defs=list(dict.fromkeys(cl[i+1] for i,x in enumerate(cl) if x=='/D' and cl[i+1]!='NDEBUG'))
 libs=list(dict.fromkeys(x for x in link if x.lower().endswith('.lib') or x.lower().startswith('/libpath:')))
 libs=[x for x in libs if Path(x).name.lower() not in ('magpie.lib','magpie.core.lib')]
 cmd=['cl.exe','/nologo','/std:c++20','/EHsc','/utf-8','/MT','/O2','/bigobj','/I'+str(a.source/'src/Magpie.Core'),'/I'+str(a.source/'src/Magpie.Core/include')]+includes+[y for x in defs for y in ['/D',x]]+[str(Path(__file__).with_name('DLSSPresetSmoke.cpp')),'/Fe:'+str(exe),'/Fo:'+str(a.output/'smoke.obj'),'/link','/LTCG','/MANIFEST:EMBED','/MANIFESTINPUT:'+str(Path(__file__).with_name('DLSSPresetSmoke.manifest')),str(archive)]+libs+['d3d11.lib','d3dcompiler.lib','dxgi.lib','d3d12.lib','WindowsApp.lib']
 (a.output/'compile-command.json').write_text(json.dumps(cmd,indent=2),encoding='utf-8')
 compiled=subprocess.run(cmd,cwd=a.source/'src/Magpie.Core')
 if compiled.returncode: raise SystemExit(compiled.returncode)
name='-'.join(a.case);case=a.output/name;index=1
while case.exists():
 case=a.output/(name+'-attempt'+str(index));index+=1
case.mkdir(exist_ok=False);(case/'logs').mkdir()
# DLL search uses the immutable EXE-side snapshot; logs/cache are per child cwd.
try:
 r=subprocess.run([str(exe)]+a.case,cwd=case,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=120)
 result={'case':a.case,'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
except subprocess.TimeoutExpired as error:
 result={'case':a.case,'returncode':'timeout','stdout':str(error.stdout),'stderr':str(error.stderr)}
(case/'result.json').write_text(json.dumps(result,indent=2,ensure_ascii=False),encoding='utf-8')
print(json.dumps({'case':result['case'],'returncode':result['returncode'],'evidence':str(case),'summary':[line for line in result['stdout'].splitlines() if line.startswith(('kind=','GPU=','featureCreated=','gpuCompleted=','evaluateReturned=','duplicateOutputUnchanged=','PASS','FAIL'))],'stderr':result['stderr']},ensure_ascii=False))
raise SystemExit(0 if result['returncode']==0 else 1)
