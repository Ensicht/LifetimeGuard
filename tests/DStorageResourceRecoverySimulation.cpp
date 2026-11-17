#include "ResourceRecoveryPolicy.h"

#include <cstdint>
#include <cstdio>

namespace {

using dstorage_guard::ResourceRecoveryDecision;
using dstorage_guard::ResourceRecoverySnapshot;
using dstorage_guard::decide_resource_recovery;
using dstorage_guard::is_resource_recovery;

struct SimulatedTexture {
    bool parsed{};
    bool runtime{};
    bool ready{};
};

ResourceRecoverySnapshot valid_snapshot(std::uint64_t completed) {
    return ResourceRecoverySnapshot{
        .native_complete = false,
        .completion_callback_seen = true,
        .resource_path_present = true,
        .resource_readable = true,
        .texture_resource = true,
        .resource_path_matches = true,
        .resource_attached = true,
        .resource_ready = false,
        .parsed_texture_present = true,
        .parsed_texture_readable = true,
        .runtime_texture_present = false,
        .expected_bytes = 0x20000,
        .completed_bytes = completed,
    };
}

bool run_worker(
    SimulatedTexture& texture,
    std::uint64_t expected,
    std::uint64_t completed,
    bool guard_enabled) {
    const bool native_complete = expected == 0 || expected == completed;
    auto snapshot = valid_snapshot(completed);
    snapshot.native_complete = native_complete;
    snapshot.resource_ready = texture.ready;
    snapshot.parsed_texture_present = texture.parsed;
    snapshot.parsed_texture_readable = texture.parsed;
    snapshot.runtime_texture_present = texture.runtime;
    snapshot.expected_bytes = expected;
    const auto decision = decide_resource_recovery(snapshot);
    const bool may_finalize = native_complete ||
        (guard_enabled && is_resource_recovery(decision));
    if (may_finalize) {
        texture.runtime = true;
        texture.ready = true;
    }
    return texture.ready;
}

bool expect(
    const char* name,
    ResourceRecoveryDecision actual,
    ResourceRecoveryDecision expected) {
    if (actual == expected) {
        return true;
    }
    std::printf("FAIL: %s decision=%u expected=%u\n", name,
        static_cast<unsigned>(actual), static_cast<unsigned>(expected));
    return false;
}

bool run_exhaustive_policy_matrix() {
    std::uint64_t checked{};
    for (std::uint32_t bits = 0; bits < (1U << 11U); ++bits) {
        for (unsigned count_case = 0; count_case < 4; ++count_case) {
            ResourceRecoverySnapshot snapshot{
                .native_complete = (bits & (1U << 0U)) != 0,
                .completion_callback_seen = (bits & (1U << 1U)) != 0,
                .resource_path_present = (bits & (1U << 2U)) != 0,
                .resource_readable = (bits & (1U << 3U)) != 0,
                .texture_resource = (bits & (1U << 4U)) != 0,
                .resource_path_matches = (bits & (1U << 5U)) != 0,
                .resource_attached = (bits & (1U << 6U)) != 0,
                .resource_ready = (bits & (1U << 7U)) != 0,
                .parsed_texture_present = (bits & (1U << 8U)) != 0,
                .parsed_texture_readable = (bits & (1U << 9U)) != 0,
                .runtime_texture_present = (bits & (1U << 10U)) != 0,
                .expected_bytes = count_case == 0 ? 0ULL : 0x20000ULL,
                .completed_bytes = count_case == 0 ? 0ULL :
                    (count_case == 1 ? 0ULL :
                    (count_case == 2 ? 0x1F000ULL : 0x21000ULL)),
            };
            const bool expected_recovery =
                !snapshot.native_complete &&
                snapshot.completion_callback_seen &&
                snapshot.expected_bytes != 0 &&
                snapshot.completed_bytes <= snapshot.expected_bytes &&
                snapshot.resource_path_present &&
                snapshot.resource_readable &&
                snapshot.texture_resource &&
                snapshot.resource_path_matches &&
                snapshot.resource_attached &&
                !snapshot.resource_ready &&
                snapshot.parsed_texture_present &&
                snapshot.parsed_texture_readable &&
                !snapshot.runtime_texture_present;
            const bool actual_recovery = is_resource_recovery(
                decide_resource_recovery(snapshot));
            ++checked;
            if (actual_recovery != expected_recovery) {
                std::printf(
                    "FAIL: exhaustive bits=0x%X count_case=%u actual=%d "
                    "expected=%d\n",
                    bits, count_case, actual_recovery, expected_recovery);
                return false;
            }
        }
    }
    std::printf("policy_matrix=PASS cases=%llu\n",
        static_cast<unsigned long long>(checked));
    return true;
}

} // namespace

int main() {
    bool passed = true;
    const auto base = valid_snapshot(0x1F000);

    passed &= expect(
        "short texture", decide_resource_recovery(base),
        ResourceRecoveryDecision::rescue_texture_partial);

    auto value = valid_snapshot(0);
    passed &= expect(
        "zero-byte terminal texture", decide_resource_recovery(value),
        ResourceRecoveryDecision::rescue_texture_zero);

    value = base;
    value.native_complete = true;
    value.completed_bytes = value.expected_bytes;
    passed &= expect(
        "native complete", decide_resource_recovery(value),
        ResourceRecoveryDecision::native_complete);

    value = base;
    value.completion_callback_seen = false;
    passed &= expect(
        "callback pending", decide_resource_recovery(value),
        ResourceRecoveryDecision::not_complete);

    value = base;
    value.completed_bytes = value.expected_bytes + 1;
    passed &= expect(
        "completed exceeds expected", decide_resource_recovery(value),
        ResourceRecoveryDecision::completed_exceeds_expected);

    value = base;
    value.resource_path_present = false;
    passed &= expect(
        "missing resource path", decide_resource_recovery(value),
        ResourceRecoveryDecision::missing_resource_path);

    value = base;
    value.resource_readable = false;
    passed &= expect(
        "unreadable resource", decide_resource_recovery(value),
        ResourceRecoveryDecision::unreadable_resource);

    value = base;
    value.texture_resource = false;
    passed &= expect(
        "non texture", decide_resource_recovery(value),
        ResourceRecoveryDecision::not_texture);

    value = base;
    value.resource_path_matches = false;
    passed &= expect(
        "path mismatch", decide_resource_recovery(value),
        ResourceRecoveryDecision::resource_path_mismatch);

    value = base;
    value.resource_attached = false;
    passed &= expect(
        "detached", decide_resource_recovery(value),
        ResourceRecoveryDecision::resource_detached);

    value = base;
    value.resource_ready = true;
    passed &= expect(
        "already ready", decide_resource_recovery(value),
        ResourceRecoveryDecision::resource_already_ready);

    value = base;
    value.parsed_texture_present = false;
    passed &= expect(
        "parse missing", decide_resource_recovery(value),
        ResourceRecoveryDecision::missing_parsed_texture);

    value = base;
    value.parsed_texture_readable = false;
    passed &= expect(
        "parse unreadable", decide_resource_recovery(value),
        ResourceRecoveryDecision::unreadable_parsed_texture);

    value = base;
    value.runtime_texture_present = true;
    passed &= expect(
        "runtime already present", decide_resource_recovery(value),
        ResourceRecoveryDecision::runtime_already_present);

    passed &= run_exhaustive_policy_matrix();

    SimulatedTexture baseline{true, false, false};
    SimulatedTexture guarded_partial{true, false, false};
    SimulatedTexture guarded_zero{true, false, false};
    const bool baseline_ready = run_worker(
        baseline, 0x20000, 0, false);
    const bool partial_ready = run_worker(
        guarded_partial, 0x20000, 0x1F000, true);
    const bool zero_ready = run_worker(
        guarded_zero, 0x20000, 0, true);
    if (baseline_ready || !partial_ready || !guarded_partial.runtime ||
        !zero_ready || !guarded_zero.runtime) {
        std::printf(
            "FAIL: worker simulation baseline=%d partial=%d zero=%d\n",
            baseline_ready, partial_ready, zero_ready);
        passed = false;
    }

    SimulatedTexture overrun{true, false, false};
    if (run_worker(overrun, 0x20000, 0x20001, true)) {
        std::printf("FAIL: byte-overrun texture was rescued\n");
        passed = false;
    }

    std::printf(
        "resource_recovery_simulation=%s baseline_stalls=1 "
        "partial_recovers=1 zero_recovers=1\n",
        passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
