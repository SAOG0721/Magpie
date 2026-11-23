#include "pch.h"
#include "DLSSNRDetailParameters.h"
#include "EffectCompiler.h"
#include "EffectDesc.h"
#include "Logger.h"
#include "NvidiaVfxParameterPolicy.h"
#include "OKLabParameterTable.h"
#include "ScalingOptions.h"
#include "StrHelper.h"
#include <fstream>
#include <iostream>

namespace {
using namespace Magpie;

bool CompileParameterCase(const std::string& effect, uint32_t flags,
    const phmap::flat_hash_map<std::string, float>& values, bool expectSuccess = true) {
    EffectDesc desc{ .name = effect };
    const uint32_t status = EffectCompiler::Compile(desc, flags | EffectCompilerFlags::NoCache, &values);
    const bool passed = expectSuccess
        ? status == 0 && !desc.passes.empty() &&
            std::ranges::all_of(desc.passes, [](const auto& pass) { return bool(pass.cso); })
        : status != 0;
    if (!passed) std::cerr << "Unexpected shader compile result: " << effect << ", flags=" << flags << '\n';
    return passed;
}

bool CheckInlineParameters() {
    phmap::flat_hash_map<std::wstring, float> oldDlssNr{ { L"processingMode", 1.f } };
    NormalizeDLSSNRDetailParameters(oldDlssNr);
    if (!oldDlssNr.contains(L"__dlssnrModeVersion")) return false;
    phmap::flat_hash_map<std::string, float> dlssNr;
    for (const auto& [name, value] : oldDlssNr) dlssNr.emplace(StrHelper::UTF16ToUTF8(name), value);

    EffectOption vsr{ .name = std::string(NVIDIA_VFX_VSR_ID) };
    vsr.parameters.emplace("strength", 2.f);
    ChangeNvidiaVfxVsrCategory(vsr, 3);
    ChangeNvidiaVfxVsrCategory(vsr, 0);
    if (!vsr.parameters.contains(NvidiaVfxStateKey<char>(0, "scaleX"))) return false;

    constexpr uint32_t inlineFlag = EffectCompilerFlags::InlineParams;
    const std::string dlssNrEffect = "DLSSNR\\DLSSNR_AI_Filter";
    for (const uint32_t flags : { 0u, inlineFlag }) {
        if (!CompileParameterCase(dlssNrEffect, flags, dlssNr) ||
            !CompileParameterCase(vsr.name, flags, vsr.parameters)) return false;
    }
    if (!CompileParameterCase(dlssNrEffect, inlineFlag | EffectCompilerFlags::HdrCompatibility |
        EffectCompilerFlags::NoFP16, dlssNr)) return false;

    EffectDesc first{ .name = dlssNrEffect };
    EffectDesc cached{ .name = dlssNrEffect };
    if (EffectCompiler::Compile(first, inlineFlag, &dlssNr) || first.passes.empty()) return false;
    dlssNr.erase("__dlssnrModeVersion");
    if (EffectCompiler::Compile(cached, inlineFlag, &dlssNr) || cached.passes.empty() ||
        cached.passes.front().cso.get() != first.passes.front().cso.get()) {
        std::cerr << "DLSSNR saved state changed the shader cache key\n";
        return false;
    }

    vsr.parameters.emplace("__nvidiaVfx0.unknownSuffix", 1.f);
    if (!CompileParameterCase(vsr.name, inlineFlag, vsr.parameters, false)) return false;
    std::cout << "Production EffectCompiler: DLSSNR migration and VSR category state accepted with inline parameters\n";
    return true;
}
}

int main() {
    using namespace Magpie;
    Logger::Get().Initialize(spdlog::level::info,L"compiler.log",10000000,1);
    const char* formats[]{"fp32","fp16","r8"};
    for(unsigned format=0;format<3;++format) {
        EffectDesc desc;desc.name="OKLabImageAdjustment";
        uint32_t flags=EffectCompilerFlags::NoCache|EffectCompilerFlags::WarningsAreErrors|EffectCompilerFlags::SaveSources;
        if(format<2)flags|=(format+1)<<EffectCompilerFlags::InputFormatShift|(format+1)<<EffectCompilerFlags::OutputFormatShift;
        if(EffectCompiler::Compile(desc,flags)!=0) {std::cerr<<"Production compiler failed: "<<formats[format]<<'\n';return 1;}
        if(desc.params.size()!=std::size(OKLab::Parameters)||desc.textures.size()!=6||desc.passes.size()!=3)return 2;
        for(size_t i=0;i<desc.params.size();++i)if(desc.params[i].name!=OKLab::Parameters[i].name)return 3;
        for(size_t pass=0;pass<3;++pass) {
            const auto& p=desc.passes[pass];
            if(p.blockSize.first!=16||p.blockSize.second!=16)return 4;
            std::ofstream out(std::string(formats[format])+"-pass"+std::to_string(pass)+".cso",std::ios::binary);
            out.write(static_cast<const char*>(p.cso->GetBufferPointer()),p.cso->GetBufferSize());
        }
        std::cout<<"Production EffectCompiler: "<<formats[format]<<", "<<desc.params.size()<<" parameters, 3 passes\n";
    }
    if (!CheckInlineParameters()) return 5;
    Logger::Get().Flush();
}
