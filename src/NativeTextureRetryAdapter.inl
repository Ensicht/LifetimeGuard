 
 
using NativeResourceReleaseFn = void (*)(void *, void *);
using NativeResourceReadQueueFn = void (*)(void *, void *);
using NativeTextureDeleteFn = void *(*)(void *, std::uint32_t);
using NativeTextureReadyFn = bool (*)(void *);

std::atomic<NativeResourceReleaseFn> g_retry_original_release{};
std::atomic<NativeTextureDeleteFn> g_retry_original_delete{};
std::atomic<NativeTextureReadyFn> g_retry_original_ready{};
std::atomic<bool> g_retry_armed{};
std::atomic<std::uint32_t> g_retry_install_state{};
std::atomic<int> g_retry_create_status{k_mh_status_not_attempted};
std::atomic<unsigned> g_retry_hook_mode{};  
std::atomic<int> g_retry_enable_status{k_mh_status_not_attempted};
std::uintptr_t g_retry_terminal_callers[4]{};
void *g_retry_texture_vtable{};
void **g_retry_manager_slot{};
NativeResourceReadQueueFn g_retry_read_queue{};
std::atomic<bool> g_retry_gameplay_enabled{true};
std::atomic<bool> g_retry_test_clock{};
std::atomic<std::uint64_t> g_retry_test_ms{};

 
dstorage_guard::TextureRetryGate retry_gate() {
    const auto gate = g_loading_gate.snapshot();
    const bool gameplay = !gate.active && g_retry_gameplay_enabled.load(std::memory_order_relaxed);
    return {gate.generation,
            (gate.active || gameplay) && gate.lifecycle_available && !gate.bootstrap &&
                gate.generation != 0,
            gameplay};
}

std::uint64_t retry_milliseconds() {
    if (g_retry_test_clock.load(std::memory_order_relaxed)) {
        return g_retry_test_ms.load();
    }
    return GetTickCount64();
}

template <class T> T retry_read(void *object, std::size_t offset) {
    return *reinterpret_cast<volatile T *>(static_cast<std::uint8_t *>(object) + offset);
}

dstorage_guard::TextureRetryResource retry_snapshot(void *resource) {
    dstorage_guard::TextureRetryResource result{};
     
     
    if (resource == nullptr || retry_read<void *>(resource, 0) != g_retry_texture_vtable) {
        return result;
    }
    result.texture = true;
    result.parsed = retry_read<std::uintptr_t>(resource, 0x70);
    result.identity = retry_read<std::uint64_t>(resource, 0x30);
    result.references = retry_read<std::uint32_t>(resource, 0x28);
    result.ready = retry_read<std::uint8_t>(resource, 0x38) != 0;
    result.task = retry_read<void *>(resource, 0x50) != nullptr;
    result.runtime = retry_read<void *>(resource, 0x78) != nullptr;
    result.disposing = (retry_read<std::uint8_t>(resource, 0x3B) & 6U) != 0;
    return result;
}

bool retry_manager_valid(void *manager) {
    return manager != nullptr && g_retry_manager_slot != nullptr &&
           *reinterpret_cast<void *volatile *>(g_retry_manager_slot) == manager &&
           retry_read<HANDLE>(manager, 0x5E0) != nullptr;
}

void retry_add_ref(void *resource) {
    InterlockedIncrement(
        reinterpret_cast<volatile LONG *>(static_cast<std::uint8_t *>(resource) + 0x28));
}

void retry_release(void *manager, void *resource) {
    g_retry_original_release.load(std::memory_order_acquire)(manager, resource);
}

bool retry_enqueue(void *manager, void *resource) {
    g_retry_read_queue(manager, resource);
     
     
    return true;
}

bool retry_wake(void *manager) {
     
    auto *pending = reinterpret_cast<volatile LONG *>(static_cast<std::uint8_t *>(manager) + 0x5E8);
    if (InterlockedCompareExchange(pending, 1, 0) != 0) {
        return true;
    }
    return SetEvent(retry_read<HANDLE>(manager, 0x5E0)) != FALSE;
}

void retry_notify() {
    if (g_report_event != nullptr) {
        SetEvent(g_report_event);
    }
}

std::uint32_t retry_thread_id() {
    return GetCurrentThreadId();
}

dstorage_guard::DetachedTextureRetry g_texture_retry{
    {&retry_gate, &retry_snapshot, &retry_manager_valid, &retry_add_ref, &retry_release,
     &retry_enqueue, &retry_wake, &retry_notify, &query_qpc, &retry_thread_id,
     &retry_milliseconds}};

bool retry_terminal_caller(std::uintptr_t caller) {
    return caller == g_retry_terminal_callers[0] || caller == g_retry_terminal_callers[1] ||
           caller == g_retry_terminal_callers[2] || caller == g_retry_terminal_callers[3];
}

 
void hook_retry_resource_release(void *manager, void *resource) {
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    if (g_retry_armed.load(std::memory_order_acquire) && retry_terminal_caller(caller) &&
        g_texture_retry.terminal(manager, resource)) {
        return;
    }
    retry_release(manager, resource);
}

 
bool hook_retry_texture_ready(void *resource) {
    const bool ready = g_retry_original_ready.load(std::memory_order_acquire)(resource);
    if (!ready && g_retry_armed.load(std::memory_order_acquire)) {
        g_texture_retry.queried(resource);
    }
     
    return ready;
}

 
void *hook_retry_texture_delete(void *resource, std::uint32_t flags) {
    if (g_retry_armed.load(std::memory_order_acquire)) {
        g_texture_retry.retired(resource);
    }
    return g_retry_original_delete.load(std::memory_order_acquire)(resource, flags);
}

template <std::size_t N>
bool retry_code_matches(std::uint8_t *base, std::uintptr_t rva, const std::uint8_t (&code)[N]) {
    const auto matches = is_readable_range(base + rva, N) && std::memcmp(base + rva, code, N) == 0;
    if (!matches) {
        record_install_event(InstallComponent::texture_retry, InstallStage::validate_target,
                             static_cast<std::uint32_t>(rva), base + rva, nullptr, nullptr, 1, 0);
    }
    return matches;
}

 
bool validate_retry_profile(HMODULE module) {
    const auto *nt = get_nt_headers(module);
    if (nt == nullptr || nt->FileHeader.TimeDateStamp != 0x6A7D2E58 ||
        nt->OptionalHeader.SizeOfImage != 0x2079C000) {
        return false;
    }
    auto *b = reinterpret_cast<std::uint8_t *>(module);
    constexpr std::uint8_t release[]{0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                     0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x48};
    constexpr std::uint8_t queue[]{0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83,
                                   0xEC, 0x30, 0x48, 0x89, 0xD7, 0x48, 0x89, 0xCE};
    constexpr std::uint8_t ready[]{0x8A, 0x41, 0x38, 0xC3};
    constexpr std::uint8_t destroy[]{0x56, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x89, 0xD7,
                                     0x48, 0x89, 0xCE, 0xE8, 0x20, 0,    0,    0};
    constexpr std::uint8_t clear_a[]{0x48, 0xC7, 0x45, 0x50, 0, 0, 0, 0};
    constexpr std::uint8_t clear_b[]{0x49, 0xC7, 0x44, 0x24, 0x50, 0, 0, 0, 0};
    constexpr std::uint8_t work_dec[]{0xF0, 0xFF, 0x8E, 0xC0, 0x0E, 0, 0};
    constexpr std::uint8_t read_inc[]{0xF0, 0xFF, 0x86, 0xAC, 0x0E, 0, 0};
    constexpr std::uint8_t release_ref[]{0xF0, 0xFF, 0x4A, 0x28};
    if (!retry_code_matches(b, 0x24E070, release) || !retry_code_matches(b, 0xB0CA3B0, queue) ||
        !retry_code_matches(b, 0x12C3A0, ready) || !retry_code_matches(b, 0x12C250, destroy) ||
        !retry_code_matches(b, 0xA4A7AAF, clear_a) || !retry_code_matches(b, 0xA4A7341, clear_b) ||
        !retry_code_matches(b, 0xA4A7ACE, work_dec) ||
        !retry_code_matches(b, 0xA4A736A, work_dec) ||
        !retry_code_matches(b, 0xB0CA40A, read_inc) ||
        !retry_code_matches(b, 0xB0CA5C5, read_inc) ||
        !retry_code_matches(b, 0x24E09D, release_ref)) {
        return false;
    }
    for (const auto rva : {0xA4A7ADFU, 0xA4A737BU, 0xA4A71C0U, 0xA4A79C6U}) {
        if (!is_readable_range(b + rva, 5) || b[rva] != 0xE8) {
            return false;
        }
        std::int32_t displacement{};
        std::memcpy(&displacement, b + rva + 1, 4);
        if (b + rva + 5 + displacement != b + 0x24E070) {
            return false;
        }
    }
    auto **vtable = reinterpret_cast<void **>(b + 0xE05AB80);
    return is_readable_range(vtable, 0x60) && vtable[6] == b + 0x12C250 &&
           vtable[9] == b + 0xAA377E0 && vtable[10] == b + 0x12C3A0 &&
           vtable[11] == b + 0xAA38790 && is_readable_range(b + 0x147C4330, sizeof(void *));
}

 
bool install_texture_retry_targets(const dstorage_guard::TextureRetryNativeTargets &targets) {
    if (targets.texture_vtable == nullptr || targets.manager_slot == nullptr ||
        targets.release_target == nullptr || targets.read_queue == nullptr ||
        targets.delete_original == nullptr || targets.ready_original == nullptr ||
        targets.terminal_callers[0] == 0 ||
        (targets.offline_before_enable != nullptr && !g_offline_test_mode.load())) {
        g_retry_install_state.store(3, std::memory_order_release);
        return false;
    }
    auto **vtable = targets.texture_vtable;
    auto *release = targets.release_target;
    std::memcpy(g_retry_terminal_callers, targets.terminal_callers,
                sizeof(g_retry_terminal_callers));
    g_retry_texture_vtable = vtable;
    g_retry_manager_slot = targets.manager_slot;
    g_retry_read_queue = reinterpret_cast<NativeResourceReadQueueFn>(targets.read_queue);
    g_retry_original_delete.store(reinterpret_cast<NativeTextureDeleteFn>(targets.delete_original));
    g_retry_original_ready.store(reinterpret_cast<NativeTextureReadyFn>(targets.ready_original));
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
        record_install_event(InstallComponent::texture_retry, InstallStage::mh_initialize, 0,
                             release, nullptr, nullptr, initialized, 0);
        g_retry_install_state.store(4);
        return false;
    }
    void *original{};
    const auto saved_create_error = GetLastError();
    SetLastError(ERROR_SUCCESS);
    auto created =
        MH_CreateHook(release, reinterpret_cast<void *>(&hook_retry_resource_release), &original);
    const auto create_error = GetLastError();
    SetLastError(saved_create_error);
    g_retry_create_status.store(created);
    record_install_event(InstallComponent::texture_retry, InstallStage::allocator_result, 0,
                         release, nullptr, original, created, create_error);
    record_install_event(InstallComponent::texture_retry, InstallStage::mh_create, 0, release,
                         reinterpret_cast<void *>(&hook_retry_resource_release), original, created,
                         create_error);
    if (created == MH_OK) {
        g_retry_hook_mode.store(1);
    }
    if (created == MH_ERROR_MEMORY_ALLOC) {
        SetLastError(ERROR_SUCCESS);
        created = MH_CreateHookStackPrologue16(
            release, reinterpret_cast<void *>(&hook_retry_resource_release), &original);
        const auto fallback_error = GetLastError();
        SetLastError(saved_create_error);
        g_retry_create_status.store(created);
        if (created == MH_OK) {
            g_retry_hook_mode.store(2);
        }
        record_install_event(InstallComponent::texture_retry, InstallStage::mh_create, 1, release,
                             reinterpret_cast<void *>(&hook_retry_resource_release), original,
                             created, fallback_error);
    }
    if (created != MH_OK) {
        g_retry_install_state.store(5);
        return false;
    }
    g_retry_original_release.store(reinterpret_cast<NativeResourceReleaseFn>(original),
                                   std::memory_order_release);

    NativePointerHookOperations operations;
    const auto deleted = dstorage_guard::install_pointer_hook(
        vtable + 6, targets.delete_original, reinterpret_cast<void *>(&hook_retry_texture_delete),
        operations);
    record_install_event(InstallComponent::texture_retry, InstallStage::pointer_install, 0,
                         vtable + 6, reinterpret_cast<void *>(&hook_retry_texture_delete),
                         targets.delete_original, static_cast<int>(deleted.status),
                         deleted.protect_error ? deleted.protect_error : deleted.restore_error);
    dstorage_guard::PointerHookResult queried{};
    if (deleted.status == dstorage_guard::PointerHookStatus::installed) {
        queried = dstorage_guard::install_pointer_hook(
            vtable + 10, targets.ready_original,
            reinterpret_cast<void *>(&hook_retry_texture_ready), operations);
        record_install_event(InstallComponent::texture_retry, InstallStage::pointer_install, 1,
                             vtable + 10, reinterpret_cast<void *>(&hook_retry_texture_ready),
                             targets.ready_original, static_cast<int>(queried.status),
                             queried.protect_error ? queried.protect_error : queried.restore_error);
    }
    if (deleted.status == dstorage_guard::PointerHookStatus::installed &&
        queried.status == dstorage_guard::PointerHookStatus::installed) {
        if (targets.offline_before_enable != nullptr) {
            targets.offline_before_enable();
        }
        const auto saved_enable_error = GetLastError();
        SetLastError(ERROR_SUCCESS);
        const auto enabled = MH_EnableHook(release);
        const auto enable_error = GetLastError();
        SetLastError(saved_enable_error);
        g_retry_enable_status.store(enabled);
        record_install_event(InstallComponent::texture_retry, InstallStage::mh_enable, 0, release,
                             reinterpret_cast<void *>(&hook_retry_resource_release), original,
                             enabled, enable_error);
        if (enabled == MH_OK) {
            g_retry_armed.store(true, std::memory_order_release);
            g_retry_install_state.store(2, std::memory_order_release);
            return true;
        }
    }
     
     
    const auto ready_rollback = dstorage_guard::install_pointer_hook(
        vtable + 10, reinterpret_cast<void *>(&hook_retry_texture_ready), targets.ready_original,
        operations);
    const auto delete_rollback = dstorage_guard::install_pointer_hook(
        vtable + 6, reinterpret_cast<void *>(&hook_retry_texture_delete), targets.delete_original,
        operations);
    record_install_event(InstallComponent::texture_retry, InstallStage::pointer_rollback, 0,
                         vtable + 6, targets.delete_original,
                         reinterpret_cast<void *>(&hook_retry_texture_delete),
                         static_cast<int>(delete_rollback.status), delete_rollback.restore_error);
    record_install_event(InstallComponent::texture_retry, InstallStage::pointer_rollback, 1,
                         vtable + 10, targets.ready_original,
                         reinterpret_cast<void *>(&hook_retry_texture_ready),
                         static_cast<int>(ready_rollback.status), ready_rollback.restore_error);
    g_retry_install_state.store(6, std::memory_order_release);
    return false;
}

bool install_texture_retry_hooks() {
    std::uint32_t expected{};
    if (!g_retry_install_state.compare_exchange_strong(expected, 1)) {
        return expected == 2;
    }
    auto *module = GetModuleHandleW(nullptr);
    const auto profile_valid =
        g_loading_gate.lifecycle_available() && validate_retry_profile(module);
    record_install_event(InstallComponent::texture_retry, InstallStage::validate_target, 0, module,
                         nullptr, nullptr, profile_valid ? 0 : 1, 0);
    if (!profile_valid) {
        g_retry_install_state.store(3);
        return false;
    }
    auto *base = reinterpret_cast<std::uint8_t *>(module);
    const auto address = reinterpret_cast<std::uintptr_t>(base);
    return install_texture_retry_targets(
        {reinterpret_cast<void **>(base + 0xE05AB80),
         reinterpret_cast<void **>(base + 0x147C4330),
         base + 0x24E070,
         base + 0xB0CA3B0,
         base + 0x12C250,
         base + 0x12C3A0,
         {address + 0xA4A7AE4, address + 0xA4A7380, address + 0xA4A71C5, address + 0xA4A79CB}});
}
