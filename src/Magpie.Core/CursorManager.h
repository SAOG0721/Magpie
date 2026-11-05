#pragma once
#include "FrameTrace.h"
#include "ScalingSessionLifetime.h"
#include <memory>
#include <optional>

namespace Magpie {

class CursorManager {
public:
	CursorManager();
	CursorManager(const CursorManager&) = delete;
	CursorManager(CursorManager&&) = delete;

	~CursorManager() noexcept;
	void BeginShutdown() noexcept { _lifetime->RequestStop(); }
	// After the window owner cut, restore clips and final capture coordinates.
	// False retains a pending restore: the caller must keep this manager alive
	// and retry through CompletePendingDestroy, independently of cursor Update.
	bool ReleasePendingCursorReposition() noexcept;

	void Update() noexcept;

	void OnScalingPosChanged() noexcept;

	void OnSrcStartMove() noexcept;

	void OnSrcEndMove() noexcept;

	void OnStartMove() noexcept;

	void OnEndResizeMove() noexcept;

	void OnSrcRectChanged() noexcept;

	// 光标不在缩放窗口上或隐藏时为 NULL
	HCURSOR CursorHandle() const noexcept {
		return _hCursor;
	}

	// 屏幕坐标
	POINT CursorPos() const noexcept {
		return _cursorPos;
	}

	bool IsCursorCaptured() const noexcept {
		return _isUnderCapture;
	}

	bool IsCursorCapturedOnForeground() const noexcept {
		return _isCapturedOnForeground;
	}

	bool IsCursorOnOverlay() const noexcept {
		return _isOnOverlay;
	}
	void IsCursorOnOverlay(bool value) noexcept;

	bool IsCursorCapturedOnOverlay() const noexcept {
		return _isCapturedOnOverlay;
	}
	void IsCursorCapturedOnOverlay(bool value) noexcept;

	int16_t SrcHitTest() const noexcept {
		return _lastCompletedHitTestResult;
	}

private:
	const std::shared_ptr<ScalingSessionLifetime> _lifetime;
	void _ShowSystemCursor(bool show, bool onDestory = false);

	void _AdjustCursorSpeed() noexcept;

	void _RestoreCursorSpeed() noexcept;

	void _ReliableSetCursorPos(POINT pos) noexcept;
	winrt::DispatcherQueueTimer _CreateCursorRepositionTimer(uint64_t id);
	bool _ReliableSetCursorPosOnDestroy(POINT pos) noexcept;
	void _CompleteCursorReposition(uint64_t id) noexcept;
	bool _FinishCursorReposition(FrameTrace::CursorRepositionReason reason) noexcept;
	bool _FinishCursorRepositionForShutdown() noexcept;
	bool _DeferClipCursor(const RECT* clipRect) noexcept;
	winrt::fire_and_forget _RefreshCursorShapeAsync(uint64_t id, POINT pos) noexcept;

	winrt::fire_and_forget _SrcHitTestAsync(POINT screenPos) noexcept;

	void _ClearHitTestResult() noexcept;

	void _UpdateCursorState() noexcept;
	bool _UpdateParameterCursor() noexcept;

	void _ClipCursorForMonitors(POINT cursorPos) noexcept;

	void _ClipCursorOnSrcMoving() noexcept;

	void _UpdateCursorPos() noexcept;

	void _StartCapture(POINT& cursorPos) noexcept;

	bool _StopCapture(POINT& cursorPos, bool onDestroy = false) noexcept;

	void _SetClipCursor(const RECT& clipRect, bool is3DGameMode = false) noexcept;

	void _RestoreClipCursor() noexcept;

	HCURSOR _hCursor = NULL;
	POINT _cursorPos{ std::numeric_limits<LONG>::max() };

	// 用于确保拖拽源窗口和缩放窗口时光标位置稳定，使用相对于渲染矩形的局部坐标
	POINT _localCursorPosOnMoving{ std::numeric_limits<LONG>::max() };

	// 用于防止光标移动到边框的过程中闪烁
	std::chrono::steady_clock::time_point _sizeCursorStartTime{};

	RECT _lastClip{ std::numeric_limits<LONG>::max() };
	RECT _lastRealClip{ std::numeric_limits<LONG>::max() };

	// Only the frontend accesses this state or calls ClipCursor. While the point
	// clip drains queued hardware input, normal confinement changes update the
	// restoration target rather than shortening the hold or saving the point clip.
	struct CursorReposition {
		uint64_t id;
		POINT pos;
		RECT pointClip, restoreClip;
		bool restoreManaged, restoreDefault = false;
		std::chrono::steady_clock::time_point deadline;
		int64_t traceStart;
		uint64_t traceFrame;
		winrt::DispatcherQueueTimer timer{ nullptr };
	};
	std::optional<CursorReposition> _cursorReposition;
	std::optional<POINT> _nextCursorReposition;
	uint64_t _cursorRepositionId = 0;
	std::chrono::steady_clock::time_point _nextCursorCleanupAttempt{};
	bool _cursorCleanupFailureReported = false;
	bool _cursorShutdownInputReleased = false;
	struct CursorShapeRequest { uint64_t id; POINT pos; };
	std::optional<CursorShapeRequest> _nextCursorShape;
	bool _isCursorShapeInFlight = false;

	int _originCursorSpeed = 0;

	uint32_t _nextHitTestId = 0;
	uint32_t _lastCompletedHitTestId = 0;
	POINT _lastCompletedHitTestPos{ std::numeric_limits<LONG>::max() };
	int16_t _lastCompletedHitTestResult = HTNOWHERE;

	bool _isUnderCapture = false;
	// 当缩放后的光标位置在交换链窗口上且没有被其他窗口挡住时应绘制光标
	bool _shouldDrawCursor = false;
	
	bool _isCapturedOnForeground = false;

	bool _isOnOverlay = false;
	bool _isCapturedOnOverlay = false;

	bool _isSystemCursorShown = true;

	static inline const HCURSOR _hDiagonalSize1Cursor = LoadCursor(NULL, IDC_SIZENWSE);
	static inline const HCURSOR _hDiagonalSize2Cursor = LoadCursor(NULL, IDC_SIZENESW);
	static inline const HCURSOR _hHorizontalSizeCursor = LoadCursor(NULL, IDC_SIZEWE);
	static inline const HCURSOR _hVerticalSizeCursor = LoadCursor(NULL, IDC_SIZENS);
};

}
