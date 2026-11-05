#pragma once
#include "EffectDesc.h"
#include "HdrEffectBoundary.h"
#include "HdrSurfaceAdapter.h"
#include "HdrComponentRuntime.h"
#include <utility>
#include "SmallVector.h"
#include "OKLabRuntime.h"
// Conan 的 muparser 不含 UNICODE 支持
#pragma push_macro("_UNICODE")
#undef _UNICODE
#pragma warning(push)
#pragma warning(disable: 4310)	// 类型强制转换截断常量值
#include <muParser.h>
#pragma warning(push)
#pragma pop_macro("_UNICODE")

namespace Magpie {

struct EffectOption;
class DeviceResources;
class BackendDescriptorStore;
class EffectsProfiler;

class EffectDrawer {
public:
	EffectDrawer() = default;
	EffectDrawer(const EffectDrawer&) = delete;
	EffectDrawer(EffectDrawer&&) = default;

	~EffectDrawer();

	bool Initialize(
		const EffectDesc& desc,
		const EffectOption& option,
		DeviceResources& deviceResources,
		BackendDescriptorStore& descriptorStore,
		ID3D11Texture2D** inOutTexture
	) noexcept;

	bool Draw(EffectsProfiler& profiler) const noexcept;
	bool ResetCaptureHistory(const EffectDesc& desc) const noexcept;
	void SetHdrComponent(const HdrComponentStage& stage, HdrTransformParameters parameters) noexcept {
		_component = stage;
		_componentTransform = parameters;
	}
	bool DrawHdrComponent(EffectsProfiler& profiler) const noexcept;

	void SetHdrBoundary(HdrEffectBoundaryContext context) noexcept { _hdrBoundary = std::move(context); }
	const HdrEffectBoundaryContext& GetHdrBoundary() const noexcept { return _hdrBoundary; }
	// The renderer may replace the canonical handoff texture after capture,
	// resize, or an upstream effect rebuild. Keep the boundary source bound to
	// the current production texture instead of a stale initialization pointer.
	void SetHdrInputSource(ID3D11Texture2D* texture) noexcept { _hdrInputSource = texture; }
	bool PrepareHdrInput() const noexcept;
	bool CompleteHdrOutput() const noexcept;
	ID3D11Texture2D* GetExternalOutputTexture() const noexcept {
		return _hdrEnabled && _hdrOutput ? _hdrOutput.get() : _textures[1].get();
	}

	void DrawForExport(const EffectDesc& desc, uint32_t passIdx) const noexcept;

	bool ResizeTextures(
		const EffectDesc& desc,
		const EffectOption& option,
		DeviceResources& deviceResources,
		ID3D11Texture2D** inOutTexture
	) noexcept;

	bool UpdateParameters(
		const EffectDesc& desc,
		const EffectOption& option,
		DeviceResources& deviceResources
	) noexcept;

	ID3D11Texture2D* GetOutputTexture() const noexcept {
		return _textures[1].get();
	}

	ID3D11Texture2D* GetTexture(uint32_t idx) const noexcept {
		return _textures[idx].get();
	}
	bool IsOKLabLinearInput() const noexcept { return _oklab && _oklab->LinearInput(); }

	// Source-equivalent matrix tests use the exact production drawer and expose
	// its actual canonical handoff rather than reimplementing effect formulas.
	ID3D11Texture2D* GetCanonicalOutputTextureForTesting() const noexcept {
		return GetExternalOutputTexture();
	}

private:
	SIZE _CalcOutputSize(
		const EffectDesc& desc,
		const EffectOption& option,
		SIZE inputSize
	) const noexcept;

	bool _UpdatePassResources(const EffectDesc& desc) noexcept;

	bool _UpdateConstants(
		const EffectDesc& desc,
		const EffectOption& option,
		DeviceResources& deviceResources,
		SIZE inputSize,
		SIZE outputSize
	) noexcept;

	void _PrepareForDraw() const noexcept;

	void _DrawPass(uint32_t i) const noexcept;
	bool _DrawHdrComponent() const noexcept;
	bool _UsesDirectHdrPath() const noexcept;
	DXGI_FORMAT _GetHdrInputFormat(const EffectDesc& desc) const noexcept;
	DXGI_FORMAT _GetHdrOutputFormat(const EffectDesc& desc) const noexcept;
	HdrTransformParameters _GetHdrTransformParameters() const noexcept;

	SmallVector<uint32_t> _CalcPassesToDrawForExport(
		const EffectDesc& desc,
		uint32_t passIdx
	) const noexcept;

	ID3D11DeviceContext* _d3dDC = nullptr;
	BackendDescriptorStore* _descriptorStore = nullptr;

	SmallVector<ID3D11SamplerState*> _samplers;
	SmallVector<winrt::com_ptr<ID3D11Texture2D>> _textures;
	std::vector<SmallVector<ID3D11ShaderResourceView*>> _srvs;
	// 后半部分为空，用于解绑
	std::vector<SmallVector<ID3D11UnorderedAccessView*>> _uavs;

	winrt::com_ptr<ID3D11Buffer> _constantBuffer;

	SmallVector<winrt::com_ptr<ID3D11ComputeShader>> _shaders;

	SmallVector<std::pair<uint32_t, uint32_t>> _dispatches;
	HdrEffectBoundaryContext _hdrBoundary{};
	HdrSurfaceAdapter _hdrSurfaceAdapter;
	winrt::com_ptr<ID3D11Texture2D> _hdrOutput;
	ID3D11Texture2D* _hdrInputSource = nullptr;
	bool _hdrEnabled = false;
	HdrComponentStage _component;
	HdrTransformParameters _componentTransform;
	std::unique_ptr<OKLabRuntime> _oklab;

	static inline mu::Parser _exprParser;
};

}
