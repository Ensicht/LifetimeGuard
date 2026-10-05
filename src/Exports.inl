// 稳定版导出 ABI，包括离线测试入口。仅被显式调用时执行，不注册额外运行任务。

extern "C" __declspec(dllexport) bool
dstorage_file_lifetime_guard_get_texture_retry_stats(dstorage_guard::TextureRetryStats *output,
                                                     std::size_t size, std::uint32_t *state) {
    if (output == nullptr || size < sizeof(*output)) {
        return false;
    }
    *output = g_texture_retry.stats();
    if (state != nullptr) {
        *state = g_retry_install_state.load(std::memory_order_acquire);
    }
    return true;
}

// Standalone-host ABI. Game initialization with a real REFramework parameter
// never enables these entry points. They cannot rebind an installed adapter.
extern "C" __declspec(dllexport) bool dstorage_file_lifetime_guard_test_retry_profile(void *image) {
    return g_offline_test_mode.load() && image != nullptr &&
           validate_retry_profile(static_cast<HMODULE>(image));
}

extern "C" __declspec(dllexport) bool dstorage_file_lifetime_guard_test_install_texture_retry(
    const dstorage_guard::TextureRetryNativeTargets *targets, std::size_t size) {
    if (!g_offline_test_mode.load() || g_retry_armed.load() || targets == nullptr ||
        size != sizeof(*targets)) {
        return false;
    }
    std::uint32_t expected{};
    if (!g_retry_install_state.compare_exchange_strong(expected, 1)) {
        return false;
    }
    return install_texture_retry_targets(*targets);
}

extern "C" __declspec(dllexport) bool
dstorage_file_lifetime_guard_test_retry_detours(void **ready, void **release, void **destroy) {
    if (!g_offline_test_mode.load() || ready == nullptr || release == nullptr ||
        destroy == nullptr) {
        return false;
    }
    *ready = reinterpret_cast<void *>(&hook_retry_texture_ready);
    *release = reinterpret_cast<void *>(&hook_retry_resource_release);
    *destroy = reinterpret_cast<void *>(&hook_retry_texture_delete);
    return true;
}

extern "C" __declspec(dllexport) bool
dstorage_file_lifetime_guard_test_bind_texture_retry(void **texture_vtable, void **manager_slot,
                                                     NativeResourceReleaseFn release,
                                                     NativeResourceReadQueueFn queue) {
    if (!g_offline_test_mode.load() || g_retry_armed.load() || texture_vtable == nullptr ||
        manager_slot == nullptr || release == nullptr || queue == nullptr) {
        return false;
    }
    g_retry_texture_vtable = texture_vtable;
    g_retry_manager_slot = manager_slot;
    g_retry_read_queue = queue;
    g_retry_original_release.store(release);
    g_retry_original_ready.store(reinterpret_cast<NativeTextureReadyFn>(texture_vtable[10]));
    g_retry_original_delete.store(reinterpret_cast<NativeTextureDeleteFn>(texture_vtable[6]));
    g_retry_armed.store(true);
    return true;
}

extern "C" __declspec(dllexport) bool
dstorage_file_lifetime_guard_test_texture_retry(unsigned operation, void *manager, void *resource) {
    if (!g_offline_test_mode.load() || !g_retry_armed.load()) {
        return false;
    }
    if (operation == 0) {
        hook_retry_resource_release(manager, resource);
        return true;
    }
    if (operation == 1) {
        if (!g_texture_retry.terminal(manager, resource)) {
            retry_release(manager, resource);
        }
        return true;
    }
    if (operation == 2) {
        return hook_retry_texture_ready(resource);
    }
    if (operation == 3) {
        return hook_retry_texture_delete(resource, 0) == resource;
    }
    return false;
}

extern "C" __declspec(dllexport) bool
dstorage_file_lifetime_guard_test_gameplay_retry(bool enabled, std::uint64_t milliseconds) {
    if (!g_offline_test_mode.load(std::memory_order_acquire)) {
        return false;
    }
    g_retry_test_ms.store(milliseconds);
    g_retry_test_clock.store(true);
    g_retry_gameplay_enabled.store(enabled);
    return true;
}

extern "C" __declspec(dllexport) bool
dstorage_file_lifetime_guard_get_gameplay_retry_stats(dstorage_guard::GameplayRetryStats *output,
                                                      std::size_t size) {
    if (output == nullptr || size < sizeof(*output)) {
        return false;
    }
    *output = g_texture_retry.gameplay_stats();
    return true;
}

extern "C" __declspec(dllexport) bool dstorage_file_lifetime_guard_enable_resource_recovery() {
    return install_resource_recovery_hook();
}

extern "C" __declspec(dllexport) bool
dstorage_file_lifetime_guard_test_set_loading_state(bool active) {
    if (!g_offline_test_mode.load(std::memory_order_acquire)) {
        return false;
    }
    if (active) {
        mark_loading_start(LoadingEventSource::offline_test, false);
    } else {
        mark_loading_end(LoadingEventSource::offline_test, true);
    }
    return g_loading_gate.accepts_new_work() == active;
}

extern "C" __declspec(dllexport) bool dstorage_file_lifetime_guard_get_stats(GuardStats *output,
                                                                             std::size_t size) {
    if (output == nullptr || size < sizeof(GuardStats)) {
        return false;
    }
    output->version = k_version;
    output->install_state = g_install_state.load(std::memory_order_acquire);
    output->guarded_requests = g_guarded_requests.load(std::memory_order_acquire);
    output->completed_requests = g_completed_requests.load(std::memory_order_acquire);
    output->active_leases = g_active_leases.load(std::memory_order_acquire);
    output->deferred_closes = g_deferred_closes.load(std::memory_order_acquire);
    output->replayed_closes = g_replayed_closes.load(std::memory_order_acquire);
    output->unpaired_requests = g_unpaired_requests.load(std::memory_order_acquire);
    output->registry_collisions = g_registry_collisions.load(std::memory_order_acquire);
    return true;
}

extern "C" __declspec(dllexport) bool
dstorage_file_lifetime_guard_get_loading_stats(LoadingLifecycleStats *output, std::size_t size) {
    if (output == nullptr || size < sizeof(LoadingLifecycleStats)) {
        return false;
    }
    const auto snapshot = g_loading_gate.snapshot();
    output->version = k_version;
    output->lifecycle_state = g_lifecycle_hook_state.load(std::memory_order_acquire);
    output->active = snapshot.active ? 1U : 0U;
    output->requires_fade_in = snapshot.requires_fade_in ? 1U : 0U;
    output->end_seen = snapshot.end_seen ? 1U : 0U;
    output->saw_loading = snapshot.saw_loading ? 1U : 0U;
    output->generation = snapshot.generation;
    return true;
}

extern "C" __declspec(dllexport) void
reframework_plugin_required_version(REFrameworkPluginVersion *version) {
    if (version == nullptr) {
        return;
    }
    version->major = REFRAMEWORK_PLUGIN_VERSION_MAJOR;
    version->minor = REFRAMEWORK_PLUGIN_VERSION_MINOR;
    version->patch = REFRAMEWORK_PLUGIN_VERSION_PATCH;
}

extern "C" __declspec(dllexport) bool dstorage_file_lifetime_guard_test_cleanup_bind(void *image,
                                                                                     void *host) {
    if (!g_offline_test_mode.load() || !g_hooks_armed.load()) {
        return false;
    }
    g_loose_cleanup.initialize(image, host);
    return g_loose_cleanup.caller() != 0;
}
extern "C" __declspec(dllexport) std::uint64_t dstorage_file_lifetime_guard_cleanup_pairs() {
    return g_loose_cleanup.paired();
}
extern "C" __declspec(dllexport) bool
reframework_plugin_initialize(const REFrameworkPluginInitializeParam *param) {
    std::uint32_t expected{};
    if (!g_initialize_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
        return expected == 2 ||
               (expected == 3 && g_status_ui_state.load(std::memory_order_acquire) == 1);
    }
    g_loose_cleanup_host.store(param ? param->reframework_module : nullptr);
    pin_module();
    rotate_report_history();
    FILETIME session_time{};
    GetSystemTimeAsFileTime(&session_time);
    g_session_utc = (static_cast<std::uint64_t>(session_time.dwHighDateTime) << 32U) |
                    session_time.dwLowDateTime;
    g_report_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_report_event != nullptr) {
        g_report_thread_state.store(1, std::memory_order_release);
        g_report_thread = CreateThread(nullptr, 0, &report_thread_proc, nullptr, 0, nullptr);
        if (g_report_thread != nullptr) {
            g_report_thread_state.store(3, std::memory_order_release);
        } else {
            g_report_create_error = GetLastError();
        }
    } else {
        g_report_create_error = GetLastError();
    }

    install_status_ui(param);
    bool lifecycle_available{};
    if (param == nullptr) {
        g_offline_test_mode.store(true, std::memory_order_release);
        g_retry_gameplay_enabled.store(false, std::memory_order_release);
        g_lifecycle_hook_state.store(k_lifecycle_offline_test, std::memory_order_release);
        g_loading_gate.configure(true, true);
        lifecycle_available = true;
    } else {
        lifecycle_available = install_loading_lifecycle_hooks(param);
    }
    if (!lifecycle_available) {
        write_startup_log("lifecycle_hooks_missing_bypass");
        g_initialize_state.store(2, std::memory_order_release);
        request_protection_report();
        if (g_report_thread == nullptr) {
            write_protection_report();
        }
        return true;
    }

    auto installed = install_internal_hooks();
    const auto wait_for_factory =
        !installed && g_install_state.load(std::memory_order_acquire) == 0;
    const auto iat_installed = wait_for_factory && install_iat_hook();
    g_iat_hook_state.store(wait_for_factory ? (iat_installed ? 1U : 2U) : 0U,
                           std::memory_order_release);
    if (iat_installed) {
        install_internal_hooks();
        installed = g_install_state.load(std::memory_order_acquire) == 1;
    }
    const auto resource_installed = installed && install_resource_recovery_hook();
    const auto retry_installed = installed && install_texture_retry_hooks();
    char retry_status[192]{};
    const auto retry_length = std::snprintf(retry_status, sizeof(retry_status),
                                            "texture_retry_install installed=%u state=%u create=%d "
                                            "enable=%d mode=%u build=" LG_BUILD_TEXT "\r\n",
                                            retry_installed ? 1U : 0U, g_retry_install_state.load(),
                                            g_retry_create_status.load(),
                                            g_retry_enable_status.load(), g_retry_hook_mode.load());
    if (retry_length > 0 && static_cast<std::size_t>(retry_length) < sizeof(retry_status)) {
        append_report(retry_status, retry_length);
    }
    const auto success =
        installed || (iat_installed && g_install_state.load(std::memory_order_acquire) == 0);
    write_startup_log(
        installed && resource_installed && retry_installed
            ? "all_hooks_installed_gameplay_texture_retry"
            : (installed && resource_installed
                   ? "original_layers_only_loading_only"
                   : (installed ? "lifetime_only_loading_only"
                                : (success ? "waiting_for_factory" : "install_failed"))));
    g_initialize_state.store(success ? 2U : 3U, std::memory_order_release);
    request_protection_report();
    if (g_report_thread == nullptr) {
        write_protection_report();
    }
    // Keep the read-only status panel available even if protection failed.
    return success || g_status_ui_state.load(std::memory_order_acquire) == 1;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
