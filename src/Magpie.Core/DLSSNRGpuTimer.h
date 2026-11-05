#pragma once
#include <array>

namespace Magpie {

// Samples remain on their own D3D11 timeline, including queued D3D12 waits.
// A busy query slot is skipped; observation never waits for rendering.
class DLSSNRGpuTimer {
public:
	bool Initialize(ID3D11Device* device) noexcept {
		for (auto& slot : _slots) {
			D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
			if (FAILED(device->CreateQuery(&desc, slot.disjoint.put()))) return false;
			desc.Query = D3D11_QUERY_TIMESTAMP;
			if (FAILED(device->CreateQuery(&desc, slot.start.put())) ||
				FAILED(device->CreateQuery(&desc, slot.end.put())) ||
				FAILED(device->CreateQuery(&desc, slot.sectionStart.put())) ||
				FAILED(device->CreateQuery(&desc, slot.sectionEnd.put()))) return false;
		}
		return true;
	}
	void Poll(ID3D11DeviceContext* dc) noexcept {
		for (auto& slot : _slots) {
			if (!slot.pending) continue;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
			uint64_t start = 0, end = 0, sectionStart = 0, sectionEnd = 0;
			if (dc->GetData(slot.disjoint.get(), &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
				dc->GetData(slot.start.get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
				dc->GetData(slot.end.get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
			if (slot.hasSection && (dc->GetData(slot.sectionStart.get(), &sectionStart, sizeof(sectionStart), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
				dc->GetData(slot.sectionEnd.get(), &sectionEnd, sizeof(sectionEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)) continue;
			slot.pending = false;
			if (!data.Disjoint && data.Frequency && end >= start) {
				if (slot.serial > _lastObserved) {
					milliseconds = double(end-start)*1000.0/double(data.Frequency);
					if (slot.hasSection && sectionEnd >= sectionStart)
						sectionMilliseconds = double(sectionEnd-sectionStart)*1000.0/double(data.Frequency);
					_lastObserved = slot.serial;
				}
				++samples;
			}
		}
	}
	void Begin(ID3D11DeviceContext* dc) noexcept {
		_active = nullptr;
		for (auto& slot : _slots) if (!slot.pending && slot.disjoint) {
			_active = &slot; slot.serial = ++_submitted; slot.hasSection = false;
			dc->Begin(slot.disjoint.get()); dc->End(slot.start.get()); break;
		}
	}
	void End(ID3D11DeviceContext* dc) noexcept {
		if (!_active) return;
		dc->End(_active->end.get()); dc->End(_active->disjoint.get());
		_active->pending = true; _active = nullptr;
	}
	void BeginSection(ID3D11DeviceContext* dc) noexcept { if (_active) dc->End(_active->sectionStart.get()); }
	void EndSection(ID3D11DeviceContext* dc) noexcept {
		if (_active) { dc->End(_active->sectionEnd.get()); _active->hasSection = true; }
	}
	double milliseconds = 0;
	double sectionMilliseconds = 0;
	uint64_t samples = 0;
private:
	struct Slot {
		winrt::com_ptr<ID3D11Query> disjoint, start, end, sectionStart, sectionEnd;
		bool pending = false, hasSection = false;
		uint64_t serial = 0;
	};
	std::array<Slot, 4> _slots;
	Slot* _active = nullptr;
	uint64_t _submitted = 0, _lastObserved = 0;
};

}
