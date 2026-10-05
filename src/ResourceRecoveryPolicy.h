 
#pragma once

#include <cstdint>

namespace dstorage_guard {

enum class ResourceRecoveryDecision : std::uint32_t {
    native_complete,
    not_complete,
    no_expected_bytes,
    completed_exceeds_expected,
    missing_resource_path,
    unreadable_resource,
    not_texture,
    resource_path_mismatch,
    resource_detached,
    resource_already_ready,
    missing_parsed_texture,
    unreadable_parsed_texture,
    runtime_already_present,
    snapshot_changed,
    rescue_texture_partial,
    rescue_texture_zero,
};

struct ResourceRecoverySnapshot {
    bool native_complete{};
    bool completion_callback_seen{};
    bool resource_path_present{};
    bool resource_readable{};
    bool texture_resource{};
    bool resource_path_matches{};
    bool resource_attached{};
    bool resource_ready{};
    bool parsed_texture_present{};
    bool parsed_texture_readable{};
    bool runtime_texture_present{};
    std::uint64_t expected_bytes{};
    std::uint64_t completed_bytes{};
};

constexpr ResourceRecoveryDecision
decide_resource_recovery(const ResourceRecoverySnapshot &snapshot) {
    if (snapshot.native_complete) {
        return ResourceRecoveryDecision::native_complete;
    }
    if (!snapshot.completion_callback_seen) {
        return ResourceRecoveryDecision::not_complete;
    }
    if (snapshot.expected_bytes == 0) {
        return ResourceRecoveryDecision::no_expected_bytes;
    }
    if (snapshot.completed_bytes > snapshot.expected_bytes) {
        return ResourceRecoveryDecision::completed_exceeds_expected;
    }
    if (!snapshot.resource_path_present) {
        return ResourceRecoveryDecision::missing_resource_path;
    }
    if (!snapshot.resource_readable) {
        return ResourceRecoveryDecision::unreadable_resource;
    }
    if (!snapshot.texture_resource) {
        return ResourceRecoveryDecision::not_texture;
    }
    if (!snapshot.resource_path_matches) {
        return ResourceRecoveryDecision::resource_path_mismatch;
    }
    if (!snapshot.resource_attached) {
        return ResourceRecoveryDecision::resource_detached;
    }
    if (snapshot.resource_ready) {
        return ResourceRecoveryDecision::resource_already_ready;
    }
    if (!snapshot.parsed_texture_present) {
        return ResourceRecoveryDecision::missing_parsed_texture;
    }
    if (!snapshot.parsed_texture_readable) {
        return ResourceRecoveryDecision::unreadable_parsed_texture;
    }
    if (snapshot.runtime_texture_present) {
        return ResourceRecoveryDecision::runtime_already_present;
    }
    return snapshot.completed_bytes == 0 ? ResourceRecoveryDecision::rescue_texture_zero
                                         : ResourceRecoveryDecision::rescue_texture_partial;
}

constexpr bool is_resource_recovery(ResourceRecoveryDecision decision) {
    return decision == ResourceRecoveryDecision::rescue_texture_partial ||
           decision == ResourceRecoveryDecision::rescue_texture_zero;
}

constexpr const char *recovery_decision_name(ResourceRecoveryDecision decision) {
    switch (decision) {
    case ResourceRecoveryDecision::native_complete:
        return "native_complete";
    case ResourceRecoveryDecision::not_complete:
        return "not_complete";
    case ResourceRecoveryDecision::no_expected_bytes:
        return "no_expected_bytes";
    case ResourceRecoveryDecision::completed_exceeds_expected:
        return "completed_exceeds_expected";
    case ResourceRecoveryDecision::missing_resource_path:
        return "missing_resource_path";
    case ResourceRecoveryDecision::unreadable_resource:
        return "unreadable_resource";
    case ResourceRecoveryDecision::not_texture:
        return "not_texture";
    case ResourceRecoveryDecision::resource_path_mismatch:
        return "resource_path_mismatch";
    case ResourceRecoveryDecision::resource_detached:
        return "resource_detached";
    case ResourceRecoveryDecision::resource_already_ready:
        return "resource_already_ready";
    case ResourceRecoveryDecision::missing_parsed_texture:
        return "missing_parsed_texture";
    case ResourceRecoveryDecision::unreadable_parsed_texture:
        return "unreadable_parsed_texture";
    case ResourceRecoveryDecision::runtime_already_present:
        return "runtime_already_present";
    case ResourceRecoveryDecision::snapshot_changed:
        return "snapshot_changed";
    case ResourceRecoveryDecision::rescue_texture_partial:
        return "rescue_texture_partial";
    case ResourceRecoveryDecision::rescue_texture_zero:
        return "rescue_texture_zero";
    }
    return "unknown";
}

}  
