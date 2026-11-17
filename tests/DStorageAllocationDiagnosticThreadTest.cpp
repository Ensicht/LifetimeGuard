#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include "MinHookAllocationDiagnostics.h"
extern "C" {
#include "buffer.h"
}

__attribute__((noinline)) int target_a() { return 1; }
__attribute__((noinline)) int target_b() { return 2; }

int main() {
    InitializeBuffer();
    std::atomic<unsigned> phase{};
    std::atomic<unsigned> failures{};
    void* slots[2]{};
    auto inspect = [&](unsigned index) {
        MinHookAllocationDiagnostics empty{};
        MinHookAllocationDiagnostics before{};
        MinHookGetAllocationDiagnostics(&before);
        failures += std::memcmp(&empty, &before, sizeof(empty)) != 0;
        auto* origin = index == 0 ? reinterpret_cast<void*>(&target_a)
                                  : reinterpret_cast<void*>(&target_b);
        SetLastError(0xA000 + index);
        slots[index] = AllocateBuffer(origin);
        const auto error = GetLastError();
        MinHookAllocationDiagnostics first{};
        MinHookGetAllocationDiagnostics(&first);
        failures += slots[index] == nullptr;
        failures += first.origin != reinterpret_cast<std::uintptr_t>(origin);
        failures += GetLastError() != error;
        return first;
    };
    // Serialize the allocator (normally protected by MinHook's lock), but
    // retain both threads so each snapshot must survive another caller.
    std::thread a([&] {
        const auto first = inspect(0);
        phase.store(1, std::memory_order_release);
        while (phase.load(std::memory_order_acquire) != 2) { std::this_thread::yield(); }
        MinHookAllocationDiagnostics after{};
        MinHookGetAllocationDiagnostics(&after);
        failures += std::memcmp(&first, &after, sizeof(first)) != 0;
        phase.store(3, std::memory_order_release);
    });
    std::thread b([&] {
        while (phase.load(std::memory_order_acquire) != 1) { std::this_thread::yield(); }
        const auto first = inspect(1);
        phase.store(2, std::memory_order_release);
        while (phase.load(std::memory_order_acquire) != 3) { std::this_thread::yield(); }
        MinHookAllocationDiagnostics after{};
        MinHookGetAllocationDiagnostics(&after);
        failures += std::memcmp(&first, &after, sizeof(first)) != 0;
    });
    a.join();
    b.join();
    MinHookAllocationDiagnostics main_thread{}, zero{};
    MinHookGetAllocationDiagnostics(&main_thread);
    failures += std::memcmp(&main_thread, &zero, sizeof(zero)) != 0;
    for (auto* slot : slots) { if (slot) { FreeBuffer(slot); } }
    UninitializeBuffer();
    std::printf("allocator_thread_local_isolation=3_threads last_error_preserved=1 failures=%u result=%s\n",
        failures.load(), failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
