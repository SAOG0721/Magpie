#pragma once
#include "ImGuiBackend.h"
#include <deque>
#include <parallel_hashmap/phmap.h>

namespace Magpie {

class DeviceResources;
struct OverlayWindowOption;

enum class ImGuiInputResult : uint8_t {
	None,
	Redraw,
	Urgent
};

class ImGuiImpl {
public:
	ImGuiImpl() = default;
	ImGuiImpl(const ImGuiImpl&) = delete;
	ImGuiImpl(ImGuiImpl&&) = delete;

	~ImGuiImpl() noexcept;

	bool Initialize(DeviceResources& deviceResource) noexcept;

	bool BuildFonts() noexcept;

	void NewFrame(
		phmap::flat_hash_map<std::string, OverlayWindowOption>& windowOptions,
		float fittsLawAdjustment,
		float dpiScale
	) noexcept;

	void Draw(POINT drawOffset, float hdrWhiteScale = 0) noexcept;

	void ClearStates() noexcept;
	void ParameterEditing(bool value) noexcept;
	void ParameterFocusSwitchingEnabled(bool value) noexcept { _parameterFocusSwitchingEnabled = value; }
	void ParameterPreview(bool value) noexcept { _parameterPreview = value; }
	bool IsParameterPreviewAt(POINT screenPoint) const noexcept;
	bool OwnsPointerAtCursor() const noexcept;
	bool OwnsPointerAt(POINT screenPoint) const noexcept;
	bool DismissParameterPopup() noexcept;
	std::optional<ImVec4> PresentedParameterRect() const noexcept { return _presentedParameterRect; }
	void OnPresentSucceeded() noexcept;

	ImGuiInputResult MessageHandler(UINT msg, WPARAM wParam, LPARAM lParam,
		std::optional<POINT> pointerPosition = std::nullopt) noexcept;
	bool HasPendingInput() const noexcept;
	bool HasUrgentInput() const noexcept;
	bool HasCriticalInput() const noexcept { return !_pendingInput.criticalEvents.empty(); }
	uint64_t LeftPressTimeUs() const noexcept { return _leftPressTimeUs; }
	bool LeftPressHasControl() const noexcept { return _leftPressHasControl; }
	bool LeftReleaseWasDrag() const noexcept { return _leftReleaseWasDrag; }
	bool FrameInputCanceled() const noexcept { return _frameContainsCancel; }
	ImVec2 FrameMousePosition() const noexcept { return _frameMousePosition; }

	std::optional<ImVec4> GetWindowRect(const char* id) const noexcept;

	const char* GetHoveredWindowId() const noexcept;

	// 将提示窗口限制在屏幕内
	void Tooltip(
		const char* content,
		float dpiScale,
		const char* description = nullptr,
		float maxWidth = -1.0f
	) noexcept;
private:
	bool _parameterFocusSwitchingEnabled = false;
	bool _parameterEditing = false;
	bool _parameterPreview = false;
	enum class PendingInputEventType : uint8_t {
		Move,
		Button,
		Wheel,
		Leave,
		Key,
		Character,
		Cancel
	};

	struct PendingInputEvent {
		uint64_t sequence = 0;
		PendingInputEventType type = PendingInputEventType::Move;
		ImVec2 position{};
		float wheelX = 0.0f;
		float wheelY = 0.0f;
		int button = -1;
		bool down = false;
		uint64_t timestampUs = 0;
		bool dragged = false;
		bool controlDown = false;
		bool shiftDown = false;
		ImGuiKey key = ImGuiKey_None;
		unsigned int character = 0;
	};

	struct PendingInputBuffer {
		static constexpr size_t MAX_EVENTS = 256;

		uint64_t Push(PendingInputEvent event, bool urgent, bool critical) noexcept;
		void Reset() noexcept;
		bool HasPending() const noexcept { return receivedSerial > presentedSerial; }
		bool HasUrgent() const noexcept { return urgentSerial > presentedSerial; }

		std::deque<PendingInputEvent> events;
		std::deque<std::pair<uint64_t, std::chrono::steady_clock::time_point>>
			criticalEvents;
		uint64_t receivedSerial = 0;
		uint64_t consumedSerial = 0;
		uint64_t presentedSerial = 0;
		uint64_t urgentSerial = 0;
		uint32_t overflowCount = 0;
	};

	ImVec2 _CaptureMousePos() const noexcept;
	void _QueueMove(ImVec2 position, bool urgent) noexcept;
	void _QueueCancel(ImVec2 position) noexcept;
	void _FlushPendingInput() noexcept;
	void _StagePresentedWindowRects() noexcept;
	const char* _GetHoveredWindowId(ImVec2 mousePos) const noexcept;
	const char* _GetPresentedHoveredWindowId(ImVec2 mousePos) const noexcept;
	void _ReleaseOwnedMouseButtons(bool releaseCapture = true) noexcept;

	ImGuiBackend _backend;

	phmap::flat_hash_map<std::string, ImVec4> _windowRects;
	std::vector<std::pair<std::string, ImVec4>> _stagedPresentedWindowRects;
	std::vector<std::pair<std::string, ImVec4>> _presentedWindowRects;
	std::optional<ImVec4> _stagedParameterRect, _presentedParameterRect;
	bool _stagedHasOpenPopup = false;
	bool _presentedHasOpenPopup = false;
	PendingInputBuffer _pendingInput;
	uint32_t _ownedMouseButtons = 0;
	ImVec2 _leftPressPosition{};
	float _resetDragTolerance = 4.0f;
	bool _rawLeftDragged = false;
	uint64_t _leftPressTimeUs = 0;
	bool _leftPressHasControl = false;
	bool _leftReleaseWasDrag = false;
	float _fittsLawAdjustment = 0.0f;
	ImVec2 _lastQueuedMousePos{ -FLT_MAX, -FLT_MAX };
	ImVec2 _frameMousePosition{ -FLT_MAX, -FLT_MAX };
	uint64_t _frameConsumedSerial = 0;
	bool _frameContainsCancel = false;
	std::chrono::steady_clock::time_point _lastSlowInputLog{};
};

}
