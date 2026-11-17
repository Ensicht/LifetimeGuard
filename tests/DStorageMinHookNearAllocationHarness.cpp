#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "MinHook.h"

namespace {

constexpr std::uintptr_t kLegacyRange = 0x40000000ULL;
constexpr std::uintptr_t kFixedRange = 0x7FFF0000ULL;

using TargetFn = int (*)(int);
TargetFn g_original{};

int detour_target(int value) {
    return value + 100;
}

std::uintptr_t align_up(std::uintptr_t value, std::uintptr_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

std::uintptr_t align_down(std::uintptr_t value, std::uintptr_t alignment) {
    return value / alignment * alignment;
}

class Reservations {
public:
    ~Reservations() {
        for (auto* allocation : allocations_) {
            VirtualFree(allocation, 0, MEM_RELEASE);
        }
    }

    bool occupy_legacy_window(void* origin) {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        const auto granularity =
            static_cast<std::uintptr_t>(info.dwAllocationGranularity);
        const auto address = reinterpret_cast<std::uintptr_t>(origin);
        const auto system_min =
            reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
        const auto system_max =
            reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
        const auto lower = address > kLegacyRange
            ? std::max(system_min, address - kLegacyRange)
            : system_min;
        const auto upper = system_max - address > kLegacyRange
            ? address + kLegacyRange
            : system_max;

        auto cursor = lower;
        while (cursor < upper) {
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(
                    reinterpret_cast<void*>(cursor), &memory,
                    sizeof(memory)) == 0) {
                std::printf(
                    "FAIL: VirtualQuery address=%p error=%lu\n",
                    reinterpret_cast<void*>(cursor), GetLastError());
                return false;
            }
            const auto region_begin =
                reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
            const auto region_end = region_begin + memory.RegionSize;
            const auto clipped_end = std::min(region_end, upper);
            if (memory.State == MEM_FREE) {
                const auto reserve_begin = align_up(
                    std::max(region_begin, lower), granularity);
                const auto reserve_end = align_down(clipped_end, granularity);
                if (reserve_end > reserve_begin) {
                    const auto size = reserve_end - reserve_begin;
                    auto* allocation = VirtualAlloc(
                        reinterpret_cast<void*>(reserve_begin), size,
                        MEM_RESERVE, PAGE_NOACCESS);
                    if (allocation == nullptr) {
                        std::printf(
                            "FAIL: reserve address=%p size=%llu error=%lu\n",
                            reinterpret_cast<void*>(reserve_begin),
                            static_cast<unsigned long long>(size),
                            GetLastError());
                        return false;
                    }
                    allocations_.push_back(allocation);
                    reserved_bytes_ += size;
                }
            }
            if (clipped_end <= cursor) {
                std::printf("FAIL: address walk made no progress\n");
                return false;
            }
            cursor = clipped_end;
        }
        return !allocations_.empty();
    }

    std::size_t count() const {
        return allocations_.size();
    }

    std::uint64_t bytes() const {
        return reserved_bytes_;
    }

private:
    std::vector<void*> allocations_;
    std::uint64_t reserved_bytes_{};
};

void* make_target() {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    auto* memory = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, info.dwAllocationGranularity,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (memory == nullptr) {
        return nullptr;
    }
    constexpr std::uint8_t code[] = {
        0x89, 0xC8,             // mov eax, ecx
        0x83, 0xC0, 0x01,       // add eax, 1
        0xC3,                   // ret
        0x90, 0x90, 0x90, 0x90,
    };
    std::memcpy(memory, code, sizeof(code));
    DWORD previous{};
    if (!VirtualProtect(
            memory, info.dwAllocationGranularity,
            PAGE_EXECUTE_READ, &previous)) {
        VirtualFree(memory, 0, MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), memory, sizeof(code));
    return memory;
}

std::uint64_t distance_between(void* left, void* right) {
    const auto a = reinterpret_cast<std::uintptr_t>(left);
    const auto b = reinterpret_cast<std::uintptr_t>(right);
    return a > b ? a - b : b - a;
}

} // namespace

int main(int argc, char** argv) {
    const bool expect_failure =
        argc == 2 && std::strcmp(argv[1], "--expect-failure") == 0;
    const bool expect_success =
        argc == 2 && std::strcmp(argv[1], "--expect-success") == 0;
    if (!expect_failure && !expect_success) {
        std::printf("usage: harness --expect-failure|--expect-success\n");
        return 2;
    }

    auto* target = make_target();
    if (target == nullptr) {
        std::printf("FAIL: target allocation error=%lu\n", GetLastError());
        return 3;
    }

    Reservations reservations;
    if (!reservations.occupy_legacy_window(target)) {
        VirtualFree(target, 0, MEM_RELEASE);
        return 4;
    }
    std::printf(
        "reserved_regions=%llu reserved_bytes=%llu target=%p\n",
        static_cast<unsigned long long>(reservations.count()),
        static_cast<unsigned long long>(reservations.bytes()), target);

    auto status = MH_Initialize();
    if (status != MH_OK) {
        std::printf("FAIL: MH_Initialize status=%d\n", status);
        VirtualFree(target, 0, MEM_RELEASE);
        return 5;
    }
    void* original{};
    status = MH_CreateHook(
        target, reinterpret_cast<void*>(&detour_target), &original);
    std::printf(
        "create_status=%d status_name=%s original=%p\n",
        status, MH_StatusToString(status), original);

    if (expect_failure) {
        const bool passed = status == MH_ERROR_MEMORY_ALLOC && original == nullptr;
        MH_Uninitialize();
        VirtualFree(target, 0, MEM_RELEASE);
        std::printf("legacy_window_failure=%s\n", passed ? "PASS" : "FAIL");
        return passed ? 0 : 6;
    }

    if (status != MH_OK || original == nullptr) {
        MH_Uninitialize();
        VirtualFree(target, 0, MEM_RELEASE);
        return 7;
    }
    const auto trampoline_distance = distance_between(target, original);
    std::printf(
        "trampoline_distance=%llu legacy_range=%llu fixed_range=%llu\n",
        static_cast<unsigned long long>(trampoline_distance),
        static_cast<unsigned long long>(kLegacyRange),
        static_cast<unsigned long long>(kFixedRange));
    if (trampoline_distance <= kLegacyRange ||
        trampoline_distance > kFixedRange) {
        std::printf("FAIL: trampoline not allocated in expanded window\n");
        MH_RemoveHook(target);
        MH_Uninitialize();
        VirtualFree(target, 0, MEM_RELEASE);
        return 8;
    }
    status = MH_EnableHook(target);
    if (status != MH_OK) {
        std::printf("FAIL: MH_EnableHook status=%d\n", status);
        MH_RemoveHook(target);
        MH_Uninitialize();
        VirtualFree(target, 0, MEM_RELEASE);
        return 9;
    }

    const auto hooked_result = reinterpret_cast<TargetFn>(target)(5);
    g_original = reinterpret_cast<TargetFn>(original);
    const auto original_result = g_original(5);
    const bool passed = hooked_result == 105 && original_result == 6;
    std::printf(
        "hooked_result=%d original_result=%d expanded_window=%s\n",
        hooked_result, original_result, passed ? "PASS" : "FAIL");

    MH_DisableHook(target);
    MH_RemoveHook(target);
    MH_Uninitialize();
    VirtualFree(target, 0, MEM_RELEASE);
    return passed ? 0 : 10;
}
