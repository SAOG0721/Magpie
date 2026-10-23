#pragma once
#include "XeSSFGPatchTransaction.h"
#include "XeSSFGPacing.h"
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <mutex>
#pragma comment(lib, "bcrypt.lib")

namespace Magpie::XeSSFGCompatibility {
// One lease covers EVERY XeFG context, including native 2x. No SDK call is
// permitted before acquisition or after release. Failed destruction poisons
// this module for the process lifetime; it must not patch live SDK workers.
inline std::mutex leaseMutex;
inline bool leased = false, poisoned = false;
inline std::array<Patch, 8> installed{};

struct ImageBackend {
    uint8_t* base = nullptr;
    uint32_t imageSize = 0;
    bool Preflight(Patch& patch) const noexcept {
        if (!patch.size || patch.size > 16 || patch.rva > imageSize ||
            patch.size > imageSize - patch.rva) return false;
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(base + patch.rva, &info, sizeof(info)) ||
            info.AllocationBase != base || info.State != MEM_COMMIT || info.Type != MEM_IMAGE ||
            (info.Protect != PAGE_EXECUTE_READ && info.Protect != PAGE_EXECUTE_WRITECOPY) ||
            reinterpret_cast<uintptr_t>(base + patch.rva + patch.size) >
            reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize) return false;
        patch.protection = info.Protect;
        return memcmp(base + patch.rva, patch.original.data(), patch.size) == 0;
    }
    bool Write(const Patch& patch, bool restore) const noexcept {
        auto* address = base + patch.rva;
        DWORD previous = 0;
        if (!VirtualProtect(address, patch.size, PAGE_EXECUTE_READWRITE, &previous)) return false;
        const auto& bytes = restore ? patch.original : patch.replacement;
        memcpy(address, bytes.data(), patch.size);
        const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, patch.size) != FALSE;
        DWORD ignored = 0;
        const bool protectedAgain = VirtualProtect(address, patch.size, patch.protection, &ignored) != FALSE;
        MEMORY_BASIC_INFORMATION info{};
        return flushed && protectedAgain && VirtualQuery(address, &info, sizeof(info)) &&
            info.Protect == patch.protection && memcmp(address, bytes.data(), patch.size) == 0;
    }
};

inline bool KnownFile(HMODULE module) noexcept {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return false;
    // Deny concurrent writers/replacement while hashing the actual loaded file.
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart == 22957432;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (ok) ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<uint8_t, 65536> buffer{};
    DWORD bytes = 0;
    while (ok) {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) { ok = false; break; }
        if (!bytes) break;
        ok = BCryptHashData(hash, buffer.data(), bytes, 0) >= 0;
    }
    std::array<uint8_t, 32> digest{};
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    constexpr std::array<uint8_t, 32> expected{0xEC,0x5E,0x0C,0x65,0xE0,0x75,0x57,0x0C,
        0x6E,0xDE,0x72,0x61,0x8B,0xB6,0x66,0xD0,0xBE,0x0C,0x2E,0x10,0xB2,0xEA,0x97,0x62,
        0xC0,0xFE,0x8C,0xB8,0xE3,0x75,0xAB,0x27};
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok && digest == expected;
}

inline std::array<Patch, 8> MakePatches(uint32_t interpolations) noexcept {
    std::array<Patch, 8> patches{{
        {0x20DA4F,6,{0x0F,0x85,0xCC,0,0,0},{0xE9,0xCD,0,0,0,0x90}},
        {0x1A5DE4,2,{0x74,0x09},{0xEB,0x06}},
        {0x1A517D,5,{0xBB,3,0,0,0},{0xBB,0,0,0,0}},
        {0x1A45C2,10,{0xC7,0x87,0x6C,1,0,0,1,0,0,0},{0xC7,0x87,0x6C,1,0,0,0,0,0,0}},
        {0x20973B,5,{0xB8,1,0,0,0},{0xB8,0,0,0,0}},
        {Pacing::PresentThunkRva,16}, {Pacing::SchedThunkRva,16}, {Pacing::TimestampThunkRva,16}
    }};
    memcpy(patches[2].replacement.data()+1, &interpolations, 4);
    memcpy(patches[3].replacement.data()+6, &interpolations, 4);
    memcpy(patches[4].replacement.data()+1, &interpolations, 4);
    const uint8_t* originals[]{Pacing::PresentThunkExpected, Pacing::SchedThunkExpected, Pacing::TimestampThunkExpected};
    const uintptr_t targets[]{reinterpret_cast<uintptr_t>(&Pacing::Detour),
        reinterpret_cast<uintptr_t>(&Pacing::SchedForwarder), reinterpret_cast<uintptr_t>(&Pacing::TsDetour)};
    for (size_t i = 0; i < 3; ++i) {
        auto& patch = patches[i+5];
        memcpy(patch.original.data(), originals[i], 16);
        patch.replacement.fill(0xCC);
        patch.replacement[0] = 0xFF; patch.replacement[1] = 0x25;
        memset(patch.replacement.data()+2, 0, 4);
        memcpy(patch.replacement.data()+6, &targets[i], 8);
    }
    return patches;
}

class Lease {
public:
    Lease() = default;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    // Caller must explicitly certify context destruction before release.
    ~Lease() { if (_held) Release(false); }
    bool Acquire(bool compatibility, uint32_t multiplier, const void* createEntry) noexcept {
        std::lock_guard lock(leaseMutex);
        _failure = "context lease busy/poisoned, or invalid multiplier";
        if (_held || leased || poisoned || multiplier < 2 || multiplier > 4) return false;
        if (compatibility && multiplier == 2) return false;
        _patched = _observed = false;
        // Native 2x keeps working with an unknown SDK. Observation is optional,
        // and must never enable the compatibility/pacing patches on that path.
        if (!_Install(compatibility, multiplier, createEntry) && (compatibility || poisoned)) return false;
        Pacing::outputProfiler.Reset(false);
        leased = _held = true;
        return true;
    }
    bool Release(bool workersStopped) noexcept {
        std::lock_guard lock(leaseMutex);
        if (!_held) return true;
        bool ok = workersStopped && Pacing::activeCallbacks.load(std::memory_order_acquire) == 0;
        if (ok && _observed) {
            ok = RestorePatches(_Patches(), _backend);
            if (ok) Pacing::g_enabled = false;
        }
        Pacing::outputProfiler.Reset(false);
        poisoned |= !ok;
        _held = false;
        leased = false;
        return ok;
    }
    bool Patched() const noexcept { return _patched; }
    bool ObservesOutput() const noexcept { return _observed; }
    const char* Failure() const noexcept { return _failure; }
    void Reset() noexcept {
        Pacing::sourcePeriodNs.store(0, std::memory_order_relaxed);
        Pacing::resetEpoch.fetch_add(1, std::memory_order_release);
    }
private:
    std::span<Patch> _Patches() noexcept {
        return _patched ? std::span(installed) : std::span(installed).subspan(5, 1);
    }
    bool _Install(bool compatibility, uint32_t multiplier, const void* createEntry) noexcept {
        _failure = "loaded libxess_fg.dll file identity/hash is not in the compatibility allowlist";
        HMODULE module = GetModuleHandleW(L"libxess_fg.dll");
        if (!module || !KnownFile(module)) return false;
        auto* base = reinterpret_cast<uint8_t*>(module);
        _failure = "loaded libxess_fg.dll PE identity is not supported";
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->FileHeader.TimeDateStamp != 0x69CB0F4D || nt->OptionalHeader.SizeOfImage != 0x015ED000 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        const auto entry = reinterpret_cast<uintptr_t>(createEntry);
        _failure = "XeFG create entry does not belong to the pinned provider module";
        if (entry < reinterpret_cast<uintptr_t>(base) || entry >= reinterpret_cast<uintptr_t>(base)+0x015ED000 ||
            createEntry != reinterpret_cast<const void*>(GetProcAddress(module, "xefgSwapChainD3D12CreateContext"))) return false;
        _backend = {base, nt->OptionalHeader.SizeOfImage};
        _failure = "native provider code/protection differs from the pinned runtime";
        // The pinned file contains the provider itself. Confirm native targets
        // are executable pages in this image, not exports forwarded elsewhere.
        constexpr uint8_t nativeExpected[4][16]{
            {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41},
            {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57},
            {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20},
            {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56}
        };
        size_t nativeIndex = 0;
        for (uint32_t rva : {Pacing::NativePresentRva, Pacing::SchedFnRva,
            Pacing::TimestampFnRva, Pacing::RingSnapshotFnRva}) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(base+rva, &info, sizeof(info)) || info.AllocationBase != base ||
                info.State != MEM_COMMIT || info.Protect != PAGE_EXECUTE_READ ||
                memcmp(base+rva, nativeExpected[nativeIndex++], 16) != 0) return false;
        }
        if (!QueryPerformanceFrequency(&Pacing::g_freq) || Pacing::g_freq.QuadPart <= 0) return false;
        Pacing::g_base = base;
        Pacing::g_native = reinterpret_cast<Pacing::PresentFn>(base+Pacing::NativePresentRva);
        Pacing::g_schedNative = reinterpret_cast<Pacing::SchedFn>(base+Pacing::SchedFnRva);
        Pacing::g_ringSnapshot = reinterpret_cast<Pacing::RingSnapshotFn>(base+Pacing::RingSnapshotFnRva);
        Pacing::g_tsNative = reinterpret_cast<Pacing::TimestampFn>(base+Pacing::TimestampFnRva);
        installed = MakePatches(multiplier-1);
        _patched = compatibility;
        const auto result = InstallPatches(_Patches(), _backend);
        if (result != PatchResult::Success) {
            poisoned = result == PatchResult::Poisoned;
            _failure = result == PatchResult::Rejected ? "original patch bytes or page protection did not match" :
                result == PatchResult::RolledBack ? "patch write/protection/cache operation failed; original bytes restored" :
                "patch rollback could not be verified; further XeFG sessions require a process restart";
            return false;
        }
        Reset();
        Pacing::outputPosition = Pacing::outputSize = 0;
        Pacing::outputCalls = 0; Pacing::schedulerCalls = 0; Pacing::extraWaitNs = 0;
        Pacing::diagnosticMedianNs = 0; Pacing::diagnosticUnitNs = 0;
        Pacing::diagnosticDeadlineShiftNs = 0;
        Pacing::g_enabled = compatibility;
        _observed = true;
        _failure = "";
        return true;
    }
    ImageBackend _backend{};
    bool _held = false, _patched = false, _observed = false;
    const char* _failure = "";
};
}
