// 原生纹理重读适配：验证精确指令和回调所有权，不把恢复任务永久挂起。
// Included inside the plugin's private namespace, after native hook helpers.
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
std::atomic<unsigned> g_retry_hook_mode{}; // 1: near trampoline, 2: verified fixed16 fallback.
std::atomic<int> g_retry_enable_status{k_mh_status_not_attempted};
std::uintptr_t g_retry_terminal_callers[4]{};
void *g_retry_texture_vtable{};
void **g_retry_manager_slot{};
NativeResourceReadQueueFn g_retry_read_queue{};
std::atomic<bool> g_retry_gameplay_enabled{true};
std::atomic<bool> g_retry_test_clock{};
std::atomic<std::uint64_t> g_retry_test_ms{};

// 加载期与游玩期分别放行；启动未取得有效代次时，不提前处理未初始化资源。
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
    // Resource Release/ready callers already own a reference. A receipt is
    // never used as authority to dereference an arbitrary cached address.
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
    // This native function has no recoverable allocation-failure return.
    // It owns EAC/list bookkeeping; never invent success by setting ready.
    return true;
}

bool retry_wake(void *manager) {
    // Same queue notification as native ResourceManager.create_resource.
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

// 仅四个已验证的终态调用点允许转移引用；其余 Release 原样转发。
void hook_retry_resource_release(void *manager, void *resource) {
    const auto caller = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    if (g_retry_armed.load(std::memory_order_acquire) && retry_terminal_caller(caller) &&
        g_texture_retry.terminal(manager, resource)) {
        return;
    }
    retry_release(manager, resource);
}

// 复用原生查询机会触发有界重读，不修改本次 ready 返回值，也不增加扫描。
bool hook_retry_texture_ready(void *resource) {
    const bool ready = g_retry_original_ready.load(std::memory_order_acquire)(resource);
    if (!ready && g_retry_armed.load(std::memory_order_acquire)) {
        g_texture_retry.queried(resource);
    }
    // A retry never changes the return value of this particular ready query.
    return ready;
}

// 对象销毁时注销对应记录；地址复用后不能沿用旧纹理的恢复状态。
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

// 入口、调用点、队列及引用指令共同授权；游戏版本元数据不决定兼容性。
bool validate_retry_profile(HMODULE module) {
    return dstorage_guard::find_native_texture_profile(module) != nullptr;
}

// 主 Hook 内存分配失败时才使用固定入口后备方案；冲突回滚不覆盖其他插件。
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
    // Originals/trampoline remain pinned even on failure: a racing native
    // caller may have fetched one of the now-unarmed forwarding wrappers.
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
    const auto *profile = g_loading_gate.lifecycle_available()
                              ? dstorage_guard::find_native_texture_profile(module)
                              : nullptr;
    const auto profile_valid = profile != nullptr;
    record_install_event(InstallComponent::texture_retry, InstallStage::validate_target, 0, module,
                         nullptr, nullptr, profile_valid ? 0 : 1, 0);
    if (!profile_valid) {
        g_retry_install_state.store(3);
        return false;
    }
    auto *base = reinterpret_cast<std::uint8_t *>(module);
    const auto address = reinterpret_cast<std::uintptr_t>(base);
    return install_texture_retry_targets(
        {reinterpret_cast<void **>(base + profile->vtable),
         reinterpret_cast<void **>(base + profile->manager),
         base + profile->release,
         base + profile->queue,
         base + profile->destroy,
         base + profile->ready,
         {address + profile->terminal_calls[0] + 5, address + profile->terminal_calls[1] + 5,
          address + profile->terminal_calls[2] + 5, address + profile->terminal_calls[3] + 5}});
}
