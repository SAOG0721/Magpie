#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace Magpie {

// One CPU submission owns the feature until the owner takes its completion.
// The replaceable capture request stays outside this worker, on the owner.
class DLSSNRSubmissionWorker {
public:
	using Wake = void(*)(uintptr_t) noexcept;
	DLSSNRSubmissionWorker(Wake wake, uintptr_t owner) noexcept : _wake(wake), _owner(owner) {}
	DLSSNRSubmissionWorker(const DLSSNRSubmissionWorker&) = delete;
	DLSSNRSubmissionWorker& operator=(const DLSSNRSubmissionWorker&) = delete;
	~DLSSNRSubmissionWorker() {
		Wait();
		{
			std::lock_guard lock(_mutex);
			_stopping = true;
		}
		_changed.notify_one();
		if (_thread.joinable()) _thread.join();
	}

	bool Submit(std::function<void()> work) noexcept {
		std::lock_guard lock(_mutex);
		if (_outstanding || _stopping || !work) return false;
		try {
			if (!_thread.joinable()) _thread = std::thread([this] { _Run(); });
		} catch (...) {
			return false;
		}
		_work = std::move(work);
		_outstanding = true;
		_changed.notify_one();
		return true;
	}

	// Successful take/Wait acquires all writes made by the job. Polling a busy
	// job never waits for its CPU call and never exposes its mutable payload.
	bool TryTakeCompletion() noexcept {
		std::lock_guard lock(_mutex);
		if (!_completed) return false;
		_completed = _outstanding = false;
		return true;
	}
	void Wait() noexcept {
		std::unique_lock lock(_mutex);
		_changed.wait(lock, [this] { return !_outstanding || _completed; });
		_completed = _outstanding = false;
	}

private:
	void _Run() noexcept {
		std::unique_lock lock(_mutex);
		for (;;) {
			_changed.wait(lock, [this] { return _stopping || bool(_work); });
			if (_stopping) return;
			auto work = std::move(_work);
			lock.unlock();
			work();
			work = {};
			lock.lock();
			_completed = true;
			// Publish completion before waking an idle capture owner. The wake
			// contains only a thread identity, never feature/renderer pointers.
			lock.unlock();
			_changed.notify_all();
			if (_wake) _wake(_owner);
			lock.lock();
		}
	}
	std::mutex _mutex;
	std::condition_variable _changed;
	std::thread _thread;
	std::function<void()> _work;
	Wake _wake;
	uintptr_t _owner;
	bool _outstanding = false, _completed = false, _stopping = false;
};

}
