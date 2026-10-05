 
void record_resource_recovery(void *task, void *resource, std::uint64_t expected_bytes,
                              std::uint64_t completed_bytes,
                              dstorage_guard::ResourceRecoveryDecision decision,
                              const dstorage_guard::ResourceRecoverySnapshot &snapshot) {
    const auto sequence = g_resource_event_next.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence > k_resource_event_capacity) {
        return;
    }
    auto &event = g_resource_events[sequence - 1];
    event.commit.store((sequence << 1U) | 1U, std::memory_order_release);
    event.qpc = query_qpc();
    event.task = reinterpret_cast<std::uint64_t>(task);
    event.resource = reinterpret_cast<std::uint64_t>(resource);
    event.expected_bytes = expected_bytes;
    event.completed_bytes = completed_bytes;
    event.thread_id = GetCurrentThreadId();
    event.decision = static_cast<std::uint32_t>(decision);
    event.generation = g_loading_gate.snapshot().generation;
    event.snapshot = snapshot;
    std::atomic_thread_fence(std::memory_order_release);
    event.commit.store(sequence << 1U, std::memory_order_release);
    if (g_report_event != nullptr) {
        SetEvent(g_report_event);
    }
}

 
bool hook_resource_task_ready(void *task) {
    const auto original = g_original_resource_task_ready.load(std::memory_order_acquire);
    const auto native_complete = original != nullptr && original(task);
    if (native_complete || !g_resource_hook_armed.load(std::memory_order_acquire)) {
        return native_complete;
    }
    if (!g_loading_gate.accepts_new_work()) {
        return native_complete;
    }
    if (!is_readable_range(task, 0x138)) {
        return native_complete;
    }

    const auto *task_bytes = static_cast<const std::uint8_t *>(task);
    const auto completion_seen = task_bytes[0x120] != 0;
    const auto expected_bytes = *reinterpret_cast<const std::uint64_t *>(task_bytes + 0x128);
    const auto completed_bytes = *reinterpret_cast<const std::uint64_t *>(task_bytes + 0x130);
     
    auto *resource_path = *reinterpret_cast<void *const *>(task_bytes + 0x118);
    const auto resource_path_address = reinterpret_cast<std::uintptr_t>(resource_path);
    const auto resource_path_present = resource_path != nullptr && resource_path_address >= 0xE0 &&
                                       (resource_path_address & (alignof(void *) - 1)) == 0;
    auto *resource =
        resource_path_present ? reinterpret_cast<void *>(resource_path_address - 0xE0) : nullptr;
    const auto resource_readable = is_readable_range(resource, 0xE8);
    const auto *resource_bytes = static_cast<const std::uint8_t *>(resource);
    const auto texture_resource =
        resource_readable && *reinterpret_cast<void *const *>(resource_bytes) == g_texture_vtable;
    const auto resource_path_matches =
        resource_readable &&
        *reinterpret_cast<void *const *>(resource_bytes + 0x08) == resource_path;
    const auto resource_attached =
        resource_readable && *reinterpret_cast<void *const *>(resource_bytes + 0x50) == task;
    const auto resource_ready = resource_readable && resource_bytes[0x38] != 0;
    auto *parsed_texture =
        resource_readable ? *reinterpret_cast<void *const *>(resource_bytes + 0x70) : nullptr;
    const auto parsed_texture_present = parsed_texture != nullptr;
    const auto parsed_texture_readable =
        parsed_texture_present && is_readable_range(parsed_texture, 0x18);
    const auto runtime_texture_present =
        resource_readable && *reinterpret_cast<void *const *>(resource_bytes + 0x78) != nullptr;

    const dstorage_guard::ResourceRecoverySnapshot snapshot{
        native_complete,        completion_seen,         resource_path_present,   resource_readable,
        texture_resource,       resource_path_matches,   resource_attached,       resource_ready,
        parsed_texture_present, parsed_texture_readable, runtime_texture_present, expected_bytes,
        completed_bytes,
    };
    auto decision = dstorage_guard::decide_resource_recovery(snapshot);
    if (dstorage_guard::is_resource_recovery(decision)) {
         
        const auto snapshot_stable =
            is_readable_range(task, 0x138) && is_readable_range(resource, 0xE8) &&
            is_readable_range(parsed_texture, 0x18) &&
            *reinterpret_cast<void *const *>(task_bytes + 0x118) == resource_path &&
            task_bytes[0x120] != 0 &&
            *reinterpret_cast<const std::uint64_t *>(task_bytes + 0x128) == expected_bytes &&
            *reinterpret_cast<const std::uint64_t *>(task_bytes + 0x130) == completed_bytes &&
            *reinterpret_cast<void *const *>(resource_bytes) == g_texture_vtable &&
            *reinterpret_cast<void *const *>(resource_bytes + 0x08) == resource_path &&
            *reinterpret_cast<void *const *>(resource_bytes + 0x50) == task &&
            resource_bytes[0x38] == 0 &&
            *reinterpret_cast<void *const *>(resource_bytes + 0x70) == parsed_texture &&
            *reinterpret_cast<void *const *>(resource_bytes + 0x78) == nullptr;
        if (!snapshot_stable) {
            decision = dstorage_guard::ResourceRecoveryDecision::snapshot_changed;
        }
    }
    if (completion_seen && expected_bytes != completed_bytes) {
        g_resource_mismatches.fetch_add(1, std::memory_order_relaxed);
        record_resource_recovery(task, resource, expected_bytes, completed_bytes, decision,
                                 snapshot);
    }
    if (dstorage_guard::is_resource_recovery(decision)) {
        g_resource_recoveries.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}
