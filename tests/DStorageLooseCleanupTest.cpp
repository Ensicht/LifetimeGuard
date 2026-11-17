#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "LooseTextureCleanup.h"
#include <dstorage.h>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <thread>
#include <vector>

using namespace dstorage_guard;
void check(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); } }
constexpr GUID iid_file{0x5de95e7b,0x955a,0x4868,{0xa7,0x3c,0x24,0x3b,0x29,0xf4,0xb8,0xda}};
constexpr GUID iid_factory{0x6924ea0c,0xc3cd,0x4826,{0xb1,0x0a,0xf6,0x4f,0x4e,0xd9,0x27,0xc1}};
constexpr GUID iid_queue{0xcfdbd83f,0x9e06,0x4fda,{0x8e,0xa5,0x69,0x04,0x21,0x37,0xf4,0x9b}};
constexpr GUID iid_status{0x82397587,0x7cd5,0x453b,{0xa0,0x2e,0x31,0x37,0x9b,0xd6,0x46,0x56}};

struct Image {
    std::uint8_t* base{};
    Image(std::uint32_t stamp, std::uint32_t size) {
        base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS));
        check(base != nullptr, "reserve sparse fixture");
        commit(0, 4096);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x100;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + 0x100);
        nt->Signature = IMAGE_NT_SIGNATURE; nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt->FileHeader.TimeDateStamp = stamp; nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.SizeOfImage = size;
    }
    ~Image() { VirtualFree(base, 0, MEM_RELEASE); }
    void commit(std::uintptr_t rva, std::size_t n) {
        const auto start = rva & ~std::uintptr_t(4095);
        check(VirtualAlloc(base + start, ((rva - start + n + 4095) / 4096) * 4096, MEM_COMMIT, PAGE_EXECUTE_READWRITE) != nullptr, "commit fixture page");
    }
    void hex(std::uintptr_t rva, const char* text) {
        const auto n = std::strlen(text) / 2; commit(rva, n);
        for (std::size_t i = 0; i < n; ++i) { base[rva + i] = (cleanup_nibble(text[i * 2]) << 4U) | cleanup_nibble(text[i * 2 + 1]); }
    }
    void jump(std::uintptr_t rva, void* target) {
        hex(rva, "48b80000000000000000ffe0");
        std::memcpy(base + rva + 2, &target, sizeof(target));
    }
    void pointer(std::uintptr_t rva, void* value) { commit(rva, 8); std::memcpy(base + rva, &value, 8); }
};

struct Node { std::uintptr_t words[6]{}; };
std::uint64_t erased{};
void* unlink_node(void*, void* node) { return node; }
void free_node(void*) { ++erased; }
const wchar_t* routed_wcsstr(const wchar_t* path, const wchar_t*) {
    return std::wcsstr(path, L".sub_000.pak") || is_loose_texture_path(path, std::wcslen(path)) ? path : nullptr;
}
struct Info { void* file; wchar_t path[512]; };
using CleanupFn = void (*)(Info*);

void native_cleanup(Image& game, IDStorageFile* file, const wchar_t* path) {
    Node head{}, node{};
    head.words[1] = reinterpret_cast<std::uintptr_t>(&node);
    reinterpret_cast<std::uint8_t*>(&head)[0x19] = 1;
    node.words[0] = node.words[1] = node.words[2] = reinterpret_cast<std::uintptr_t>(&head);
    node.words[4] = node.words[5] = reinterpret_cast<std::uintptr_t>(file);
    game.pointer(0x14964478, &head);
    Info info{}; info.file = file; wcscpy_s(info.path, path);
    reinterpret_cast<CleanupFn>(game.base + 0xCF03310)(&info);
    check(node.words[5] == 0, "native eraser cleared owned pointer");
}

void create_source(const wchar_t* path) {
    auto h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(h != INVALID_HANDLE_VALUE, "create test source");
    std::array<std::uint8_t, 4096> data{};
    for (unsigned i = 0; i < data.size(); ++i) { data[i] = (i * 37U) & 255U; }
    DWORD written{}; check(WriteFile(h, data.data(), data.size(), &written, nullptr) && written == data.size(), "write test source");
    CloseHandle(h);
}

struct FakeFile {
    std::array<std::uintptr_t, 6> internal{};
    void** vt{};
    std::uintptr_t gap[3]{};
    void* file{};
    std::atomic<ULONG> refs{1};
    static ULONG add(void* p) {
        // Wrapper begins at vt, not the containing test object.
        auto* w = static_cast<std::uint8_t*>(p);
        auto* owner = reinterpret_cast<FakeFile*>(w - offsetof(FakeFile, vt));
        return ++owner->refs;
    }
    explicit FakeFile(const wchar_t* path) {
        static void* table[] = {nullptr, reinterpret_cast<void*>(&add), nullptr};
        vt = table; file = internal.data(); internal[2] = reinterpret_cast<std::uintptr_t>(path);
        internal[4] = std::wcslen(path); internal[5] = internal[4] < 8 ? 8 : internal[4];
    }
    void* wrapper() { return &vt; }
};

int wmain() {
    Image game(cleanup_game_timestamp, cleanup_game_size);
    for (const auto& block : cleanup_blocks) { game.hex(block.rva, block.hex); }
    check(validate_cleanup_image(game.base) == 0, "complete game profile");
    for (unsigned i = 0; i < sizeof(cleanup_blocks)/sizeof(cleanup_blocks[0]); ++i) {
        game.base[cleanup_blocks[i].rva] ^= 1;
        check(validate_cleanup_image(game.base) == 10 + i, "mutated ownership block rejected");
        game.base[cleanup_blocks[i].rva] ^= 1;
    }
    check(validate_cleanup_image(nullptr) == 1, "missing game rejected");
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(game.base + 0x100);
    nt->FileHeader.TimeDateStamp ^= 1;
    check(validate_cleanup_image(game.base) == 0, "ownership profile survives timestamp-only changes");
    nt->FileHeader.TimeDateStamp ^= 1;
    nt->OptionalHeader.SizeOfImage ^= 1;
    check(validate_cleanup_image(game.base) == 0, "ownership profile survives harmless image-size metadata changes");
    nt->OptionalHeader.SizeOfImage ^= 1;
    LooseTextureCleanup policy;
    policy.initialize(game.base);
    FakeFile sample(L"D:/test/source.tex.241106027");
    check(!policy.pair(reinterpret_cast<void*>(1), 9), "other callers do not read wrapper");
    check(policy.pair(sample.wrapper(), policy.caller()) && sample.refs == 2, "one paired reference");
    FakeFile short_path(L"unused");
    short_path.internal[2] = short_path.internal[3] = 0;
    const wchar_t short_name[] = L".tex.1";
    std::memcpy(short_path.internal.data() + 2, short_name, sizeof(short_name));
    short_path.internal[4] = 6; short_path.internal[5] = 7;
    check(policy.pair(short_path.wrapper(), policy.caller()) && short_path.refs == 2, "short-string path layout");
    const auto pak_dir = L"dir.tex.1/re_chunk_000.pak.sub_000.pak";
    check(!is_loose_texture_path(pak_dir, std::wcslen(pak_dir)), "pak in tex directory excluded");
    const wchar_t* cases[] = {L"source.mesh.1", L"source.tex.123.pak", L"source.tex.", L"source.tex.bad", L"dir.tex.1/other.pak"};
    for (auto p : cases) { FakeFile f(p); check(!policy.pair(f.wrapper(), policy.caller()) && f.refs == 1, "non texture excluded"); }
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i) {
        threads.emplace_back([&] { for (unsigned n = 0; n < 2500; ++n) {
            FakeFile f(L"source.tex.241106027");
            check(policy.pair(f.wrapper(), policy.caller()) && f.refs == 2, "concurrent pairing");
            check(--f.refs == 1 && --f.refs == 0, "both consumers balanced");
        }});
    }
    for (auto& t : threads) { t.join(); }
    check(policy.paired() == 20002, "concurrent counter exact");
    policy.initialize(nullptr);
    check(!policy.pair(reinterpret_cast<void*>(1), reinterpret_cast<std::uintptr_t>(game.base) + cleanup_close_return_rva), "rejected game disables pairing without reading wrapper");

    // Execute the verified native cleanup and node eraser instructions. Only
    // tree unlink/free, the critical-section imports and the REF path routing
    // are fixture stubs; the two Release calls are the actual captured code.
    game.hex(0xCF03310, "564883ec204889ce4883c108488d15eb6d6f01e8f86e2600");
    game.hex(0x1F5E7450, "56574883ec28");
    game.jump(0xD16A220, reinterpret_cast<void*>(&routed_wcsstr));
    game.jump(0xCDE3530, reinterpret_cast<void*>(&unlink_node));
    game.jump(0x1330, reinterpret_cast<void*>(&free_node));
    game.pointer(0xE620D70, reinterpret_cast<void*>(&EnterCriticalSection));
    game.pointer(0xE620F68, reinterpret_cast<void*>(&LeaveCriticalSection));
    game.commit(0x14964490, sizeof(CRITICAL_SECTION));
    auto* mutex = reinterpret_cast<CRITICAL_SECTION*>(game.base + 0x14964490);
    InitializeCriticalSection(mutex);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

    IDStorageFactory* factory{};
    check(SUCCEEDED(DStorageGetFactory(iid_factory, reinterpret_cast<void**>(&factory))), "real DS factory");
    auto dll = LoadLibraryW(L"DStorageFileLifetimeGuard.dll"); check(dll != nullptr, "production DLL load");
    auto init = reinterpret_cast<bool (*)(const void*)>(GetProcAddress(dll, "reframework_plugin_initialize"));
    auto bind = reinterpret_cast<bool (*)(void*,void*)>(GetProcAddress(dll, "dstorage_file_lifetime_guard_test_cleanup_bind"));
    auto pairs = reinterpret_cast<std::uint64_t (*)()>(GetProcAddress(dll, "dstorage_file_lifetime_guard_cleanup_pairs"));
    auto loading = reinterpret_cast<bool (*)(bool)>(GetProcAddress(dll, "dstorage_file_lifetime_guard_test_set_loading_state"));
    check(init && bind && pairs && loading, "test exports found");
    check(init(nullptr) && bind(game.base, nullptr), "production Close hook needs no host image");
    Image unknown(42, 0x10000);
    check(bind(game.base, unknown.base), "unlisted REF build accepted");
    check(bind(game.base, reinterpret_cast<void*>(1)), "host pointer never dereferenced");
    check(!bind(nullptr, unknown.base), "production missing game rejected");
    for (unsigned i = 0; i < sizeof(cleanup_blocks)/sizeof(cleanup_blocks[0]); ++i) {
        game.base[cleanup_blocks[i].rva] ^= 1;
        check(!bind(game.base, unknown.base), "production mutated ownership rejected");
        game.base[cleanup_blocks[i].rva] ^= 1;
    }
    check(bind(game.base, nullptr), "valid game rearmed without a host whitelist");
    const wchar_t* tex = L"cleanup_source.tex.241106027";
    const wchar_t* pak = L"cleanup_source.sub_000.pak";
    create_source(tex); create_source(pak);
    for (unsigned mode = 0; mode < 2; ++mode) {
        check(loading(mode != 0), "loading state toggle");
        for (unsigned i = 0; i < 1000; ++i) {
            IDStorageFile* f{};
            check(SUCCEEDED(factory->OpenFile(tex, iid_file, reinterpret_cast<void**>(&f))), "open loose texture");
            check(f->AddRef() == 2 && f->Release() == 1, "one initial cache owner");
            if (i % 2) { check(f->AddRef() == 2, "independent witness reference"); }
            const auto before = pairs();
            native_cleanup(game, f, tex);
            if (i % 2) { check(f->Release() == 0, "no leaked cleanup ref with another owner"); }
            check(pairs() == before + 1, "real caller paired exactly once");
            check(SUCCEEDED(factory->OpenFile(pak, iid_file, reinterpret_cast<void**>(&f))), "open pak");
            check(f->AddRef() == 2, "pak separate ownership fixture");
            native_cleanup(game, f, pak);
            check(pairs() == before + 1, "PAK untouched");
        }
    }
    IDStorageFile* normal{};
    check(SUCCEEDED(factory->OpenFile(tex, iid_file, reinterpret_cast<void**>(&normal))), "open ordinary caller");
    const auto before = pairs(); normal->Close();
    check(normal->Release() == 0 && pairs() == before, "other Close caller unchanged and no leak");
    check(loading(true), "inflight test loading start");
    IDStorageQueue* queue{}; IDStorageStatusArray* status{};
    DSTORAGE_QUEUE_DESC desc{}; desc.Capacity = 128; desc.Priority = DSTORAGE_PRIORITY_NORMAL;
    desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    check(SUCCEEDED(factory->CreateQueue(&desc, iid_queue, reinterpret_cast<void**>(&queue))), "create real IO queue");
    check(SUCCEEDED(factory->CreateStatusArray(1, "cleanup test", iid_status, reinterpret_cast<void**>(&status))), "create IO status");
    check(SUCCEEDED(factory->OpenFile(tex, iid_file, reinterpret_cast<void**>(&normal))), "open inflight source");
    std::array<std::uint8_t,64> destination{};
    DSTORAGE_REQUEST request{};
    request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    request.Source.File.Source = normal; request.Source.File.Size = destination.size();
    request.Destination.Memory.Buffer = destination.data(); request.Destination.Memory.Size = destination.size();
    request.UncompressedSize = destination.size();
    queue->EnqueueRequest(&request); queue->EnqueueStatus(status, 0);
    native_cleanup(game, normal, tex);
    check(loading(false), "gate closes while old lease drains");
    queue->Submit();
    const auto deadline = GetTickCount64() + 10000;
    while (!status->IsComplete(0) && GetTickCount64() < deadline) { Sleep(1); }
    check(status->IsComplete(0) && SUCCEEDED(status->GetHResult(0)), "inflight request completes after cleanup");
    for (unsigned i = 0; i < destination.size(); ++i) { check(destination[i] == ((i * 37U) & 255U), "inflight bytes exact"); }
    queue->Release(); status->Release();
    // The file must be deletable after all native ownership paths are retired.
    check(DeleteFileW(tex) && DeleteFileW(pak), "real files have no retained handles");
    factory->Release();
    check(erased == 4001 && pairs() == 2001, "native cleanup totals");
    DeleteCriticalSection(mutex);
    std::printf("loose_cleanup=PASS real_native_cleanup=4001 paired=2001 pak_unchanged=2000 concurrent=20000 loading_and_gameplay=1 no_handle_leak=1 profile_rejection=1 host_build_filter=0 unreadable_host_ignored=1 inflight_read_after_gate_end=1\n");
    return 0;
}
