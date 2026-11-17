#include "../src/DetachedTextureRecoveryPolicy.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace dstorage_guard;

namespace {
void check(bool condition, const char* name) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        std::exit(1);
    }
}

DetachedTextureSnapshot candidate() {
    return {0x10000, 0x20000, 18, true, false, false, false, true, true, false};
}

// Protocol model only: this does not pretend to validate the game's renderer
// or establish a native terminal-state proof. Those are separate release gates.
struct Model {
    DetachedTextureSnapshot state{candidate()};
    std::atomic<bool> claimed{false};
    std::atomic<unsigned> references{1};
    std::atomic<unsigned> queued{0};
    std::atomic<unsigned> attempts{0};
    std::atomic<bool> attempt_consumed{false};
    bool change_generation_on_claim{};
    bool fail_queue{};
    unsigned finalized{};
    unsigned rollbacks{};

    DetachedTextureSnapshot snapshot() const {
        auto current = state;
        current.attempted = current.attempted || attempt_consumed.load();
        return current;
    }
    DetachedTextureSnapshot owned_snapshot() const { return state; }

    bool try_acquire(const DetachedTextureSnapshot&) {
        bool expected = false;
        if (!claimed.compare_exchange_strong(expected, true)) { return false; }
        if (attempt_consumed.exchange(true)) {
            claimed.store(false);
            return false;
        }
        references.fetch_add(1);
        if (change_generation_on_claim) { ++state.generation; }
        return true;
    }
    void rollback() {
        ++rollbacks;
        references.fetch_sub(1);
        claimed.store(false);
    }
    bool enqueue() {
        if (fail_queue) { return false; }
        attempts.fetch_add(1);
        queued.fetch_add(1);
        return true;
    }
    void worker_complete(bool loading, bool descriptor_same, bool runtime_created) {
        check(queued.load() == 1, "worker owns exactly one queued job");
        if (loading && descriptor_same) {
            ++finalized;
            state.runtime_present = runtime_created;
            state.resource_ready = runtime_created;
        }
        state.attempted = true;
        queued.fetch_sub(1);
        references.fetch_sub(1);
        claimed.store(false);
    }
};
}

int main() {
    const auto observed = candidate();
    check(decide_detached_texture(observed, observed) ==
        DetachedTextureDecision::eligible, "eligible candidate");

    unsigned negative_cases{};
    auto reject = [&](auto edit, DetachedTextureDecision expected) {
        auto current = observed;
        edit(current);
        check(decide_detached_texture(observed, current) == expected, "negative policy");
        ++negative_cases;
    };
    reject([](auto& s) { s.loading_active = false; }, DetachedTextureDecision::loading_closed);
    reject([](auto& s) { ++s.generation; }, DetachedTextureDecision::generation_changed);
    reject([](auto& s) { s.resource_ready = true; }, DetachedTextureDecision::native_ready);
    reject([](auto& s) { s.task_attached = true; }, DetachedTextureDecision::task_still_attached);
    reject([](auto& s) { s.runtime_present = true; }, DetachedTextureDecision::runtime_present);
    reject([](auto& s) { s.terminal_state_verified = false; }, DetachedTextureDecision::unverified_terminal_state);
    reject([](auto& s) { s.descriptor_verified = false; }, DetachedTextureDecision::invalid_texture_descriptor);
    reject([](auto& s) { ++s.parsed; }, DetachedTextureDecision::identity_changed);
    reject([](auto& s) { ++s.resource; }, DetachedTextureDecision::identity_changed);
    reject([](auto& s) { s.attempted = true; }, DetachedTextureDecision::already_attempted);

    Model game;
    game.state.loading_active = false;
    for (unsigned i = 0; i < 100000; ++i) {
        check(queue_detached_texture(observed, game) == DetachedQueueResult::rejected,
            "no recovery outside loading");
    }
    check(game.attempts == 0 && game.references == 1 && !game.claimed,
        "idle path does not pin or enqueue");

    for (unsigned i = 0; i < 1000; ++i) {
        Model model;
        const bool cancel = (i % 3) == 0;
        const bool identity_changed = (i % 3) == 1;
        check(queue_detached_texture(observed, model) == DetachedQueueResult::queued,
            "enqueue returns before finalization");
        check(model.finalized == 0 && !model.state.resource_ready,
            "consumer never finalizes or sets ready");
        model.worker_complete(!cancel, !identity_changed, true);
        check(model.finalized == (!cancel && !identity_changed ? 1U : 0U),
            "worker cancellation and identity guard");
        check(model.references == 1 && model.queued == 0 && !model.claimed,
            "all worker branches release runtime ownership");
        check(queue_detached_texture(observed, model) == DetachedQueueResult::rejected,
            "completed or failed job is not repeatedly queued");
    }

    Model cancelled;
    cancelled.change_generation_on_claim = true;
    check(queue_detached_texture(observed, cancelled) == DetachedQueueResult::changed_after_acquire,
        "generation recheck after pin");
    check(cancelled.rollbacks == 1 && cancelled.references == 1 && !cancelled.claimed,
        "generation cancellation rolls back pin");

    Model failed;
    failed.fail_queue = true;
    check(queue_detached_texture(observed, failed) == DetachedQueueResult::queue_failed,
        "enqueue error");
    check(failed.references == 1 && !failed.claimed && failed.rollbacks == 1,
        "enqueue error rolls back exactly once");
    for (unsigned i = 0; i < 10000; ++i) {
        check(queue_detached_texture(observed, failed) == DetachedQueueResult::rejected,
            "failed queue cannot retry on every readiness query");
    }
    check(failed.rollbacks == 1, "queue failure attempt remains consumed");

    Model no_runtime;
    check(queue_detached_texture(observed, no_runtime) == DetachedQueueResult::queued,
        "native creation failure setup");
    no_runtime.worker_complete(true, true, false);
    check(!no_runtime.state.resource_ready && no_runtime.references == 1,
        "no fake ready on native creation failure");

    Model concurrent;
    std::vector<std::jthread> callers;
    for (unsigned i = 0; i < 16; ++i) {
        callers.emplace_back([&] {
            for (unsigned n = 0; n < 1000; ++n) {
                (void)queue_detached_texture(observed, concurrent);
            }
        });
    }
    callers.clear();
    check(concurrent.queued == 1 && concurrent.attempts == 1 && concurrent.references == 2,
        "16000 competing queries admit one task");
    concurrent.worker_complete(true, true, true);
    check(concurrent.references == 1 && concurrent.state.resource_ready,
        "concurrent test drains ownership");
    std::printf("detached_protocol=PASS negative_cases=%u idle_queries=100000 "
        "lifecycle_cases=1000 concurrent_queries=16000 native_adapter_tested=NO\n", negative_cases);
}
