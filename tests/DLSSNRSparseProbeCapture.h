#pragma once
struct CombinedBgraCapture {
    DeviceResources& resources;
    uint32_t width,height;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    winrt::com_ptr<ID3D11ShaderResourceView> rgbaSrv;
    winrt::com_ptr<ID3D11VertexShader> vertex;
    winrt::com_ptr<ID3D11PixelShader> pixel;
    winrt::com_ptr<ID3D11RasterizerState> rasterizer;
    CombinedBgraCapture(DeviceResources& r,ID3D11Texture2D* rgba,uint32_t w,uint32_t h) :
        resources(r),width(w),height(h) {
        texture=DirectXHelper::CreateTexture2D(r.GetD3DDevice(),DXGI_FORMAT_B8G8R8A8_UNORM,w,h,
            D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET);
        Check(r.GetD3DDevice()->CreateRenderTargetView(texture.get(),nullptr,rtv.put()));
        Check(r.GetD3DDevice()->CreateShaderResourceView(rgba,nullptr,rgbaSrv.put()));
        constexpr std::string_view shader=R"hlsl(
Texture2D<float4> Source : register(t0);
float4 VS(uint id:SV_VertexID):SV_Position {
    float2 p=float2((id<<1)&2,id&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
float4 PS(float4 position:SV_Position):SV_Target {
    return Source.Load(int3(uint2(position.xy),0));
}
)hlsl";
        winrt::com_ptr<ID3DBlob> vs,ps,error;
        Check(D3DCompile(shader.data(),shader.size(),"CombinedBgraCapture",nullptr,nullptr,"VS","vs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,0,vs.put(),error.put()));
        error=nullptr;
        Check(D3DCompile(shader.data(),shader.size(),"CombinedBgraCapture",nullptr,nullptr,"PS","ps_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,0,ps.put(),error.put()));
        Check(r.GetD3DDevice()->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,vertex.put()));
        Check(r.GetD3DDevice()->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,pixel.put()));
        D3D11_RASTERIZER_DESC state{};state.FillMode=D3D11_FILL_SOLID;state.CullMode=D3D11_CULL_NONE;state.DepthClipEnable=TRUE;
        Check(r.GetD3DDevice()->CreateRasterizerState(&state,rasterizer.put()));
    }
    void Render() {
        auto* dc=resources.GetD3DDC();auto* target=rtv.get();auto* source=rgbaSrv.get();
        D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};
        dc->RSSetViewports(1,&viewport);dc->RSSetState(rasterizer.get());
        dc->OMSetBlendState(nullptr,nullptr,UINT_MAX);dc->OMSetDepthStencilState(nullptr,0);
        dc->OMSetRenderTargets(1,&target,nullptr);
        dc->IASetInputLayout(nullptr);dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dc->VSSetShader(vertex.get(),nullptr,0);dc->PSSetShader(pixel.get(),nullptr,0);
        dc->PSSetShaderResources(0,1,&source);dc->Draw(3,0);
        ID3D11ShaderResourceView* empty=nullptr;
        dc->PSSetShaderResources(0,1,&empty);dc->PSSetShader(nullptr,nullptr,0);dc->VSSetShader(nullptr,nullptr,0);
        dc->OMSetRenderTargets(0,nullptr,nullptr);
    }
    bool VerifyLogicalRgba(ID3D11Texture2D* rgba) {
        // Only called after the measurement trace is closed and GPU drained.
        std::array<winrt::com_ptr<ID3D11Texture2D>,2> staging;
        ID3D11Texture2D* originals[]{rgba,texture.get()};
        for(size_t i=0;i<2;++i) {
            D3D11_TEXTURE2D_DESC desc{};originals[i]->GetDesc(&desc);
            desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            Check(resources.GetD3DDevice()->CreateTexture2D(&desc,nullptr,staging[i].put()));
            resources.GetD3DDC()->CopyResource(staging[i].get(),originals[i]);
        }
        std::array<D3D11_MAPPED_SUBRESOURCE,2> mapped;
        for(size_t i=0;i<2;++i)Check(resources.GetD3DDC()->Map(staging[i].get(),0,D3D11_MAP_READ,0,&mapped[i]));
        bool equal=true;
        for(uint32_t y=0;y<height&&equal;++y) {
            const auto* a=static_cast<const uint8_t*>(mapped[0].pData)+size_t(y)*mapped[0].RowPitch;
            const auto* b=static_cast<const uint8_t*>(mapped[1].pData)+size_t(y)*mapped[1].RowPitch;
            for(uint32_t x=0;x<width;++x) if(a[x*4]!=b[x*4+2]||a[x*4+1]!=b[x*4+1]||
                a[x*4+2]!=b[x*4]||a[x*4+3]!=b[x*4+3]) { equal=false;break; }
        }
        for(size_t i=0;i<2;++i)resources.GetD3DDC()->Unmap(staging[i].get(),0);
        return equal;
    }
};
