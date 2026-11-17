#include "LoadingGatePolicy.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

bool expect(bool condition, const char* name) {
    std::printf("case=%s result=%s\n", name, condition ? "PASS" : "FAIL");
    return condition;
}

} // namespace

int run_tests() {
    bool passed = true;

    dstorage_guard::LoadingGate gate;
    auto snapshot = gate.snapshot();
    passed &= expect(
        snapshot.lifecycle_available && snapshot.active && snapshot.bootstrap &&
            snapshot.generation == 1,
        "bootstrap_active");

    std::uint64_t generation{};
    passed &= expect(
        gate.mark_start(false, &generation) && generation == 1 &&
            !gate.snapshot().bootstrap,
        "bootstrap_adopts_environment_start");
    passed &= expect(
        gate.mark_end_signal(false, &generation) && generation == 1 &&
            gate.accepts_new_work() && gate.snapshot().end_seen,
        "normal_load_end_is_evidence_only");
    passed &= expect(
        !gate.try_end_stable_scene(true, true, &generation) &&
            gate.accepts_new_work(),
        "normal_loading_true_keeps_gate_active");
    passed &= expect(
        !gate.try_end_stable_scene(false, false, &generation) &&
            gate.accepts_new_work(),
        "normal_missing_scene_keeps_gate_active");
    passed &= expect(
        gate.try_end_stable_scene(false, true, &generation) &&
            generation == 1 && !gate.accepts_new_work(),
        "normal_stable_scene_disarms");
    passed &= expect(
        gate.mark_start(false, &generation) && generation == 2 &&
            gate.accepts_new_work(),
        "next_environment_start_rearms");
    passed &= expect(
        gate.mark_loading_seen() && gate.snapshot().saw_loading,
        "loading_true_is_remembered");
    passed &= expect(
        gate.try_end_stable_scene(false, true, &generation) &&
            !gate.accepts_new_work(),
        "loading_true_to_false_is_lifecycle_evidence");
    passed &= expect(
        gate.mark_start(true, &generation) && generation == 3 &&
            gate.accepts_new_work(),
        "same_scene_black_screen_starts_new_generation");
    passed &= expect(
        !gate.mark_start(true, &generation) && generation == 3 &&
            gate.snapshot().requires_fade_in,
        "duplicate_black_screen_start_is_ignored");
    passed &= expect(
        !gate.mark_end_signal(false, &generation) &&
            !gate.try_end_stable_scene(false, true, &generation) &&
            gate.accepts_new_work(),
        "early_evidence_ignored_while_fade_required");
    passed &= expect(
        gate.mark_end(true, &generation) && !gate.accepts_new_work(),
        "camera_fade_in_disarms_fast_travel");

    gate.configure(false, false);
    passed &= expect(
        !gate.accepts_new_work() && !gate.mark_start(false),
        "missing_lifecycle_is_fail_closed");

    gate.configure(true, false);
    passed &= expect(
        !gate.accepts_new_work() && !gate.mark_end(true),
        "seamless_transition_without_start_stays_bypassed");

    gate.configure(true, false);
    constexpr int k_threads = 8;
    constexpr int k_iterations = 20000;
    std::atomic<int> starts{};
    std::atomic<int> ends{};
    std::vector<std::thread> workers;
    workers.reserve(k_threads);
    for (int thread = 0; thread < k_threads; ++thread) {
        workers.emplace_back([&gate, &starts, &ends, thread]() {
            for (int iteration = 0; iteration < k_iterations; ++iteration) {
                if (((iteration + thread) & 1) == 0) {
                    if (gate.mark_start(false)) {
                        starts.fetch_add(1, std::memory_order_relaxed);
                    }
                } else if (gate.mark_end(true)) {
                    ends.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    snapshot = gate.snapshot();
    const auto start_count = starts.load(std::memory_order_relaxed);
    const auto end_count = ends.load(std::memory_order_relaxed);
    passed &= expect(
        start_count == end_count || start_count == end_count + 1,
        "concurrent_start_end_balance");
    passed &= expect(
        snapshot.generation == static_cast<std::uint64_t>(start_count),
        "concurrent_generation_matches_starts");

    std::printf(
        "loading_gate_simulation=%s starts=%d ends=%d generation=%llu active=%d\n",
        passed ? "PASS" : "FAIL", start_count, end_count,
        static_cast<unsigned long long>(snapshot.generation), snapshot.active);
    return passed ? 0 : 1;
}

int main() {
    try {
        return run_tests();
    } catch (...) {
        std::printf("loading_gate_simulation=FAIL exception=unhandled\n");
        return 2;
    }
}
