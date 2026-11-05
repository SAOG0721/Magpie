#define OKLAB_NO_MAIN
#include "OKLabTests.cpp"
#include <dxgi.h>

int wmain(int argc,wchar_t**argv){
    Require(argc==2,"Expected production shader directory");
    std::ofstream report(std::filesystem::path(argv[1])/"benchmark.csv");
    report<<"width,height,case,format,passes,median_ms,p95_ms\n";
    const std::vector<std::pair<std::string,Values>> cases{
        {"neutral",{}},{"P0",{{"exposure",.3f},{"contrast",1.1f},{"vibrance",.2f}}},
        {"P1",{{"mix3Hue",15.f},{"wheel0Chroma",.02f},{"gamutSoft",.25f}}},
        {"gamut_pressure",{{"exposure",2.f},{"chroma",2.5f}}},
        {"clarity",{{"clarity",.4f}}},{"texture",{{"textureStrength",.4f}}},
        {"chroma_smooth",{{"chromaSmooth",.5f}}},{"deband",{{"deband",.5f}}},
        {"bloom",{{"bloom",.5f}}},{"dehaze",{{"dehaze",.5f}}},
        {"dither",{{"dither",1.f}}},{"automatic",{{"autoExposure",1.f},{"autoWhite",1.f}}},
        {"combined",{{"clarity",.4f},{"textureStrength",.3f},{"chromaSmooth",.5f},{"deband",.5f},{"bloom",.5f},{"dehaze",.3f},{"autoExposure",1.f},{"autoWhite",1.f},{"gamutSoft",.3f}}}
    };
    for(auto size:{std::pair{1920u,1080u},{2560u,1440u},{3840u,2160u}}){
        W=size.first;H=size.second;Harness h(argv[1],false,true,true);
        if(W==1920){ComPtr<IDXGIDevice> device;Check(h.device.As(&device));ComPtr<IDXGIAdapter> adapter;Check(device->GetAdapter(&adapter));DXGI_ADAPTER_DESC desc{};Check(adapter->GetDesc(&desc));std::wcout<<L"Adapter: "<<desc.Description<<std::endl;
            std::wofstream info(std::filesystem::path(argv[1])/"benchmark-adapter.txt");info<<desc.Description<<L"\nVendor="<<desc.VendorId<<L" Device="<<desc.DeviceId<<L"\n";}
        Image input(W*H);for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)input[y*W+x]={float(x%256)/255,float(y%256)/255,float((x+y)%256)/255,1};
        for(const auto& [name,values]:cases){
            for(const auto& [key,value]:values)Require(Magpie::OKLab::Find(key)!=nullptr,"Benchmark references nonexistent parameter");
            std::vector<double> timings;
            for(unsigned i=0;i<25;++i){h.Run(input,values,false,i==0);if(i>=5)timings.push_back(h.milliseconds);}
            std::sort(timings.begin(),timings.end());const double median=(timings[9]+timings[10])*.5,p95=timings[18];
            report<<W<<','<<H<<','<<name<<",R8_UNORM,"<<(h.runtime.Automatic()?3:1)<<','<<median<<','<<p95<<'\n';report.flush();
            std::cout<<W<<'x'<<H<<' '<<name<<": "<<median<<" / "<<p95<<" ms"<<std::endl;
        }
    }
}
