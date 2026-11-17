// 失败纹理重读资格与令牌状态：地址相同不代表同一份资源。
#pragma once

#include <cstdint>

namespace dstorage_guard {

enum class DetachedTextureDecision : std::uint32_t {
    loading_closed,
    generation_changed,
    native_ready,
    task_still_attached,
    runtime_present,
    unverified_terminal_state,
    invalid_texture_descriptor,
    identity_changed,
    already_attempted,
    eligible,
};

struct DetachedTextureSnapshot {
    std::uintptr_t resource{};
    std::uintptr_t parsed{};
    std::uint64_t generation{};
    bool loading_active{};
    bool resource_ready{};
    bool task_attached{};
    bool runtime_present{};
    bool terminal_state_verified{};
    bool descriptor_verified{};
    bool attempted{};
};

// An empty task pointer, a readable allocation, or elapsed time alone is NOT
// terminal evidence. The native adapter must prove that parsing/IO has ended
// and serialize ownership against native parsing, finalization and unload.
constexpr DetachedTextureDecision decide_detached_texture(const DetachedTextureSnapshot &observed,
                                                          const DetachedTextureSnapshot &current) {
    if (!observed.loading_active || !current.loading_active) {
        return DetachedTextureDecision::loading_closed;
    }
    if (observed.generation == 0 || observed.generation != current.generation) {
        return DetachedTextureDecision::generation_changed;
    }
    if (observed.resource_ready || current.resource_ready) {
        return DetachedTextureDecision::native_ready;
    }
    if (observed.task_attached || current.task_attached) {
        return DetachedTextureDecision::task_still_attached;
    }
    if (observed.runtime_present || current.runtime_present) {
        return DetachedTextureDecision::runtime_present;
    }
    if (!observed.terminal_state_verified || !current.terminal_state_verified) {
        return DetachedTextureDecision::unverified_terminal_state;
    }
    if (!observed.descriptor_verified || !current.descriptor_verified) {
        return DetachedTextureDecision::invalid_texture_descriptor;
    }
    if (observed.resource == 0 || observed.parsed == 0 || observed.resource != current.resource ||
        observed.parsed != current.parsed) {
        return DetachedTextureDecision::identity_changed;
    }
    if (observed.attempted || current.attempted) {
        return DetachedTextureDecision::already_attempted;
    }
    return DetachedTextureDecision::eligible;
}

enum class DetachedQueueResult : std::uint32_t {
    rejected,
    ownership_unavailable,
    changed_after_acquire,
    queue_failed,
    queued,
};

// try_acquire is nonblocking and owns one native reference plus task binding.
// It consumes the attempt for this identity/generation even if enqueue fails,
// so a failing allocation/queue cannot cause a retry storm on readiness queries.
// enqueue transfers them to the native worker only on success. No finalizer
// or ready-bit write is allowed on this caller/consumer thread.
template <class Operations>
DetachedQueueResult queue_detached_texture(const DetachedTextureSnapshot &observed,
                                           Operations &operations) {
    if (decide_detached_texture(observed, operations.snapshot()) !=
        DetachedTextureDecision::eligible) {
        return DetachedQueueResult::rejected;
    }
    if (!operations.try_acquire(observed)) {
        return DetachedQueueResult::ownership_unavailable;
    }
    // The adapter omits its own reserved task and consumed attempt here.
    if (decide_detached_texture(observed, operations.owned_snapshot()) !=
        DetachedTextureDecision::eligible) {
        operations.rollback();
        return DetachedQueueResult::changed_after_acquire;
    }
    if (!operations.enqueue()) {
        operations.rollback();
        return DetachedQueueResult::queue_failed;
    }
    return DetachedQueueResult::queued;
}

} // namespace dstorage_guard
