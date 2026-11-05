"""Compile the current product Filter/Sparse and wrapper against a new Core lib.

The copied base driver supplies its existing fixture/trace build machinery.
Only diagnostic timing scopes are inserted; no mode algorithm is substituted.
"""
from pathlib import Path

driver=Path(__file__).with_name("run_dlssnr_cost_benchmark.py")
source=driver.read_text(encoding="utf-8")
replacements={
    'str(repo / "tests/DLSSNRCostBenchmark.cpp")':'str(repo / "tests/DLSSNRSparseProductionProbe.cpp")',
    'output / "DLSSNRCostBenchmark.exe"':'output / "DLSSNRSparseProductionProbe.exe"',
    'output / "DLSSNRCostBenchmark.build.json"':'output / "DLSSNRSparseProductionProbe.build.json"',
}
for before,after in replacements.items():
    if source.count(before)!=1:raise RuntimeError(f"Base driver changed: {before}")
    source=source.replace(before,after)
marker="shell32 = ctypes.windll.shell32\n"
if source.count(marker)!=1:raise RuntimeError("Sparse instrumentation insertion changed")
source=source.replace(marker,'''sparse=(repo / "src/Magpie.Core/DLSSNRSparse.h").read_text(encoding="utf-8-sig")
dispatch="\\t\\tauto* cb=constants.get(); auto* sample=sampler.get();"
if sparse.count(dispatch)!=1:raise RuntimeError("Sparse Dispatch point changed")
sparse=sparse.replace(dispatch, "\\t\\tCostBenchmark::Region sparseRegion(shader==mapShader.get()?\\\"sparse_map_motion\\\":shader==composeShader.get()?\\\"sparse_compose_residual\\\":\\\"sparse_fill_output\\\",dc,width,height);\\n"+dispatch)
(output / "DLSSNRSparse.h").write_text(sparse,encoding="utf-8")
sparse_impl=(repo / "src/Magpie.Core/DLSSNRSparse.inl").read_text(encoding="utf-8-sig")
copies="\\tp.dc->CopyResource(native.input.texture.get(),_impl->sharedInput11.get());\\n\\tp.dc->CopyResource(native.output.texture.get(),last.sharedOutput11.get());"
if sparse_impl.count(copies)!=1:raise RuntimeError("Sparse native snapshot copy point changed")
sparse_impl=sparse_impl.replace(copies,"\\t{ CostBenchmark::Region snapshots(\\\"sparse_native_snapshots\\\",p.dc,_impl->width,_impl->height);\\n"+copies+"\\n\\t}")
(output / "DLSSNRSparse.inl").write_text(sparse_impl,encoding="utf-8")
pri=runtime / "resources.pri"
if pri.is_file():shutil.copy2(pri,output / pri.name)
source_core_library=options.core_library.resolve()
core_snapshot=output / "Magpie.Core.probe-source.lib"
shutil.copy2(source_core_library,core_snapshot)
options.core_library=core_snapshot
''' + marker)
marker="if result.returncode == 0:\n"
if source.count(marker)!=1:raise RuntimeError("Report insertion changed")
source=source.replace(marker,'''report["probe"]="actual_production_wrapper_sparse_real_flow"
report["currentFilterAndSparseCompiledIntoProbe"]=True
report["sourceCoreLibrary"]=str(source_core_library)
report["sourceSha256"].update({name:sha(repo / name) for name in [
    "src/Magpie.Core/DLSSNRFilter.h","src/Magpie.Core/DLSSNRMultiPass.h",
    "src/Magpie.Core/DLSSNRSparse.h","src/Magpie.Core/DLSSNRSparse.inl",
    "src/Magpie.Core/DLSSNRSparseShader.h","src/Magpie.Core/include/DLSSNRSparseState.h",
    "src/Magpie.Core/NativeEffectBackend.h","src/Magpie.Core/FrameGuidanceTypes.h",
    "src/Magpie.Core/include/DLSSNRSubmissionWorker.h","src/Magpie.Core/DLSSNRTemporal.cpp",
    "tests/DLSSNRSparseProductionProbe.cpp","tests/DLSSNRSparseProbeCapture.h",
    "tests/DLSSNRRecoveryScene.h","tests/DLSSNRBenchmarkPacing.h",
    "tests/run_dlssnr_sparse_production_probe.py"]})
report["generatedSparseSha256"]=sha(output / "DLSSNRSparse.h")
report["generatedSparseImplementationSha256"]=sha(output / "DLSSNRSparse.inl")
report["notes"].extend(["Actual FrameGuidanceService and normal NGX temporal history; no oracle validity, ideal motion or per-Evaluate reset.",
    "CoreLibrary must match the current four-mode stage ABI."])
''' + marker)
exec(compile(source,str(driver),"exec"),{"__name__":"__main__","__file__":str(driver)})
