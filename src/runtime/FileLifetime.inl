// 文件租约、Close 延迟和请求配对。保留锁顺序、原子内存序以及 AddRef/Release 配对。
std::uint64_t hash_pair(std::uint64_t first, std::uint64_t second) {
    auto value = first ^ ((second << 23U) | (second >> 41U));
    value ^= value >> 30U;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27U;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31U;
    return value <= k_tombstone_key ? 2 : value;
}

SRWLOCK *get_file_lock(void *wrapper, void *internal_file) {
    const auto key = hash_pair(reinterpret_cast<std::uint64_t>(wrapper),
                               reinterpret_cast<std::uint64_t>(internal_file));
    return &g_file_locks[key & (k_file_lock_count - 1)];
}

class ExclusiveFileLock {
  public:
    ExclusiveFileLock(void *wrapper, void *internal_file)
        : lock_(get_file_lock(wrapper, internal_file)) {
        AcquireSRWLockExclusive(lock_);
    }

    ~ExclusiveFileLock() {
        ReleaseSRWLockExclusive(lock_);
    }

    ExclusiveFileLock(const ExclusiveFileLock &) = delete;
    ExclusiveFileLock &operator=(const ExclusiveFileLock &) = delete;

  private:
    SRWLOCK *lock_{};
};

void *get_internal_file(void *wrapper) {
    if (wrapper == nullptr) {
        return nullptr;
    }
    return *reinterpret_cast<void **>(static_cast<std::uint8_t *>(wrapper) + 0x20);
}

std::uint64_t get_internal_handle(void *internal_file) {
    if (internal_file == nullptr) {
        return 0;
    }
    return *reinterpret_cast<const std::uint64_t *>(
        static_cast<const std::uint8_t *>(internal_file) + 0x08);
}

ULONG add_ref(void *wrapper) {
    auto **vtable = *reinterpret_cast<void ***>(wrapper);
    return reinterpret_cast<ComRefFn>(vtable[1])(wrapper);
}

ULONG release_ref(void *wrapper) {
    auto **vtable = *reinterpret_cast<void ***>(wrapper);
    return reinterpret_cast<ComRefFn>(vtable[2])(wrapper);
}

FileState *find_file_state(void *wrapper, void *internal_file, bool create) {
    const auto wrapper_value = reinterpret_cast<std::uint64_t>(wrapper);
    const auto internal_value = reinterpret_cast<std::uint64_t>(internal_file);
    const auto key = hash_pair(wrapper_value, internal_value);
    const auto start = static_cast<std::size_t>(key) & (k_file_capacity - 1);
    const auto publish = [key, wrapper_value, internal_value](
                             FileState *target, std::uint64_t expected) -> FileState * {
        if (!target->key.compare_exchange_strong(expected, key, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            return nullptr;
        }
        target->wrapper.store(wrapper_value, std::memory_order_relaxed);
        target->internal_file.store(internal_value, std::memory_order_relaxed);
        target->state.store(0, std::memory_order_release);
        target->event_sequence.store(0, std::memory_order_relaxed);
        return target;
    };
    for (std::size_t restart = 0; restart < k_file_probe_limit; ++restart) {
        FileState *tombstone{};
        bool retry{};
        for (std::size_t probe = 0; probe < k_file_probe_limit; ++probe) {
            auto &slot = g_files[(start + probe) & (k_file_capacity - 1)];
            const auto observed = slot.key.load(std::memory_order_acquire);
            if (observed == key && slot.wrapper.load(std::memory_order_acquire) == wrapper_value &&
                slot.internal_file.load(std::memory_order_acquire) == internal_value) {
                return &slot;
            }
            if (observed == k_tombstone_key) {
                if (tombstone == nullptr) {
                    tombstone = &slot;
                }
                continue;
            }
            if (observed != k_empty_key) {
                continue;
            }
            if (!create) {
                return nullptr;
            }
            auto *target = tombstone != nullptr ? tombstone : &slot;
            auto expected = tombstone != nullptr ? k_tombstone_key : k_empty_key;
            if (auto *published = publish(target, expected)) {
                return published;
            } else {
                retry = true;
                break;
            }
        }
        if (retry) {
            continue;
        }
        if (create && tombstone != nullptr) {
            if (auto *published = publish(tombstone, k_tombstone_key)) {
                return published;
            }
            continue;
        }
        if (create) {
            g_registry_collisions.fetch_add(1, std::memory_order_relaxed);
        }
        return nullptr;
    }
    g_registry_collisions.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void recycle_file_state(FileState *file_state) {
    file_state->state.store(0, std::memory_order_relaxed);
    file_state->event_sequence.store(0, std::memory_order_relaxed);
    file_state->wrapper.store(0, std::memory_order_relaxed);
    file_state->internal_file.store(0, std::memory_order_relaxed);
    file_state->key.store(k_tombstone_key, std::memory_order_release);
}

// 在 Close 与请求登记之间持有同一文件锁，先取得租约再允许原请求入队。
FileState *acquire_file_lease(void *wrapper, void *internal_file, bool require_open_handle = true) {
    if (wrapper == nullptr || internal_file == nullptr) {
        return nullptr;
    }
    ExclusiveFileLock lock(wrapper, internal_file);
    if (require_open_handle && get_internal_handle(internal_file) == 0) {
        return nullptr;
    }
    add_ref(wrapper);
    auto *file_state = find_file_state(wrapper, internal_file, true);
    if (file_state == nullptr) {
        release_ref(wrapper);
        return nullptr;
    }
    auto state = file_state->state.load(std::memory_order_acquire);
    for (;;) {
        const auto count = state & k_count_mask;
        if (count == k_count_mask || (state & k_close_claimed) != 0) {
            release_ref(wrapper);
            g_registry_collisions.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        if (file_state->state.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            g_active_leases.fetch_add(1, std::memory_order_acq_rel);
            return file_state;
        }
    }
}

std::uint64_t record_deferred_close(void *wrapper, void *internal_file, std::uint32_t outstanding) {
    const auto sequence = g_event_next.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence > k_event_capacity) {
        return 0;
    }
    auto &event = g_events[sequence - 1];
    event.commit.store((sequence << 1U) | 1U, std::memory_order_release);
    event.replay_commit.store(0, std::memory_order_relaxed);
    event.replay_reported.store(0, std::memory_order_relaxed);
    event.qpc = query_qpc();
    event.replay_qpc = 0;
    event.wrapper = reinterpret_cast<std::uint64_t>(wrapper);
    event.internal_file = reinterpret_cast<std::uint64_t>(internal_file);
    event.thread_id = GetCurrentThreadId();
    event.replay_thread_id = 0;
    event.outstanding = outstanding;
    void *frames[k_stack_capacity]{};
    event.stack_count =
        CaptureStackBackTrace(0, static_cast<DWORD>(k_stack_capacity), frames, nullptr);
    for (std::size_t index = 0; index < event.stack_count; ++index) {
        event.stack[index] = reinterpret_cast<std::uint64_t>(frames[index]);
    }
    std::atomic_thread_fence(std::memory_order_release);
    event.commit.store(sequence << 1U, std::memory_order_release);
    if (g_report_event != nullptr) {
        SetEvent(g_report_event);
    }
    return sequence;
}

void record_replayed_close(FileState *file_state) {
    const auto sequence = file_state->event_sequence.load(std::memory_order_acquire);
    if (sequence == 0 || sequence > k_event_capacity) {
        return;
    }
    auto &event = g_events[sequence - 1];
    event.replay_qpc = query_qpc();
    event.replay_thread_id = GetCurrentThreadId();
    std::atomic_thread_fence(std::memory_order_release);
    event.replay_commit.store(sequence, std::memory_order_release);
    if (g_report_event != nullptr) {
        SetEvent(g_report_event);
    }
}

// 最后一个在途租约释放时重放被延迟的 Close，再释放对应引用。
void complete_file_lease(FileState *file_state, void *wrapper) {
    if (file_state == nullptr || wrapper == nullptr) {
        return;
    }
    bool replay_close{};
    auto *internal_file =
        reinterpret_cast<void *>(file_state->internal_file.load(std::memory_order_acquire));
    {
        ExclusiveFileLock lock(wrapper, internal_file);
        auto state = file_state->state.load(std::memory_order_acquire);
        for (;;) {
            const auto count = state & k_count_mask;
            if (count == 0) {
                g_registry_collisions.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            auto next = state - 1;
            if (count == 1 && (state & k_close_pending) != 0 && (state & k_close_claimed) == 0) {
                next |= k_close_claimed;
                replay_close = true;
            }
            if (file_state->state.compare_exchange_weak(state, next, std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
                break;
            }
            replay_close = false;
        }
        if (replay_close) {
            const auto original = g_original_file_close.load(std::memory_order_acquire);
            if (original != nullptr) {
                original(wrapper);
                g_replayed_closes.fetch_add(1, std::memory_order_relaxed);
                record_replayed_close(file_state);
            }
        }
        if ((file_state->state.load(std::memory_order_acquire) & k_count_mask) == 0) {
            recycle_file_state(file_state);
        }
    }
    g_active_leases.fetch_sub(1, std::memory_order_acq_rel);
    release_ref(wrapper);
}

// 以请求身份和代次登记租约，避免地址复用把旧完成回调配到新请求。
bool publish_request_lease(void *request, std::uint64_t request_id, void *wrapper,
                           void *internal_file, FileState *file_state) {
    const auto start = request_id & (k_request_capacity - 1);
    for (std::size_t probe = 0; probe < k_request_probe_limit; ++probe) {
        auto &slot = g_requests[(start + probe) & (k_request_capacity - 1)];
        auto expected = slot.state.load(std::memory_order_acquire);
        if ((expected & k_request_status_mask) != k_request_status_empty) {
            continue;
        }
        const auto busy = (expected & ~k_request_status_mask) | k_request_status_busy;
        if (!slot.state.compare_exchange_strong(expected, busy, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            continue;
        }
        slot.request_id.store(request_id, std::memory_order_relaxed);
        slot.request.store(reinterpret_cast<std::uint64_t>(request), std::memory_order_relaxed);
        slot.wrapper.store(reinterpret_cast<std::uint64_t>(wrapper), std::memory_order_relaxed);
        slot.internal_file.store(reinterpret_cast<std::uint64_t>(internal_file),
                                 std::memory_order_relaxed);
        slot.file_state.store(file_state, std::memory_order_relaxed);
        slot.state.store((busy & ~k_request_status_mask) | k_request_status_live,
                         std::memory_order_release);
        g_guarded_requests.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_registry_collisions.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// 只完成匹配的租约；重复、过时或未配对回调不释放其他请求的引用。
void finish_request_lease(std::uint64_t request_id, void *internal_file) {
    const auto start = request_id & (k_request_capacity - 1);
    const auto internal_file_key = reinterpret_cast<std::uint64_t>(internal_file);
    for (std::size_t probe = 0; probe < k_request_probe_limit; ++probe) {
        auto &slot = g_requests[(start + probe) & (k_request_capacity - 1)];
        auto expected = slot.state.load(std::memory_order_acquire);
        if ((expected & k_request_status_mask) != k_request_status_live ||
            slot.request_id.load(std::memory_order_relaxed) != request_id ||
            slot.internal_file.load(std::memory_order_relaxed) != internal_file_key) {
            continue;
        }
        const auto busy = (expected & ~k_request_status_mask) | k_request_status_busy;
        if (!slot.state.compare_exchange_strong(expected, busy, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            continue;
        }
        auto *wrapper = reinterpret_cast<void *>(slot.wrapper.load(std::memory_order_relaxed));
        auto *file_state = slot.file_state.load(std::memory_order_relaxed);
        slot.request_id.store(0, std::memory_order_relaxed);
        slot.request.store(0, std::memory_order_relaxed);
        slot.wrapper.store(0, std::memory_order_relaxed);
        slot.internal_file.store(0, std::memory_order_relaxed);
        slot.file_state.store(nullptr, std::memory_order_relaxed);
        const auto next_generation = (busy & ~k_request_status_mask) + k_request_generation_step;
        slot.state.store(next_generation | k_request_status_empty, std::memory_order_release);
        complete_file_lease(file_state, wrapper);
        g_completed_requests.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

// 只修已验证的松散纹理清理调用点；加载期关闭另由在途租约延迟。
void hook_public_file_close(void *wrapper) {
    const auto original = g_original_file_close.load(std::memory_order_acquire);
    if (original == nullptr || wrapper == nullptr) {
        return;
    }
    if (!g_hooks_armed.load(std::memory_order_acquire)) {
        original(wrapper);
        return;
    }
    g_loose_cleanup.pair(wrapper, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
    auto *internal_file = get_internal_file(wrapper);
    ExclusiveFileLock lock(wrapper, internal_file);
    if (!g_loading_gate.accepts_new_work() &&
        g_active_leases.load(std::memory_order_acquire) == 0) {
        original(wrapper);
        return;
    }
    auto *file_state = find_file_state(wrapper, internal_file, false);
    if (file_state == nullptr) {
        original(wrapper);
        return;
    }
    auto state = file_state->state.load(std::memory_order_acquire);
    for (;;) {
        const auto count = state & k_count_mask;
        if (count == 0) {
            recycle_file_state(file_state);
            original(wrapper);
            return;
        }
        if ((state & k_close_pending) != 0) {
            return;
        }
        const auto next = state | k_close_pending;
        if (file_state->state.compare_exchange_weak(state, next, std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            g_deferred_closes.fetch_add(1, std::memory_order_relaxed);
            const auto event_sequence =
                record_deferred_close(wrapper, internal_file, static_cast<std::uint32_t>(count));
            file_state->event_sequence.store(event_sequence, std::memory_order_release);
            return;
        }
    }
}

// 正常游玩无新增文件租约。加载结束后仍允许已登记请求完成排空。
void hook_public_enqueue_request(void *self, const void *descriptor) {
    const auto original = g_original_enqueue.load(std::memory_order_acquire);
    if (original == nullptr) {
        return;
    }
    if (!g_hooks_armed.load(std::memory_order_acquire) || !g_loading_gate.accepts_new_work()) {
        original(self, descriptor);
        return;
    }
    const auto *bytes = static_cast<const std::uint8_t *>(descriptor);
    if (bytes == nullptr || (*reinterpret_cast<const std::uint64_t *>(bytes + 0x08) & 1ULL) != 0) {
        original(self, descriptor);
        return;
    }
    auto *wrapper = *reinterpret_cast<void *const *>(bytes + 0x10);
    auto *internal_file = get_internal_file(wrapper);
    auto *file_state = acquire_file_lease(wrapper, internal_file);
    if (file_state == nullptr) {
        original(self, descriptor);
        return;
    }
    PendingEnqueue pending{wrapper, internal_file, file_state, false, g_pending_enqueue};
    g_pending_enqueue = &pending;
    original(self, descriptor);
    g_pending_enqueue = pending.previous;
    if (!pending.consumed) {
        g_unpaired_requests.fetch_add(1, std::memory_order_relaxed);
        complete_file_lease(file_state, wrapper);
    }
}

// 只接收当前线程入队上下文中的租约，不跨线程猜测请求归属。
void *hook_request_ctor(void *self, const void *source) {
    const auto original = g_original_request_ctor.load(std::memory_order_acquire);
    auto *result = original != nullptr ? original(self, source) : self;
    if (!g_hooks_armed.load(std::memory_order_acquire)) {
        return result;
    }
    auto *pending = g_pending_enqueue;
    const auto *bytes = static_cast<const std::uint8_t *>(self);
    if (pending == nullptr || pending->consumed || bytes == nullptr ||
        (*reinterpret_cast<const std::uint64_t *>(bytes + 0x10) & 1ULL) != 0) {
        return result;
    }
    auto *internal_file = *reinterpret_cast<void *const *>(bytes + 0x18);
    if (internal_file != pending->internal_file) {
        return result;
    }
    const auto request_id = *reinterpret_cast<const std::uint64_t *>(bytes);
    if (publish_request_lease(self, request_id, pending->wrapper, internal_file,
                              pending->file_state)) {
        pending->consumed = true;
    }
    return result;
}

// 先执行原完成流程，再处理已绑定租约；保持返回值与原生语义。
void *hook_request_try_complete(void *self, void *optional_result) {
    const auto original = g_original_try_complete.load(std::memory_order_acquire);
    const auto may_have_tracked_request =
        g_hooks_armed.load(std::memory_order_acquire) &&
        (g_loading_gate.accepts_new_work() || g_active_leases.load(std::memory_order_acquire) != 0);
    if (!may_have_tracked_request) {
        return original != nullptr ? original(self, optional_result) : optional_result;
    }
    const auto request_id = self != nullptr ? *reinterpret_cast<const std::uint64_t *>(self) : 0;
    const auto is_file_request =
        self != nullptr &&
        (*reinterpret_cast<const std::uint64_t *>(static_cast<const std::uint8_t *>(self) + 0x10) &
         1ULL) == 0;
    auto *internal_file = self != nullptr
                              ? *reinterpret_cast<void **>(static_cast<std::uint8_t *>(self) + 0x18)
                              : nullptr;
    auto *result = original != nullptr ? original(self, optional_result) : optional_result;
    const auto *bytes = static_cast<const std::uint8_t *>(optional_result);
    if (is_file_request && bytes != nullptr && bytes[4] != 0) {
        finish_request_lease(request_id, internal_file);
    }
    return result;
}
