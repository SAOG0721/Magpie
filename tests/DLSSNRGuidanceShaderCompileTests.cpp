#define NOMINMAX
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include "DLSSNRTemporalShader.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

std::string Extract(const char* path, const char* name) {
	std::ifstream file(path,std::ios::binary);
	if (!file) throw std::runtime_error(std::string("Cannot read ")+path);
	const std::string text((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
	const auto symbol=text.find(name);
	const auto start=text.find("R\"(",symbol);
	const auto end=text.find(")\";",start);
	if (symbol==std::string::npos || start==std::string::npos || end==std::string::npos) throw std::runtime_error("Shader literal not found");
	return text.substr(start+3,end-start-3);
}

ComPtr<ID3D11ShaderReflection> Compile(const std::string& source,const char* entry,const char* label,
	const char* constants,unsigned expectedBytes) {
	ComPtr<ID3DBlob> blob,errors;
	const auto result=D3DCompile(source.data(),source.size(),label,nullptr,nullptr,entry,"cs_5_0",
		D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
	if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
	if (FAILED(result)) throw std::runtime_error(std::string("Compile failed: ")+label);
	ComPtr<ID3D11ShaderReflection> reflection;
	if (FAILED(D3DReflect(blob->GetBufferPointer(),blob->GetBufferSize(),IID_ID3D11ShaderReflection,
		reinterpret_cast<void**>(reflection.GetAddressOf())))) throw std::runtime_error("Shader reflection failed");
	D3D11_SHADER_BUFFER_DESC desc{};
	if (FAILED(reflection->GetConstantBufferByName(constants)->GetDesc(&desc)) || desc.Size!=expectedBytes)
		throw std::runtime_error(std::string("Constant-buffer ABI changed: ")+label);
	return reflection;
}

void Binding(ID3D11ShaderReflection* reflection,const char* name,unsigned slot) {
	D3D11_SHADER_INPUT_BIND_DESC desc{};
	if (FAILED(reflection->GetResourceBindingDescByName(name,&desc)) || desc.BindPoint!=slot)
		throw std::runtime_error(std::string("Unexpected binding: ")+name);
}

int main(int argc,char** argv) {
	if (argc!=3) return 2;
	try {
		Compile(Magpie::DLSSNR_TEMPORAL_SHADER,"main","DLSSNRTemporal","Settings",64);
		Compile(std::string(Magpie::DLSSNR_TEMPORAL_REDUCE_SHADER),"main","DLSSNRTemporalReduce","Settings",64);
		const auto nv=Compile(Extract(argv[1],"DENSIFY_FLOW_HLSL"),"Densify","NVOFReverse","Params",32);
		Binding(nv.Get(),"DenseReverseMotion",2); Binding(nv.Get(),"DenseReverseConfidence",3);
		const auto adapt=Compile(Extract(argv[2],"ADAPT_GUIDANCE_HLSL"),"Adapt","FrozenGuidanceAdapter","Params",32);
		Binding(adapt.Get(),"TargetReverseMotion",3); Binding(adapt.Get(),"TargetReverseConfidence",4);
		std::cout << "Four production HLSL kernels compiled/reflected on CPU; no D3D device or GPU work.\n";
		return 0;
	} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
