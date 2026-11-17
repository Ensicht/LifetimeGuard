#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "NearAddressReservation.h"

extern "C" bool synthetic_task_ready(void* task);
extern "C" void synthetic_texture_finalizer();
extern "C" void synthetic_resource_parser();
extern "C" bool synthetic_ready_getter(void* resource);
extern "C" void synthetic_task_submit_anchor();
extern "C" void synthetic_worker_anchor();
extern "C" bool synthetic_task_dispatch(void* task);
extern "C" unsigned synthetic_submit_caller();

namespace {

alignas(4096) __attribute__((section(".taskvt"))) void* task_vtable[13] = {
    reinterpret_cast<void*>(&synthetic_resource_parser), reinterpret_cast<void*>(&synthetic_resource_parser),
    reinterpret_cast<void*>(&synthetic_resource_parser), reinterpret_cast<void*>(&synthetic_resource_parser),
    reinterpret_cast<void*>(&synthetic_resource_parser), reinterpret_cast<void*>(&synthetic_resource_parser),
    reinterpret_cast<void*>(&synthetic_resource_parser), reinterpret_cast<void*>(&synthetic_task_ready),
    reinterpret_cast<void*>(&synthetic_task_submit_anchor), reinterpret_cast<void*>(&synthetic_resource_parser),
    reinterpret_cast<void*>(&synthetic_resource_parser), reinterpret_cast<void*>(&synthetic_resource_parser),
    reinterpret_cast<void*>(&synthetic_resource_parser)};
alignas(16) void* second_task_vtable[13]{};
bool virtual_route{};
using SubmitFn = unsigned (*)();
SubmitFn original_submit{};
unsigned foreign_submit_calls{};

unsigned foreign_submit_hook() {
    ++foreign_submit_calls;
    return original_submit() + 1;
}

bool call_task_ready(void* task) {
    if (!virtual_route) { return synthetic_task_ready(task); }
    auto* table = static_cast<const void*>(task_vtable);
    std::memcpy(task, &table, sizeof(table));
    return synthetic_task_dispatch(task);
}

bool mutate_bytes(void* target, const void* value, std::size_t size) {
    DWORD old{};
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &old)) { return false; }
    std::memcpy(target, value, size);
    FlushInstructionCache(GetCurrentProcess(), target, size);
    DWORD ignored{};
    return VirtualProtect(target, size, old, &ignored) != FALSE;
}

using EnableRecoveryFn = bool (*)();
using InitializeFn = bool (*)(const void*);
using SetLoadingStateFn = bool (*)(bool);

constexpr std::uint64_t k_benchmark_iterations = 2000000;

alignas(16) void* synthetic_texture_vtable[12] = {
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr,
    nullptr,
    reinterpret_cast<void*>(&synthetic_resource_parser),
    reinterpret_cast<void*>(&synthetic_ready_getter),
    reinterpret_cast<void*>(&synthetic_texture_finalizer),
};

struct CaseOptions {
    const char* name{};
    std::uint64_t expected{0x20000};
    std::uint64_t completed{0x1F000};
    bool callback_seen{true};
    bool resource_path_present{true};
    bool inline_resource_path{true};
    bool path_back_reference{true};
    bool texture{true};
    bool attached{true};
    bool ready{};
    bool parsed{true};
    bool parsed_readable{true};
    bool runtime_present{};
    bool expected_result{};
    bool expect_finalize{};
};

void write_pointer(std::uint8_t* bytes, std::size_t offset, void* value) {
    std::memcpy(bytes + offset, &value, sizeof(value));
}

void* read_pointer(const std::uint8_t* bytes, std::size_t offset) {
    void* value{};
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

void write_u64(
    std::uint8_t* bytes, std::size_t offset, std::uint64_t value) {
    std::memcpy(bytes + offset, &value, sizeof(value));
}

bool run_case(const CaseOptions& options) {
    alignas(16) std::array<std::uint8_t, 0x138> task{};
    alignas(16) std::array<std::uint8_t, 0x100> resource{};
    alignas(16) std::array<std::uint8_t, 0x20> parsed_texture{};
    alignas(16) std::array<std::uint8_t, 0x100> external_storage{};
    std::uint64_t runtime_texture_token{0xC0DEC0DE};

    auto* resource_path = options.inline_resource_path
        ? static_cast<void*>(resource.data() + 0xE0)
        : static_cast<void*>(external_storage.data() + 0xE0);
    if (!options.resource_path_present) {
        resource_path = nullptr;
    }

    if (options.texture) {
        write_pointer(resource.data(), 0, synthetic_texture_vtable);
    }
    write_pointer(
        resource.data(), 0x08,
        options.path_back_reference ? resource_path : nullptr);
    write_pointer(
        resource.data(), 0x50,
        options.attached ? static_cast<void*>(task.data()) : nullptr);
    resource[0x38] = options.ready ? 1 : 0;
    write_pointer(
        resource.data(), 0x70,
        !options.parsed ? nullptr :
            (options.parsed_readable
                ? static_cast<void*>(parsed_texture.data())
                : reinterpret_cast<void*>(0x10000)));
    write_pointer(
        resource.data(), 0x78,
        options.runtime_present
            ? static_cast<void*>(&runtime_texture_token)
            : nullptr);

    write_pointer(task.data(), 0x118, resource_path);
    task[0x120] = options.callback_seen ? 1 : 0;
    write_u64(task.data(), 0x128, options.expected);
    write_u64(task.data(), 0x130, options.completed);

    const auto result = call_task_ready(task.data());
    bool finalized{};
    if (options.expect_finalize && result) {
        write_pointer(resource.data(), 0x78, &runtime_texture_token);
        resource[0x38] = 1;
        finalized = read_pointer(resource.data(), 0x78) ==
                &runtime_texture_token &&
            resource[0x38] != 0;
    }

    const auto passed = result == options.expected_result &&
        finalized == options.expect_finalize;
    std::printf(
        "case=%s result=%d expected=%d finalized=%d expected_finalize=%d\n",
        options.name, result, options.expected_result,
        finalized, options.expect_finalize);
    return passed;
}

bool run_unreadable_resource_case() {
    alignas(16) std::array<std::uint8_t, 0x138> task{};
    write_pointer(task.data(), 0x118, reinterpret_cast<void*>(0x1000));
    task[0x120] = 1;
    write_u64(task.data(), 0x128, 0x20000);
    write_u64(task.data(), 0x130, 0);
    const auto result = call_task_ready(task.data());
    std::printf("case=unreadable_resource result=%d expected=0\n", result);
    return !result;
}

bool run_cross_page_resource_case() {
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const auto page_size = static_cast<std::size_t>(system_info.dwPageSize);
    auto* pages = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, page_size * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (pages == nullptr) {
        std::printf("FAIL: VirtualAlloc error=%lu\n", GetLastError());
        return false;
    }
    DWORD previous{};
    if (!VirtualProtect(
            pages + page_size, page_size, PAGE_NOACCESS, &previous)) {
        std::printf("FAIL: VirtualProtect error=%lu\n", GetLastError());
        VirtualFree(pages, 0, MEM_RELEASE);
        return false;
    }

    alignas(16) std::array<std::uint8_t, 0x138> task{};
    write_pointer(task.data(), 0x118, pages + page_size);
    task[0x120] = 1;
    write_u64(task.data(), 0x128, 0x20000);
    write_u64(task.data(), 0x130, 0);
    const auto result = call_task_ready(task.data());
    VirtualFree(pages, 0, MEM_RELEASE);
    std::printf(
        "case=cross_page_resource result=%d expected=0\n", result);
    return !result;
}

double benchmark_native_complete(
    const char* name, std::array<std::uint8_t, 0x138>& task) {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    std::uint64_t true_count{};
    for (std::uint64_t index = 0; index < k_benchmark_iterations; ++index) {
        true_count += call_task_ready(task.data()) ? 1U : 0U;
    }
    QueryPerformanceCounter(&end);
    const auto elapsed = static_cast<double>(end.QuadPart - start.QuadPart);
    const auto nanoseconds_per_call = elapsed * 1000000000.0 /
        static_cast<double>(frequency.QuadPart) /
        static_cast<double>(k_benchmark_iterations);
    std::printf(
        "benchmark=%s calls=%llu true=%llu ns_per_call=%.3f\n",
        name,
        static_cast<unsigned long long>(k_benchmark_iterations),
        static_cast<unsigned long long>(true_count), nanoseconds_per_call);
    return true_count == k_benchmark_iterations
        ? nanoseconds_per_call
        : -1.0;
}

double benchmark_native_incomplete(
    const char* name, std::array<std::uint8_t, 0x138>& task) {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    std::uint64_t true_count{};
    for (std::uint64_t index = 0; index < k_benchmark_iterations; ++index) {
        true_count += call_task_ready(task.data()) ? 1U : 0U;
    }
    QueryPerformanceCounter(&end);
    const auto elapsed = static_cast<double>(end.QuadPart - start.QuadPart);
    const auto nanoseconds_per_call = elapsed * 1000000000.0 /
        static_cast<double>(frequency.QuadPart) /
        static_cast<double>(k_benchmark_iterations);
    std::printf(
        "benchmark=%s calls=%llu true=%llu ns_per_call=%.3f\n", name,
        static_cast<unsigned long long>(k_benchmark_iterations),
        static_cast<unsigned long long>(true_count), nanoseconds_per_call);
    return true_count == 0 ? nanoseconds_per_call : -1.0;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    DWORD table_old_protection{};
    if (!VirtualProtect(task_vtable, sizeof(task_vtable), PAGE_READONLY, &table_old_protection)) { return 5; }
    const bool fallback = argc == 2 && std::strcmp(argv[1], "--vtable-fallback") == 0;
    const bool conflict = argc == 2 && std::strcmp(argv[1], "--vtable-conflict") == 0;
    const bool ambiguous = argc == 2 && std::strcmp(argv[1], "--vtable-ambiguous") == 0;
    const bool bad_worker = argc == 2 && std::strcmp(argv[1], "--vtable-missing-worker") == 0;
    const bool bad_submit = argc == 2 && std::strcmp(argv[1], "--vtable-missing-submit") == 0;
    const bool prehook = argc == 2 && std::strncmp(argv[1], "--vtable-prehook", 15) == 0;
    const bool prehook_ok = prehook && std::strcmp(argv[1], "--vtable-prehook") == 0;
    const bool bad_body = prehook && std::strcmp(argv[1], "--vtable-prehook-bad-body") == 0;
    const bool bad_jump = prehook && std::strcmp(argv[1], "--vtable-prehook-bad-jump") == 0;
    const bool self_jump = prehook && std::strcmp(argv[1], "--vtable-prehook-self-jump") == 0;
    if (argc != 1 && !fallback && !conflict && !ambiguous && !bad_worker && !bad_submit &&
        !prehook_ok && !bad_body && !bad_jump && !self_jump) { return 2; }
    virtual_route = argc == 2;
    alignas(16) std::array<std::uint8_t, 0x138> benchmark_task{};
    benchmark_task[0x120] = 1;
    write_u64(benchmark_task.data(), 0x128, 0x20000);
    write_u64(benchmark_task.data(), 0x130, 0x20000);
    const auto baseline_ns = benchmark_native_complete(
        "native_complete_unhooked", benchmark_task);
    alignas(16) std::array<std::uint8_t, 0x138> incomplete_task{};
    const auto incomplete_baseline_ns = benchmark_native_incomplete(
        "native_incomplete_unhooked", incomplete_task);

    const auto guard = LoadLibraryW(L"DStorageFileLifetimeGuard.dll");
    if (guard == nullptr) {
        std::printf("FAIL: LoadLibrary error=%lu\n", GetLastError());
        return 2;
    }
    const auto enable = reinterpret_cast<EnableRecoveryFn>(
        GetProcAddress(
            guard,
            "dstorage_file_lifetime_guard_enable_resource_recovery"));
    const auto initialize = reinterpret_cast<InitializeFn>(
        GetProcAddress(guard, "reframework_plugin_initialize"));
    const auto set_loading_state = reinterpret_cast<SetLoadingStateFn>(
        GetProcAddress(
            guard,
            "dstorage_file_lifetime_guard_test_set_loading_state"));
    if (enable == nullptr || initialize == nullptr ||
        set_loading_state == nullptr) {
        std::printf("FAIL: missing guard exports\n");
        return 3;
    }
    initialize(nullptr);
    std::uint8_t submit_before[32]{};
    if (prehook) {
        auto* submit = reinterpret_cast<void*>(&synthetic_task_submit_anchor);
        void* trampoline{};
        if (MH_Initialize() != MH_OK ||
            MH_CreateHook(submit, reinterpret_cast<void*>(&foreign_submit_hook), &trampoline) != MH_OK) { return 9; }
        original_submit = reinterpret_cast<SubmitFn>(trampoline);
        if (MH_EnableHook(submit) != MH_OK || synthetic_submit_caller() != 0x45 || foreign_submit_calls != 1) { return 9; }
        std::memcpy(submit_before, submit, sizeof(submit_before));
        if (submit_before[0] != 0xE9) { return 9; }
        std::printf("preexisting_submit_hook=1 calls_original=1 table_submit_unchanged=%d\n",
            task_vtable[8] == submit);
        if (bad_body) {
            const std::uint8_t nop = 0x90;
            if (!mutate_bytes(static_cast<std::uint8_t*>(submit) + 12, &nop, 1)) { return 9; }
        }
        if (bad_jump || self_jump) {
            const auto target = self_jump ? reinterpret_cast<std::intptr_t>(submit) :
                reinterpret_cast<std::intptr_t>(task_vtable);
            const auto distance = target - reinterpret_cast<std::intptr_t>(submit) - 5;
            if (distance < INT32_MIN || distance > INT32_MAX) { return 9; }
            const auto relative = static_cast<std::int32_t>(distance);
            if (!mutate_bytes(static_cast<std::uint8_t*>(submit) + 1, &relative, sizeof(relative))) { return 9; }
        }
    }
    NearAddressReservation occupied;
    if (virtual_route && !occupied.fill(reinterpret_cast<void*>(&synthetic_task_ready))) { return 5; }
    void* foreign = reinterpret_cast<void*>(&synthetic_resource_parser);
    if (conflict && !mutate_bytes(const_cast<void**>(task_vtable + 7), &foreign, sizeof(foreign))) { return 5; }
    if (conflict && *static_cast<void* const volatile*>(task_vtable + 7) != foreign) { return 8; }
    if (ambiguous) { std::memcpy(second_task_vtable, task_vtable, sizeof(task_vtable)); }
    const std::uint8_t changed = 0x90;
    if (bad_worker && !mutate_bytes(reinterpret_cast<void*>(&synthetic_worker_anchor), &changed, 1)) { return 5; }
    if (bad_submit && !mutate_bytes(reinterpret_cast<void*>(&synthetic_task_submit_anchor), &changed, 1)) { return 5; }
    std::uint8_t code_before[38]{};
    std::memcpy(code_before, reinterpret_cast<void*>(&synthetic_task_ready), sizeof(code_before));
    MEMORY_BASIC_INFORMATION before{};
    VirtualQuery(task_vtable, &before, sizeof(before));
    if (virtual_route && !fallback && !prehook_ok) {
        const bool installed = enable();
        MEMORY_BASIC_INFORMATION after{};
        VirtualQuery(task_vtable, &after, sizeof(after));
        void* observed = *static_cast<void* const volatile*>(task_vtable + 7);
        const bool rejected = !installed && before.Protect == after.Protect &&
            observed == (conflict ? foreign : reinterpret_cast<void*>(&synthetic_task_ready)) &&
            std::memcmp(code_before, reinterpret_cast<void*>(&synthetic_task_ready), sizeof(code_before)) == 0;
        std::printf("vtable_negative=%s rejected=%d\n", argv[1], rejected);
        Sleep(250);
        return rejected ? 0 : 6;
    }
    if (!set_loading_state(true) || !enable()) {
        std::printf("FAIL: recovery hook installation\n");
        return 3;
    }
    if (fallback || prehook_ok) {
        MEMORY_BASIC_INFORMATION after{};
        VirtualQuery(task_vtable, &after, sizeof(after));
        void* observed = *static_cast<void* const volatile*>(task_vtable + 7);
        if (before.Protect != after.Protect || observed == reinterpret_cast<void*>(&synthetic_task_ready) ||
            std::memcmp(code_before, reinterpret_cast<void*>(&synthetic_task_ready), sizeof(code_before)) != 0) { return 7; }
        std::printf("vtable_fallback_active=1 code_unchanged=1 protection_restored=1\n");
    }
    if (prehook_ok) {
        auto* submit = reinterpret_cast<void*>(&synthetic_task_submit_anchor);
        if (std::memcmp(submit_before, submit, sizeof(submit_before)) != 0 ||
            *static_cast<void* const volatile*>(task_vtable + 8) != submit ||
            synthetic_submit_caller() != 0x45 || foreign_submit_calls != 2) { return 10; }
        std::printf("foreign_submit_preserved=1 original_and_detour_still_callable=1\n");
    }
    const auto hooked_ns = benchmark_native_complete(
        "native_complete_hooked", benchmark_task);

    bool passed = baseline_ns >= 0.0 && hooked_ns >= 0.0 &&
        incomplete_baseline_ns >= 0.0;
    passed &= run_case(CaseOptions{
        .name = "native_complete",
        .completed = 0x20000,
        .expected_result = true,
    });
    passed &= run_case(CaseOptions{
        .name = "short_texture",
        .expected_result = true,
        .expect_finalize = true,
    });
    passed &= run_case(CaseOptions{
        .name = "zero_byte_terminal_texture",
        .expected = 22372608,
        .completed = 0,
        .expected_result = true,
        .expect_finalize = true,
    });
    passed &= run_case(CaseOptions{
        .name = "completed_exceeds_expected",
        .completed = 0x20001,
    });
    passed &= run_case(CaseOptions{
        .name = "callback_pending",
        .callback_seen = false,
    });
    passed &= run_case(CaseOptions{
        .name = "missing_resource_path",
        .resource_path_present = false,
    });
    passed &= run_case(CaseOptions{
        .name = "external_resource_path",
        .inline_resource_path = false,
    });
    passed &= run_case(CaseOptions{
        .name = "path_back_reference_mismatch",
        .path_back_reference = false,
    });
    passed &= run_case(CaseOptions{
        .name = "non_texture",
        .texture = false,
    });
    passed &= run_case(CaseOptions{
        .name = "resource_detached",
        .attached = false,
    });
    passed &= run_case(CaseOptions{
        .name = "resource_already_ready",
        .ready = true,
    });
    passed &= run_case(CaseOptions{
        .name = "parse_missing",
        .parsed = false,
    });
    passed &= run_case(CaseOptions{
        .name = "parse_unreadable",
        .parsed_readable = false,
    });
    passed &= run_case(CaseOptions{
        .name = "runtime_already_present",
        .runtime_present = true,
    });
    passed &= run_unreadable_resource_case();
    passed &= run_cross_page_resource_case();
    passed &= set_loading_state(false);
    const auto incomplete_bypass_ns = benchmark_native_incomplete(
        "native_incomplete_loading_gate_off", incomplete_task);
    passed &= incomplete_bypass_ns >= 0.0;
    passed &= run_case(CaseOptions{
        .name = "short_texture_bypassed_outside_loading",
    });
    passed &= set_loading_state(true);

    std::printf(
        "native_complete_overhead_ns=%.3f ratio=%.3f "
        "loading_off_incomplete_overhead_ns=%.3f ratio=%.3f\n",
        hooked_ns - baseline_ns,
        baseline_ns > 0.0 ? hooked_ns / baseline_ns : 0.0,
        incomplete_bypass_ns - incomplete_baseline_ns,
        incomplete_baseline_ns > 0.0
            ? incomplete_bypass_ns / incomplete_baseline_ns
            : 0.0);
    std::printf("resource_recovery_hook_harness=%s\n",
        passed ? "PASS" : "FAIL");
    Sleep(250);
    return passed ? 0 : 4;
}
