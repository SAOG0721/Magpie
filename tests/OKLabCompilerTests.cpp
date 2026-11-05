#include "pch.h"
#include "EffectCompiler.h"
#include "EffectDesc.h"
#include "Logger.h"
#include "OKLabParameterTable.h"
#include <fstream>
#include <iostream>

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
    Logger::Get().Flush();
}
