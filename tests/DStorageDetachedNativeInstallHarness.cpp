#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../src/DetachedTextureRetry.h"
#include "../src/TextureRetryNativeTargets.h"
#include "../src/NativeTextureProfile.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
using Release = void (*)(void*, void*);
using Ready = bool (*)(void*);
using Destroy = void* (*)(void*, std::uint32_t);
using Install = bool (*)(const dstorage_guard::TextureRetryNativeTargets*, std::size_t);
using Stats = bool (*)(dstorage_guard::TextureRetryStats*, std::size_t, std::uint32_t*);
using Loading = bool (*)(bool);
using Detours = bool (*)(void**, void**, void**);
std::array<void*, 12> table{};
alignas(16) std::array<std::uint8_t, 0x2000> manager{};
void* manager_slot = manager.data();
std::atomic<unsigned> releases{}, deletes{}, reads{};
void* read_job{};
void* doomed_target{};
Release worker_release{};

void check(bool result, const char* label) {
    if (!result) { std::fprintf(stderr, "FAIL %s error=%lu\n", label, GetLastError()); std::exit(1); }
}
template<class T> T& field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
}
struct alignas(16) Resource {
    std::array<std::uint8_t, 0xE8> bytes{};
    explicit Resource(unsigned refs = 2) {
        field<void*>(ptr(), 0) = table.data();
        field<unsigned>(ptr(), 0x28) = refs;
        field<std::uint64_t>(ptr(), 0x30) = 0x1234567890ABCDEF;
        field<std::uintptr_t>(ptr(), 0x70) = 0x100000;
    }
    void* ptr() { return bytes.data(); }
    unsigned refs() { return field<unsigned>(ptr(), 0x28); }
};
__declspec(noinline) bool original_ready(void* object) { return field<std::uint8_t>(object, 0x38) != 0; }
__declspec(noinline) void* original_delete(void* object, std::uint32_t flags) {
    check(flags == 7, "native delete argument forwarded");
    deletes.fetch_add(1);
    return object;
}
__declspec(noinline) void original_release(void* native_manager, void* object) {
    check(native_manager == manager_slot, "native release manager");
    releases.fetch_add(1);
    if (InterlockedDecrement(&field<LONG>(object, 0x28)) == 0) {
        auto** object_table = static_cast<void**>(field<void*>(object, 0));
        reinterpret_cast<Destroy>(object_table[6])(object, 7);
    }
}
void read_queue(void* native_manager, void* object) {
    check(native_manager == manager_slot && read_job == nullptr, "one native read at a time");
    check(field<void*>(object, 0x50) == nullptr, "old native task gone");
    check(field<unsigned>(object, 0x28) >= 2, "native read owns reference");
    reads.fetch_add(1);
    read_job = object;
    field<void*>(object, 0x50) = reinterpret_cast<void*>(0xABC0);
}
bool foreign_ready(void*) { return true; }
void* foreign_delete(void* object, std::uint32_t) { return object; }

void* executable(const std::uint8_t* code, std::size_t size) {
    auto* memory = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check(memory != nullptr, "fixture allocation");
    std::memcpy(memory, code, size);
    DWORD previous{};
    check(VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &previous), "fixture executable");
    FlushInstructionCache(GetCurrentProcess(), memory, size);
    return memory;
}
void remove_target_before_enable() {
    check(VirtualFree(doomed_target, 0, MEM_RELEASE), "force real MH_EnableHook protection failure");
}
void conflict_target_before_enable() {
    DWORD previous{}, ignored{};
    check(VirtualProtect(doomed_target, 16, PAGE_EXECUTE_READWRITE, &previous), "foreign hook fixture writable");
    *static_cast<std::uint8_t*>(doomed_target) = 0x90;
    VirtualProtect(doomed_target, 16, previous, &ignored);
}
void finish(bool success) {
    auto* object = read_job;
    check(object != nullptr, "queued native job");
    read_job = nullptr;
    field<void*>(object, 0x50) = nullptr;
    field<std::uintptr_t>(object, 0x70) += 256;
    field<std::uintptr_t>(object, 0x78) = success ? 0x987600 : 0;
    field<std::uint8_t>(object, 0x38) = success ? 1 : 0;
    worker_release(manager_slot, object);
}
template<class T> T symbol(HMODULE dll, const char* name) {
    const auto address = GetProcAddress(dll, name);
    check(address != nullptr, name);
    return reinterpret_cast<T>(address);
}
double benchmark(Ready function, void* object) {
    LARGE_INTEGER frequency{}, start{}, end{};
    QueryPerformanceFrequency(&frequency);
    unsigned sum{};
    Ready volatile entry = function;
    QueryPerformanceCounter(&start);
    for (unsigned i = 0; i < 2000000; ++i) { sum += entry(object); }
    QueryPerformanceCounter(&end);
    check(sum == 2000000, "native ready result preserved in benchmark");
    return static_cast<double>(end.QuadPart - start.QuadPart) * 1e9 /
        static_cast<double>(frequency.QuadPart) / 2000000.0;
}

void profile_test(HMODULE dll, const char* path) {
    const auto validate = symbol<bool (*)(void*)>(dll, "dstorage_file_lifetime_guard_test_retry_profile");
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    check(file != INVALID_HANDLE_VALUE, "open native image read-only");
    // 映射保留 PE 段权限以测试 VirtualQuery；不调用任何映射代码或游戏初始化入口。
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE, 0, 0, nullptr);
    check(mapping != nullptr, "map native image without invoking game code");
    auto* image = static_cast<std::uint8_t*>(MapViewOfFileEx(mapping, FILE_MAP_READ, 0, 0, 0,
        reinterpret_cast<void*>(0x140000000ULL)));
    check(image != nullptr, "native preferred image base available in offline host");
    check(validate(image), "actual DLL accepts exact current native image");
    const auto* profile = dstorage_guard::find_native_texture_profile(image);
    check(profile != nullptr, "exact native profile selected by structure");
    const auto texture_table = symbol<void* (*)(void*, void*)>(dll, "dstorage_file_lifetime_guard_test_texture_vtable");
    check(texture_table(image, image + profile->finalize) == image + profile->vtable,
          "actual DLL resolves texture type despite shared finalizer");
    const std::uintptr_t negative_offsets[]{
        profile->queue, profile->release, profile->destroy, profile->ready,
        profile->clear_task[0], profile->clear_task[1],
        profile->work_dec[0], profile->work_dec[1],
        profile->read_inc[0], profile->read_inc[1], profile->release_ref,
        profile->terminal_calls[0], profile->terminal_calls[1],
        profile->terminal_calls[2], profile->terminal_calls[3],
        profile->terminal_calls[0] - 7, profile->terminal_calls[1] - 7,
        profile->terminal_calls[2] - 7, profile->terminal_calls[3] - 7, profile->vtable + 0x30,
        profile->vtable + 0x48, profile->vtable + 0x50, profile->vtable + 0x58};
    for (auto offset : negative_offsets) {
        DWORD previous{}, ignored{};
        check(VirtualProtect(image + offset, 1, PAGE_WRITECOPY, &previous), "private profile fault page");
        image[offset] ^= 1U;
        check(!validate(image), "actual DLL rejects modified native code or foreign vtable entry");
        image[offset] ^= 1U;
        check(VirtualProtect(image + offset, 1, previous, &ignored), "restore image page");
        check(validate(image), "profile returns to valid after private fault removed");
    }
    for (auto slot : {6, 9, 10, 11}) {
        DWORD previous{}, ignored{};
        auto* entry = image + profile->vtable + slot * sizeof(void*);
        check(VirtualProtect(entry, sizeof(void*), PAGE_WRITECOPY, &previous), "private completion slot fault");
        *entry ^= 1U;
        check(texture_table(image, image + profile->finalize) == nullptr,
              "completion refuses corrupt texture table instead of derived table");
        *entry ^= 1U;
        check(VirtualProtect(entry, sizeof(void*), previous, &ignored), "restore completion slot");
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    DWORD previous{}, ignored{};
    check(VirtualProtect(&nt->FileHeader.TimeDateStamp, 4, PAGE_WRITECOPY, &previous), "private timestamp fault");
    nt->FileHeader.TimeDateStamp ^= 1U;
    check(validate(image), "unchanged native ownership survives metadata-only revision");
    nt->FileHeader.TimeDateStamp ^= 1U;
    check(VirtualProtect(&nt->FileHeader.TimeDateStamp, 4, previous, &ignored), "restore timestamp page");
    const auto image_size = nt->OptionalHeader.SizeOfImage;
    check(VirtualProtect(&nt->OptionalHeader.SizeOfImage, 4, PAGE_WRITECOPY, &previous), "private image size metadata");
    nt->OptionalHeader.SizeOfImage += 0x1000;
    check(validate(image), "larger image metadata alone remains compatible");
    nt->OptionalHeader.SizeOfImage = 0x1000;
    check(!validate(image), "required native addresses outside image are rejected");
    nt->OptionalHeader.SizeOfImage = image_size;
    check(VirtualProtect(&nt->OptionalHeader.SizeOfImage, 4, previous, &ignored), "restore image size");
    check(validate(image), "final native profile valid");
    if (profile->destructor_vtable) {
        check(VirtualProtect(image + profile->destructor_vtable, 10, PAGE_WRITECOPY, &previous), "private destructor proof fault");
        image[profile->destructor_vtable + 3] ^= 1U;
        check(!validate(image), "changed destructor ownership is rejected");
        image[profile->destructor_vtable + 3] ^= 1U;
        check(VirtualProtect(image + profile->destructor_vtable, 10, previous, &ignored), "restore destructor proof");
        check(validate(image), "destructor ownership restored");
    }
    UnmapViewOfFile(image);
    CloseHandle(mapping);
    CloseHandle(file);
    std::printf("native_retry_profile=PASS current_image=1 code_fault_cases=%zu completion_slot_faults=4 metadata_only=accepted game_code_executed=NO\n",
                sizeof(negative_offsets) / sizeof(negative_offsets[0]));
}
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "success";
    const bool far_mode = std::strncmp(mode, "far-", 4) == 0;
    if (far_mode) mode += 4;
    const auto dll = LoadLibraryW(far_mode ? L"DStorageFileLifetimeGuardNoNearTest.dll" : L"DStorageFileLifetimeGuard.dll");
    check(dll != nullptr, "load actual production DLL");
    const auto init = symbol<bool (*)(const void*)>(dll, "reframework_plugin_initialize");
    const auto install = symbol<Install>(dll, "dstorage_file_lifetime_guard_test_install_texture_retry");
    const auto loading = symbol<Loading>(dll, "dstorage_file_lifetime_guard_test_set_loading_state");
    const auto get_stats = symbol<Stats>(dll, "dstorage_file_lifetime_guard_get_texture_retry_stats");
    const auto detours = symbol<Detours>(dll, "dstorage_file_lifetime_guard_test_retry_detours");
    check(!install(nullptr, 0), "fixture installer unavailable before initialization");
    init(nullptr);
    loading(false);
    if (std::strcmp(mode, "profile") == 0) {
        check(argc == 3, "profile mode native image argument");
        profile_test(dll, argv[2]);
        return 0;
    }
    table[6] = reinterpret_cast<void*>(&original_delete);
    table[10] = reinterpret_cast<void*>(&original_ready);
    field<HANDLE>(manager_slot, 0x5E0) = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(field<HANDLE>(manager_slot, 0x5E0) != nullptr, "fixture native queue event");
    dstorage_guard::TextureRetryNativeTargets targets{
        table.data(), &manager_slot, reinterpret_cast<void*>(&original_release),
        reinterpret_cast<void*>(&read_queue), table[6], table[10], {1, 2, 3, 4}};
    if (far_mode) {
        std::uint8_t release_code[]{
            0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x55,0x53,0x48,0x83,0xEC,0x48,
            0x48,0x8B,0x05,0,0,0,0,
            0x48,0x83,0xC4,0x48,0x5B,0x5D,0x5F,0x5E,0x41,0x5C,0x41,0x5D,0x41,0x5E,0x41,0x5F,
            0xFF,0x25,0,0,0,0,0,0,0,0,0,0,0,0};
        const auto next = reinterpret_cast<void*>(&original_release);
        std::memcpy(release_code + 45, &next, sizeof(next));
        if (std::strcmp(mode,"prefix-conflict")==0) release_code[0]=0x90;
        targets.release_target = executable(release_code, sizeof(release_code));
        if (std::strcmp(mode,"enable-conflict")==0) {
            doomed_target=targets.release_target;
            targets.offline_before_enable=&conflict_target_before_enable;
        }
    }
    if (std::strcmp(mode, "delete-conflict") == 0) { table[6] = reinterpret_cast<void*>(&foreign_delete); }
    if (std::strcmp(mode, "ready-conflict") == 0) { table[10] = reinterpret_cast<void*>(&foreign_ready); }
    if (std::strcmp(mode, "create-fail") == 0) {
        targets.release_target = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    if (std::strcmp(mode, "protect-fail") == 0) {
        targets.texture_vtable = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS));
    }
    if (std::strcmp(mode, "enable-fail") == 0) {
        constexpr std::uint8_t code[]{0xC3,0x90,0x90,0x90,0x90,0x90,0x90,0x90};
        doomed_target = executable(code, sizeof(code));
        targets.release_target = doomed_target;
        targets.offline_before_enable = &remove_target_before_enable;
    }
    // A genuine indirect CALL gives the release hook a known return address,
    // just like the four separately validated game worker exits.
    std::uint8_t worker[]{0x48,0x83,0xEC,0x28,0x48,0xB8,0,0,0,0,0,0,0,0,0xFF,0xD0,
        0x48,0x83,0xC4,0x28,0xC3};
    std::memcpy(worker + 6, &targets.release_target, sizeof(void*));
    auto* worker_code = static_cast<std::uint8_t*>(executable(worker, sizeof(worker)));
    worker_release = reinterpret_cast<Release>(worker_code);
    targets.terminal_callers[0] = reinterpret_cast<std::uintptr_t>(worker_code + 16);
    const auto installed = install(&targets, sizeof(targets));
    dstorage_guard::TextureRetryStats stats{};
    std::uint32_t state{};
    check(get_stats(&stats, sizeof(stats), &state), "installation state");
    if (std::strcmp(mode, "success") != 0) {
        const bool create_failed = std::strcmp(mode,"create-fail")==0 || std::strcmp(mode,"prefix-conflict")==0;
        check(!installed && state == (create_failed ? 5U : 6U), "real installer rejects fault at intended stage");
        check(table[6] == (std::strcmp(mode, "delete-conflict") == 0 ?
            reinterpret_cast<void*>(&foreign_delete) : reinterpret_cast<void*>(&original_delete)), "delete slot rollback preserves foreign hook");
        check(table[10] == (std::strcmp(mode, "ready-conflict") == 0 ?
            reinterpret_cast<void*>(&foreign_ready) : reinterpret_cast<void*>(&original_ready)), "ready slot rollback preserves foreign hook");
        if (!create_failed) {
            void *ready{}, *release{}, *destroy{};
            check(detours(&ready, &release, &destroy), "late fetched forwarders");
            Resource resource;
            check(!reinterpret_cast<Ready>(ready)(resource.ptr()), "late ready forwarder remains valid");
            check(reinterpret_cast<Destroy>(destroy)(resource.ptr(), 7) == resource.ptr(), "late delete forwarder remains valid");
            reinterpret_cast<Release>(release)(manager_slot, resource.ptr());
        }
        check(get_stats(&stats, sizeof(stats), &state) && stats.queued == 0, "failed installer never arms recovery");
        std::printf("native_retry_install=%s%s result=PASS state=%u\n", far_mode?"far-":"", mode, state);
        return 0;
    }
    check(installed && state == 2, "actual MinHook and both vtable hooks installed");
    check(!install(&targets, sizeof(targets)), "installation cannot rebind live hooks");
    Resource old;
    worker_release(manager_slot, old.ptr());
    check(old.refs() == 1 && reads.load() == 0, "verified worker receipt outside loading");
    loading(true);
    check(!reinterpret_cast<Ready>(table[10])(old.ptr()), "patched ready returns original value");
    check(reads.load() == 1 && old.refs() == 2, "actual ready hook requeues one job");
    finish(true);
    check(old.refs() == 1 && reinterpret_cast<Ready>(table[10])(old.ptr()), "actual release hook observes successful native completion");
    Resource immediate;
    worker_release(manager_slot, immediate.ptr());
    check(reads.load() == 2 && immediate.refs() == 2, "actual release hook transfers worker reference");
    loading(false);
    finish(false);
    check(immediate.refs() == 1, "failure drains after loading ends");
    for (unsigned i = 0; i < 100000; ++i) { reinterpret_cast<Ready>(table[10])(immediate.ptr()); }
    check(reads.load() == 2, "normal play does not recover failed texture");
    Resource unrelated;
    original_release(manager_slot, unrelated.ptr());
    check(unrelated.refs() == 1 && reads.load() == 2, "unknown release callsite forwarded");
    const auto baseline = benchmark(&original_ready, old.ptr());
    const auto hooked = benchmark(reinterpret_cast<Ready>(table[10]), old.ptr());
    check(get_stats(&stats, sizeof(stats), &state) && stats.recovered == 1 && stats.failed == 1, "actual installed hook counters");
    for (auto* resource : {&old, &immediate, &unrelated}) {
        original_release(manager_slot, resource->ptr());
        check(resource->refs() == 0, "native consumer reference drained");
    }
    get_stats(&stats, sizeof(stats), &state);
    check(stats.resident == 0 && deletes.load() == 3, "actual destructor hook clears scalar receipts");
    std::printf("native_retry_install=success result=PASS ready_base_ns=%.3f ready_hook_ns=%.3f extra_ns=%.3f normal_queries=100000 queued=2 refs_leaked=0 game_queue_executed=NO\n",
        baseline, hooked, hooked - baseline);
    return 0;
}
