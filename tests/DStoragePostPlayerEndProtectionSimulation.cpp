#include "LoadingGatePolicy.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <random>

namespace {

enum class CloseBoundary {
    player_load_end,
    stable_scene,
    scene_fade_in,
};

struct PipelineResult {
    bool request_tracked{};
    bool late_request_tracked{};
    bool close_deferred{};
    bool resource_recovered{};
    bool resource_ready{};
    bool permanent_wait{};
    bool zero_leases_before_resource_worker{};
    bool fade_reached{};
    bool normal_play_bypass{};
};

class TransitionGate {
public:
    explicit TransitionGate(CloseBoundary boundary) : boundary_(boundary) {
        gate_.configure(true, false);
    }

    void environment_start() noexcept {
        gate_.mark_start(boundary_ == CloseBoundary::scene_fade_in);
    }

    void player_load_end() noexcept {
        // Stable BSFN/AVM treat load-end as evidence only. Their actual
        // release is deferred until Loading is false and a live Scene exists.
        if (boundary_ == CloseBoundary::player_load_end) {
            gate_.mark_end(false);
        } else {
            gate_.mark_end_signal(false);
        }
    }

    void stable_scene_ready() noexcept {
        if (boundary_ == CloseBoundary::stable_scene) {
            gate_.try_end_stable_scene(false, true);
        }
    }

    void scene_fade_in() noexcept {
        gate_.mark_end(true);
    }

    bool accepts_new_work() const noexcept {
        return gate_.accepts_new_work();
    }

private:
    CloseBoundary boundary_;
    dstorage_guard::LoadingGate gate_{};
};

PipelineResult run_pipeline(
    CloseBoundary boundary, bool complete_before_player_end,
    bool enqueue_late_child, bool mismatch_after_player_end) {
    TransitionGate gate(boundary);
    gate.environment_start();

    PipelineResult result{};
    result.request_tracked = gate.accepts_new_work();
    std::uint32_t active_leases = result.request_tracked ? 1U : 0U;

    if (complete_before_player_end && active_leases != 0) {
        --active_leases;
    }

    gate.player_load_end();

    if (enqueue_late_child) {
        result.late_request_tracked = gate.accepts_new_work();
        if (result.late_request_tracked) {
            ++active_leases;
        }
    }

    // A shared file Close is protected only while a tracked lease exists.
    result.close_deferred = active_leases != 0;

    // DirectStorage completion and the later resource worker are separate
    // stages. The worker can observe a completed task after the file lease has
    // drained and after PlayerManager's load-end callback.
    if (active_leases != 0) {
        active_leases = 0;
    }
    result.zero_leases_before_resource_worker = active_leases == 0;
    if (mismatch_after_player_end) {
        result.resource_recovered = gate.accepts_new_work();
        result.resource_ready = result.resource_recovered;
        result.permanent_wait = !result.resource_ready;
    } else {
        result.resource_ready = true;
    }

    // SceneLoadFadeIn is downstream of the resource/instantiation waits. A
    // permanently non-terminal resource prevents the transition from reaching
    // the callback that closes a fade-bound gate.
    if (result.resource_ready) {
        // This models the exact ordinary-load release boundary used by stable
        // BSFN/AVM: GameFlowManager.Loading is false and SceneManager supplies
        // a live identity. The later fade remains a no-op for this policy.
        gate.stable_scene_ready();
        gate.scene_fade_in();
        result.fade_reached = true;
        result.normal_play_bypass = !gate.accepts_new_work();
    }
    return result;
}

bool expect(bool condition, const char* name) {
    std::printf("case=%s result=%s\n", name, condition ? "PASS" : "FAIL");
    return condition;
}

bool run_randomized_window_check(CloseBoundary boundary) {
    std::mt19937_64 random(0xD570A6EULL);
    std::uniform_int_distribution<int> coin(0, 1);
    for (int iteration = 0; iteration < 250000; ++iteration) {
        const bool complete_before_end = coin(random) != 0;
        const bool late_child = coin(random) != 0;
        const auto result = run_pipeline(
            boundary, complete_before_end, late_child, true);
        if (boundary == CloseBoundary::scene_fade_in ||
            boundary == CloseBoundary::stable_scene) {
            if (!result.resource_ready || result.permanent_wait ||
                !result.fade_reached || !result.normal_play_bypass ||
                (late_child && !result.late_request_tracked)) {
                return false;
            }
        } else if (result.resource_ready || !result.permanent_wait ||
                   (late_child && result.late_request_tracked)) {
            return false;
        }
    }
    return true;
}

bool run_pre_player_end_recovery() {
    TransitionGate gate(CloseBoundary::player_load_end);
    gate.environment_start();
    std::uint32_t active_leases = 1;
    --active_leases;
    const bool recovered = active_leases == 0 && gate.accepts_new_work();
    gate.player_load_end();
    if (recovered) {
        gate.scene_fade_in();
    }
    return recovered && !gate.accepts_new_work();
}

bool reproduce_close_before_first_lease_gap() {
    bool file_open = true;
    bool file_state_exists = false;
    bool lease_acquired = false;

    // Enqueue has entered the hook but has not reached acquire_file_lease.
    // Close wins the shared file lock and sees no published FileState.
    if (!file_state_exists) {
        file_open = false;
    }

    // Enqueue resumes. acquire_file_lease rejects the now-zero handle and the
    // original request is submitted without a lease or a deferred-close event.
    if (file_open) {
        file_state_exists = true;
        lease_acquired = true;
    }
    return !file_open && !file_state_exists && !lease_acquired;
}

bool reproduce_overlapping_generation_gap() {
    dstorage_guard::LoadingGate gate;
    gate.configure(true, false);
    std::uint64_t first_generation{};
    std::uint64_t second_generation{};
    const bool first_started = gate.mark_start(false, &first_generation);
    const bool second_started = gate.mark_start(false, &second_generation);
    const bool first_ended = gate.mark_end(false);
    return first_started && !second_started && first_ended &&
        first_generation == second_generation && !gate.accepts_new_work();
}

} // namespace

int main() {
    bool passed = true;

    passed &= expect(
        run_pre_player_end_recovery(),
        "current_policy_can_recover_before_player_end");
    passed &= expect(
        reproduce_close_before_first_lease_gap(),
        "current_policy_close_can_win_before_first_lease");
    passed &= expect(
        reproduce_overlapping_generation_gap(),
        "current_gate_cannot_represent_overlapping_generations");

    const auto current = run_pipeline(
        CloseBoundary::player_load_end, true, false, true);
    passed &= expect(
        current.request_tracked && !current.resource_recovered &&
            current.permanent_wait &&
            current.zero_leases_before_resource_worker,
        "current_policy_misses_post_player_end_resource_terminal");

    const auto current_late = run_pipeline(
        CloseBoundary::player_load_end, true, true, true);
    passed &= expect(
        !current_late.late_request_tracked && !current_late.close_deferred &&
            current_late.permanent_wait,
        "current_policy_bypasses_post_player_end_child_request");

    const auto fade_bound = run_pipeline(
        CloseBoundary::scene_fade_in, true, false, true);
    passed &= expect(
        fade_bound.resource_recovered && fade_bound.resource_ready &&
            !fade_bound.permanent_wait && fade_bound.fade_reached &&
            fade_bound.normal_play_bypass,
        "fade_bound_policy_covers_resource_terminal_then_bypasses");

    const auto stable_scene = run_pipeline(
        CloseBoundary::stable_scene, true, false, true);
    passed &= expect(
        stable_scene.resource_recovered && stable_scene.resource_ready &&
            !stable_scene.permanent_wait && stable_scene.fade_reached &&
            stable_scene.normal_play_bypass,
        "bsfn_avm_stable_scene_boundary_covers_resource_terminal");

    const auto fade_bound_late = run_pipeline(
        CloseBoundary::scene_fade_in, true, true, true);
    passed &= expect(
        fade_bound_late.late_request_tracked &&
            fade_bound_late.close_deferred &&
            fade_bound_late.resource_ready &&
            fade_bound_late.fade_reached &&
            fade_bound_late.normal_play_bypass,
        "fade_bound_policy_covers_late_child_then_bypasses");

    passed &= expect(
        run_randomized_window_check(CloseBoundary::player_load_end),
        "current_policy_randomized_window_reproduces_gap");
    passed &= expect(
        run_randomized_window_check(CloseBoundary::stable_scene),
        "bsfn_avm_stable_scene_boundary_closes_gap");
    passed &= expect(
        run_randomized_window_check(CloseBoundary::scene_fade_in),
        "fade_bound_policy_randomized_window_closes_gap");

    const auto native_success = run_pipeline(
        CloseBoundary::player_load_end, true, false, false);
    passed &= expect(
        native_success.resource_ready && !native_success.resource_recovered &&
            !native_success.permanent_wait && native_success.fade_reached &&
            native_success.normal_play_bypass,
        "native_success_does_not_need_guard_after_player_end");

    std::printf(
        "post_player_end_protection_simulation=%s iterations=750000\n",
        passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
