#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include "../src/PointerHookTransaction.h"

struct FakeOperations {
    bool deny{};
    unsigned restore_failures{};
    unsigned restore_calls{};
    unsigned exchanges{};
    void* foreign_on_rollback{};
    bool writable(void**, std::uint32_t& old, std::uint32_t& error) {
        old = PAGE_READONLY;
        error = deny ? ERROR_ACCESS_DENIED : 0;
        return !deny;
    }
    bool restore(void**, std::uint32_t, std::uint32_t& error) {
        error = ++restore_calls <= restore_failures ? ERROR_ACCESS_DENIED : 0;
        return !error;
    }
    void* exchange(void** slot, void* replacement, void* expected) {
        if (++exchanges == 2 && foreign_on_rollback) { *slot = foreign_on_rollback; }
        auto* seen = *slot;
        if (seen == expected) { *slot = replacement; }
        return seen;
    }
};

int old_call() { return 1; }
int new_call() { return 2; }
struct AtomicOperations {
    bool writable(void**, std::uint32_t& old, std::uint32_t& error) { old = PAGE_READWRITE; error = 0; return true; }
    bool restore(void**, std::uint32_t, std::uint32_t& error) { error = 0; return true; }
    void* exchange(void** slot, void* value, void* expected) { return InterlockedCompareExchangePointer(slot, value, expected); }
};

int main() {
    using namespace dstorage_guard;
    int originals[3]{};
    auto* original = static_cast<void*>(&originals[0]);
    auto* detour = static_cast<void*>(&originals[1]);
    auto* foreign = static_cast<void*>(&originals[2]);
    unsigned failed{};
    for (unsigned mode = 0; mode < 7; ++mode) {
        void* slot = mode == 2 ? foreign : original;
        FakeOperations operations;
        operations.deny = mode == 1;
        operations.restore_failures = mode == 3 || mode == 5 || mode == 6 ? 1 : (mode == 4 ? 2 : 0);
        operations.foreign_on_rollback = mode == 5 ? foreign : nullptr;
        if (mode == 6) { slot = foreign; }
        const auto result = install_pointer_hook(&slot, original, detour, operations);
        bool ok{};
        switch (mode) {
        case 0: ok = slot == detour && result.status == PointerHookStatus::installed && result.protection_restored; break;
        case 1: ok = slot == original && result.status == PointerHookStatus::protect_failed && operations.exchanges == 0; break;
        case 2: ok = slot == foreign && result.status == PointerHookStatus::conflict && result.protection_restored; break;
        case 3: ok = slot == original && result.status == PointerHookStatus::restore_failed && result.rolled_back && result.protection_restored; break;
        case 4: ok = slot == original && result.status == PointerHookStatus::restore_failed && result.rolled_back && !result.protection_restored; break;
        case 5: ok = slot == foreign && result.status == PointerHookStatus::restore_failed && !result.rolled_back && result.protection_restored; break;
        case 6: ok = slot == foreign && result.status == PointerHookStatus::conflict && result.protection_restored && operations.exchanges == 1; break;
        }
        std::printf("pointer_transaction_case=%u result=%s\n", mode, ok ? "PASS" : "FAIL");
        failed += !ok;
    }
    alignas(8) void* slot = reinterpret_cast<void*>(&old_call);
    std::atomic<bool> start{};
    std::atomic<unsigned> invalid{};
    auto reader = [&] {
        while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        for (unsigned i = 0; i < 500000; ++i) {
            auto* fn = std::atomic_ref<void*>(slot).load(std::memory_order_acquire);
            const auto value = reinterpret_cast<int(*)()>(fn)();
            if (value != 1 && value != 2) { ++invalid; }
        }
    };
    std::thread a(reader), b(reader);
    start.store(true, std::memory_order_release);
    AtomicOperations operations;
    for (unsigned i = 0; i < 10000; ++i) {
        auto* expected = reinterpret_cast<void*>(i % 2 == 0 ? &old_call : &new_call);
        auto* replacement = reinterpret_cast<void*>(i % 2 == 0 ? &new_call : &old_call);
        failed += install_pointer_hook(&slot, expected, replacement, operations).status != PointerHookStatus::installed;
    }
    a.join();
    b.join();
    failed += invalid.load();
    std::printf("pointer_concurrent_calls=1000000 exchanges=10000 invalid=%u result=%s\n", invalid.load(), failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
