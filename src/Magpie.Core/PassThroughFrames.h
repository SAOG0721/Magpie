#pragma once
#include "HdrColorTransform.h"
#include "PassThroughFrameHistory.h"
#include <array>
#include <atomic>
#include <optional>
#include <d3d11_4.h>

namespace Magpie {

class DeviceResources;

// Reference images use the same slot ownership transaction as the processed
// color and motion images. Reconfiguration runs only while the frontend waits.
class PassThroughFrames {
public:
	static constexpr uint32_t MAX_SLOTS = 4;
	bool InitializeBackend(DeviceResources& resources, ID3D11Texture2D* input,
		ID3D11Texture2D* output, uint32_t slotCount, bool hdrEnabled = false,
		const HdrTransformParameters& hdrParameters = {},
		const HdrFrameMetadata& frameMetadata = {}, std::optional<bool> inputHdr = std::nullopt) noexcept;
	bool OpenFrontend(DeviceResources& resources, uint32_t slotCount) noexcept;
	// Backend initialization only, before opening the frontend. Extra textures
	// are allocated exclusively for buffered-output comparison. Failure disables
	// comparison sharing while leaving processed output unaffected.
	bool EnableCaptureHistory(uint32_t capacity = PassThroughFrameHistory::MaxCapacity) noexcept;
	void UpdateBackend(uint64_t captureFrameId, bool newCapture) noexcept;
	void UpdateBackend(const HdrFrame& frame, bool newCapture) noexcept;
	// Call before publishing an output whose target differs from physical-now.
	// The return value describes the target; generated Publish separately needs
	// previousOutputFrameId. Missing references invalidate only their own slot.
	bool SelectBackendFrame(uint64_t targetFrameId, uint64_t previousOutputFrameId) noexcept;
	void Publish(uint32_t slot, bool generatedFrame) noexcept;
	bool Consume(uint32_t slot) noexcept;
	void OnPresented() noexcept;
	bool DisableSharing() noexcept { return _sharingEnabled.exchange(false); }

	IDXGIKeyedMutex* BackendMutex(uint32_t slot) const noexcept {
		return _sharingEnabled ? _backendSlots[slot].mutex.get() : nullptr;
	}
	IDXGIKeyedMutex* FrontendMutex(uint32_t slot) const noexcept {
		return _sharingEnabled ? _frontendSlots[slot].mutex.get() : nullptr;
	}
	ID3D11Texture2D* FrontendTexture(bool presented = false) const noexcept {
		return presented ? (_presentedValid ? _presented.get() : nullptr)
			: (_baseValid ? _base.get() : nullptr);
	}
	uint64_t PresentedCaptureFrameId() const noexcept { return _presentedFrameId; }
	const HdrFrameMetadata& PresentedFrameMetadata() const noexcept {
		return _presentedMetadata;
	}

private:
	struct Slot {
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<IDXGIKeyedMutex> mutex;
	};
	struct Reference {
		winrt::com_ptr<ID3D11Texture2D> texture;
		HdrFrameMetadata metadata{};
	};
	void _ClearBackend() noexcept;
	void _UpdateBackend(uint64_t captureFrameId, bool newCapture,
		const HdrFrameMetadata* sourceMetadata) noexcept;
	HdrFrameMetadata _ReferenceMetadata(const HdrFrameMetadata& source, bool canonical) const noexcept;
	DeviceResources* _backendResources = nullptr;
	DeviceResources* _frontendResources = nullptr;
	std::array<Slot, MAX_SLOTS> _backendSlots;
	std::array<Slot, MAX_SLOTS> _frontendSlots;
	std::array<HANDLE, MAX_SLOTS> _handles{};
	std::array<uint64_t, MAX_SLOTS> _frameIds{};
	std::array<HdrFrameMetadata, MAX_SLOTS> _metadata{};
	std::array<bool, MAX_SLOTS> _valid{};
	// Reconfiguration is frontend-blocked. Failure disables future reference
	// transactions atomically, retaining resources until in-flight work is done.
	std::atomic<bool> _sharingEnabled = false;
	bool _allocationFailed = false;
	winrt::com_ptr<ID3D11Texture2D> _current;
	winrt::com_ptr<ID3D11Texture2D> _previous;
	winrt::com_ptr<ID3D11ShaderResourceView> _inputView;
	winrt::com_ptr<ID3D11UnorderedAccessView> _outputView;
	winrt::com_ptr<ID3D11Buffer> _constants;
	winrt::com_ptr<ID3D11ComputeShader> _shader;
	winrt::com_ptr<ID3D11SamplerState> _sampler;
	bool _hdrEnabled = false;
	HdrTransformParameters _hdrParameters{};
	uint32_t _width = 0;
	uint32_t _height = 0;
	uint64_t _currentFrameId = 0;
	uint64_t _previousFrameId = 0;
	HdrFrameMetadata _currentMetadata{};
	HdrFrameMetadata _previousMetadata{};
	bool _currentValid = false;
	bool _previousValid = false;
	PassThroughFrameHistory _captureHistory;
	std::array<Reference, PassThroughFrameHistory::MaxCapacity> _captureReferences;
	Reference _selectedTarget;
	Reference _selectedPrevious;
	bool _selectedTargetValid = false;
	bool _selectedPreviousValid = false;
	winrt::com_ptr<ID3D11Texture2D> _base;
	winrt::com_ptr<ID3D11Texture2D> _presented;
	bool _baseValid = false;
	bool _presentedValid = false;
	uint64_t _baseFrameId = 0;
	uint64_t _presentedFrameId = 0;
	HdrFrameMetadata _baseMetadata{};
	HdrFrameMetadata _presentedMetadata{};
};

}
