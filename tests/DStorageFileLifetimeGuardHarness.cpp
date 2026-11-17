#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <dstorage.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace {

constexpr GUID iid_storage_file{
    0x5de95e7b, 0x955a, 0x4868, {0xa7, 0x3c, 0x24, 0x3b, 0x29, 0xf4, 0xb8, 0xda}};
constexpr GUID iid_storage_factory{
    0x6924ea0c, 0xc3cd, 0x4826, {0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1}};
constexpr GUID iid_storage_status_array{
    0x82397587, 0x7cd5, 0x453b, {0xa0, 0x2e, 0x31, 0x37, 0x9b, 0xd6, 0x46, 0x56}};
constexpr GUID iid_storage_queue{
    0xcfdbd83f, 0x9e06, 0x4fda, {0x8e, 0xa5, 0x69, 0x04, 0x21, 0x37, 0xf4, 0x9b}};

struct GuardStats {
    std::uint32_t version{};
    std::uint32_t install_state{};
    std::uint64_t guarded_requests{};
    std::uint64_t completed_requests{};
    std::uint64_t active_leases{};
    std::uint64_t deferred_closes{};
    std::uint64_t replayed_closes{};
    std::uint64_t unpaired_requests{};
    std::uint64_t registry_collisions{};
};

using InitializeFn = bool (*)(const void*);
using GetStatsFn = bool (*)(GuardStats*, std::size_t);
using SetLoadingStateFn = bool (*)(bool);

bool wait_status(IDStorageStatusArray* status, UINT32 index) {
    for (unsigned attempt = 0; attempt < 15000; ++attempt) {
        if (status->IsComplete(index)) {
            return true;
        }
        Sleep(1);
    }
    return false;
}

bool write_source(const wchar_t* path) {
    const auto file = CreateFileW(
        path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::array<std::uint8_t, 4096> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>((index * 37U) & 0xFFU);
    }
    DWORD written{};
    const auto result = WriteFile(
        file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
        nullptr) && written == bytes.size();
    CloseHandle(file);
    return result;
}

void fill_request(
    DSTORAGE_REQUEST& request, IDStorageFile* source,
    std::array<std::uint8_t, 64>& destination,
    std::uint64_t offset, const char* name) {
    request = {};
    request.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    request.Source.File.Source = source;
    request.Source.File.Offset = offset;
    request.Source.File.Size = destination.size();
    request.Destination.Memory.Buffer = destination.data();
    request.Destination.Memory.Size = destination.size();
    request.UncompressedSize = destination.size();
    request.Name = name;
}

bool destination_matches(
    const std::array<std::uint8_t, 64>& destination,
    std::uint64_t offset) {
    for (std::size_t index = 0; index < destination.size(); ++index) {
        const auto expected = static_cast<std::uint8_t>(
            ((index + offset) * 37U) & 0xFFU);
        if (destination[index] != expected) {
            return false;
        }
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool late_guard =
        argc == 2 && std::wcscmp(argv[1], L"--guard-late") == 0;
    const bool bypass_guard =
        argc == 2 && std::wcscmp(argv[1], L"--guard-bypass") == 0;
    const bool drain_guard =
        argc == 2 && std::wcscmp(argv[1], L"--guard-drain") == 0;
    const bool use_guard = late_guard ||
        bypass_guard || drain_guard ||
        (argc == 2 && std::wcscmp(argv[1], L"--guard") == 0);
    HMODULE guard{};
    GetStatsFn get_stats{};
    SetLoadingStateFn set_loading_state{};
    const auto initialize_guard = [
        &guard, &get_stats, &set_loading_state]() {
        guard = LoadLibraryW(L"DStorageFileLifetimeGuard.dll");
        if (guard == nullptr) {
            std::printf("FAIL: guard LoadLibrary error=%lu\n", GetLastError());
            return false;
        }
        const auto initialize = reinterpret_cast<InitializeFn>(
            GetProcAddress(guard, "reframework_plugin_initialize"));
        get_stats = reinterpret_cast<GetStatsFn>(
            GetProcAddress(guard, "dstorage_file_lifetime_guard_get_stats"));
        set_loading_state = reinterpret_cast<SetLoadingStateFn>(
            GetProcAddress(
                guard,
                "dstorage_file_lifetime_guard_test_set_loading_state"));
        if (initialize == nullptr || get_stats == nullptr ||
            set_loading_state == nullptr || !initialize(nullptr)) {
            std::printf("FAIL: guard initialize\n");
            return false;
        }
        return true;
    };
    if (use_guard && !late_guard && !initialize_guard()) {
        return 1;
    }

    IDStorageFactory* factory{};
    auto result = DStorageGetFactory(
        iid_storage_factory, reinterpret_cast<void**>(&factory));
    if (FAILED(result) || factory == nullptr) {
        std::printf("FAIL: DStorageGetFactory hr=0x%08lX\n",
            static_cast<unsigned long>(result));
        return 3;
    }

    DSTORAGE_QUEUE_DESC queue_desc{};
    queue_desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    queue_desc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    queue_desc.Priority = DSTORAGE_PRIORITY_NORMAL;
    queue_desc.Name = use_guard ? "LifetimeGuard" : "LifetimeBaseline";
    IDStorageQueue* queue{};
    result = factory->CreateQueue(
        &queue_desc, iid_storage_queue, reinterpret_cast<void**>(&queue));
    if (FAILED(result) || queue == nullptr) {
        std::printf("FAIL: CreateQueue hr=0x%08lX\n",
            static_cast<unsigned long>(result));
        return 4;
    }

    IDStorageStatusArray* status{};
    result = factory->CreateStatusArray(
        1, queue_desc.Name, iid_storage_status_array,
        reinterpret_cast<void**>(&status));
    if (FAILED(result) || status == nullptr) {
        std::printf("FAIL: CreateStatusArray hr=0x%08lX\n",
            static_cast<unsigned long>(result));
        return 5;
    }

    wchar_t source_path[MAX_PATH]{};
    std::swprintf(
        source_path, MAX_PATH, L"lifetime_guard_source_%lu.bin",
        GetCurrentProcessId());
    if (!write_source(source_path)) {
        std::printf("FAIL: source creation\n");
        return 6;
    }

    IDStorageFile* source{};
    result = factory->OpenFile(
        source_path, iid_storage_file, reinterpret_cast<void**>(&source));
    if (FAILED(result) || source == nullptr) {
        std::printf("FAIL: OpenFile hr=0x%08lX\n",
            static_cast<unsigned long>(result));
        DeleteFileW(source_path);
        return 7;
    }
    if (late_guard && !initialize_guard()) {
        return 8;
    }
    if (bypass_guard && !set_loading_state(false)) {
        std::printf("FAIL: loading gate bypass setup\n");
        return 12;
    }

    std::array<std::uint8_t, 64> destination_a{};
    std::array<std::uint8_t, 64> destination_b{};
    DSTORAGE_REQUEST request_a{};
    DSTORAGE_REQUEST request_b{};
    fill_request(request_a, source, destination_a, 0, "SharedPakStreamA");
    fill_request(request_b, source, destination_b, 128, "SharedPakStreamB");

    // This exactly models one stream evicting a shared PAK file after another
    // stream has enqueued work but before the DirectStorage worker starts it.
    queue->EnqueueRequest(&request_a);
    queue->EnqueueRequest(&request_b);
    if (drain_guard && !set_loading_state(false)) {
        std::printf("FAIL: loading gate drain setup\n");
        return 13;
    }
    source->Close();
    queue->EnqueueStatus(status, 0);
    queue->Submit();

    if (!wait_status(status, 0)) {
        std::printf("FAIL: status timeout\n");
        return 9;
    }
    const auto hresult = status->GetHResult(0);
    GuardStats stats{};
    if (use_guard) {
        Sleep(100);
        if (!get_stats(&stats, sizeof(stats))) {
            std::printf("FAIL: stats export\n");
            return 10;
        }
    }

    std::printf(
        "mode=%s hr=0x%08lX guarded=%llu completed=%llu active=%llu "
        "deferred=%llu replayed=%llu unpaired=%llu collisions=%llu\n",
        late_guard ? "guard-late" :
            (bypass_guard ? "guard-bypass" :
            (drain_guard ? "guard-drain" :
            (use_guard ? "guard" : "baseline"))),
        static_cast<unsigned long>(hresult),
        static_cast<unsigned long long>(stats.guarded_requests),
        static_cast<unsigned long long>(stats.completed_requests),
        static_cast<unsigned long long>(stats.active_leases),
        static_cast<unsigned long long>(stats.deferred_closes),
        static_cast<unsigned long long>(stats.replayed_closes),
        static_cast<unsigned long long>(stats.unpaired_requests),
        static_cast<unsigned long long>(stats.registry_collisions));

    const bool protected_result = SUCCEEDED(hresult) &&
            destination_matches(destination_a, 0) &&
            destination_matches(destination_b, 128) &&
            stats.install_state == 1 &&
            stats.guarded_requests == 2 &&
            stats.completed_requests == 2 &&
            stats.active_leases == 0 &&
            stats.deferred_closes == 1 &&
            stats.replayed_closes == 1 &&
            stats.unpaired_requests == 0 &&
            stats.registry_collisions == 0;
    const bool bypass_result =
        hresult == static_cast<HRESULT>(0x8924000B) &&
        stats.install_state == 1 &&
        stats.guarded_requests == 0 &&
        stats.completed_requests == 0 &&
        stats.active_leases == 0 &&
        stats.deferred_closes == 0 &&
        stats.replayed_closes == 0 &&
        stats.unpaired_requests == 0 &&
        stats.registry_collisions == 0;
    const bool passed = bypass_guard
        ? bypass_result
        : (use_guard ? protected_result
                     : hresult == static_cast<HRESULT>(0x8924000B));

    source->Release();
    status->Release();
    queue->Release();
    factory->Release();
    DeleteFileW(source_path);
    return passed ? 0 : 11;
}
