#include "DLSSNRSubmissionWorker.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>

using namespace Magpie;
using namespace std::chrono_literals;

struct WakeState {
	std::atomic<unsigned> count = 0;
	static void Wake(uintptr_t owner) noexcept {
		reinterpret_cast<WakeState*>(owner)->count.fetch_add(1, std::memory_order_release);
	}
};

int main() {
	WakeState wakes;
	unsigned result = 0;
	const auto owner = std::this_thread::get_id();
	DLSSNRSubmissionWorker worker(WakeState::Wake, reinterpret_cast<uintptr_t>(&wakes));
	std::promise<void> entered, release;
	auto gate = release.get_future().share();
	assert(worker.Submit([&] {
		assert(std::this_thread::get_id() != owner);
		entered.set_value();
		gate.wait();
		result = 42;
	}));
	assert(entered.get_future().wait_for(5s) == std::future_status::ready);
	assert(!worker.Submit([] {}));
	const auto started = std::chrono::steady_clock::now();
	for (unsigned i = 0; i < 10000; ++i) assert(!worker.TryTakeCompletion());
	assert(std::chrono::steady_clock::now() - started < 1s);
	assert(wakes.count.load(std::memory_order_acquire) == 0);
	release.set_value();
	worker.Wait();
	assert(result == 42);
	assert(!worker.TryTakeCompletion());
	// Taking CPU completion is also the lifetime boundary for the payload.
	// Repeated submissions must not overlap or lose a wake during reuse.
	for (unsigned i = 0; i < 1000; ++i) {
		assert(worker.Submit([&, i] { result = i; }));
		worker.Wait();
		assert(result == i);
	}
	{
		std::promise<void> destructorEntered, destructorRelease;
		auto destructorGate = destructorRelease.get_future().share();
		auto retiring = std::make_unique<DLSSNRSubmissionWorker>(WakeState::Wake, reinterpret_cast<uintptr_t>(&wakes));
		assert(retiring->Submit([&] {
			destructorEntered.set_value();
			destructorGate.wait();
			result = 2026;
		}));
		assert(destructorEntered.get_future().wait_for(5s) == std::future_status::ready);
		std::promise<void> destructorStarted, destructorFinished;
		auto finished = destructorFinished.get_future();
		std::thread retire([&] {
			destructorStarted.set_value();
			retiring.reset();
			destructorFinished.set_value();
		});
		destructorStarted.get_future().wait();
		assert(finished.wait_for(20ms) == std::future_status::timeout);
		destructorRelease.set_value();
		assert(finished.wait_for(5s) == std::future_status::ready);
		retire.join();
		assert(result == 2026);
	}
	// Destructor join also retires the callback, so no wake can use dead state.
	std::cout << "DLSSNR submission worker: blocked-job owner polling, bounded admission, completion ownership, reuse and teardown passed\n";
}
