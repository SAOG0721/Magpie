#include "pch.h"
#include "Renderer.h"
#include <DirectXPackedVector.h>
#include <cstring>

namespace Magpie {
bool Renderer::RequestOKLabSample(uint32_t effect,float u,float v,std::shared_ptr<OKLab::PickResult> result) noexcept {
    if(!result || !std::isfinite(u) || !std::isfinite(v) || IsStopping() || !_backendThreadDispatcher)return false;
    return _backendThreadDispatcher.TryEnqueue([this,session=_sessionLifetime,effect,u,v,result] {
        bool success=false;
        const auto publish=wil::scope_exit([&]{result->completion.store(success?1:-1,std::memory_order_release);});
        if(session->IsStopping() || effect>=_effectDrawers.size() || effect>=_runtimeEffectOptions.size() ||
            _runtimeEffectOptions[effect].name!=OKLab::EffectId)return;
        auto* source=_effectDrawers[effect].GetTexture(0);if(!source)return;
        D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        const auto format=desc.Format;
        if(format!=DXGI_FORMAT_R8G8B8A8_UNORM && format!=DXGI_FORMAT_B8G8R8A8_UNORM &&
            format!=DXGI_FORMAT_R16G16B16A16_FLOAT && format!=DXGI_FORMAT_R32G32B32A32_FLOAT)return;
        const UINT x=std::min(desc.Width-1,UINT(std::clamp(u,0.f,1.f)*desc.Width));
        const UINT y=std::min(desc.Height-1,UINT(std::clamp(v,0.f,1.f)*desc.Height));
        desc.Width=desc.Height=desc.MipLevels=desc.ArraySize=1;desc.BindFlags=0;desc.MiscFlags=0;
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging;
        if(FAILED(_backendResources.GetD3DDevice()->CreateTexture2D(&desc,nullptr,staging.put())))return;
        auto* dc=_backendResources.GetD3DDC();D3D11_BOX box{x,y,0,x+1,y+1,1};
        dc->CopySubresourceRegion(staging.get(),0,0,0,0,source,0,&box);
        D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(dc->Map(staging.get(),0,D3D11_MAP_READ,0,&mapped)))return;
        OKLab::Color color{};
        for(unsigned c=0;c<3;++c){
            if(format==DXGI_FORMAT_R16G16B16A16_FLOAT)color[c]=DirectX::PackedVector::XMConvertHalfToFloat(static_cast<const DirectX::PackedVector::HALF*>(mapped.pData)[c]);
            else if(format==DXGI_FORMAT_R32G32B32A32_FLOAT)color[c]=static_cast<const float*>(mapped.pData)[c];
            else color[c]=static_cast<const uint8_t*>(mapped.pData)[format==DXGI_FORMAT_B8G8R8A8_UNORM?2-c:c]/255.f;
        }
        dc->Unmap(staging.get(),0);
        float white=203;const auto it=_runtimeEffectOptions[effect].parameters.find("referenceWhite");
        if(it!=_runtimeEffectOptions[effect].parameters.end())white=OKLab::Normalize(*OKLab::Find("referenceWhite"),it->second);
        for(float& c:color)c=_effectDrawers[effect].IsOKLabLinearInput()?c*(80.f/white):OKLab::Decode(c);
        result->lab=OKLab::ToLab(color);success=true;
    });
}
}
