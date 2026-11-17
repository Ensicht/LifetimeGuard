#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <dstorage.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr GUID iid_storage_file{
    0x5de95e7b, 0x955a, 0x4868, {0xa7, 0x3c, 0x24, 0x3b, 0x29, 0xf4, 0xb8, 0xda}};
constexpr GUID iid_storage_factory{
    0x6924ea0c, 0xc3cd, 0x4826, {0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1}};
constexpr GUID iid_storage_status_array{
    0x82397587, 0x7cd5, 0x453b, {0xa0, 0x2e, 0x31, 0x37, 0x9b, 0xd6, 0x46, 0x56}};
constexpr GUID iid_storage_queue{
    0xcfdbd83f, 0x9e06, 0x4fda, {0x8e, 0xa5, 0x69, 0x04, 0x21, 0x37, 0xf4, 0x9b}};

constexpr std::size_t k_iterations = 128;
constexpr std::size_t k_requests_per_iteration = 32;
constexpr std::size_t k_request_size = 64;
constexpr std::size_t k_cancel_requests = 8;
constexpr std::size_t k_extra_status_slots = 2;
constexpr std::size_t k_bulk_requests = 2048;
constexpr std::size_t k_collision_candidates = 64;
constexpr std::size_t k_collision_requests_per_file = 512;

constexpr std::uint64_t k_request_status_mask = 0x3ULL;
constexpr std::uint64_t k_request_status_empty = 0x0ULL;
constexpr std::uint64_t k_request_status_live = 0x1ULL;
constexpr std::uint64_t k_request_status_busy = 0x2ULL;
constexpr std::uint64_t k_request_generation_step = 0x4ULL;

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

bool write_source(const wchar_t* path) {
    const auto file = CreateFileW(
        path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::array<std::uint8_t, k_requests_per_iteration * k_request_size> bytes{};
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

bool wait_status(IDStorageStatusArray* status, UINT32 index) {
    for (unsigned attempt = 0; attempt < 15000; ++attempt) {
        if (status->IsComplete(index)) {
            return true;
        }
        Sleep(1);
    }
    return false;
}

void fill_request(
    DSTORAGE_REQUEST& request, IDStorageFile* source,
    std::array<std::uint8_t, k_request_size>& destination,
    std::uint64_t offset) {
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
    request.Name = "ConcurrentSharedPakStream";
}

bool destination_matches(
    const std::array<std::uint8_t, k_request_size>& destination,
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

std::size_t file_lock_stripe(IDStorageFile* file) {
    const auto wrapper = reinterpret_cast<std::uint64_t>(file);
    const auto internal = reinterpret_cast<std::uint64_t>(
        *reinterpret_cast<void**>(
            reinterpret_cast<std::uint8_t*>(file) + 0x20));
    auto value = wrapper ^ ((internal << 23U) | (internal >> 41U));
    value ^= value >> 30U;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27U;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31U;
    if (value <= 1) {
        value = 2;
    }
    return static_cast<std::size_t>(value & 0xFFU);
}

bool request_slot_generation_model_passes() {
    std::atomic<std::uint64_t> state{k_request_status_live};

    auto stale_completion = state.load(std::memory_order_acquire);

    auto owner = k_request_status_live;
    if (!state.compare_exchange_strong(
            owner, k_request_status_busy, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    state.store(k_request_generation_step | k_request_status_empty,
        std::memory_order_release);

    auto publisher = k_request_generation_step | k_request_status_empty;
    if (!state.compare_exchange_strong(
            publisher, k_request_generation_step | k_request_status_busy,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    state.store(k_request_generation_step | k_request_status_live,
        std::memory_order_release);

    const auto stale_busy =
        (stale_completion & ~k_request_status_mask) |
        k_request_status_busy;
    return !state.compare_exchange_strong(
        stale_completion, stale_busy, std::memory_order_acq_rel,
        std::memory_order_acquire) &&
        state.load(std::memory_order_acquire) ==
            (k_request_generation_step | k_request_status_live);
}

} // namespace

int wmain() {
    if (!request_slot_generation_model_passes()) {
        std::printf("FAIL: request-slot generation/ABA model\n");
        return 29;
    }
    std::printf("request-slot ABA model=pass\n");

    const auto guard = LoadLibraryW(L"DStorageFileLifetimeGuard.dll");
    if (guard == nullptr) {
        std::printf("FAIL: guard LoadLibrary error=%lu\n", GetLastError());
        return 1;
    }
    const auto initialize = reinterpret_cast<InitializeFn>(
        GetProcAddress(guard, "reframework_plugin_initialize"));
    const auto get_stats = reinterpret_cast<GetStatsFn>(
        GetProcAddress(guard, "dstorage_file_lifetime_guard_get_stats"));
    if (initialize == nullptr || get_stats == nullptr || !initialize(nullptr)) {
        std::printf("FAIL: guard initialize\n");
        return 2;
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
    queue_desc.Name = "LifetimeGuardStress";
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
        static_cast<UINT32>(k_iterations + k_extra_status_slots), queue_desc.Name,
        iid_storage_status_array, reinterpret_cast<void**>(&status));
    if (FAILED(result) || status == nullptr) {
        std::printf("FAIL: CreateStatusArray hr=0x%08lX\n",
            static_cast<unsigned long>(result));
        return 5;
    }

    wchar_t source_path[MAX_PATH]{};
    std::swprintf(
        source_path, MAX_PATH, L"lifetime_guard_stress_%lu.bin",
        GetCurrentProcessId());
    if (!write_source(source_path)) {
        std::printf("FAIL: source creation\n");
        return 6;
    }

    for (std::size_t iteration = 0; iteration < k_iterations; ++iteration) {
        IDStorageFile* source{};
        result = factory->OpenFile(
            source_path, iid_storage_file, reinterpret_cast<void**>(&source));
        if (FAILED(result) || source == nullptr) {
            std::printf("FAIL: OpenFile iteration=%zu hr=0x%08lX\n",
                iteration, static_cast<unsigned long>(result));
            return 7;
        }

        std::array<std::array<std::uint8_t, k_request_size>,
            k_requests_per_iteration> destinations{};
        std::array<DSTORAGE_REQUEST, k_requests_per_iteration> requests{};
        for (std::size_t index = 0; index < requests.size(); ++index) {
            fill_request(
                requests[index], source, destinations[index],
                index * k_request_size);
        }

        queue->EnqueueRequest(&requests[0]);
        const auto start_close = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (start_close == nullptr) {
            std::printf("FAIL: CreateEvent iteration=%zu\n", iteration);
            return 8;
        }
        std::thread closer([source, start_close]() {
            WaitForSingleObject(start_close, INFINITE);
            source->Close();
        });
        SetEvent(start_close);
        for (std::size_t index = 1; index < requests.size(); ++index) {
            queue->EnqueueRequest(&requests[index]);
        }
        closer.join();
        CloseHandle(start_close);

        // The game cache evicts in this exact order: Close, then Release,
        // before the DirectStorage worker necessarily starts the requests.
        source->Release();
        source = nullptr;

        queue->EnqueueStatus(status, static_cast<UINT32>(iteration));
        queue->Submit();
        if (!wait_status(status, static_cast<UINT32>(iteration))) {
            std::printf("FAIL: status timeout iteration=%zu\n", iteration);
            return 9;
        }
        const auto request_result = status->GetHResult(
            static_cast<UINT32>(iteration));
        if (FAILED(request_result)) {
            std::printf("FAIL: request iteration=%zu hr=0x%08lX\n",
                iteration, static_cast<unsigned long>(request_result));
            return 10;
        }
        for (std::size_t index = 0; index < destinations.size(); ++index) {
            if (!destination_matches(
                    destinations[index], index * k_request_size)) {
                std::printf("FAIL: data iteration=%zu request=%zu\n",
                    iteration, index);
                return 11;
            }
        }
    }

    IDStorageFile* cancel_source{};
    result = factory->OpenFile(
        source_path, iid_storage_file,
        reinterpret_cast<void**>(&cancel_source));
    if (FAILED(result) || cancel_source == nullptr) {
        std::printf("FAIL: cancel OpenFile hr=0x%08lX\n",
            static_cast<unsigned long>(result));
        return 12;
    }
    std::array<std::array<std::uint8_t, k_request_size>,
        k_cancel_requests> cancel_destinations{};
    std::array<DSTORAGE_REQUEST, k_cancel_requests> cancel_requests{};
    constexpr std::uint64_t cancel_tag = 0xD570CACEULL;
    for (std::size_t index = 0; index < cancel_requests.size(); ++index) {
        fill_request(
            cancel_requests[index], cancel_source,
            cancel_destinations[index], index * k_request_size);
        cancel_requests[index].CancellationTag = cancel_tag;
        queue->EnqueueRequest(&cancel_requests[index]);
    }
    cancel_source->Close();
    cancel_source->Release();
    queue->CancelRequestsWithTag(~0ULL, cancel_tag);
    queue->EnqueueStatus(status, static_cast<UINT32>(k_iterations));
    queue->Submit();
    if (!wait_status(status, static_cast<UINT32>(k_iterations))) {
        std::printf("FAIL: cancel status timeout\n");
        return 13;
    }

    IDStorageFile* failure_source{};
    result = factory->OpenFile(
        source_path, iid_storage_file,
        reinterpret_cast<void**>(&failure_source));
    if (FAILED(result) || failure_source == nullptr) {
        std::printf("FAIL: failure OpenFile hr=0x%08lX\n",
            static_cast<unsigned long>(result));
        return 14;
    }
    std::array<std::uint8_t, k_request_size> failure_destination{};
    DSTORAGE_REQUEST failure_request{};
    fill_request(
        failure_request, failure_source, failure_destination,
        k_requests_per_iteration * k_request_size + 4096);
    queue->EnqueueRequest(&failure_request);
    failure_source->Close();
    failure_source->Release();
    queue->EnqueueStatus(
        status, static_cast<UINT32>(k_iterations + 1));
    queue->Submit();
    if (!wait_status(status, static_cast<UINT32>(k_iterations + 1))) {
        std::printf("FAIL: failure status timeout\n");
        return 15;
    }

    DSTORAGE_QUEUE_DESC bulk_queue_desc = queue_desc;
    bulk_queue_desc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
    bulk_queue_desc.Name = "LifetimeGuardBulk";
    IDStorageQueue* bulk_queue{};
    result = factory->CreateQueue(
        &bulk_queue_desc, iid_storage_queue,
        reinterpret_cast<void**>(&bulk_queue));
    if (FAILED(result) || bulk_queue == nullptr) {
        std::printf("FAIL: bulk CreateQueue hr=0x%08lX\n",
            static_cast<unsigned long>(result));
        return 16;
    }
    IDStorageStatusArray* bulk_status{};
    result = factory->CreateStatusArray(
        1, bulk_queue_desc.Name, iid_storage_status_array,
        reinterpret_cast<void**>(&bulk_status));
    if (FAILED(result) || bulk_status == nullptr) {
        std::printf("FAIL: bulk CreateStatusArray hr=0x%08lX\n",
            static_cast<unsigned long>(result));
        return 17;
    }
    IDStorageFile* bulk_source{};
    result = factory->OpenFile(
        source_path, iid_storage_file,
        reinterpret_cast<void**>(&bulk_source));
    if (FAILED(result) || bulk_source == nullptr) {
        std::printf("FAIL: bulk OpenFile hr=0x%08lX\n",
            static_cast<unsigned long>(result));
        return 18;
    }
    std::vector<std::array<std::uint8_t, k_request_size>>
        bulk_destinations(k_bulk_requests);
    std::vector<DSTORAGE_REQUEST> bulk_requests(k_bulk_requests);
    for (std::size_t index = 0; index < bulk_requests.size(); ++index) {
        const auto offset =
            (index % k_requests_per_iteration) * k_request_size;
        fill_request(
            bulk_requests[index], bulk_source,
            bulk_destinations[index], offset);
        bulk_queue->EnqueueRequest(&bulk_requests[index]);
    }
    bulk_source->Close();
    bulk_source->Release();
    bulk_queue->EnqueueStatus(bulk_status, 0);
    bulk_queue->Submit();
    if (!wait_status(bulk_status, 0) || FAILED(bulk_status->GetHResult(0))) {
        std::printf("FAIL: bulk status\n");
        return 19;
    }
    for (std::size_t index = 0; index < bulk_destinations.size(); ++index) {
        const auto offset =
            (index % k_requests_per_iteration) * k_request_size;
        if (!destination_matches(bulk_destinations[index], offset)) {
            std::printf("FAIL: bulk data request=%zu\n", index);
            return 20;
        }
    }

    std::array<int, 256> first_by_stripe{};
    first_by_stripe.fill(-1);
    std::vector<IDStorageFile*> collision_files;
    collision_files.reserve(k_collision_candidates);
    int collision_first{-1};
    int collision_second{-1};
    for (std::size_t index = 0; index < k_collision_candidates; ++index) {
        IDStorageFile* candidate{};
        result = factory->OpenFile(
            source_path, iid_storage_file,
            reinterpret_cast<void**>(&candidate));
        if (FAILED(result) || candidate == nullptr) {
            std::printf("FAIL: collision OpenFile index=%zu hr=0x%08lX\n",
                index, static_cast<unsigned long>(result));
            return 21;
        }
        collision_files.push_back(candidate);
        const auto stripe = file_lock_stripe(candidate);
        if (first_by_stripe[stripe] >= 0) {
            collision_first = first_by_stripe[stripe];
            collision_second = static_cast<int>(index);
            break;
        }
        first_by_stripe[stripe] = static_cast<int>(index);
    }
    if (collision_first < 0 || collision_second < 0) {
        std::printf("FAIL: no file-lock stripe collision\n");
        return 22;
    }
    for (std::size_t index = 0; index < collision_files.size(); ++index) {
        if (static_cast<int>(index) == collision_first ||
            static_cast<int>(index) == collision_second) {
            continue;
        }
        collision_files[index]->Close();
        collision_files[index]->Release();
        collision_files[index] = nullptr;
    }

    IDStorageFile* collision_sources[2] = {
        collision_files[static_cast<std::size_t>(collision_first)],
        collision_files[static_cast<std::size_t>(collision_second)],
    };
    IDStorageStatusArray* collision_status{};
    result = factory->CreateStatusArray(
        1, "LifetimeGuardStripeCollision", iid_storage_status_array,
        reinterpret_cast<void**>(&collision_status));
    if (FAILED(result) || collision_status == nullptr) {
        std::printf("FAIL: collision CreateStatusArray hr=0x%08lX\n",
            static_cast<unsigned long>(result));
        return 23;
    }
    constexpr auto collision_request_count =
        k_collision_requests_per_file * 2;
    std::vector<std::array<std::uint8_t, k_request_size>>
        collision_destinations(collision_request_count);
    std::vector<DSTORAGE_REQUEST> collision_requests(collision_request_count);
    for (std::size_t index = 0; index < collision_request_count; ++index) {
        const auto source_index = index & 1U;
        const auto offset =
            ((index / 2) % k_requests_per_iteration) * k_request_size;
        fill_request(
            collision_requests[index], collision_sources[source_index],
            collision_destinations[index], offset);
        bulk_queue->EnqueueRequest(&collision_requests[index]);
    }
    const auto close_collision = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (close_collision == nullptr) {
        std::printf("FAIL: collision CreateEvent\n");
        return 24;
    }
    std::thread collision_closer_a([&]() {
        WaitForSingleObject(close_collision, INFINITE);
        collision_sources[0]->Close();
        collision_sources[0]->Release();
    });
    std::thread collision_closer_b([&]() {
        WaitForSingleObject(close_collision, INFINITE);
        collision_sources[1]->Close();
        collision_sources[1]->Release();
    });
    SetEvent(close_collision);
    collision_closer_a.join();
    collision_closer_b.join();
    CloseHandle(close_collision);
    bulk_queue->EnqueueStatus(collision_status, 0);
    bulk_queue->Submit();
    if (!wait_status(collision_status, 0) ||
        FAILED(collision_status->GetHResult(0))) {
        std::printf("FAIL: collision status\n");
        return 25;
    }
    for (std::size_t index = 0; index < collision_request_count; ++index) {
        const auto offset =
            ((index / 2) % k_requests_per_iteration) * k_request_size;
        if (!destination_matches(collision_destinations[index], offset)) {
            std::printf("FAIL: collision data request=%zu\n", index);
            return 26;
        }
    }

    Sleep(100);
    GuardStats stats{};
    if (!get_stats(&stats, sizeof(stats))) {
        std::printf("FAIL: stats export\n");
        return 27;
    }
    constexpr auto expected_requests =
        k_iterations * k_requests_per_iteration + k_cancel_requests + 1 +
        k_bulk_requests + collision_request_count;
    constexpr auto expected_closes = k_iterations + 5;
    std::printf(
        "stress iterations=%zu requests=%llu guarded=%llu completed=%llu "
        "active=%llu deferred=%llu replayed=%llu unpaired=%llu "
        "collisions=%llu\n",
        k_iterations,
        static_cast<unsigned long long>(expected_requests),
        static_cast<unsigned long long>(stats.guarded_requests),
        static_cast<unsigned long long>(stats.completed_requests),
        static_cast<unsigned long long>(stats.active_leases),
        static_cast<unsigned long long>(stats.deferred_closes),
        static_cast<unsigned long long>(stats.replayed_closes),
        static_cast<unsigned long long>(stats.unpaired_requests),
        static_cast<unsigned long long>(stats.registry_collisions));

    const bool passed = stats.install_state == 1 &&
        stats.guarded_requests == expected_requests &&
        stats.completed_requests == expected_requests &&
        stats.active_leases == 0 &&
        stats.deferred_closes == expected_closes &&
        stats.replayed_closes == expected_closes &&
        stats.unpaired_requests == 0 &&
        stats.registry_collisions == 0;

    status->Release();
    queue->Release();
    bulk_status->Release();
    collision_status->Release();
    bulk_queue->Release();
    factory->Release();
    DeleteFileW(source_path);
    return passed ? 0 : 28;
}
