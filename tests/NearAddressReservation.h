#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <vector>

class NearAddressReservation {
public:
    ~NearAddressReservation() {
        for (auto* address : owned_) { VirtualFree(address, 0, MEM_RELEASE); }
    }
    bool fill(void* target) {
        constexpr std::uintptr_t range = 0x7FFF0000ULL;
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto step = static_cast<std::uintptr_t>(system.dwAllocationGranularity);
        const auto origin = reinterpret_cast<std::uintptr_t>(target);
        const auto lower = std::max(origin - range, reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress));
        const auto upper = std::min(origin + range, reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress));
        unsigned count{};
        for (auto cursor = lower; cursor < upper;) {
            if (++count > 10000) { return false; }
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info))) { return false; }
            const auto end = std::min(upper, reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
            if (end <= cursor) { return false; }
            if (info.State == MEM_FREE) {
                const auto begin = (cursor + step - 1) / step * step;
                const auto finish = (end + step - 1) / step * step;
                if (finish > begin) {
                    auto* memory = VirtualAlloc(reinterpret_cast<void*>(begin), finish - begin, MEM_RESERVE, PAGE_NOACCESS);
                    if (!memory) { return false; }
                    owned_.push_back(memory);
                }
            }
            cursor = end;
        }
        return true;
    }
private:
    std::vector<void*> owned_;
};
