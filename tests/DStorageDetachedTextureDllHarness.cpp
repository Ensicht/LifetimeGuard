#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../src/DetachedTextureRetry.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using Initialize = bool (*)(const void*);
using SetLoading = bool (*)(bool);
using Release = void (*)(void*, void*);
using Queue = void (*)(void*, void*);
using Bind = bool (*)(void**, void**, Release, Queue);
using Event = bool (*)(unsigned, void*, void*);
using Stats = bool (*)(dstorage_guard::TextureRetryStats*, std::size_t, std::uint32_t*);
Event event{};
SetLoading loading{};
Stats get_stats{};
std::array<void*, 12> vtable{};
alignas(16) std::array<std::uint8_t, 0x2000> manager{};
void* manager_slot = manager.data();
std::mutex queue_lock;
std::vector<void*> queued;
std::atomic<unsigned> released{}, deleted{}, enqueued{};
bool complete_inside_queue{};

void check(bool result, const char* label) {
    if (!result) { std::fprintf(stderr, "FAIL %s\n", label); std::exit(1); }
}
template<class T> T& field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
}
struct alignas(16) Resource {
    std::array<std::uint8_t, 0xE8> bytes{};
    explicit Resource(unsigned refs = 2) {
        field<void*>(bytes.data(), 0) = vtable.data();
        field<unsigned>(bytes.data(), 0x28) = refs;
        field<std::uint64_t>(bytes.data(), 0x30) = 0x1122334455667788;
        field<std::uintptr_t>(bytes.data(), 0x70) = 0x98765000;
    }
    void* ptr() { return bytes.data(); }
    unsigned refs() { return field<unsigned>(ptr(), 0x28); }
};
bool original_ready(void* object) { return field<std::uint8_t>(object, 0x38) != 0; }
void* original_delete(void* object, std::uint32_t flags) {
    check(flags == 0, "delete flags forwarded");
    ++deleted;
    return object;
}
void original_release(void* native_manager, void* object) {
    check(native_manager == manager_slot, "release manager");
    check(field<LONG>(object, 0x28) > 0, "release owns a reference");
    ++released;
    if (InterlockedDecrement(&field<LONG>(object, 0x28)) == 0) {
        check(event(3, manager_slot, object), "delete forwarding");
    }
}
void finish(void* object, bool success) {
    field<void*>(object, 0x50) = nullptr;
    field<std::uintptr_t>(object, 0x70) += 0x100;
    field<std::uintptr_t>(object, 0x78) = success ? 0xABCDEF00 : 0;
    field<std::uint8_t>(object, 0x38) = success ? 1 : 0;
    check(event(1, manager_slot, object), "worker completion");
}
void original_queue(void* native_manager, void* object) {
    check(native_manager == manager_slot, "queue manager");
    check(field<unsigned>(object, 0x28) >= 2, "queued job owns reference");
    check(!original_ready(object) && field<void*>(object, 0x50) == nullptr, "native fresh read eligibility");
    ++enqueued;
    field<void*>(object, 0x50) = reinterpret_cast<void*>(0x10203040);
    if (complete_inside_queue) {
        finish(object, true);
    } else {
        const std::lock_guard lock{queue_lock};
        queued.push_back(object);
    }
}
void drain(bool success) {
    std::vector<void*> batch;
    { const std::lock_guard lock{queue_lock}; batch.swap(queued); }
    for (void* object : batch) { finish(object, success); }
}
dstorage_guard::TextureRetryStats stats() {
    dstorage_guard::TextureRetryStats output{};
    check(get_stats(&output, sizeof(output), nullptr), "read stats");
    return output;
}
template<class T> T symbol(HMODULE dll, const char* name) {
    auto address = GetProcAddress(dll, name);
    check(address != nullptr, name);
    return reinterpret_cast<T>(address);
}
}

int main() {
    HMODULE dll = LoadLibraryW(L"DStorageFileLifetimeGuard.dll");
    check(dll != nullptr, "load production DLL");
    auto initialize = symbol<Initialize>(dll, "reframework_plugin_initialize");
    auto bind = symbol<Bind>(dll, "dstorage_file_lifetime_guard_test_bind_texture_retry");
    loading = symbol<SetLoading>(dll, "dstorage_file_lifetime_guard_test_set_loading_state");
    event = symbol<Event>(dll, "dstorage_file_lifetime_guard_test_texture_retry");
    get_stats = symbol<Stats>(dll, "dstorage_file_lifetime_guard_get_texture_retry_stats");
    check(!bind(vtable.data(), &manager_slot, original_release, original_queue), "test ABI unavailable before offline init");
    initialize(nullptr);
    vtable[6] = reinterpret_cast<void*>(&original_delete);
    vtable[10] = reinterpret_cast<void*>(&original_ready);
    field<HANDLE>(manager_slot, 0x5E0) = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(field<HANDLE>(manager_slot, 0x5E0) != nullptr, "native event fixture");
    check(bind(vtable.data(), &manager_slot, original_release, original_queue), "bind actual native adapter");
    check(!bind(vtable.data(), &manager_slot, original_release, original_queue), "no live rebind");

    check(loading(false), "normal game gate closed");
    Resource previous;
    check(event(1, manager_slot, previous.ptr()), "capture terminal receipt outside loading");
    check(previous.refs() == 1 && enqueued == 0, "receipt keeps no reference and does not recover outside loading");
    for (int i = 0; i < 100000; ++i) { check(!event(2, manager_slot, previous.ptr()), "unready getter unchanged"); }
    check(enqueued == 0 && stats().resident == 1, "normal gameplay never submits retry");

    check(loading(true), "scene loading opened");
    std::array<std::thread, 16> readers;
    for (auto& reader : readers) {
        reader = std::thread([&] {
            for (int i = 0; i < 1000; ++i) { check(!event(2, manager_slot, previous.ptr()), "parallel getter unchanged"); }
        });
    }
    for (auto& reader : readers) { reader.join(); }
    check(enqueued == 1 && previous.refs() == 2, "16000 queries one job one added ref");
    check(WaitForSingleObject(field<HANDLE>(manager_slot, 0x5E0), 0) == WAIT_OBJECT_0, "read queue event signaled");
    check(loading(false), "loading closes before native job completion");
    drain(true);
    check(previous.refs() == 1 && stats().recovered == 1, "native job drains after gate closure");
    check(event(2, manager_slot, previous.ptr()), "ready getter now true natively");

    check(loading(true), "next generation");
    Resource immediate;
    const auto releases_before = released.load();
    check(event(1, manager_slot, immediate.ptr()), "immediate terminal retry");
    check(immediate.refs() == 2 && released == releases_before && enqueued == 2, "existing worker ref transferred");
    drain(false);
    check(immediate.refs() == 1 && stats().failed == 1, "failed reread releases job ref");
    for (int i = 0; i < 10000; ++i) { event(2, manager_slot, immediate.ptr()); }
    check(enqueued == 2, "failed reread not repeated this generation");
    loading(false); loading(true);
    event(2, manager_slot, immediate.ptr());
    check(enqueued == 3 && immediate.refs() == 2, "failed file may retry next loading generation");
    drain(true);

    Resource unobserved(1);
    for (int i = 0; i < 1000; ++i) { event(2, manager_slot, unobserved.ptr()); }
    check(enqueued == 3, "null task alone never proves terminal");
    Resource other_caller;
    event(0, manager_slot, other_caller.ptr());
    check(other_caller.refs() == 1 && enqueued == 3, "unrelated Release caller never qualifies");

    loading(false);
    Resource reused;
    event(1, manager_slot, reused.ptr());
    event(0, manager_slot, reused.ptr());
    check(reused.refs() == 0, "resource retired with receipt");
    reused = Resource(1);
    loading(true);
    event(2, manager_slot, reused.ptr());
    check(enqueued == 3, "ABA address identity parsed reuse cannot revive old receipt");

    Resource sync;
    complete_inside_queue = true;
    event(1, manager_slot, sync.ptr());
    complete_inside_queue = false;
    check(sync.refs() == 1 && original_ready(sync.ptr()), "completion inside enqueue safe");
    check(stats().queued == 4 && stats().recovered == 3, "recovery accounting");

    Resource orphan(1);
    event(1, manager_slot, orphan.ptr());
    check(orphan.refs() == 0 && enqueued == 4, "no consumer no reread");
    check(stats().resident == 0, "ledger drains without retaining resources");
    for (auto* resource : {&previous, &immediate, &unobserved, &other_caller, &reused, &sync}) {
        event(0, manager_slot, resource->ptr());
        check(resource->refs() == 0, "all remaining consumers released");
    }
    check(deleted == 8, "all resource destructors forwarded exactly once");
    std::puts("detached_DLL=PASS normal_queries=100000 parallel_queries=16000 queued=4 recovered=3 failed=1 refs_leaked=0 native_game_queue_tested=NO");

    using Gameplay = bool (*)(bool, std::uint64_t);
    using GameplayStats = bool (*)(dstorage_guard::GameplayRetryStats*, std::size_t);
    const auto gameplay = symbol<Gameplay>(dll, "dstorage_file_lifetime_guard_test_gameplay_retry");
    const auto gameplay_stats = symbol<GameplayStats>(dll, "dstorage_file_lifetime_guard_get_gameplay_retry_stats");
    check(loading(false) && gameplay(true, 1000), "gameplay recovery independent of loading");
    const auto queued_before = enqueued.load();
    std::array<Resource, 9> backlog;
    for (auto& resource : backlog) { event(1, manager_slot, resource.ptr()); }
    check(enqueued == queued_before, "gameplay terminal records without immediate worker retry");
    for (auto& reader : readers) {
        reader = std::thread([&] {
            for (unsigned round = 0; round < 100; ++round) {
                for (auto& resource : backlog) { event(2, manager_slot, resource.ptr()); }
            }
        });
    }
    for (auto& reader : readers) { reader.join(); }
    check(enqueued == queued_before + 4, "actual DLL gameplay cap four");
    dstorage_guard::GameplayRetryStats runtime{};
    check(gameplay_stats(&runtime, sizeof(runtime)) && runtime.inflight == 4, "actual native adapter reservations");
    drain(true);
    for (auto& resource : backlog) { event(2, manager_slot, resource.ptr()); }
    drain(true);
    for (auto& resource : backlog) { event(2, manager_slot, resource.ptr()); }
    drain(true);
    check(enqueued == queued_before + 9, "nine textures recover without advancing fake time");
    for (auto& resource : backlog) {
        check(resource.refs() == 1 && original_ready(resource.ptr()), "backlog ready natively");
        event(0, manager_slot, resource.ptr());
    }
    Resource repeated;
    event(1, manager_slot, repeated.ptr());
    std::uint64_t time = 1000;
    for (const std::uint64_t delay : {250ULL, 1000ULL, 5000ULL, 30000ULL, 30000ULL}) {
        const auto before = enqueued.load();
        event(2, manager_slot, repeated.ptr());
        check(enqueued == before + 1, "native adapter retries after previous failure");
        drain(false);
        for (unsigned i = 0; i < 1000; ++i) { event(2, manager_slot, repeated.ptr()); }
        check(enqueued == before + 1 && repeated.refs() == 1, "native adapter cooldown avoids retry storm");
        time += delay;
        gameplay(true, time);
    }
    event(2, manager_slot, repeated.ptr());
    check(loading(true), "scene transition with a gameplay retry in flight");
    drain(true);
    check(repeated.refs() == 1 && original_ready(repeated.ptr()), "old job completes during new load");
    event(0, manager_slot, repeated.ptr());
    check(gameplay_stats(&runtime, sizeof(runtime)) && runtime.inflight == 0 && stats().resident == 0,
          "actual DLL all gameplay references and reservations drain");
    std::puts("gameplay_DLL=PASS backlog=9 max_inflight=4 repeated_failures=5 native_readiness=YES refs_leaked=0 native_game_queue_tested=NO");
    CloseHandle(field<HANDLE>(manager_slot, 0x5E0));
    return 0;
}
