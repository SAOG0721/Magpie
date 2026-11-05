#pragma once
#include "OKLabParameters.h"
#include "OKLabLut.h"
#include <chrono>
#include <d3d11.h>
#include <wrl/client.h>

namespace Magpie {
// Owns only per-instance adjustment state. EffectDrawer owns the actual image
// pipeline and its textures, so no parallel backend or SDK path is introduced.
class OKLabRuntime {
public:
    template<class Get> bool Configure(ID3D11Device* device,Get&& get,bool hdr) noexcept {
        const bool automatic=OKLab::Value("autoExposure",get)!=0 || OKLab::Value("autoWhite",get)!=0;
        _locked=OKLab::Value("autoLock",get)!=0;
        if(automatic!=_automatic)Reset(true);
        _automatic=automatic;
        _neutralExceptLut=OKLab::Neutral([&](std::string_view name,float fallback) {
            return name=="lutAmount"?0.f:get(name,fallback);
        }) && (!hdr ||
            (OKLab::Value("outputGamut",get)==2 && OKLab::Value("peakNits",get)==1000));
        _lutStrength=OKLab::Value("lutAmount",get);
        _UpdateNeutral();
        _constants.linearInput=hdr;
        if(!_buffer) {
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(Constants);desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if(FAILED(device->CreateBuffer(&desc,nullptr,&_buffer)))return false;
        }
        return true;
    }
    bool SetLut(ID3D11DeviceContext* dc,ID3D11Texture2D* target,std::string_view text,std::string& error) {
        OKLab::Cube cube;
        if(!text.empty() && !OKLab::ParseCube(text,cube,error))return false;
        auto data=OKLab::Atlas(cube);
        dc->UpdateSubresource(target,0,nullptr,data.data(),OKLab::LutAtlasWidth*sizeof(data[0]),0);
        _hasLut=cube.size!=0;
        _constants.lutMinimum={cube.minimum[0],cube.minimum[1],cube.minimum[2],float(cube.size)};
        _constants.lutMaximum={cube.maximum[0],cube.maximum[1],cube.maximum[2],0};
        _UpdateNeutral();
        return true;
    }
    void Reset(bool force=false) noexcept { if(force || !_locked)_constants.historyValid=0;_last={}; }
    bool Automatic() const noexcept { return _automatic; }
    bool LinearInput() const noexcept { return _constants.linearInput!=0; }
    void BindForExport(ID3D11DeviceContext* dc) noexcept {
        auto data=_constants;data.deltaSeconds=0;
        dc->UpdateSubresource(_buffer.Get(),0,nullptr,&data,0,0);
        ID3D11Buffer* buffer=_buffer.Get();dc->CSSetConstantBuffers(2,1,&buffer);
    }
    void Begin(ID3D11DeviceContext* dc,std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now()) noexcept {
        _constants.deltaSeconds=_last.time_since_epoch().count()==0 ? 0.f :
            std::chrono::duration<float>(now-_last).count();
        // Pausing / a discontinuity starts a fresh estimate instead of blending
        // a stale scene. Ordinary UI-only redraws are capture-clock cached.
        if(_constants.deltaSeconds>2 && !_locked) _constants.historyValid=0;
        _last=now;
        dc->UpdateSubresource(_buffer.Get(),0,nullptr,&_constants,0,0);
        ID3D11Buffer* buffer=_buffer.Get();dc->CSSetConstantBuffers(2,1,&buffer);
    }
    void End(ID3D11DeviceContext* dc,ID3D11Texture2D* adaptive,ID3D11Texture2D* history) noexcept {
        ID3D11Buffer* emptyBuffer=nullptr;
        dc->CSSetConstantBuffers(2,1,&emptyBuffer);
        if(!_automatic)return;
        ID3D11ShaderResourceView* empty[3]{};
        dc->CSSetShaderResources(0,3,empty);
        dc->CopyResource(history,adaptive);
        _constants.historyValid=1;
    }
private:
    struct Constants {
        float deltaSeconds=0;unsigned historyValid=0,linearInput=0,neutral=1;
        std::array<float,4> lutMinimum{0,0,0,0},lutMaximum{1,1,1,0};
    };
    static_assert(sizeof(Constants)==48);
    Constants _constants;
    bool _automatic=false;
    bool _locked=false;
    bool _hasLut=false,_neutralExceptLut=true;
    float _lutStrength=0;
    void _UpdateNeutral() noexcept { _constants.neutral=_neutralExceptLut && (!_hasLut || _lutStrength==0); }
    std::chrono::steady_clock::time_point _last{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> _buffer;
};
}
