// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <fstream>
#include <stdexcept>
// Explicit diagnostic opt-in only. Normal rendering never reads textures back.
namespace Magpie {
inline bool ValidateNativeOutput(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* input,
								 ID3D11Texture2D* output, const char* label) noexcept {
	try {
		wchar_t enabled[8]{};
		if (!GetEnvironmentVariableW(L"MAGPIE_VALIDATE_NATIVE_OUTPUT", enabled, 8) || enabled[0] != L'1')
			return true;
		auto read = [&](ID3D11Texture2D* texture, const char* suffix) {
			D3D11_TEXTURE2D_DESC d{};
			texture->GetDesc(&d);
			if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
				throw std::runtime_error("validation expects SDR RGBA8/BGRA8");
			const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
			d.Usage = D3D11_USAGE_STAGING;
			d.BindFlags = 0;
			d.MiscFlags = 0;
			d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			winrt::com_ptr<ID3D11Texture2D> staging;
			winrt::check_hresult(device->CreateTexture2D(&d, nullptr, staging.put()));
			context->CopyResource(staging.get(), texture);
			std::vector<uint8_t> rgb(size_t(d.Width) * d.Height * 3);
			D3D11_MAPPED_SUBRESOURCE m{};
			winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m));
			for (UINT y = 0; y < d.Height; ++y)
				for (UINT x = 0; x < d.Width; ++x) {
					const auto* pixel = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch + x * 4;
					const auto offset = (size_t(y) * d.Width + x) * 3;
					rgb[offset] = pixel[bgra ? 2 : 0];
					rgb[offset + 1] = pixel[1];
					rgb[offset + 2] = pixel[bgra ? 0 : 2];
				}
			context->Unmap(staging.get(), 0);
			std::ofstream file(fmt::format("validation-{}-{}x{}-{}.ppm", label, d.Width, d.Height, suffix),
							   std::ios::binary);
			file << "P6\n" << d.Width << " " << d.Height << "\n255\n";
			file.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
			if (!file)
				throw std::runtime_error("native validation snapshot write failed");
			return rgb;
		};
		auto before = read(input, "input"), after = read(output, "output");
		double sum = 0, difference = 0;
		uint8_t low = 255, high = 0;
		for (size_t i = 0; i < after.size(); ++i) {
			sum += after[i];
			low = std::min(low, after[i]);
			high = std::max(high, after[i]);
			if (before.size() == after.size())
				difference += std::abs(int(before[i]) - int(after[i]));
		}
		Logger::Get().Info(fmt::format("Native output validation: backend={} diagnostic_cpu_readback=true "
									   "RGBmean={:.4f} min={} max={} sameSize={} meanAbsoluteDifference={:.4f}/255",
									   label, sum / after.size(), low, high, before.size() == after.size(),
									   before.size() == after.size() ? difference / after.size() : -1));
		return true;
	} catch (const std::exception& e) {
		Logger::Get().Error(fmt::format("Native output validation failed: backend={} {}", label, e.what()));
		return false;
	}
}
} // namespace Magpie
