#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "LooseTextureCleanup.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace dstorage_guard;
namespace {
unsigned checks{}, native_cleanups{}, native_assignments{};
void check(bool ok, const char *message) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}
struct Image {
    std::uint8_t *base{};
    Image(std::uint32_t stamp, std::uint32_t size) {
        base = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS));
        check(base != nullptr, "reserve isolated sparse image");
        commit(0, 4096);
        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x100;
        auto *header = nt();
        header->Signature = IMAGE_NT_SIGNATURE;
        header->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        header->FileHeader.TimeDateStamp = stamp;
        header->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        header->OptionalHeader.SizeOfImage = size;
    }
    ~Image() { VirtualFree(base, 0, MEM_RELEASE); }
    IMAGE_NT_HEADERS64 *nt() { return reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + 0x100); }
    void commit(std::uintptr_t rva, std::size_t size) {
        const auto start = rva & ~std::uintptr_t(4095);
        const auto count = ((rva - start + size + 4095) / 4096) * 4096;
        check(VirtualAlloc(base + start, count, MEM_COMMIT, PAGE_EXECUTE_READWRITE) != nullptr,
              "commit isolated fixture page");
    }
    void hex(std::uintptr_t rva, const char *text) {
        const auto size = std::strlen(text) / 2;
        commit(rva, size);
        for (std::size_t i = 0; i < size; ++i) {
            base[rva + i] = (cleanup_nibble(text[2 * i]) << 4U) | cleanup_nibble(text[2 * i + 1]);
        }
    }
    void profile(const CleanupGameProfile &profile) {
        for (unsigned i = 0; i < profile.block_count; ++i) {
            hex(profile.blocks[i].rva, profile.blocks[i].hex);
        }
    }
    void pointer(std::uintptr_t rva, void *value) {
        commit(rva, sizeof(value));
        std::memcpy(base + rva, &value, sizeof(value));
    }
    void jump(std::uintptr_t rva, void *target) {
        hex(rva, "48b80000000000000000ffe0");
        std::memcpy(base + rva + 2, &target, sizeof(target));
    }
};
struct Layout {
    std::uint32_t stamp, size;
    std::uintptr_t entry;
    const char *prefix;
    std::uintptr_t route, unlink, head, mutex, enter_slot, leave_slot;
    std::uintptr_t lookup, assignment_exit, dead_read;
};
const Layout layouts[] = {
    {0x6A7D2E58, 0x2079C000, 0xCF03310,
     "564883ec204889ce4883c108488d15eb6d6f01e8f86e2600",
     0xD16A220, 0xCDE3530, 0x14964478, 0x14964490, 0xE620D70, 0xE620F68,
     0xB691730, 0x5178B6, 0},
    {0x6ACECA63, 0x20DDA000, 0xCF03AC0,
     "564883ec204889ce4883c108488d159d797101e8086f2600",
     0xD16A9E0, 0xCDE4500, 0x14985498, 0x149854B0, 0xE641FC0, 0xE6421B8,
     0xB692630, 0x5175E6, 0x201FC8C4},
};
enum Event { Enter, Close, Add, Release, Unlink, Free, Leave };
std::array<Event, 32> trace{};
unsigned trace_size{};
void event(Event value) {
    check(trace_size < trace.size(), "bounded fixture trace");
    trace[trace_size++] = value;
}
LooseTextureCleanup *active_policy{};
struct File {
    void **vtable{};
    std::uintptr_t gap[3]{};
    void *internal{};
    ULONG refs{1};
    bool destroyed{};
    std::array<std::uintptr_t, 6> info{};
    wchar_t path[256]{};
    explicit File(const wchar_t *name) {
        static void *methods[] = {nullptr, reinterpret_cast<void *>(&add),
                                  reinterpret_cast<void *>(&release), reinterpret_cast<void *>(&close)};
        vtable = methods;
        internal = info.data();
        wcscpy_s(path, name);
        info[2] = reinterpret_cast<std::uintptr_t>(path);
        info[4] = std::wcslen(path);
        info[5] = info[4] < 8 ? 8 : info[4];
    }
    static ULONG add(void *object) {
        auto &file = *static_cast<File *>(object);
        check(!file.destroyed, "AddRef cannot use a dead object");
        event(Add);
        return ++file.refs;
    }
    static ULONG release(void *object) {
        auto &file = *static_cast<File *>(object);
        check(!file.destroyed && file.refs != 0, "Release cannot use a dead object");
        event(Release);
        if (!--file.refs) {
            file.destroyed = true;
        }
        return file.refs;
    }
    __attribute__((noinline)) static void close(void *object) {
        event(Close);
        const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        active_policy->pair(object, caller);
    }
};
static_assert(offsetof(File, internal) == 0x20);
struct Node { std::uintptr_t words[6]{}; };
Node *lookup_node{};
void *expected_tree{};
void *unlink_node(void *tree, void *node) {
    check(tree == expected_tree, "eraser uses the same cache tree");
    event(Unlink);
    return node;
}
void free_node(void *node) {
    check(static_cast<Node *>(node)->words[5] == 0, "node owner cleared before free");
    event(Free);
}
void lookup(void *tree, Node **result, void *) {
    check(tree == expected_tree, "assignment and cleanup share the tree");
    *result = lookup_node;
}
const wchar_t *route(const wchar_t *path, const wchar_t *) {
    return std::wcsstr(path, L".sub_000.pak") || is_loose_texture_path(path, std::wcslen(path))
               ? path : nullptr;
}
void enter(CRITICAL_SECTION *mutex) { event(Enter); EnterCriticalSection(mutex); }
void leave(CRITICAL_SECTION *mutex) { event(Leave); LeaveCriticalSection(mutex); }

void prepare_native(Image &image, const CleanupGameProfile &profile, const Layout &layout) {
    image.hex(layout.entry, layout.prefix);
    image.hex(profile.blocks[3].rva - 6, "56574883ec28");
    image.jump(layout.route, reinterpret_cast<void *>(&route));
    image.jump(layout.unlink, reinterpret_cast<void *>(&unlink_node));
    image.jump(0x1330, reinterpret_cast<void *>(&free_node));
    image.pointer(layout.enter_slot, reinterpret_cast<void *>(&enter));
    image.pointer(layout.leave_slot, reinterpret_cast<void *>(&leave));
    image.commit(layout.mutex, sizeof(CRITICAL_SECTION));
    InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION *>(image.base + layout.mutex));
    if (layout.dead_read) {
        image.commit(layout.dead_read, sizeof(std::uint16_t));
    }
    image.jump(layout.lookup, reinterpret_cast<void *>(&lookup));
    // The captured assignment block returns through a fixture epilogue. No ownership
    // instruction is replaced; tree allocation and security-cookie machinery are stubs.
    image.hex(layout.assignment_exit, "4883c4585f5ec3");
    image.hex(0x3000, "56574883ec584889d648894c2428e900000000");
    const auto displacement = static_cast<std::int32_t>(profile.blocks[4].rva - (0x3000 + 19));
    std::memcpy(image.base + 0x3000 + 15, &displacement, sizeof(displacement));
    FlushInstructionCache(GetCurrentProcess(), image.base, layout.size);
    expected_tree = image.base + layout.head;
}
void assign(Image &image, Node &node, File &file) {
    lookup_node = &node;
    void *key{};
    reinterpret_cast<void (*)(void *, void **)>(image.base + 0x3000)(&file, &key);
    check(key == &file && node.words[5] == reinterpret_cast<std::uintptr_t>(&file),
          "captured assignment stores the incoming object");
    ++native_assignments;
}
void cleanup(Image &image, const Layout &layout, Node &node, File &file, bool paired) {
    Node head{};
    head.words[1] = reinterpret_cast<std::uintptr_t>(&node);
    reinterpret_cast<std::uint8_t *>(&head)[0x19] = 1;
    node.words[0] = node.words[1] = node.words[2] = reinterpret_cast<std::uintptr_t>(&head);
    node.words[4] = reinterpret_cast<std::uintptr_t>(&file);
    image.pointer(layout.head, &head);
    struct Info { void *file; wchar_t path[256]; } info{};
    info.file = &file;
    wcscpy_s(info.path, file.path);
    trace_size = 0;
    reinterpret_cast<void (*)(Info *)>(image.base + layout.entry)(&info);
    const Event with_pair[] = {Enter, Close, Add, Release, Unlink, Release, Free, Leave};
    const Event without_pair[] = {Enter, Close, Release, Unlink, Release, Free, Leave};
    const auto *expected = paired ? with_pair : without_pair;
    const auto count = paired ? std::size(with_pair) : std::size(without_pair);
    check(trace_size == count, "native call count exact");
    for (unsigned i = 0; i < count; ++i) {
        check(trace[i] == expected[i], "Close/AddRef/two Releases/lock order exact");
    }
    check(node.words[5] == 0, "native eraser clears the cache owner");
    ++native_cleanups;
}

void test_profile(unsigned index) {
    const auto &profile = cleanup_game_profiles[index];
    const auto &layout = layouts[index];
    Image image(layout.stamp, layout.size);
    image.profile(profile);
    std::uintptr_t caller{};
    check(validate_cleanup_image(image.base, &caller) == 0 && caller == profile.close_return_rva,
          "one complete profile selects its own caller");
    for (unsigned block = 0; block < profile.block_count; ++block) {
        for (std::size_t byte = 0; byte < std::strlen(profile.blocks[block].hex) / 2; ++byte) {
            image.base[profile.blocks[block].rva + byte] ^= 1;
            caller = 1;
            check(validate_cleanup_image(image.base, &caller) == 10 + block && caller == 0,
                  "every ownership byte is required and clears the output");
            image.base[profile.blocks[block].rva + byte] ^= 1;
        }
    }
    LooseTextureCleanup policy;
    policy.initialize(image.base);
    check(policy.failure() == 0 && policy.caller() == reinterpret_cast<std::uintptr_t>(image.base) +
          profile.close_return_rva, "policy publishes only the selected caller");
    const auto saved = policy.caller();
    image.nt()->FileHeader.TimeDateStamp = 42;
    image.nt()->OptionalHeader.SizeOfImage += 4096;
    policy.initialize(image.base);
    check(policy.failure() == 0 && policy.caller() == saved, "metadata is not ownership authority");
    check(!policy.pair(reinterpret_cast<void *>(1), reinterpret_cast<std::uintptr_t>(image.base) +
          cleanup_game_profiles[1 - index].close_return_rva), "foreign-profile caller is rejected first");
    check(!policy.pair(nullptr, saved), "null wrapper rejected");
    image.nt()->OptionalHeader.SizeOfImage = 4096;
    policy.initialize(image.base);
    check(policy.failure() != 0 && policy.caller() == 0 &&
          !policy.pair(reinterpret_cast<void *>(1), saved), "insufficient image bounds disarm stale caller");
    image.nt()->OptionalHeader.SizeOfImage = layout.size;
    image.nt()->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    check(validate_cleanup_image(image.base) == 1, "wrong architecture rejected");
    image.nt()->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    policy.initialize(image.base);
    active_policy = &policy;
    prepare_native(image, profile, layout);
    check(validate_cleanup_image(image.base) == 0, "fixture stubs do not alter certified windows");
    for (unsigned i = 0; i < 1000; ++i) {
        Node node{};
        File file(L"source.tex.241106027");
        trace_size = 0;
        assign(image, node, file);
        check(file.refs == 1 && !file.destroyed, "assignment retains exactly one cache reference");
        cleanup(image, layout, node, file, true);
        check(file.refs == 0 && file.destroyed, "both native consumers balance the temporary reference");
    }
    {
        Node node{};
        File old(L"old.tex.1"), file(L"new.tex.1");
        node.words[5] = reinterpret_cast<std::uintptr_t>(&old);
        trace_size = 0;
        assign(image, node, file);
        check(old.destroyed && file.refs == 1, "replacement releases the former cache owner");
        trace_size = 0;
        File::add(&file);
        assign(image, node, file);
        check(file.refs == 1, "same cached object consumes only its new local reference");
        trace_size = 0;
        File::add(&file);
        cleanup(image, layout, node, file, true);
        check(file.refs == 1 && !file.destroyed, "independent observer remains alive");
        File::release(&file);
        check(file.destroyed, "independent observer releases without leak");
    }
    {
        Node node{};
        File file(L"source.sub_000.pak");
        trace_size = 0;
        assign(image, node, file);
        File::add(&file);
        const auto pairs = policy.paired();
        cleanup(image, layout, node, file, false);
        check(file.destroyed && policy.paired() == pairs, "PAK ownership is unchanged and never paired");
    }
    policy.initialize(nullptr);
    check(policy.failure() == 1 && policy.caller() == 0 &&
          !policy.pair(reinterpret_cast<void *>(1), saved), "failed reinitialization clears caller before use");
    DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION *>(image.base + layout.mutex));
}
void test_ambiguity() {
    Image image(42, 0x20DDA000);
    for (const auto &profile : cleanup_game_profiles) {
        image.profile(profile);
    }
    std::uintptr_t caller{1};
    check(validate_cleanup_image(image.base, &caller) == 3 && caller == 0,
          "two complete profiles are ambiguous, never first-match accepted");
    LooseTextureCleanup policy;
    policy.initialize(image.base);
    check(policy.failure() == 3 && policy.caller() == 0, "ambiguous policy remains disarmed");
    Image unknown(42, 0x10000);
    check(validate_cleanup_image(unknown.base) == 2, "unknown ownership remains rejected");
}
void test_disk_image(const std::filesystem::path &path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                             FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    check(file != INVALID_HANDLE_VALUE, "open actual EXE read-only");
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr);
    check(mapping != nullptr, "map EXE without loading or executing it");
    const void *image = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    check(image != nullptr, "read-only image view");
    std::uintptr_t caller{};
    check(validate_cleanup_image(image, &caller) == 0 && caller == 0xCF03B59,
          "actual current EXE selects the new complete ownership profile");
    MEMORY_BASIC_INFORMATION region{};
    check(VirtualQuery(static_cast<const std::uint8_t *>(image) + caller, &region, sizeof(region)) != 0 &&
          !(region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                              PAGE_EXECUTE_WRITECOPY)), "actual game image is never executable in this test");
    UnmapViewOfFile(image);
    CloseHandle(mapping);
    CloseHandle(file);
}
} // namespace
int main(int argc, char **argv) {
    check(argc == 2, "supply current EXE path for read-only mapping");
    test_profile(0);
    test_profile(1);
    test_ambiguity();
    test_disk_image(std::filesystem::path(argv[1]));
    std::printf("cleanup_profiles=PASS checks=%u native_cleanups=%u native_assignments=%u "
                "mutated_bytes=737 profiles=2 ambiguity_rejected=1 current_exe_readonly=1 "
                "game_executed=0 sdk_calls=0\n", checks, native_cleanups, native_assignments);
}
