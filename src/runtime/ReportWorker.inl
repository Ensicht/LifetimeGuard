// 既有报告线程及启动摘要。不增加计时器，不在后台重新解引用游戏资源。
// 消费固定容量事件快照；资源异常只传标量，不从报告线程访问原生对象。
DWORD WINAPI report_thread_proc(void *) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    for (;;) {
        if (WaitForSingleObject(g_report_event, INFINITE) != WAIT_OBJECT_0) {
            g_report_create_error = GetLastError();
            g_report_thread_state.store(4, std::memory_order_release);
            write_protection_report();
            return 0;
        }
        write_texture_retry_events();
        auto last = g_event_written.load(std::memory_order_acquire);
        const auto observed_next = g_event_next.load(std::memory_order_acquire);
        const auto next = observed_next < k_event_capacity ? observed_next : k_event_capacity;
        while (last < next) {
            const auto sequence = last + 1;
            auto &event = g_events[sequence - 1];
            if (event.commit.load(std::memory_order_acquire) != sequence << 1U) {
                break;
            }
            write_event(event, sequence);
            last = sequence;
        }
        g_event_written.store(last, std::memory_order_release);
        for (std::uint64_t sequence = 1; sequence <= next; ++sequence) {
            auto &event = g_events[sequence - 1];
            if (event.replay_commit.load(std::memory_order_acquire) != sequence) {
                continue;
            }
            std::uint32_t expected{};
            if (event.replay_reported.compare_exchange_strong(
                    expected, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                write_replay_event(event, sequence);
            }
        }
        auto resource_last = g_resource_event_written.load(std::memory_order_acquire);
        const auto observed_resource_next = g_resource_event_next.load(std::memory_order_acquire);
        const auto resource_next = observed_resource_next < k_resource_event_capacity
                                       ? observed_resource_next
                                       : k_resource_event_capacity;
        while (resource_last < resource_next) {
            const auto sequence = resource_last + 1;
            auto &event = g_resource_events[sequence - 1];
            if (event.commit.load(std::memory_order_acquire) != sequence << 1U) {
                break;
            }
            write_resource_event(event, sequence);
            resource_last = sequence;
        }
        g_resource_event_written.store(resource_last, std::memory_order_release);

        auto loading_last = g_loading_event_written.load(std::memory_order_acquire);
        const auto observed_loading_next = g_loading_event_next.load(std::memory_order_acquire);
        const auto loading_next = observed_loading_next < k_loading_event_capacity
                                      ? observed_loading_next
                                      : k_loading_event_capacity;
        while (loading_last < loading_next) {
            const auto sequence = loading_last + 1;
            auto &event = g_loading_events[sequence - 1];
            if (event.commit.load(std::memory_order_acquire) != sequence << 1U) {
                break;
            }
            write_loading_event(event, sequence);
            if (event.kind == static_cast<std::uint32_t>(LoadingEventKind::end)) {
                write_failure_counters();
            }
            loading_last = sequence;
        }
        g_loading_event_written.store(loading_last, std::memory_order_release);

        auto install_last = g_install_event_written.load(std::memory_order_acquire);
        const auto observed_install_next = g_install_event_next.load(std::memory_order_acquire);
        const auto install_next = observed_install_next < k_install_event_capacity
                                      ? observed_install_next
                                      : k_install_event_capacity;
        const bool install_changed = install_last != install_next;
        while (install_last < install_next) {
            const auto sequence = install_last + 1;
            auto &event = g_install_events[sequence - 1];
            if (event.commit.load(std::memory_order_acquire) != sequence << 1U) {
                break;
            }
            write_install_event(event, sequence);
            install_last = sequence;
        }
        g_install_event_written.store(install_last, std::memory_order_release);
        if (g_protection_report_requested.exchange(false, std::memory_order_acq_rel) ||
            install_changed) {
            write_protection_report();
        }
    }
}

void write_startup_log(const char *status) {
    char buffer[1536]{};
    const auto loading = g_loading_gate.snapshot();
    const auto length = std::snprintf(
        buffer, sizeof(buffer),
        "startup version=%u pid=%u utc_filetime=%llu status=%s "
        "report_thread_state=%u report_rotation_failures=%u "
        "report_rotation_last_error=%u "
        "lifetime_state=%u iat_state=%u "
        "resource_state=%u lifecycle_state=0x%08X loading_gate=%u "
        "loading_generation=%llu loading_end_seen=%u saw_loading=%u "
        "lifetime_mh_create=%d lifetime_mh_index=%u "
        "lifetime_mh_queue=%d lifetime_mh_apply=%d "
        "resource_mh_create=%d resource_mh_enable=%d "
        "resource_mismatches=%llu resource_recoveries=%llu resource_hook_mode=%u ui_state=%u "
        "build=" LG_BUILD_TEXT "\r\n",
        k_version, static_cast<unsigned>(GetCurrentProcessId()),
        static_cast<unsigned long long>(g_session_utc), status,
        g_report_thread_state.load(std::memory_order_acquire), g_report_rotation_failures,
        g_report_rotation_last_error, g_install_state.load(std::memory_order_acquire),
        g_iat_hook_state.load(std::memory_order_acquire),
        g_resource_install_state.load(std::memory_order_acquire),
        g_lifecycle_hook_state.load(std::memory_order_acquire), loading.active ? 1U : 0U,
        static_cast<unsigned long long>(loading.generation), loading.end_seen ? 1U : 0U,
        loading.saw_loading ? 1U : 0U,
        g_lifetime_create_hook_status.load(std::memory_order_acquire),
        g_lifetime_create_hook_index.load(std::memory_order_acquire),
        g_lifetime_queue_hook_status.load(std::memory_order_acquire),
        g_lifetime_apply_hook_status.load(std::memory_order_acquire),
        g_resource_create_hook_status.load(std::memory_order_acquire),
        g_resource_enable_hook_status.load(std::memory_order_acquire),
        static_cast<unsigned long long>(g_resource_mismatches.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(g_resource_recoveries.load(std::memory_order_acquire)),
        g_resource_hook_mode.load(std::memory_order_acquire),
        g_status_ui_state.load(std::memory_order_acquire));
    if (length > 0) {
        append_report(buffer, static_cast<std::size_t>(length) < sizeof(buffer)
                                  ? static_cast<std::size_t>(length)
                                  : sizeof(buffer));
    }
}
