"""Build one diagnostic derivative of the existing real-production probe.

Only files in the selected, new output directory are generated. Product source
is not edited. All audit reductions are read back after the production loop.
"""
from pathlib import Path

driver = Path(__file__).with_name("run_dlssnr_cost_benchmark.py")
source = driver.read_text(encoding="utf-8")


def once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError(f"Flow audit anchor changed: {before[:120]}")
    return text.replace(before, after)


for before, after in {
    'str(repo / "tests/DLSSNRCostBenchmark.cpp")': 'str(repo / "tests/DLSSNRFlowRejectionProbe.cpp")',
    'output / "DLSSNRCostBenchmark.exe"': 'output / "DLSSNRFlowRejectionProbe.exe"',
    'output / "DLSSNRCostBenchmark.build.json"': 'output / "DLSSNRFlowRejectionProbe.build.json"',
    '"/I" + str(output),': '"/I" + str(output), "/I" + str(repo / "tests"),',
}.items():
    source = once(source, before, after)

prepare = r'''
# Derive the driver main without changing the established immutable probes.
production=(repo / "tests/DLSSNRSparseProductionProbe.cpp").read_text(encoding="utf-8-sig")
def change(before,after):
    global production
    if production.count(before)!=1:raise RuntimeError(f"Production audit anchor changed: {before[:100]}")
    production=production.replace(before,after)
change('int idleAfter=-1,idleMs=200,tailMs=250;', 'int idleAfter=-1,idleMs=200,tailMs=250; int auditFirst=12,auditEvery=30;')
change('else if(arg=="--diagnostic-validity")o.diagnosticValidity=true;',
       'else if(arg=="--diagnostic-validity")o.diagnosticValidity=true;\n            else if(arg=="--audit-first")o.auditFirst=std::stoi(value());\n            else if(arg=="--audit-every")o.auditEvery=std::stoi(value());')
change('Require(b.scene=="translate"||b.scene=="static"', 'Require(b.scene=="translate-low"||b.scene=="translate"||b.scene=="static"')
change('if(o.validateOnly){ProductionProbeScene::Compile();ProbeValidityAudit::Compile();',
       'Require(o.auditFirst>=0&&o.auditEvery>=0,"audit window");\n        if(o.validateOnly){ProductionProbeScene::Compile();ProbeValidityAudit::Compile();FlowRejectionAudit::ValidateShaders();')
change('Production scene and diagnostic audit compiled on CPU; no GPU created', 'Production scene, validity and flow-factor audits compiled on CPU; no GPU created')
change('const auto kind=o.base.scene=="translate"?', 'const auto kind=(o.base.scene=="translate"||o.base.scene=="translate-low")?')
change('uint32_t(o.base.scene=="translate"),0', 'uint32_t(o.base.scene=="translate-low"?2:o.base.scene=="translate"),0')
change('output[id.xy]=float4(SceneColorBytes(kind,frame,width,height,warmup,period,p)/255.,1);',
       'if(translate==2){float2 q=float2(p)+.5;float3 c=float3(.45+.19*sin(q.x*.051)+.14*cos(q.y*.043),.47+.17*sin((q.x+q.y)*.031)+.12*cos(q.y*.061),.43+.18*cos(q.x*.041)+.15*sin((q.x-q.y)*.027));output[id.xy]=float4(floor(saturate(c)*255+.5)/255.,1);}\n    else output[id.xy]=float4(SceneColorBytes(kind,frame,width,height,warmup,period,p)/255.,1);')
change('auto* dc=resources.GetD3DDC();Fixture fixture',
       'auto* dc=resources.GetD3DDC();FlowRejectionAudit::Observer flowAudit(resources.GetD3DDevice(),dc,o.auditFirst,o.auditEvery,b.scene=="translate"||b.scene=="translate-low");flowAudit.useGT=b.scene=="static"||b.scene=="translate"||b.scene=="translate-low";FlowRejectionAudit::observer=&flowAudit;Fixture fixture')
change('Logger::Get().Flush();std::cout<<"Production probe complete:',
       'FlowRejectionAudit::observer=nullptr;flowAudit.Write(b.prefix);Logger::Get().Flush();std::cout<<"Flow rejection probe complete:')
change('"<<(o.diagnosticValidity?"diagnostic_readback":o.dumpTail?"quality_gpu_snapshots":"performance")<<"',
       '"<<"probe_gpu_audit_postrun_readback"<<"')
(output / "DLSSNRFlowRejectionProduction.generated.cpp").write_text(production,encoding="utf-8")

sparse=(repo / "src/Magpie.Core/DLSSNRSparse.h").read_text(encoding="utf-8-sig")
anchor='\t\tdc->CSSetShaderResources(0,28,empty); dc->CSSetUnorderedAccessViews(0,5,out,nullptr); dc->CSSetShader(nullptr,nullptr,0);'
if sparse.count(anchor)!=1:raise RuntimeError("Sparse audit unbind anchor changed")
hook="""
        if(FlowRejectionAudit::observer && (shader==changeShader.get() || shader==composeShader.get())) {
            const bool pair=shader==changeShader.get();
            auto* current=Find(sequence);auto* previous=sequence?Find(sequence-1):nullptr;
            FlowRejectionAudit::Context audit{.stage=pair?"physical_precheck":"target_compose",.recovery=recovery.Name(),
                .physical=lastFrame,.sequence=sequence,.fullHistory=lastInferenceFrame,.liteHistory=lastLiteInferenceFrame};
            if(pair){audit.target=current?current->frame:0;audit.past=previous?previous->frame:0;
                audit.reverse=current&&previous&&current->guidance.HasReverseMotionForPair(current->frame,previous->frame,{width,height});}
            else {
                for(const auto& frame:captures)if(frame.input.srv.get()==srvs[0])audit.target=frame.frame;
                for(const auto& result:results){if(result.reference.srv.get()==srvs[1])audit.past=result.frame;if(result.reference.srv.get()==srvs[3])audit.future=result.frame;}
            }
            FlowRejectionAudit::observer->Capture(audit,width,height,&values,srvs,rawValidity.srv.get(),
                current?current->reverseMotion.srv.get():nullptr,current?current->reverseConfidence.srv.get():nullptr);
        }
"""
sparse=sparse.replace(anchor,anchor+hook)
(output / "DLSSNRSparse.h").write_text(sparse,encoding="utf-8")
pri=runtime / "resources.pri"
if pri.is_file():shutil.copy2(pri,output / pri.name)
source_core_library=options.core_library.resolve()
core_snapshot=output / "Magpie.Core.probe-source.lib"
shutil.copy2(source_core_library,core_snapshot)
options.core_library=core_snapshot
'''
source = once(source, "shell32 = ctypes.windll.shell32\n", prepare + "\nshell32 = ctypes.windll.shell32\n")
report = r'''
report["probe"]="real_production_flow_rejection_factor_audit"
report["sourceCoreLibrary"]=str(source_core_library)
report["sourceSha256"].update({name:sha(repo / name) for name in [
    "src/Magpie.Core/DLSSNRSparse.h","src/Magpie.Core/DLSSNRSparse.inl",
    "src/Magpie.Core/DLSSNRSparseShader.h","src/Magpie.Core/NvidiaOpticalFlowProvider.cpp",
    "src/Magpie.Core/AmdOpticalFlowProvider.cpp","tests/DLSSNRSparseProductionProbe.cpp",
    "tests/DLSSNRFlowRejectionProbe.cpp","tests/DLSSNRFlowRejectionAudit.h",
    "tests/run_dlssnr_flow_rejection_probe.py"]})
report["generatedSparseSha256"]=sha(output / "DLSSNRSparse.h")
report["generatedProductionProbeSha256"]=sha(output / "DLSSNRFlowRejectionProduction.generated.cpp")
report["notes"].extend(["GPU reduction audits change GPU scheduling; diagnostic runs are not performance baselines.",
    "GT exists only in extra audit shader b1. The real flow provider, Sparse and NGX receive no GT.",
    "Audit buffers are read back after the production loop; public dense confidence includes provider processing."])
'''
source = once(source, "if result.returncode == 0:\n", report + "\nif result.returncode == 0:\n")
exec(compile(source, str(driver), "exec"), {"__name__": "__main__", "__file__": str(driver)})
