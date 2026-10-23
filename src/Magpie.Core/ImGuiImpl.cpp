#include "pch.h"
#include "ImGuiImpl.h"
#include "CursorManager.h"
#include "DeviceResources.h"
#include "ImGuiBackend.h"
#include "Logger.h"
#include "Renderer.h"
#include "ScalingWindow.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <ranges>

namespace Magpie {

static bool operator==(const ImVec2& l, const ImVec2& r) noexcept {
	return l.x == r.x && l.y == r.y;
}

static bool operator==(const ImVec4& l, const ImVec4& r) noexcept {
	return l.x == r.x && l.y == r.y && l.z == r.z && l.w == r.w;
}

static const char* GetWindowIDFromName(const char* name) noexcept {
	size_t idPos = std::string_view(name).find("##");
	if (idPos == std::string_view::npos) {
		return name;
	} else {
		return name + idPos + (name[idPos + 2] == '#' ? 3 : 2);
	}
}

uint64_t ImGuiImpl::PendingInputBuffer::Push(
	PendingInputEvent event,
	bool urgent,
	bool critical
) noexcept {
	event.sequence = ++receivedSerial;

	// Only adjacent continuous state may be collapsed. Never merge across a
	// button/cancel boundary because every edge must retain its own position.
	if (!events.empty() && event.type == PendingInputEventType::Move &&
		events.back().type == PendingInputEventType::Move) {
		events.back() = event;
	} else if (!events.empty() && event.type == PendingInputEventType::Wheel &&
		events.back().type == PendingInputEventType::Wheel) {
		events.back().position = event.position;
		events.back().wheelX += event.wheelX;
		events.back().wheelY += event.wheelY;
		events.back().sequence = event.sequence;
	} else {
		if (events.size() >= MAX_EVENTS) {
			auto coalescible = std::find_if(events.begin(), events.end(),
				[](const PendingInputEvent& item) noexcept {
					return item.type == PendingInputEventType::Move ||
						item.type == PendingInputEventType::Wheel;
				});
			if (coalescible != events.end()) {
				events.erase(coalescible);
			} else {
				events.clear();
				event.type = PendingInputEventType::Cancel;
				event.button = -1;
				event.down = false;
				urgent = true;
				critical = true;
				++overflowCount;
			}
		}
		events.push_back(event);
	}

	if (urgent) {
		urgentSerial = event.sequence;
	}
	if (critical) {
		criticalEvents.emplace_back(event.sequence, std::chrono::steady_clock::now());
		while (criticalEvents.size() > 64) {
			criticalEvents.pop_front();
		}
	}
	return event.sequence;
}

void ImGuiImpl::PendingInputBuffer::Reset() noexcept {
	events.clear();
	criticalEvents.clear();
	receivedSerial = 0;
	consumedSerial = 0;
	presentedSerial = 0;
	urgentSerial = 0;
	overflowCount = 0;
}

ImGuiImpl::~ImGuiImpl() noexcept {
	if (ImGui::GetCurrentContext()) {
		ImGui::DestroyContext();
	}
}

bool ImGuiImpl::Initialize(DeviceResources& deviceResources) noexcept {
#ifdef _DEBUG
	// 检查 ImGUI 版本是否匹配
	if (!IMGUI_CHECKVERSION()) {
		Logger::Get().Error("ImGui 的头文件与链接库版本不同");
		return false;
	}
#endif

	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.BackendPlatformName = "Magpie";
	io.ConfigFlags |= ImGuiConfigFlags_NavNoCaptureKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
	// The application buffer deliberately feeds at most one control edge to
	// each UI frame, so ImGui must not retain a second hidden trickle queue.
	io.ConfigInputTrickleEventQueue = false;
	// 禁用 ini 配置文件
	io.IniFilename = nullptr;
#ifndef _DEBUG
	// Release 配置下禁用重复 ID 检查
	io.ConfigDebugHighlightIdConflicts = false;
#endif

	if (!_backend.Initialize(deviceResources)) {
		Logger::Get().Error("初始化 ImGuiBackend 失败");
		return false;
	}

	return true;
}

bool ImGuiImpl::BuildFonts() noexcept {
	return _backend.BuildFonts();
}

void ImGuiImpl::ParameterEditing(bool value) noexcept {
	if (std::exchange(_parameterEditing, value) == value || !value) return;
	_parameterPreview = false;
	// NewFrame hit testing uses the previous frame's window flags. Restore
	// input before feeding the activating click, rather than one frame later
	// in Begin(), or that first control press is silently lost.
	for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
		if (std::string_view(GetWindowIDFromName(window->RootWindow->Name)) != "effectParameters") continue;
		window->Flags &= ~ImGuiWindowFlags_NoInputs;
		if (window == window->RootWindow)
			window->Flags &= ~(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
	}
	// ClearStates removes the normal input snapshot. Preview has its own
	// last-presented bounds, which authorize this first press immediately.
	if (_presentedParameterRect)
		_presentedWindowRects.emplace_back("effectParameters", *_presentedParameterRect);
}

void ImGuiImpl::NewFrame(
	phmap::flat_hash_map<std::string, OverlayWindowOption>& windowOptions,
	float fittsLawAdjustment,
	float dpiScale
) noexcept {
	ImGuiIO& io = ImGui::GetIO();
	_fittsLawAdjustment = fittsLawAdjustment;
	_resetDragTolerance = 4.0f * dpiScale;
	if (_parameterFocusSwitchingEnabled && !_parameterEditing) {
		for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
			if (std::string_view(GetWindowIDFromName(window->RootWindow->Name)) == "effectParameters")
				window->Flags |= ImGuiWindowFlags_NoInputs;
		}
	}

	{
		const SIZE destSize = Win32Helper::GetSizeOfRect(ScalingWindow::Get().Renderer().DestRect());
		ImVec2 newDisplaySize((float)destSize.cx, (float)destSize.cy);
		if (io.DisplaySize != newDisplaySize) {
			io.DisplaySize = newDisplaySize;
			// 调整缩放窗口尺寸时强制调整叠加层窗口位置
			_windowRects.clear();
		}
	}

	const ImVec2 mousePos = _CaptureMousePos();
	if (mousePos != _lastQueuedMousePos) {
		_QueueMove(mousePos, _ownedMouseButtons != 0);
	}
	_FlushPendingInput();

	// 不接受键盘输入
	if (!_parameterEditing && io.WantCaptureKeyboard) {
		io.AddKeyEvent(ImGuiKey_Enter, true);
		io.AddKeyEvent(ImGuiKey_Enter, false);
	}

	ImGui::NewFrame();
	if (_frameContainsCancel) {
		// Cancel is a lifecycle transition, not a synthetic click release.
		ImGui::ClearActiveID();
		ImGui::ClosePopupsExceptModals();
	}
	
	for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
		if (window->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoMove)) {
			continue;
		}

		// 排除 Debug##Default 窗口和尚未初始化完成的窗口
		if (window->IsFallbackWindow || window->Appearing) {
			continue;
		}

		ImVec2 pos = window->Pos;

		// 将窗口限制在视口内
		if (io.DisplaySize.x > window->Size.x) {
			pos.x = std::clamp(pos.x, 0.0f, io.DisplaySize.x - window->Size.x);
		} else {
			pos.x = 0;
		}

		if (io.DisplaySize.y > window->Size.y) {
			pos.y = std::clamp(pos.y, 0.0f, io.DisplaySize.y - window->Size.y);
		} else {
			pos.y = 0;
		}

		const char* windowId = GetWindowIDFromName(window->Name);
		// These resizable panels restore both position and size before Begin and
		// record only user edits there, so viewport clamping cannot erase their
		// preferred size or edge anchoring.
		if (std::string_view(windowId) == "effectParameters" || std::string_view(windowId) == "profiler") {
			continue;
		}
		if (auto it = windowOptions.find(windowId); it != windowOptions.end()) {
			OverlayWindowOption& option = it->second;

			auto it1 = _windowRects.find(windowId);
			if (it1 == _windowRects.end()) {
				// 第一次显示或调整缩放窗口大小时叠加层窗口应根据规则调整位置

				if (option.hArea == 0) {
					pos.x = option.hPos * dpiScale;
				} else if (option.hArea == 1) {
					pos.x = io.DisplaySize.x * option.hPos - window->Size.x / 2;
				} else if (option.hArea == 2) {
					pos.x = io.DisplaySize.x - option.hPos * dpiScale - window->Size.x;
				} else {
					assert(false);
				}

				if (option.vArea == 0) {
					pos.y = option.vPos * dpiScale;
				} else if (option.vArea == 1) {
					pos.y = io.DisplaySize.y * option.vPos - window->Size.y / 2;
				} else if (option.vArea == 2) {
					pos.y = io.DisplaySize.y - option.vPos * dpiScale - window->Size.y;
				} else {
					assert(false);
				}

				// 再次将窗口限制在视口内
				if (io.DisplaySize.x > window->Size.x) {
					pos.x = std::clamp(pos.x, 0.0f, io.DisplaySize.x - window->Size.x);
				} else {
					pos.x = 0;
				}

				if (io.DisplaySize.y > window->Size.y) {
					pos.y = std::clamp(pos.y, 0.0f, io.DisplaySize.y - window->Size.y);
				} else {
					pos.y = 0;
				}
			} else if (it1->second != ImVec4(pos.x, pos.y, window->Size.x, window->Size.y)) {
				// 当且仅当用户移动窗口或调整窗口大小后后重新计算贴靠的边，调整缩放窗口大小时应保持
				// 贴靠的边不变。我们根据两侧边距的比例决定贴靠哪边或者都不贴靠。

				// 这些阈值决定是否贴靠在某一边上，它们不是定值，而是窗口尺寸和画面尺寸的比例。这个
				// 算法的效果出乎意料的好，因为窗口两侧边距较大时人对比例更敏感，较小时则对差值更敏
				// 感。
				const float thresholdX = std::max(window->Size.x / io.DisplaySize.x, 0.2f);
				const float thresholdY = std::max(window->Size.y / io.DisplaySize.y, 0.2f);

				// 根据左右边距比例决定贴靠
				const float leftMargin = std::max(pos.x, 0.0f);
				const float rightMargin = std::max(
					io.DisplaySize.x - pos.x - window->Size.x, 0.0f);
				if (leftMargin < thresholdX * rightMargin) {
					option.hArea = 0;
					option.hPos = pos.x / dpiScale;
				} else if (leftMargin * thresholdX <= rightMargin) {
					option.hArea = 1;
					option.hPos = (pos.x + window->Size.x / 2) / io.DisplaySize.x;
				} else {
					option.hArea = 2;
					option.hPos = (io.DisplaySize.x - pos.x - window->Size.x) / dpiScale;
				}
				
				// 根据上下边距比例决定贴靠
				const float topMargin = std::max(pos.y, 0.0f);
				const float bottomMargin = std::max(
					io.DisplaySize.y - pos.y - window->Size.y, 0.0f);
				if (topMargin < thresholdY * bottomMargin) {
					option.vArea = 0;
					option.vPos = pos.y / dpiScale;
				} else if (topMargin * thresholdY <= bottomMargin) {
					option.vArea = 1;
					option.vPos = (pos.y + window->Size.y / 2) / io.DisplaySize.y;
				} else {
					option.vArea = 2;
					option.vPos = (io.DisplaySize.y - pos.y - window->Size.y) / dpiScale;
				}
			}

			ImGui::SetWindowPos(window, pos);

			// 此时 window->Pos 已更新，记录新的窗口位置
			_windowRects[windowId] = ImVec4(window->Pos.x, window->Pos.y, window->Size.x, window->Size.y);
		} else {
			ImGui::SetWindowPos(window, pos);
		}
	}

	// 调整缩放窗口大小或鼠标被前台窗口捕获时避免鼠标跳跃
	CursorManager& cursorManager = ScalingWindow::Get().CursorManager();
	if (!ScalingWindow::Get().IsResizingOrMoving() && !cursorManager.IsCursorCapturedOnForeground()) {
		cursorManager.IsCursorOnOverlay(io.WantCaptureMouse ||
			IsParameterPreviewAt(cursorManager.CursorPos()));
	}
}

void ImGuiImpl::Draw(POINT drawOffset, float hdrWhiteScale) noexcept {
	ImGui::Render();
	_StagePresentedWindowRects();

	ImDrawData* drawData = ImGui::GetDrawData();
	if (!drawData || !drawData->Valid) {
		return;
	}

	const RECT& rendererRect = ScalingWindow::Get().RendererRect();
	const RECT& destRect = ScalingWindow::Get().Renderer().DestRect();
	const POINT viewportOffset = {
		destRect.left - rendererRect.left + drawOffset.x,
		destRect.top - rendererRect.top + drawOffset.y
	};
	_backend.RenderDrawData(*drawData, viewportOffset, hdrWhiteScale);
}

void ImGuiImpl::Tooltip(
	const char* content,
	float dpiScale,
	const char* description,
	float maxWidth
) noexcept {
	const SIZE outputSize = Win32Helper::GetSizeOfRect(ScalingWindow::Get().Renderer().DestRect());
	const float availableWidth = std::max(1.0f, float(outputSize.cx));
	maxWidth = maxWidth > 0 ? std::min(maxWidth, availableWidth) : std::min(360.0f * dpiScale, availableWidth);

	static constexpr float DESCRIPTION_SCALE = 0.9f;

	ImVec2 padding = ImGui::GetStyle().WindowPadding;
	ImVec2 contentSize = ImGui::CalcTextSize(content, nullptr, false, maxWidth - 2 * padding.x);
	ImVec2 descriptionSize{};
	if (description) {
		float oldFontScale = ImGui::GetIO().FontGlobalScale;
		ImGui::GetIO().FontGlobalScale *= DESCRIPTION_SCALE;
		ImGui::PushFont(ImGui::GetFont());
		descriptionSize = ImGui::CalcTextSize(description, nullptr, false, maxWidth - 2 * padding.x);
		ImGui::GetIO().FontGlobalScale = oldFontScale;
		ImGui::PopFont();
	}
	// 稍微增加高度，否则下边框比上边框稍窄
	ImVec2 windowSize(
		std::max(contentSize.x, descriptionSize.x) + 2 * padding.x,
		contentSize.y + descriptionSize.y + 2 * padding.y + 1.5f * dpiScale
	);
	ImGui::SetNextWindowSize(windowSize);

	ImVec2 windowPos = ImGui::GetMousePos();
	windowPos.x += 16.0f * dpiScale * ImGui::GetStyle().MouseCursorScale;
	windowPos.y += 8.0f * dpiScale * ImGui::GetStyle().MouseCursorScale;

	windowPos.x = std::clamp(windowPos.x, 0.0f, std::max(0.0f, outputSize.cx - windowSize.x));
	if (_fittsLawAdjustment < 0.0f) {
		// A bottom-docked toolbar opens its hints toward the viewport interior.
		windowPos.y = ImGui::GetMousePos().y - windowSize.y - 8.0f * dpiScale;
	}
	windowPos.y = std::clamp(windowPos.y, 0.0f, std::max(0.0f, outputSize.cy - windowSize.y));

	ImGui::SetNextWindowPos(windowPos);

	ImGui::SetNextWindowBgAlpha(ImGui::GetStyle().Colors[ImGuiCol_PopupBg].w);
	ImGui::Begin("tooltip", NULL, 
		ImGuiWindowFlags_NoInputs |
		ImGuiWindowFlags_NoDecoration |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoFocusOnAppearing);

	ImGui::PushTextWrapPos(maxWidth - padding.x);
	ImGui::TextUnformatted(content);
	if (description) {
		ImGui::PushStyleColor(ImGuiCol_Text, { 1.0f,1.0f,1.0f,0.8f });
		float oldFontScale = ImGui::GetIO().FontGlobalScale;
		ImGui::GetIO().FontGlobalScale *= DESCRIPTION_SCALE;
		ImGui::PushFont(ImGui::GetFont());
		ImGui::TextUnformatted(description);
		ImGui::GetIO().FontGlobalScale = oldFontScale;
		ImGui::PopFont();
		ImGui::PopStyleColor();
	}
	ImGui::PopTextWrapPos();

	ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
	ImGui::End();
}

ImVec2 ImGuiImpl::_CaptureMousePos() const noexcept {
	// Resizing the scaling HWND and forwarding input to the source are explicit
	// ownership boundaries. Queue an unavailable position instead of mutating
	// ImGui state from WndProc.
	const CursorManager& cursorManager = ScalingWindow::Get().CursorManager();
	if (ScalingWindow::Get().IsResizingOrMoving() ||
		cursorManager.IsCursorCapturedOnForeground()) {
		return ImVec2(-FLT_MAX, -FLT_MAX);
	}

	const POINT cursorPos = cursorManager.CursorPos();
	const RECT& destRect = ScalingWindow::Get().Renderer().DestRect();
	float mouseX = float(cursorPos.x - destRect.left);
	float mouseY = float(cursorPos.y - destRect.top);
	return ImVec2(mouseX, mouseY);
}

void ImGuiImpl::_QueueMove(ImVec2 position, bool urgent) noexcept {
	if ((_ownedMouseButtons & 1u) &&
		(std::abs(position.x - _leftPressPosition.x) > _resetDragTolerance ||
		 std::abs(position.y - _leftPressPosition.y) > _resetDragTolerance)) {
		_rawLeftDragged = true;
	}
	_lastQueuedMousePos = position;
	_pendingInput.Push(PendingInputEvent{
		.type = PendingInputEventType::Move,
		.position = position
	}, urgent, false);
}

void ImGuiImpl::_QueueCancel(ImVec2 position) noexcept {
	_pendingInput.Push(PendingInputEvent{
		.type = PendingInputEventType::Cancel,
		.position = position,
		.button = -1,
		.down = false
	}, true, true);
	_ReleaseOwnedMouseButtons();
}

void ImGuiImpl::_FlushPendingInput() noexcept {
	ImGuiIO& io = ImGui::GetIO();
	// Keep the event's physical position for grip offsets and drop targets.
	// Only button hit testing receives the inward Fitts-law adjustment.
	auto feedMousePosition = [&](ImVec2 position) {
		_frameMousePosition = position;
		if (_fittsLawAdjustment > 0.0f && position.y >= 0.0f && position.y < _fittsLawAdjustment)
			position.y = _fittsLawAdjustment;
		if (_fittsLawAdjustment < 0.0f && position.y >= io.DisplaySize.y + _fittsLawAdjustment &&
			position.y < io.DisplaySize.y)
			position.y = io.DisplaySize.y + _fittsLawAdjustment - 1.0f;
		io.AddMousePosEvent(position.x, position.y);
	};
	_frameContainsCancel = false;
	_leftReleaseWasDrag = false;
	uint64_t consumedSerial = _pendingInput.consumedSerial;
	bool deliveredEdge = false;

	while (!_pendingInput.events.empty() && !deliveredEdge) {
		PendingInputEvent event = _pendingInput.events.front();
		_pendingInput.events.pop_front();
		consumedSerial = event.sequence;

		switch (event.type) {
		case PendingInputEventType::Move:
			feedMousePosition(event.position);
			break;
		case PendingInputEventType::Button:
			feedMousePosition(event.position);
			io.AddMouseButtonEvent(event.button, event.down);
			if (event.button == ImGuiMouseButton_Left) {
				if (event.down) {
					_leftPressTimeUs = event.timestampUs;
					_leftPressHasControl = event.controlDown;
				} else {
					_leftReleaseWasDrag = event.dragged;
				}
			}
			deliveredEdge = true;
			break;
		case PendingInputEventType::Wheel:
			feedMousePosition(event.position);
			io.AddMouseWheelEvent(event.wheelX, event.wheelY);
			deliveredEdge = true;
			break;
		case PendingInputEventType::Leave:
			feedMousePosition({ -FLT_MAX, -FLT_MAX });
			deliveredEdge = true;
			break;
		case PendingInputEventType::Cancel:
			io.ClearInputKeys();
			feedMousePosition({ -FLT_MAX, -FLT_MAX });
			for (int button = 0; button < ImGuiMouseButton_COUNT; ++button) {
				io.AddMouseButtonEvent(button, false);
			}
			_frameContainsCancel = true;
			deliveredEdge = true;
			break;
		case PendingInputEventType::Key:
			io.AddKeyEvent(event.key, event.down);
			deliveredEdge = true;
			break;
		case PendingInputEventType::Character:
			io.AddInputCharacterUTF16(static_cast<ImWchar16>(event.character));
			deliveredEdge = true;
			break;
		}
	}

	_pendingInput.consumedSerial = consumedSerial;
	_frameConsumedSerial = consumedSerial;
}

void ImGuiImpl::_StagePresentedWindowRects() noexcept {
	if (!_parameterFocusSwitchingEnabled) {
		_stagedPresentedWindowRects.clear();
		_stagedHasOpenPopup = !ImGui::GetCurrentContext()->OpenPopupStack.empty();
		for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows | std::views::reverse) {
			if (!window->WasActive || window->Hidden ||
				(window->Flags & ImGuiWindowFlags_NoMouseInputs)) {
				continue;
			}
			_stagedPresentedWindowRects.emplace_back(
				GetWindowIDFromName(window->Name),
				ImVec4(window->Pos.x, window->Pos.y,
					window->Pos.x + window->Size.x, window->Pos.y + window->Size.y));
			if (window->Flags & ImGuiWindowFlags_Popup) {
				break;
			}
		}
		return;
	}

	_stagedPresentedWindowRects.clear();
	_stagedParameterRect.reset();
	_stagedHasOpenPopup = !ImGui::GetCurrentContext()->OpenPopupStack.empty();
	// Preview remains read-only in ImGui. A native hit target covers only the
	// last successfully presented panel, including its title and child region.
	for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
		if (window->Active && !window->Hidden &&
			std::string_view(GetWindowIDFromName(window->Name)) == "effectParameters") {
			_stagedParameterRect = ImVec4(window->Pos.x, window->Pos.y,
				window->Pos.x + window->Size.x, window->Pos.y + window->Size.y);
			break;
		}
	}
	for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows | std::views::reverse) {
		if (!window->Active || window->Hidden ||
			(!_parameterEditing && std::string_view(GetWindowIDFromName(window->RootWindow->Name)) == "effectParameters") ||
			(window->Flags & ImGuiWindowFlags_NoMouseInputs)) {
			continue;
		}
		_stagedPresentedWindowRects.emplace_back(
			GetWindowIDFromName(window->Name),
			ImVec4(window->OuterRectClipped.Min.x, window->OuterRectClipped.Min.y,
				window->OuterRectClipped.Max.x, window->OuterRectClipped.Max.y));
		// The input host needs both popup and parent panel regions. Outside those
		// regions a game-area click is consumed as a complete return gesture.
		if ((window->Flags & ImGuiWindowFlags_Popup) && !_parameterEditing) {
			break;
		}
	}
}

void ImGuiImpl::ClearStates() noexcept {
	_ReleaseOwnedMouseButtons();
	_pendingInput.Reset();
	_lastQueuedMousePos = ImVec2(-FLT_MAX, -FLT_MAX);
	_frameMousePosition = _lastQueuedMousePos;
	_frameConsumedSerial = 0;
	_frameContainsCancel = false;
	_stagedPresentedWindowRects.clear();
	_presentedWindowRects.clear();
	_stagedHasOpenPopup = false;
	_presentedHasOpenPopup = false;

	if (ImGui::GetCurrentContext()) {
		ImGuiIO& io = ImGui::GetIO();
		io.ClearEventsQueue();
		io.ClearInputMouse();
		io.ClearInputKeys();
		ImGui::ClearActiveID();
		ImGui::ClosePopupsExceptModals();
	}

	if (CursorManager* cursorManager =
		ScalingWindow::Get().TryGetCursorManager()) {
		cursorManager->IsCursorOnOverlay(false);
	}
}

void ImGuiImpl::OnPresentSucceeded() noexcept {
	_pendingInput.presentedSerial = std::max(
		_pendingInput.presentedSerial, _frameConsumedSerial);
	_presentedWindowRects = _stagedPresentedWindowRects;
	_presentedParameterRect = _stagedParameterRect;
	_presentedHasOpenPopup = _stagedHasOpenPopup;

	const auto now = std::chrono::steady_clock::now();
	std::chrono::steady_clock::duration maxLatency{};
	while (!_pendingInput.criticalEvents.empty() &&
		_pendingInput.criticalEvents.front().first <= _pendingInput.presentedSerial) {
		maxLatency = std::max(maxLatency,
			now - _pendingInput.criticalEvents.front().second);
		_pendingInput.criticalEvents.pop_front();
	}
	if (maxLatency >= std::chrono::milliseconds(50) &&
		now - _lastSlowInputLog >= std::chrono::seconds(1)) {
		_lastSlowInputLog = now;
		Logger::Get().Warn(fmt::format(
			"Overlay input-to-present latency {:.1f} ms (received={}, consumed={}, presented={})",
			std::chrono::duration<double, std::milli>(maxLatency).count(),
			_pendingInput.receivedSerial,
			_pendingInput.consumedSerial,
			_pendingInput.presentedSerial));
	}
}

bool ImGuiImpl::HasPendingInput() const noexcept {
	return _pendingInput.HasPending();
}

bool ImGuiImpl::HasUrgentInput() const noexcept {
	return _pendingInput.HasUrgent();
}

void ImGuiImpl::_ReleaseOwnedMouseButtons(bool releaseCapture) noexcept {
	_ownedMouseButtons = 0;
	// ClearStates can be reached while a partially initialized ScalingWindow is
	// being torn down. The CursorManager is optional at that point.
	if (CursorManager* cursorManager =
		ScalingWindow::Get().TryGetCursorManager()) {
		cursorManager->IsCursorCapturedOnOverlay(false);
	}
	if (releaseCapture && (GetCapture() == ScalingWindow::Get().Handle() ||
		ScalingWindow::Get().IsParameterInputWindow(GetCapture()))) {
		ReleaseCapture();
	}
}

static int GetMouseButtonFromMessage(UINT msg, WPARAM wParam) noexcept {
	switch (msg) {
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_NCLBUTTONDOWN:
	case WM_NCLBUTTONUP:
		return 0;
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_NCRBUTTONDOWN:
	case WM_NCRBUTTONUP:
		return 1;
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_NCMBUTTONDOWN:
	case WM_NCMBUTTONUP:
		return 2;
	case WM_XBUTTONDOWN:
	case WM_XBUTTONUP:
	case WM_NCXBUTTONDOWN:
	case WM_NCXBUTTONUP:
		return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4;
	default:
		return -1;
	}
}

static bool IsMouseButtonDownMessage(UINT msg) noexcept {
	return msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
		msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN ||
		msg == WM_NCLBUTTONDOWN || msg == WM_NCRBUTTONDOWN ||
		msg == WM_NCMBUTTONDOWN || msg == WM_NCXBUTTONDOWN;
}

static bool IsNonClientMouseButtonDownMessage(UINT msg) noexcept {
	return msg == WM_NCLBUTTONDOWN || msg == WM_NCRBUTTONDOWN ||
		msg == WM_NCMBUTTONDOWN || msg == WM_NCXBUTTONDOWN;
}

static ImGuiKey ParameterKey(WPARAM key) noexcept {
	if (key >= '0' && key <= '9') return ImGuiKey(ImGuiKey_0 + key - '0');
	if (key >= 'A' && key <= 'Z') return ImGuiKey(ImGuiKey_A + key - 'A');
	if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) return ImGuiKey(ImGuiKey_Keypad0 + key - VK_NUMPAD0);
	switch (key) {
	case VK_TAB: return ImGuiKey_Tab;
	case VK_LEFT: return ImGuiKey_LeftArrow;
	case VK_RIGHT: return ImGuiKey_RightArrow;
	case VK_UP: return ImGuiKey_UpArrow;
	case VK_DOWN: return ImGuiKey_DownArrow;
	case VK_HOME: return ImGuiKey_Home;
	case VK_END: return ImGuiKey_End;
	case VK_PRIOR: return ImGuiKey_PageUp;
	case VK_NEXT: return ImGuiKey_PageDown;
	case VK_DELETE: return ImGuiKey_Delete;
	case VK_BACK: return ImGuiKey_Backspace;
	case VK_SPACE: return ImGuiKey_Space;
	case VK_RETURN: return ImGuiKey_Enter;
	case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return ImGuiMod_Ctrl;
	case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return ImGuiMod_Shift;
	case VK_MENU: case VK_LMENU: case VK_RMENU: return ImGuiMod_Alt;
	case VK_LWIN: case VK_RWIN: return ImGuiMod_Super;
	default: return ImGuiKey_None;
	}
}

bool ImGuiImpl::OwnsPointerAtCursor() const noexcept {
	POINT point{};
	GetCursorPos(&point);
	return OwnsPointerAt(point);
}

bool ImGuiImpl::OwnsPointerAt(POINT point) const noexcept {
	const RECT& dest = ScalingWindow::Get().Renderer().DestRect();
	return _ownedMouseButtons ||
		_GetPresentedHoveredWindowId({ float(point.x - dest.left), float(point.y - dest.top) });
}

bool ImGuiImpl::IsParameterPreviewAt(POINT point) const noexcept {
	if (!_parameterPreview || !_presentedParameterRect) return false;
	const RECT& dest = ScalingWindow::Get().Renderer().DestRect();
	const auto& rect = *_presentedParameterRect;
	return point.x >= dest.left + rect.x && point.y >= dest.top + rect.y &&
		point.x < dest.left + rect.z && point.y < dest.top + rect.w;
}

bool ImGuiImpl::DismissParameterPopup() noexcept {
	ImGuiContext& context = *ImGui::GetCurrentContext();
	if (!context.OpenPopupStack.empty()) {
		ImGui::ClosePopupToLevel(context.OpenPopupStack.Size - 1, true);
		return true;
	}
	if (context.InputTextState.ID && context.ActiveId == context.InputTextState.ID) {
		ImGui::ClearActiveID();
		return true;
	}
	return false;
}

ImGuiInputResult ImGuiImpl::MessageHandler(
	UINT msg,
	WPARAM wParam,
	LPARAM lParam,
	std::optional<POINT> pointerPosition
) noexcept {
	if (_parameterEditing && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN ||
		msg == WM_KEYUP || msg == WM_SYSKEYUP)) {
		const ImGuiKey key = ParameterKey(wParam);
		if (key != ImGuiKey_None) _pendingInput.Push(PendingInputEvent{
			.type = PendingInputEventType::Key,
			.down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN,
			.key = key
		}, true, true);
		return ImGuiInputResult::Urgent;
	}
	if (_parameterEditing && msg == WM_CHAR) {
		_pendingInput.Push(PendingInputEvent{
			.type = PendingInputEventType::Character, .character = static_cast<unsigned int>(wParam)
		}, true, true);
		return ImGuiInputResult::Urgent;
	}
	const int mouseButton = GetMouseButtonFromMessage(msg, wParam);
	if (mouseButton >= 0) {
		ScalingWindow::Get().CursorManager().Update();
		const RECT& dest = ScalingWindow::Get().Renderer().DestRect();
		const ImVec2 mousePos = pointerPosition
			? ImVec2(float(pointerPosition->x - dest.left), float(pointerPosition->y - dest.top))
			: _CaptureMousePos();
		_QueueMove(mousePos, _ownedMouseButtons != 0);
		const bool isDown = IsMouseButtonDownMessage(msg);
		const uint32_t buttonMask = 1u << mouseButton;
		if (isDown) {
			if (IsNonClientMouseButtonDownMessage(msg)) {
				return ImGuiInputResult::Redraw;
			}
			// A visible popup also owns the click outside its rectangle: ImGui
			// needs that edge to dismiss it. Keep hit testing based on the last
			// successful Present, and let the normal queued press/release path
			// close the popup without selecting a value or clicking through it.
			if (!(_ownedMouseButtons || _presentedHasOpenPopup ||
				_GetPresentedHoveredWindowId(mousePos))) {
				return ImGuiInputResult::Redraw;
			}
			if (_ownedMouseButtons & buttonMask) {
				return ImGuiInputResult::Redraw;
			}
			if (!_ownedMouseButtons) {
				ScalingWindow::Get().CursorManager().IsCursorCapturedOnOverlay(true);
				SetCapture(_parameterEditing ? ScalingWindow::Get().Renderer().ParameterInputHandle() : ScalingWindow::Get().Handle());
			}
			_ownedMouseButtons |= buttonMask;
			if (mouseButton == ImGuiMouseButton_Left) {
				_leftPressPosition = mousePos;
				_rawLeftDragged = false;
			}
		} else {
			if (!(_ownedMouseButtons & buttonMask)) {
				return ImGuiInputResult::Redraw;
			}
			_ownedMouseButtons &= ~buttonMask;
			if (!_ownedMouseButtons) {
				_ReleaseOwnedMouseButtons();
			}
		}
		_pendingInput.Push(PendingInputEvent{
			.type = PendingInputEventType::Button,
			.position = mousePos,
			.button = mouseButton,
			.down = isDown,
			.timestampUs = uint64_t(static_cast<uint32_t>(GetMessageTime())) * 1000,
			.dragged = mouseButton == ImGuiMouseButton_Left && _rawLeftDragged,
			.controlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0
		}, true, true);
		return ImGuiInputResult::Urgent;
	}

	switch (msg) {
	case WM_MOUSEMOVE:
	case WM_NCMOUSEMOVE:
		ScalingWindow::Get().CursorManager().Update();
		_QueueMove(_CaptureMousePos(), _ownedMouseButtons != 0);
		return _ownedMouseButtons ? ImGuiInputResult::Urgent : ImGuiInputResult::Redraw;
	case WM_MOUSELEAVE:
	case WM_NCMOUSELEAVE:
		if (!_ownedMouseButtons) {
			_lastQueuedMousePos = ImVec2(-FLT_MAX, -FLT_MAX);
			_pendingInput.Push(PendingInputEvent{
				.type = PendingInputEventType::Leave,
				.position = _lastQueuedMousePos
			}, false, false);
		}
		return _ownedMouseButtons ? ImGuiInputResult::Urgent : ImGuiInputResult::Redraw;
	case WM_MOUSEWHEEL:
	{
		ScalingWindow::Get().CursorManager().Update();
		const ImVec2 mousePos = _CaptureMousePos();
		_QueueMove(mousePos, _ownedMouseButtons != 0);
		if (_ownedMouseButtons || _GetPresentedHoveredWindowId(mousePos)) {
			_pendingInput.Push(PendingInputEvent{
				.type = PendingInputEventType::Wheel,
				.position = mousePos,
				.wheelY = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA
			}, true, true);
			return ImGuiInputResult::Urgent;
		}
		return ImGuiInputResult::Redraw;
	}
	case WM_MOUSEHWHEEL:
	{
		ScalingWindow::Get().CursorManager().Update();
		const ImVec2 mousePos = _CaptureMousePos();
		_QueueMove(mousePos, _ownedMouseButtons != 0);
		if (_ownedMouseButtons || _GetPresentedHoveredWindowId(mousePos)) {
			_pendingInput.Push(PendingInputEvent{
				.type = PendingInputEventType::Wheel,
				.position = mousePos,
				.wheelX = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA
			}, true, true);
			return ImGuiInputResult::Urgent;
		}
		return ImGuiInputResult::Redraw;
	}
	case WM_CAPTURECHANGED:
		if ((HWND)lParam != ScalingWindow::Get().Handle() &&
			!ScalingWindow::Get().IsParameterInputWindow((HWND)lParam) && _ownedMouseButtons) {
			_QueueCancel(_CaptureMousePos());
			return ImGuiInputResult::Urgent;
		}
		break;
	case WM_CANCELMODE:
	case WM_KILLFOCUS:
		_QueueCancel(_CaptureMousePos());
		return ImGuiInputResult::Urgent;
	case WM_ACTIVATEAPP:
		if (!wParam) {
			_QueueCancel(_CaptureMousePos());
			return ImGuiInputResult::Urgent;
		}
		break;
	}

	return ImGuiInputResult::None;
}

std::optional<ImVec4> ImGuiImpl::GetWindowRect(const char* id) const noexcept {
	const std::string suffix = StrHelper::Concat("##", id);
	for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
		if (std::string_view(window->Name).ends_with(suffix)) {
			return ImVec4(
				window->Pos.x,
				window->Pos.y,
				window->Pos.x + window->Size.x,
				window->Pos.y + window->Size.y
			);
		}
	}

	return std::nullopt;
}

const char* ImGuiImpl::GetHoveredWindowId() const noexcept {
	return _GetHoveredWindowId(ImGui::GetIO().MousePos);
}

const char* ImGuiImpl::_GetHoveredWindowId(ImVec2 mousePos) const noexcept {
	// 自顶向下遍历
	for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows | std::views::reverse) {
		// 排除不接受鼠标输入的窗口，来自
		// https://github.com/ocornut/imgui/blob/77f1d3b317c400c34ee02fe9a5354d0d757b55ca/imgui.cpp#L5855
		if (!window->WasActive || window->Hidden) {
			continue;
		}
		if (window->Flags & ImGuiWindowFlags_NoMouseInputs) {
			continue;
		}
		if (_parameterFocusSwitchingEnabled && !_parameterEditing && std::string_view(GetWindowIDFromName(window->RootWindow->Name)) == "effectParameters") {
			continue;
		}

		if (window->Rect().Contains(mousePos)) {
			return GetWindowIDFromName(window->Name);
		}

		// 弹窗会阻止和其他窗口交互
		if (window->Flags & ImGuiWindowFlags_Popup) {
			return nullptr;
		}
	}

	return nullptr;
}

const char* ImGuiImpl::_GetPresentedHoveredWindowId(ImVec2 mousePos) const noexcept {
	for (const auto& [windowId, rect] : _presentedWindowRects) {
		if (mousePos.x >= rect.x && mousePos.y >= rect.y &&
			mousePos.x < rect.z && mousePos.y < rect.w) {
			return windowId.c_str();
		}
	}
	return nullptr;
}

}
