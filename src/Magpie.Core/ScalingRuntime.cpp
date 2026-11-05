#include "pch.h"
#include "FrameTrace.h"
#include "ScalingRuntime.h"
#include "CommonSharedConstants.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "Renderer.h"
#include "EffectParameterValue.h"
#include "DLSSNRPerformanceParameters.h"
#include "Win32Helper.h"
#include <dispatcherqueue.h>

using namespace std::chrono;

namespace Magpie {

ScalingRuntime::ScalingRuntime() : _scalingThread(&ScalingRuntime::_ScalingThreadProc, this) {
}

ScalingRuntime::~ScalingRuntime() {
	if (_scalingThread.joinable()) {
		const HANDLE hScalingThread = _scalingThread.native_handle();

		if (!wil::handle_wait(hScalingThread, 0)) {
			const DWORD threadId = GetThreadId(hScalingThread);
			// 持续尝试直到 _scalingThread 创建了消息队列
			while (!PostThreadMessage(threadId, WM_QUIT, 0, 0)) {
				if (wil::handle_wait(hScalingThread, 1)) {
					break;
				}
			}

			// 等待缩放线程退出，在此期间必须处理消息队列，否则缩放线程调用
			// SetWindowLongPtr 会导致死锁
			while (true) {
				MSG msg;
				while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
					TranslateMessage(&msg);
					FrameTrace::Scope traceMessage(FrameTrace::Event::FrontendMessage, msg.message);
			DispatchMessage(&msg);
				}

				if (MsgWaitForMultipleObjectsEx(1, &hScalingThread,
					INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0) {
					// WAIT_OBJECT_0 表示缩放线程已退出
					// WAIT_OBJECT_0 + 1 表示有新消息
					break;
				}
			}
		}

		_scalingThread.join();
	}
}

bool ScalingRuntime::Start(HWND hwndSrc, ScalingOptions&& options, bool force) {
	assert(!options.screenshotsDir.empty() && options.showToast && options.showError && options.save);
	if (_quitting.load(std::memory_order_acquire)) return false;
	const winrt::DispatcherQueue& dispatcher = _Dispatcher();
	const uint64_t generation =
		_commandGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
	ScalingWindow::SessionWindowedMode(options.IsWindowedMode());
	_State(ScalingState::Starting);
	if (!dispatcher.TryEnqueue([
		this, hwndSrc, options(std::move(options)), force, generation
	]() mutable {
		if (_quitting.load(std::memory_order_acquire) ||
			_commandGeneration.load(std::memory_order_acquire) != generation) return;
		ScalingWindow& scalingWindow = ScalingWindow::Get();
		// 如果正在缩放且 force 为假则忽略
		if (scalingWindow && !force) {
			_State(ScalingState::Scaling);
			return;
		}

		// A forced start replaces the source/session; treat it as an external
		// ownership change, not a deferred request to return to the old game.
		if (force && scalingWindow.TryGetRenderer()) scalingWindow.Renderer().SuspendParameterInput();
		scalingWindow.Stop();
		if (_quitting.load(std::memory_order_acquire) ||
			_commandGeneration.load(std::memory_order_acquire) != generation) {
			return;
		}
		// Queue feedback before potentially slow shader/device initialization.
		// The existing toast host is non-activating and later success/error
		// messages replace this short notice without changing cancellation.
		try {
			const auto loader = winrt::ResourceLoader::GetForViewIndependentUse(
				CommonSharedConstants::APP_RESOURCE_MAP_ID);
			const auto message = loader.GetString(L"Message_ScalingPreparing");
			if (!message.empty()) options.showToast(hwndSrc, message);
		} catch (...) {
			Logger::Get().Warn("Could not show scaling preparation notice");
		}
		scalingWindow.Start(hwndSrc, std::move(options));
		if (_quitting.load(std::memory_order_acquire) ||
			_commandGeneration.load(std::memory_order_acquire) != generation) {
			// A stop requested during synchronous initialization cancels this run.
			scalingWindow.Stop();
			return;
		}
		_State(scalingWindow ? ScalingState::Scaling : ScalingState::Idle);
	})) {
		if (_commandGeneration.load(std::memory_order_acquire) == generation) {
			_State(ScalingState::Idle);
		}
		return false;
	}

	return true;
}

void ScalingRuntime::ToggleScaling(bool isWindowedMode) {
	if (_quitting.load(std::memory_order_acquire)) return;
	const uint64_t generation = _commandGeneration.load(std::memory_order_acquire);
	_Dispatcher().TryEnqueue([this, isWindowedMode, generation]() {
		if (_quitting.load(std::memory_order_acquire) ||
			_commandGeneration.load(std::memory_order_acquire) != generation ||
			State() != ScalingState::Scaling) return;
		if (ScalingWindow& scalingWindow = ScalingWindow::Get()) {
			scalingWindow.ToggleScaling(isWindowedMode);
			_State(scalingWindow ? ScalingState::Scaling : ScalingState::Idle);
		};
	});
}

void ScalingRuntime::InvokeOverlayAction(OverlayAction action) {
    if (State() != ScalingState::Scaling) return;
    const uint32_t runId = RunId();
    const uint64_t generation = _commandGeneration.load(std::memory_order_acquire);
    _Dispatcher().TryEnqueue([this, action, runId, generation]() {
        if (State() != ScalingState::Scaling || RunId() != runId ||
            _commandGeneration.load(std::memory_order_acquire) != generation) return;
        if (ScalingWindow& window = ScalingWindow::Get()) {
            window.Renderer().InvokeOverlayAction(action);
        }
    });
}

void ScalingRuntime::SwitchToolbarState() {
	_Dispatcher().TryEnqueue([]() {
		if (ScalingWindow& scalingWindow = ScalingWindow::Get()) {
			scalingWindow.SwitchToolbarState();
		};
	});
}

void ScalingRuntime::Stop() {
	if (State() == ScalingState::Idle) return;
	const uint64_t generation =
		_commandGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
	_State(ScalingState::Stopping);
	if (!_Dispatcher().TryEnqueue([this, generation]() {
		if (_commandGeneration.load(std::memory_order_acquire) != generation) return;
		ScalingWindow::Get().Stop();
		if (_commandGeneration.load(std::memory_order_acquire) == generation) {
			_State(ScalingWindow::Get() ? ScalingState::Stopping : ScalingState::Idle);
		}
	})) {
		if (_commandGeneration.load(std::memory_order_acquire) == generation) {
			_State(ScalingState::Idle);
		}
	}
}

bool ScalingRuntime::StopForTaskSwitch() {
	const ScalingState state = State();
	if (state == ScalingState::Idle || state == ScalingState::Stopping ||
		ScalingWindow::SessionWindowedMode()) return false;
	Stop();
	return true;
}

uint32_t ScalingRuntime::RunId() const noexcept {
	return ScalingWindow::RunId();
}

void ScalingRuntime::UpdateToolbarShortcutLabels(ToolbarShortcutLabels labels) {
	const uint64_t generation = _commandGeneration.load(std::memory_order_acquire);
	_Dispatcher().TryEnqueue([this, generation, labels = std::move(labels)]() mutable {
		if (_commandGeneration.load(std::memory_order_acquire) == generation)
			ScalingWindow::Get().UpdateToolbarShortcutLabels(std::move(labels));
	});
}

void ScalingRuntime::UpdateScreenshotContent(ScreenshotContent content) {
	if (State() == ScalingState::Idle || State() == ScalingState::Stopping) return;
	const uint64_t generation = _commandGeneration.load(std::memory_order_acquire);
	_Dispatcher().TryEnqueue([this, generation, content] {
		if (_commandGeneration.load(std::memory_order_acquire) != generation) return;
		auto& window = ScalingWindow::Get();
		// Updating Starting/Waiting sessions also covers a pending restart.
		window.UpdateScreenshotContent(content);
	});
}

void ScalingRuntime::UpdateMainUI(SessionWindowIdentity identity) {
	if (State() == ScalingState::Idle || State() == ScalingState::Stopping) return;
	const uint64_t generation = _commandGeneration.load(std::memory_order_acquire);
	_Dispatcher().TryEnqueue([this, generation, identity] {
		if (_commandGeneration.load(std::memory_order_acquire) == generation)
			ScalingWindow::Get().UpdateMainUI(identity);
	});
}

void ScalingRuntime::UpdateFrameRefreshSettings(FrameRefreshSettings settings, std::optional<bool> simpleMode, bool policyChanged) {
	if (State() == ScalingState::Idle || State() == ScalingState::Stopping) return;
	const uint64_t generation = _commandGeneration.load(std::memory_order_acquire);
	_Dispatcher().TryEnqueue([this, generation, settings, simpleMode, policyChanged]() {
		if (_commandGeneration.load(std::memory_order_acquire) != generation) return;
		auto& window = ScalingWindow::Get();
		if (auto session = window.Options().parameterSession) {
			session->DesiredFrameRefresh(settings, simpleMode, policyChanged);
			if (window) window.RenderOverlay();
		}
	});
}

void ScalingRuntime::UpdateEffectParameterFromSettings(uint32_t modeIdx, std::wstring modeName,
	uint32_t effectIdx, EffectOption effect, std::string parameter, float value) {
	const uint64_t generation = _commandGeneration.load(std::memory_order_acquire);
	_Dispatcher().TryEnqueue([this, generation, modeIdx, modeName = std::move(modeName),
		effectIdx, effect = std::move(effect), parameter = std::move(parameter), value]() {
		if (_commandGeneration.load(std::memory_order_acquire) != generation) return;
		auto& window = ScalingWindow::Get();
		const bool waiting = window.IsWaitingForParameterRestart();
		if (State() != ScalingState::Scaling && !(State() == ScalingState::Starting && waiting)) return;
		const auto& options = window.Options();
		if ((!window && !waiting) || options.scalingModeIdx != modeIdx || options.scalingModeName != modeName ||
			effectIdx >= options.effects.size()) return;
		const auto& active = options.effects[effectIdx];
		const bool vsrCategoryEdit = effect.name == "RTXVideo\\RTXVideo_VSR" && parameter == "category";
		if (active.name != effect.name || (!vsrCategoryEdit && (active.scale != effect.scale ||
			active.scalingType != effect.scalingType))) return;
		if (effect.name == "DLSSNR\\DLSSNR_AI_Filter") {
			// Preserve the complete UI draft, but never partly apply a combination
			// awaiting its optical-flow / processing-mode restart transaction.
			EffectParameterSessionState::Snapshot snapshot;
			options.parameterSession->ReadIfChanged(snapshot);
			if (effectIdx < snapshot.desired.size()) {
				snapshot.desired[effectIdx] = effect;
				options.parameterSession->Desired(snapshot.desired);
			}
			bool ready = IsDLSSNRPerformanceOptionValid(effect);
			const auto applied = options.parameterSession->Applied();
			if (effectIdx >= applied.size()) return;
			ready &= SameDLSSNRPerformanceControls(applied[effectIdx], effect);
			if (!ready) { window.RenderOverlay(); return; }
		}
		if (waiting) {
			window.UpdateWaitingEffectParameter(effectIdx, parameter, value);
			return;
		}
		auto& renderer = window.Renderer();
		const auto& descriptions = renderer.ActiveEffectDescs();
		if (effectIdx >= descriptions.size()) return;
		const auto& parameters = descriptions[effectIdx]->params;
		const auto it = std::ranges::find(parameters, parameter, &EffectParameterDesc::name);
		if (it == parameters.end()) return;
		const float normalized = NormalizeEffectParameterValue(*it, value);
		if (vsrCategoryEdit) {
			EffectParameterSessionState::Snapshot snapshot;
			options.parameterSession->ReadIfChanged(snapshot);
			if (effectIdx < snapshot.desired.size()) {
				snapshot.desired[effectIdx] = effect;
				options.parameterSession->Desired(snapshot.desired);
			}
			window.RenderOverlay(); return;
		}
		options.parameterSession->Desired(effectIdx, parameter, normalized);
		if (effect.name == "RTXVideo\\RTXVideo_VSR" && parameter == "strength") {
			const auto appliedOptions = options.parameterSession->Applied();
			const auto category = [](const EffectOption& option) {
				const auto it = option.parameters.find("category"); return it == option.parameters.end() ? 0.f : it->second;
			};
			if (effectIdx >= appliedOptions.size() || category(appliedOptions[effectIdx]) != category(effect)) return;
		}
		const auto index = static_cast<uint32_t>(it - parameters.begin());
		const auto& infos = renderer.EffectParameterRuntimeInfos();
		if (effectIdx < infos.size() && index < infos[effectIdx].size() &&
			infos[effectIdx][index].applyMode == EffectParameterApplyMode::Live) {
			if (!renderer.QueueEffectParameterUpdate(effectIdx, index, normalized, false) && options.reportErrorDetails) {
				options.reportErrorDetails(window.SrcTracker().Handle(),
					ScalingError::EffectParameterLiveFailed, active.name + " / " + parameter, 0);
			}
		}
		window.RenderOverlay();
	});
}

bool ScalingRuntime::RestartWithEffectParameters(
	HWND hwndSource,
	HWND hwndScaling,
	uint32_t scalingRunId,
	std::vector<EffectOption>&& effects,
	FrameRefreshSettings frameRefresh
) {
	if (_quitting.load(std::memory_order_acquire)) return false;
	const uint64_t generation = _commandGeneration.load(std::memory_order_acquire);
	return _Dispatcher().TryEnqueue([
		this, hwndSource, hwndScaling, scalingRunId, generation,
		effects = std::move(effects), frameRefresh
	]() mutable {
		if (_quitting.load(std::memory_order_acquire) ||
			_commandGeneration.load(std::memory_order_acquire) != generation ||
			State() != ScalingState::Scaling) return;
		ScalingWindow& window = ScalingWindow::Get();
		if (!window || scalingRunId != ScalingWindow::RunId() ||
			window.Handle() != hwndScaling ||
			window.SrcTracker().Handle() != hwndSource) {
			return;
		}
		window.RestartWithEffectParameters(std::move(effects), frameRefresh);
	});
}

static std::optional<bool> IsSrcRepositioning(HWND hwndSrc) noexcept {
	if (!IsWindow(hwndSrc)) {
		Logger::Get().Info("源窗口已销毁");
		return std::nullopt;
	}

	// 窗口不可见或最小化则继续等待。注意 showCmd 不能准确判断窗口可见性，
	// 应使用 IsWindowVisible。
	if (!IsWindowVisible(hwndSrc)) {
		return true;
	}

	if (Win32Helper::IsWindowHung(hwndSrc)) {
		Logger::Get().Info("源窗口已挂起");
		return std::nullopt;
	}

	const UINT showCmd = Win32Helper::GetWindowShowCmd(hwndSrc);
	if (showCmd == SW_SHOWMAXIMIZED) {
		// 窗口最大化则尝试缩放，失败会显示错误消息
		return false;
	} else if (showCmd == SW_SHOWMINIMIZED) {
		return true;
	}

	// 检查源窗口是否正在调整大小或移动
	GUITHREADINFO guiThreadInfo{ .cbSize = sizeof(GUITHREADINFO) };
	if (!GetGUIThreadInfo(GetWindowThreadProcessId(hwndSrc, nullptr), &guiThreadInfo)) {
		Logger::Get().Win32Error("GetGUIThreadInfo 失败");
		return std::nullopt;
	}

	return bool(guiThreadInfo.flags & GUI_INMOVESIZE);
}

void ScalingRuntime::_ScalingThreadProc() noexcept {
#ifdef _DEBUG
	SetThreadDescription(GetCurrentThread(), L"Magpie-缩放线程");
#endif

	winrt::init_apartment(winrt::apartment_type::single_threaded);

	{
		winrt::DispatcherQueueController dqc{ nullptr };
		HRESULT hr = CreateDispatcherQueueController(
			DispatcherQueueOptions{
				.dwSize = sizeof(DispatcherQueueOptions),
				.threadType = DQTYPE_THREAD_CURRENT
			},
			(PDISPATCHERQUEUECONTROLLER*)winrt::put_abi(dqc)
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("CreateDispatcherQueueController 失败", hr);
			return;
		}

		_dispatcher = dqc.DispatcherQueue();
		// 如果主线程正在等待则唤醒主线程
		_dispatcherInitialized.store(true, std::memory_order_release);
		_dispatcherInitialized.notify_one();
	}

	ScalingWindow& scalingWindow = ScalingWindow::Get();
	ScalingWindow::Dispatcher(_dispatcher);

	time_point<steady_clock> lastRenderTime;
	std::optional<uint64_t> parameterRestartGeneration;

	MSG msg;
	// Input/control messages are removed before frame notifications. WndProc only
	// records work; all rendering happens below, outside the window-procedure
	// call stack, so a FIFO/presenter wait cannot trap later button-up messages.
	constexpr uint32_t MAX_MESSAGES_PER_PASS = 32;
	bool quitRequested = false;
	auto requestQuit = [&]() noexcept {
		if (!quitRequested) {
			quitRequested = true;
			_quitting.store(true, std::memory_order_release);
			_commandGeneration.fetch_add(1, std::memory_order_acq_rel);
			_State(ScalingState::Stopping);
		}
		scalingWindow.Stop();
		scalingWindow.Destroy();
	};
	auto peekPriorityMessage = [&]() noexcept {
		// Preserve modifier/button/character order while the parameter host owns input.
		if (scalingWindow.TryGetRenderer() && scalingWindow.Renderer().IsEditingParameters())
			return PeekMessage(&msg, NULL, 0, 0, PM_REMOVE) != FALSE;
		if (PeekMessage(&msg, NULL, WM_QUIT, WM_QUIT, PM_REMOVE) ||
			PeekMessage(&msg, NULL, WM_CANCELMODE, WM_CANCELMODE, PM_REMOVE) ||
			PeekMessage(&msg, NULL, WM_CAPTURECHANGED, WM_CAPTURECHANGED, PM_REMOVE) ||
			PeekMessage(&msg, NULL, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE) ||
			PeekMessage(&msg, NULL, WM_NCMOUSEMOVE, WM_NCXBUTTONDBLCLK, PM_REMOVE)) {
			return true;
		}
		return PeekMessage(&msg, NULL, 0, 0, PM_REMOVE) != FALSE;
	};
	while (true) {
		scalingWindow.CompletePendingDestroy();
		// Cleanup may have completed at the start of this pass. Do not dispatch
		// queued application commands after there is nothing left to retire.
		if (quitRequested && !scalingWindow && !scalingWindow.IsRetiring()) return;
		for (uint32_t i = 0; i < MAX_MESSAGES_PER_PASS; ++i) {
			if (!peekPriorityMessage()) {
				break;
			}
			if (msg.message == WM_QUIT) {
				requestQuit();
				break;
			}
			FrameTrace::Scope traceMessage(FrameTrace::Event::FrontendMessage, msg.message);
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			scalingWindow.CompletePendingDestroy();
			if (scalingWindow.HasUrgentOverlayInput()) {
				break;
			}
		}
		// A DLSS overlay is rendered with the next queued content frame. Mouse
		// traffic must not prevent its already-published notification reaching
		// that FIFO while the producer is waiting for a free slot.
		for (uint32_t i = 0; i < MAX_MESSAGES_PER_PASS && PeekMessage(&msg, NULL,
			CommonSharedConstants::WM_FRONTEND_RENDER_DLSSFG,
			CommonSharedConstants::WM_FRONTEND_RENDER_DLSSFG, PM_REMOVE); ++i) {
			if (msg.message == WM_QUIT) { requestQuit(); break; }
			FrameTrace::Scope traceMessage(FrameTrace::Event::FrontendMessage, msg.message);
			DispatchMessage(&msg);
		}
		// Also expose regular content notifications before servicing urgent
		// overlay input, so mouse traffic cannot hide a newer captured frame.
		for (uint32_t i = 0; i < MAX_MESSAGES_PER_PASS && PeekMessage(&msg, NULL,
			CommonSharedConstants::WM_FRONTEND_RENDER,
			CommonSharedConstants::WM_FRONTEND_RENDER, PM_REMOVE); ++i) {
			if (msg.message == WM_QUIT) { requestQuit(); break; }
			FrameTrace::Scope traceMessage(FrameTrace::Event::FrontendMessage, msg.message);
			DispatchMessage(&msg);
		}

		// Parameter callbacks only queue changes. Tear down here after rendering
		// and window callbacks have returned, and service messages during the pause.
		scalingWindow.CompletePendingDestroy();
		if (scalingWindow.IsRetiring()) {
			scalingWindow.Destroy();
			MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
			continue;
		}
		// A quit request must not destroy the cursor input owner while a verified
		// point-clip restore is temporarily rejected. Retirement keeps servicing
		// its bounded retries without requiring another rendered/input frame.
		if (quitRequested) {
			if (!scalingWindow) return;
			scalingWindow.Destroy();
			continue;
		}
		scalingWindow.PollSourceState();
		if (scalingWindow.ProcessPendingStop()) {
			_State(ScalingState::Idle);
			continue;
		}
		if (scalingWindow.ProcessPendingSourceTransition()) {
			_State(scalingWindow.IsSrcRepositioning() ? ScalingState::Starting : ScalingState::Idle);
			continue;
		}
		if (scalingWindow.HasPendingSourceTransition()) {
			// Service releases/cancellation while waiting for the input owner.
			// Do not retry rendering or rely on an already-signalled DXGI gate.
			MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
			continue;
		}
		const uint64_t generation = _commandGeneration.load(std::memory_order_acquire);
		if (parameterRestartGeneration && *parameterRestartGeneration != generation) {
			if (scalingWindow.IsWaitingForParameterRestart()) scalingWindow.Stop();
			parameterRestartGeneration.reset();
		}
		if (State() == ScalingState::Scaling ||
			(parameterRestartGeneration && scalingWindow.IsWaitingForParameterRestart())) {
			const bool wasWaiting = scalingWindow.IsWaitingForParameterRestart();
			scalingWindow.ProcessPendingParameterRestart();
			if (_commandGeneration.load(std::memory_order_acquire) != generation) {
				// A user command issued during teardown/startup supersedes this work.
				if (wasWaiting || scalingWindow.IsWaitingForParameterRestart()) scalingWindow.Stop();
				parameterRestartGeneration.reset();
			} else if (scalingWindow.IsWaitingForParameterRestart()) {
				parameterRestartGeneration = generation;
				_State(ScalingState::Starting);
			} else if (wasWaiting) {
				parameterRestartGeneration.reset();
				_State(scalingWindow ? ScalingState::Scaling : ScalingState::Idle);
			}
		}
		if (scalingWindow.IsWaitingForParameterRestart()) {
			MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
			continue;
		}

		if (scalingWindow) {
			const ScalingState state = State();
			if (state != ScalingState::Starting && state != ScalingState::Stopping) {
				_State(ScalingState::Scaling);
			}

			const auto now = steady_clock::now();
			// Content/input messages wake immediately; periodic cursor checks need
			// not run at 500 Hz on a lower-refresh display.
			const nanoseconds timeout = scalingWindow.Renderer().FrontendPollInterval();
			nanoseconds rest = timeout - (now - lastRenderTime);
			if (const auto idleWait = scalingWindow.Renderer().TimeUntilIdleRedraw()) rest = std::min(rest, *idleWait);

			// One render attempt per pass. Prefer content, which includes the same
			// input, over submitting an old background and then the new frame.
			if (scalingWindow.HasPendingDLSSFGFrame()) {
				scalingWindow.RenderNextDLSSFGFrame();
				lastRenderTime = steady_clock::now();
			} else if (scalingWindow.HasPendingFrontendRender() ||
				scalingWindow.Renderer().HasPendingContent()) {
				scalingWindow.Render();
				lastRenderTime = steady_clock::now();
			} else if (scalingWindow.HasUrgentOverlayInput()) {
				scalingWindow.RenderOverlay();
				lastRenderTime = steady_clock::now();
			} else if (rest.count() <= 0) {
				scalingWindow.Render();
				lastRenderTime = steady_clock::now();
			}
			// Rendering may have stopped the window. Do not dereference its
			// Renderer or sleep on a deadline belonging to the old session.
			if (!scalingWindow) continue;
			if (scalingWindow.HasPendingSourceTransition() || scalingWindow.IsSourceStateCheckDeferred()) {
				MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
				continue;
			}
			rest = scalingWindow.Renderer().FrontendPollInterval() - (steady_clock::now() - lastRenderTime);
			if (const auto idleWait = scalingWindow.Renderer().TimeUntilIdleRedraw()) rest = std::min(rest, *idleWait);

			// 值为 1000000
			constexpr auto ratio = std::ratio_divide<std::milli, std::nano>().num;
			// 向上取整
			// Keep retries short without spinning. Any newly queued input wakes this
			// wait immediately through QS_ALLINPUT.
			const DWORD restMs = (scalingWindow.HasPendingDLSSFGFrame() ||
				scalingWindow.Renderer().HasPendingContent()) ? 1 :
				DWORD((std::max<int64_t>(rest.count(), 0) + ratio - 1) / ratio);
			scalingWindow.Renderer().WaitForFrontendWork(std::chrono::milliseconds(std::min(restMs, DWORD(250))));
		} else if (scalingWindow.IsSrcRepositioning()) {
			std::optional<bool> repositioning =
				IsSrcRepositioning(scalingWindow.SrcTracker().Handle());
			if (repositioning.has_value()) {
				if (*repositioning) {
					// 等待调整完成
					_State(ScalingState::Waiting);
					MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
				} else {
					// 重新缩放。初始化时视为处于缩放状态
					_State(ScalingState::Scaling);
					ScalingWindow::Get().RestartAfterSrcRepositioned();
				}
			} else {
				// 取消缩放
				ScalingWindow::Get().CleanAfterSrcRepositioned();
				_State(ScalingState::Idle);
			}
		} else {
			const ScalingState state = State();
			if (state != ScalingState::Starting && state != ScalingState::Stopping) {
				_State(ScalingState::Idle);
			}
			lastRenderTime = {};
			WaitMessage();
		}
	}
}

const winrt::DispatcherQueue& ScalingRuntime::_Dispatcher() noexcept {
	if (!_dispatcherInitializedCache) {
		_dispatcherInitialized.wait(false, std::memory_order_acquire);
		_dispatcherInitializedCache = true;
	}

	return _dispatcher;
}

void ScalingRuntime::_State(ScalingState value) {
	if (_state.exchange(value, std::memory_order_relaxed) != value) {
		StateChanged.Invoke(value);
	}
}

}
