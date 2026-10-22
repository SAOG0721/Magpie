// Standalone contract test, to be compiled in the later build session.
#include "../src/Magpie.Core/include/DlssgSm86Policy.h"
using namespace Magpie::DlssgSm86Policy;
static_assert(SelectRoute(7, 5) == Route::NativeFamily);
static_assert(SelectRoute(8, 6) == Route::NativeFamily);
static_assert(SelectRoute(8, 9) == Route::NewerPtx);
static_assert(SelectRoute(12, 0) == Route::NewerPtx);
static_assert(SelectRoute(6, 1) == Route::Unsupported);
static_assert(SelectRoute(8, 0) == Route::Unsupported);
static_assert(SelectRoute(99, 0) == Route::Unsupported);
static_assert(MaxMultiplier(false, 0) == 2);
static_assert(MaxMultiplier(false, UINT32_MAX) == 2);
static_assert(MaxMultiplier(true, 0) == 1);
static_assert(MaxMultiplier(true, 1) == 2);
static_assert(MaxMultiplier(true, 2) == 3);
static_assert(MaxMultiplier(true, 3) == 4);
static_assert(MaxMultiplier(true, 5) == 4);
static_assert(MaxMultiplier(true, UINT32_MAX) == 4);
static_assert(EffectiveMultiplier(0, 4) == 2);
static_assert(EffectiveMultiplier(1, 4) == 2);
static_assert(EffectiveMultiplier(2, 4) == 2);
static_assert(EffectiveMultiplier(3, 4) == 3);
static_assert(EffectiveMultiplier(4, 4) == 4);
static_assert(EffectiveMultiplier(UINT32_MAX, 4) == 4);
static_assert(EffectiveMultiplier(4, 2) == 2);
static_assert(EffectiveMultiplier(4, 1) == 1);
#include "../src/Magpie.Core/include/DLSSFGParameters.h"
#include <cassert>
#include <limits>
#include <string>
#include <vector>
using namespace Magpie;
int main() {
    for (float bad : {-1.f, 4.f, 1.5f, std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity()})
        assert(ParseDLSSFGRenderPreset(bad) == DLSSFGRenderPreset::Auto);
    for (int value = 0; value < 4; ++value) {
        auto preset = ParseDLSSFGRenderPreset(static_cast<float>(value));
        assert(static_cast<int>(preset) == value);
        std::vector<std::pair<std::string, bool>> writes;
        assert(SubmitDLSSFGRenderPreset(preset, [&](const char* key, bool flag) {
            writes.emplace_back(key, flag); return true;
        }));
        if (value == 0) { assert(writes.empty()); continue; }
        assert(writes.size() == 2);
        assert(writes[0].first == "DLSSG.UserInterfaceRecompositionEnabled");
        assert(writes[1].first == "DLSSG.LiteCandidateBlendingEnabled");
        assert(writes[0].second == (value == 2));
        assert(writes[1].second == (value == 3));
    }
    unsigned calls = 0;
    assert(!SubmitDLSSFGRenderPreset(DLSSFGRenderPreset::C, [&](const char*, bool) {
        ++calls; return false;
    }));
    assert(calls == 1); // No alternate preset is submitted on failure.
}
