#pragma once
#include "ZeroFrameGuidanceProvider.h"
#include "FrameGuidanceAvailability.h"

namespace Magpie {

class DeviceResources;

struct FrameGuidanceConsumerViews {
	FrameGuidanceView produced{};
	FrameGuidanceView zero{};
	bool adapted = false;
	bool usedFallback = false;
	FrameGuidanceAvailability motionAvailability = FrameGuidanceAvailability::Unavailable;
};

class FrameGuidanceService {
public:
	FrameGuidanceService() noexcept;
	~FrameGuidanceService();
	FrameGuidanceService(const FrameGuidanceService&) = delete;
	FrameGuidanceService& operator=(const FrameGuidanceService&) = delete;
	bool SetMotionVectorProvider(
		MotionVectorRequest request,
		std::unique_ptr<IMotionVectorProvider> provider
	) noexcept;

	bool Initialize(
		DeviceResources& resources,
		ID3D11Texture2D* sourceFrame,
		const FrameGuidanceRequirements& requirements
	) noexcept;
	const FrameGuidanceView& BeginFrame(
		FrameGuidanceFrameId frameId,
		ID3D11Texture2D* sourceFrame,
		const FrameGuidanceRequirements& requirements,
		uint64_t captureSequence = 0,
		uint64_t resourceGeneration = 0,
		int64_t timestamp100ns = 0,
		const ColorDescription& colorDescription = {}
	) noexcept;
	bool Resize(
		FrameGuidanceExtent sourceExtent,
		const FrameGuidanceRequirements& requirements
	) noexcept;
	// Prepares a provider on the D3D11 owner.
	// A failed candidate leaves existing consumers and their histories intact.
	bool PrepareMotionRequest(MotionVectorRequest request) noexcept;
	double LastGpuMilliseconds() const noexcept;
	uint64_t LastGpuSample() const noexcept;
	void ResetHistory(FrameGuidanceResetReason reason) noexcept;
	FrameGuidanceConsumerViews GetConsumerViews(
		FrameGuidanceFrameId frameId,
		FrameGuidanceExtent targetExtent,
		MotionVectorRequest request
	) noexcept;
	// Adapt a retained content target, without consulting the current provider
	// or capture. Returned adapter textures remain Renderer-owner resources.
	FrameGuidanceConsumerViews AdaptConsumerViews(const FrameGuidanceView& produced,
		const FrameGuidanceView& zero, FrameGuidanceExtent targetExtent) noexcept;

	const FrameGuidanceView& View() const noexcept { return _view; }
	const FrameGuidanceView& ZeroView() const noexcept { return _zeroView; }
	FrameGuidanceExtent SourceExtent() const noexcept { return _sourceExtent; }
	bool IsInitialized() const noexcept { return _resources != nullptr; }
	OpticalFlowMethod InitializationFailedMethod() const noexcept {
		return _initializationFailedMethod;
	}
	OpticalFlowInitializationError InitializationError() const noexcept {
		return _initializationError;
	}

private:
	struct AdapterCache;
	struct GpuTiming;

	const FrameGuidanceView& _Produce(
		const FrameGuidanceFrame& frame,
		const FrameGuidanceRequirements& requirements
	) noexcept;
	void _RollbackInitialization() noexcept;

	DeviceResources* _resources = nullptr;
	ZeroFrameGuidanceResources _zeroResources;
	ZeroDepthProvider _zeroDepthProvider;
	ZeroMotionVectorProvider _zeroMotionProvider;
	struct ProviderEntry {
		MotionVectorRequest request{};
		std::unique_ptr<IMotionVectorProvider> provider;
		FrameGuidanceView view{};
		bool ready = false;
		bool fallbackActive = false;
		uint32_t consecutiveFailures = 0;
	};
	std::vector<ProviderEntry> _providers;
	MotionVectorRequest _selectedMotionRequest{};
	MotionVectorRequest _preparedMotionRequest{};
	FrameGuidanceExtent _sourceExtent{};
	FrameGuidanceView _view{};
	FrameGuidanceView _zeroView{};
	FrameGuidanceRequirements _cachedRequirements{};
	FrameGuidanceRequirements _lastLoggedRequirements{};
	FrameGuidanceFrameId _cachedFrameId = 0;
	bool _hasCachedFrame = false;
	bool _hasLoggedRequirements = false;
	OpticalFlowMethod _initializationFailedMethod = OpticalFlowMethod::None;
	OpticalFlowInitializationError _initializationError =
		OpticalFlowInitializationError::None;
	std::unique_ptr<AdapterCache> _adapterCache;
	std::unique_ptr<GpuTiming> _gpuTiming;
};

}
