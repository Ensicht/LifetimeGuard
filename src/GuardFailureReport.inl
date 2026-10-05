 
 
 
char g_install_trace[192 * 1024]{};
std::size_t g_install_trace_size{}, g_install_trace_written{};
bool g_install_trace_truncated{}, g_failure_header_written{};
dstorage_guard::FailureBudget g_failure_budget;
std::uint64_t g_last_health_key = UINT64_MAX;
std::uint64_t g_last_unpaired{}, g_last_collisions{};

void retain_install_detail(const char *text, std::size_t length) {
    const auto available = sizeof(g_install_trace) - g_install_trace_size;
    if (length > available) {
        g_install_trace_truncated = true;
        return;
    }
    std::memcpy(g_install_trace + g_install_trace_size, text, length);
    g_install_trace_size += length;
}

void append_failure_raw(const char *text, std::size_t length) {
    const auto file = CreateFileW(L"reframework\\data\\DStorageFileLifetimeGuard\\failures.log",
                                  FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        OutputDebugStringA(
            "LifetimeGuard: cannot write failures.log; check directory permissions/disk.\n");
        return;
    }
    DWORD written{};
    if (!WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr) ||
        written != length) {
        OutputDebugStringA("LifetimeGuard: incomplete failures.log write.\n");
    }
    CloseHandle(file);
}

void write_failure_header() {
    if (g_failure_header_written) {
        return;
    }
    g_failure_header_written = true;
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const auto utc = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32U) | ft.dwLowDateTime;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    char text[1024]{};
    auto n = std::snprintf(
        text, sizeof(text),
        "failure_report build=" LG_BUILD_TEXT
        " pid=%u utc_filetime=%llu record_utc_filetime=%llu qpc=%llu qpc_frequency=%llu\r\n"
        "scope=LifetimeGuard_observed_failures_only live_object_reread=0 polling=0 "
        "detail_limit_per_category=16 history_sessions=8\r\n"
        "meaning: install failures, guarded resource anomalies and failed/cancelled rereads are "
        "distinct.\r\n"
        "An intervention is NOT proof of recovery or a prevented hang. Cancelled enqueue may be a "
        "normal scene transition.\r\n"
        "No global missing-file scan; resource addresses alone cannot identify a later freed "
        "texture or the responsible Mod.\r\n",
        static_cast<unsigned>(GetCurrentProcessId()),
        static_cast<unsigned long long>(g_session_utc), static_cast<unsigned long long>(utc),
        static_cast<unsigned long long>(query_qpc()),
        static_cast<unsigned long long>(frequency.QuadPart));
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(text)) {
        append_failure_raw(text, n);
    }
    for (const auto module : {GetModuleHandleW(nullptr), GetModuleHandleW(L"dstoragecore.dll"),
                              GetModuleHandleW(L"reframework.dll"), g_module}) {
        if (module == nullptr) {
            continue;
        }
        char path[MAX_PATH * 3]{};
        module_path_from_base(reinterpret_cast<std::uint64_t>(module), path, sizeof(path));
        const auto *nt = get_nt_headers(module);
        n = std::snprintf(text, sizeof(text),
                          "module path=\"%s\" base=0x%llX pe_timestamp=0x%X image_size=0x%X\r\n",
                          path,
                          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(module)),
                          static_cast<unsigned>(nt ? nt->FileHeader.TimeDateStamp : 0),
                          static_cast<unsigned>(nt ? nt->OptionalHeader.SizeOfImage : 0));
        if (n > 0 && static_cast<std::size_t>(n) < sizeof(text)) {
            append_failure_raw(text, n);
        }
    }
}

void flush_failure_install_trace() {
    if (g_install_trace_written < g_install_trace_size) {
        append_failure_raw(g_install_trace + g_install_trace_written,
                           g_install_trace_size - g_install_trace_written);
        g_install_trace_written = g_install_trace_size;
    }
}

void write_failure_detail(dstorage_guard::DiagnosticKind kind, const char *text,
                          std::size_t length) {
    if (!g_failure_budget.admit(kind)) {
        if (g_failure_budget.take_limit_notice(kind)) {
            char notice[256]{};
            const auto n =
                std::snprintf(notice, sizeof(notice),
                              "detail_limit category=%u limit=16 further_details_suppressed=1 "
                              "recovery_unchanged=1 see=events.log_and_UI_counters\r\n",
                              static_cast<unsigned>(kind));
            if (n > 0 && static_cast<std::size_t>(n) < sizeof(notice)) {
                append_failure_raw(notice, n);
            }
        }
        return;
    }
    write_failure_header();
    flush_failure_install_trace();
    append_failure_raw(text, length);
}

void write_protection_report() {
    if (g_initialize_state.load(std::memory_order_acquire) < 2) {
        return;
    }
    static std::uint64_t cleanup_previous = UINT64_MAX;
    const auto cleanup_key =
        (static_cast<std::uint64_t>(g_loose_cleanup.failure()) << 32U) | g_install_state.load();
    if (cleanup_key != cleanup_previous) {
        cleanup_previous = cleanup_key;
        char detail[512]{};
        const bool ready = g_loose_cleanup.caller() != 0 && g_hooks_armed.load();
        const auto n = std::snprintf(
            detail, sizeof(detail),
            "loose_cleanup ready=%u profile_failure=%u caller=0x%llX expected_timestamp=0x6A7D2E58 "
            "expected_image_size=0x2079C000 policy=temporary_ref_for_native_first_release "
            "no_new_hooks=1 "
            "no_scan=1 no_wait=1 scope=loose_tex_native_cache_cleanup\r\n",
            ready, g_loose_cleanup.failure(),
            static_cast<unsigned long long>(g_loose_cleanup.caller()));
        if (n > 0 && static_cast<std::size_t>(n) < sizeof(detail)) {
            append_report(detail, n);
            if (!ready) {
                write_failure_detail(dstorage_guard::DiagnosticKind::install, detail, n);
            }
        }
    }
    const dstorage_guard::ProtectionFailureState health{g_loading_gate.lifecycle_available(),
                                                        g_install_state.load(),
                                                        g_resource_install_state.load(),
                                                        g_retry_install_state.load(),
                                                        g_iat_hook_state.load(),
                                                        g_report_thread_state.load()};
     
    if (health.reporter != 3 && g_install_trace_size == 0) {
        const auto count = g_install_event_next.load();
        for (std::uint64_t i = 0; i < count && i < k_install_event_capacity; ++i) {
            if (g_install_events[i].commit.load(std::memory_order_acquire) == (i + 1) << 1U) {
                write_install_event(g_install_events[i], i + 1);
            }
        }
    }
    const auto mask = dstorage_guard::protection_failure_mask(health);
    const auto key = static_cast<std::uint64_t>(mask) |
                     (static_cast<std::uint64_t>(health.file) << 8U) |
                     (static_cast<std::uint64_t>(health.resource) << 16U) |
                     (static_cast<std::uint64_t>(health.retry) << 24U) |
                     (static_cast<std::uint64_t>(health.iat) << 32U) |
                     (static_cast<std::uint64_t>(health.reporter) << 40U);
    if (key == g_last_health_key) {
        if (g_failure_header_written) {
            flush_failure_install_trace();
        }
        return;
    }
    g_last_health_key = key;
    const auto gate = g_loading_gate.snapshot();
    char text[3072]{};
    const auto n = std::snprintf(
        text, sizeof(text),
        "protection_health qpc=%llu failure_mask=0x%X lifecycle=0x%X file_state=%u "
        "resource_state=%u "
        "retry_state=%u iat_state=%u reporter_state=%u reporter_error=%u "
        "file_armed=%u resource_armed=%u retry_armed=%u resource_mode=%u retry_mode=%u "
        "generation=%llu loading=%u end_seen=%u install_trace_truncated=%u\r\n"
        "mask_bits=1:lifecycle,2:file,4:texture_completion,8:texture_retry,16:reporter\r\n"
        "file=%s\r\nresource=%s\r\nretry=%s\r\n"
        "mh_file_create=%d mh_file_queue=%d mh_file_apply=%d mh_resource_create=%d "
        "mh_resource_enable=%d "
        "mh_retry_create=%d mh_retry_enable=%d\r\n",
        static_cast<unsigned long long>(query_qpc()), mask, g_lifecycle_hook_state.load(),
        health.file, health.resource, health.retry, health.iat, health.reporter,
        g_report_create_error, g_hooks_armed.load(), g_resource_hook_armed.load(),
        g_retry_armed.load(), g_resource_hook_mode.load(), g_retry_hook_mode.load(),
        static_cast<unsigned long long>(gate.generation), gate.active, gate.end_seen,
        g_install_trace_truncated,
        dstorage_guard::guard_layer_state(health.file, dstorage_guard::GuardLanguage::English),
        dstorage_guard::guard_layer_state(health.resource, dstorage_guard::GuardLanguage::English),
        dstorage_guard::guard_layer_state(health.retry == 1   ? 2U
                                          : health.retry == 2 ? 1U
                                                              : health.retry,
                                          dstorage_guard::GuardLanguage::English),
        g_lifetime_create_hook_status.load(), g_lifetime_queue_hook_status.load(),
        g_lifetime_apply_hook_status.load(), g_resource_create_hook_status.load(),
        g_resource_enable_hook_status.load(), g_retry_create_status.load(),
        g_retry_enable_status.load());
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(text)) {
        append_report(text, n);
        if (mask != 0 || g_failure_header_written) {
            write_failure_detail(dstorage_guard::DiagnosticKind::install, text, n);
        }
    }
    if ((mask & 1U) != 0) {
        constexpr const char *names[] = {
            "Environment.evSceneLoadBefore", "Environment.evSceneLoadEnd", "Player.evSceneLoadEnd",
            "FastTravel.setupLoadingEvent",  "Camera.onSceneLoadFadeIn",   "UpdateMotion_callback",
            "GameFlow.get_Loading",          "SceneManager_identity"};
        for (unsigned i = 0; i < 8; ++i) {
            if ((g_lifecycle_hook_state.load() & (1U << i)) != 0) {
                continue;
            }
            const auto count = std::snprintf(text, sizeof(text),
                                             "lifecycle_missing bit=%u name=%s\r\n", i, names[i]);
            if (count > 0 && static_cast<std::size_t>(count) < sizeof(text)) {
                append_failure_raw(text, count);
            }
        }
    }
    if (g_failure_header_written) {
        flush_failure_install_trace();
    }
}

void write_failure_counters() {
    static std::uint64_t last_cleanup_pairs{};
    const auto pairs = g_loose_cleanup.paired();
    if (pairs != last_cleanup_pairs) {
        last_cleanup_pairs = pairs;
        char totals[256]{};
        const auto n = std::snprintf(totals, sizeof(totals),
                                     "loose_cleanup_totals qpc=%llu paired=%llu "
                                     "interpretation=path_uses_not_crashes_prevented\r\n",
                                     static_cast<unsigned long long>(query_qpc()),
                                     static_cast<unsigned long long>(pairs));
        if (n > 0 && static_cast<std::size_t>(n) < sizeof(totals)) {
            append_report(totals, n);
        }
    }
     
    const auto unpaired = g_unpaired_requests.load();
    const auto collisions = g_registry_collisions.load();
    if (unpaired == g_last_unpaired && collisions == g_last_collisions) {
        return;
    }
    g_last_unpaired = unpaired;
    g_last_collisions = collisions;
    char text[768]{};
    const auto n = std::snprintf(text, sizeof(text),
                                 "file_guard_diagnostic qpc=%llu unpaired=%llu "
                                 "registry_collisions=%llu guarded=%llu completed=%llu "
                                 "active=%llu deferred=%llu replayed=%llu "
                                 "interpretation=coverage_warning_not_proof_of_hang\r\n",
                                 static_cast<unsigned long long>(query_qpc()),
                                 static_cast<unsigned long long>(unpaired),
                                 static_cast<unsigned long long>(collisions),
                                 static_cast<unsigned long long>(g_guarded_requests.load()),
                                 static_cast<unsigned long long>(g_completed_requests.load()),
                                 static_cast<unsigned long long>(g_active_leases.load()),
                                 static_cast<unsigned long long>(g_deferred_closes.load()),
                                 static_cast<unsigned long long>(g_replayed_closes.load()));
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(text)) {
        write_failure_detail(dstorage_guard::DiagnosticKind::counters, text, n);
    }
}
