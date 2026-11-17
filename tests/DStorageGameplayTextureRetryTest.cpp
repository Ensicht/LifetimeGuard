#include "../src/DetachedTextureRetry.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace dstorage_guard;
void check(bool value, const char* name) {
    if (!value) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
}
struct Model {
    TextureRetryResource state{0x123400, 0xABCD, 2, true, false, false, false, false};
};
struct Context;
Context* current{};
TextureRetryGate gate();
TextureRetryResource snapshot(void*);
bool manager_valid(void*);
void add_ref(void*);
void release(void*, void*);
bool enqueue(void*, void*);
bool wake(void*) { return true; }
std::uint64_t milliseconds();
struct Context {
    std::atomic<bool> enabled{true}, gameplay{true}, enqueue_ok{true};
    std::atomic<std::uint64_t> generation{1}, now{1000}, snapshots{}, pins{}, drops{}, attempts{}, clock_reads{};
    unsigned gate_reads{}, flip_gate_at{};
    bool sync_complete{}, close_on_enqueue{};
    std::mutex state_lock, queue_lock;
    std::vector<Model*> jobs;
    std::unique_ptr<DetachedTextureRetry> retry;
    Context() {
        current = this;
        retry = std::make_unique<DetachedTextureRetry>(TextureRetryBindings{
            &gate, &snapshot, &manager_valid, &add_ref, &release, &enqueue, &wake,
            nullptr, nullptr, nullptr, &milliseconds});
    }
    void terminal(Model& model) {
        if (!retry->terminal(this, &model)) { release(this, &model); }
    }
    bool query(Model& model) {
        bool ready;
        { const std::lock_guard lock{state_lock}; ready = model.state.ready; }
        if (!ready) { retry->queried(&model); }
        return ready;
    }
    void finish(Model& model, bool success, bool no_descriptor = false) {
        {
            const std::lock_guard lock{state_lock};
            model.state.task = false;
            model.state.parsed = no_descriptor ? 0 : model.state.parsed + 64;
            model.state.ready = model.state.runtime = success;
        }
        terminal(model);
    }
    void drain(bool success, bool no_descriptor = false) {
        std::vector<Model*> batch;
        { const std::lock_guard lock{queue_lock}; batch.swap(jobs); }
        for (auto* model : batch) { finish(*model, success, no_descriptor); }
    }
};
TextureRetryGate gate() {
    // Single-threaded fault injection between admission and native submission.
    if (current->flip_gate_at && ++current->gate_reads == current->flip_gate_at) {
        current->gameplay = !current->gameplay;
    }
    return {current->generation.load(), current->enabled.load(), current->gameplay.load()};
}
std::uint64_t milliseconds() { ++current->clock_reads; return current->now.load(); }
TextureRetryResource snapshot(void* object) {
    ++current->snapshots;
    const std::lock_guard lock{current->state_lock};
    return static_cast<Model*>(object)->state;
}
bool manager_valid(void* manager) { return manager == current; }
void add_ref(void* object) {
    ++current->pins;
    const std::lock_guard lock{current->state_lock};
    ++static_cast<Model*>(object)->state.references;
}
void release(void*, void* object) {
    ++current->drops;
    bool retired;
    {
        const std::lock_guard lock{current->state_lock};
        auto& refs = static_cast<Model*>(object)->state.references;
        check(refs != 0, "no reference underflow");
        retired = --refs == 0;
    }
    if (retired) { current->retry->retired(object); }
}
bool enqueue(void* manager, void* object) {
    check(manager == current, "native manager unchanged");
    ++current->attempts;
    if (!current->enqueue_ok) { return false; }
    {
        const std::lock_guard lock{current->state_lock};
        auto& state = static_cast<Model*>(object)->state;
        check(state.references >= 2 && !state.task && !state.ready, "one real native read reference");
        state.task = true;
    }
    if (current->close_on_enqueue) { current->enabled = false; }
    if (current->sync_complete) {
        current->finish(*static_cast<Model*>(object), true);
    } else {
        const std::lock_guard lock{current->queue_lock};
        current->jobs.push_back(static_cast<Model*>(object));
    }
    return true;
}
}

int main() {
    {
        Context c;
        std::vector<Model> models(38);
        for (auto& model : models) { c.terminal(model); }
        check(c.attempts == 0 && c.pins == 0, "gameplay records failure without worker-side requeue");
        std::vector<std::thread> workers;
        for (unsigned worker = 0; worker < 16; ++worker) {
            workers.emplace_back([&] {
                for (unsigned round = 0; round < 100; ++round) {
                    for (auto& model : models) { c.query(model); }
                }
            });
        }
        for (auto& thread : workers) { thread.join(); }
        check(c.attempts == 4 && c.retry->gameplay_stats().inflight == 4, "parallel cap includes reserved jobs");
        while (c.retry->stats().recovered != models.size()) {
            c.drain(true);
            for (auto& model : models) { c.query(model); }
            check(c.retry->gameplay_stats().inflight <= 4, "batch cap throughout backlog");
        }
        check(c.attempts == 38 && c.now == 1000, "all 38 recovered without timer or frame advancement");
        for (auto& model : models) {
            check(model.state.references == 1 && model.state.ready, "native completion owns readiness");
            release(&c, &model);
        }
        check(c.retry->gameplay_stats().inflight == 0 && c.retry->stats().resident == 0, "queue and ledger drain");
    }
    {
        Context c;
        Model model;
        c.terminal(model);
        const std::uint64_t delays[]{250, 1000, 5000, 30000, 30000, 30000};
        for (const auto delay : delays) {
            const auto attempts_before = c.attempts.load();
            c.query(model);
            check(c.attempts == attempts_before + 1, "retry remains possible after repeated failures");
            c.drain(false, true);
            check(model.state.references == 1 && c.retry->gameplay_stats().inflight == 0,
                  "failed parser releases permit and reference even without descriptor");
            const auto reads = c.snapshots.load();
            for (unsigned i = 0; i < 10000; ++i) { c.query(model); }
            check(c.snapshots == reads && c.attempts == attempts_before + 1, "cooldown avoids resource snapshots and rereads");
            c.now += delay - 1;
            c.query(model);
            check(c.attempts == attempts_before + 1, "not early by one millisecond");
            ++c.now;
        }
        c.query(model); c.drain(true);
        check(c.retry->stats().recovered == 1 && c.retry->stats().failed == 6, "eventual recovery not permanently disabled");
        release(&c, &model);
    }
    {
        Context c;
        std::vector<Model> bad(4), good(8);
        for (auto& model : bad) { c.terminal(model); c.query(model); }
        c.drain(false);
        for (auto& model : good) { c.terminal(model); }
        for (unsigned round = 0; round < 2; ++round) {
            for (auto& model : bad) { c.query(model); }
            for (auto& model : good) { c.query(model); }
            c.drain(true);
        }
        check(c.retry->stats().recovered == 8, "bad files in cooldown do not occupy good-file capacity");
        for (auto& model : bad) { release(&c, &model); }
        for (auto& model : good) { release(&c, &model); }
        check(c.retry->gameplay_stats().inflight == 0, "no abandoned permits");
    }
    {
        Context c;
        Model model;
        c.terminal(model);
        c.enqueue_ok = false;
        c.query(model);
        check(model.state.references == 1 && c.retry->gameplay_stats().inflight == 0, "rejected enqueue returns permit");
        for (unsigned i = 0; i < 10000; ++i) { c.query(model); }
        check(c.attempts == 1, "rejected enqueue backoff");
        c.now += 250;
        c.enqueue_ok = true; c.sync_complete = true;
        c.query(model);
        check(model.state.ready && model.state.references == 1 && c.retry->gameplay_stats().inflight == 0,
              "completion inside enqueue releases permit exactly once");
        release(&c, &model);
    }
    {
        Context c;
        Model old, fresh;
        c.terminal(old); c.query(old);
        c.gameplay = false; ++c.generation;
        c.terminal(fresh);
        check(c.retry->gameplay_stats().inflight == 2, "loading admits its original worker-transfer route");
        c.enabled = false;
        c.drain(true);
        check(old.state.ready && fresh.state.ready && c.retry->gameplay_stats().inflight == 0,
              "inflight jobs drain across closed gates and changed generations");
        release(&c, &old); release(&c, &fresh);
    }
    {
        Context c;
        Model unobserved;
        unobserved.state.references = 1;
        unobserved.state.parsed = 0;
        c.query(unobserved);
        check(c.attempts == 0 && c.clock_reads == 0 && c.snapshots == 0, "empty ledger fast path");
        Model waiting;
        c.terminal(waiting);
        waiting.state.disposing = true;
        c.query(waiting);
        check(c.attempts == 0, "disposing resource rejected");
        waiting.state.disposing = false; ++waiting.state.identity;
        c.query(waiting);
        check(c.retry->stats().resident == 0, "identity mismatch retires receipt");
        release(&c, &waiting); release(&c, &unobserved);
    }
    {
        Context c;
        Model model;
        c.terminal(model);
        c.gate_reads = 0; c.flip_gate_at = 3;
        c.query(model);
        check(c.attempts == 0 && model.state.references == 1 &&
              c.retry->gameplay_stats().inflight == 0 && c.retry->stats().cancelled == 1,
              "gameplay-to-loading admission cancellation returns its reference and permit");
        c.flip_gate_at = 0;
        c.query(model);
        check(c.attempts == 1, "loading can immediately reclaim cancelled gameplay receipt");
        c.drain(true); release(&c, &model);
    }
    {
        Context c;
        Model model;
        c.terminal(model);
        c.gameplay = false; c.gate_reads = 0; c.flip_gate_at = 3;
        c.query(model);
        check(c.attempts == 0 && model.state.references == 1 &&
              c.retry->gameplay_stats().inflight == 0 && c.retry->stats().cancelled == 1,
              "loading-to-gameplay admission cancellation returns its reference and permit");
        c.flip_gate_at = 0;
        c.query(model);
        check(c.attempts == 1, "gameplay can reclaim cancelled loading receipt");
        c.drain(true); release(&c, &model);
    }
    {
        Context c;
        std::vector<Model> old(7);
        c.gameplay = false;
        for (auto& model : old) { c.terminal(model); }
        check(c.retry->gameplay_stats().inflight == 7, "loading retains existing admission without new cap");
        c.gameplay = true;
        Model fresh;
        c.terminal(fresh); c.query(fresh);
        check(c.attempts == 7, "gameplay accounts for jobs still draining from loading");
        c.drain(true); c.query(fresh);
        check(c.attempts == 8, "draining old jobs unblocks gameplay immediately");
        c.drain(true);
        for (auto& model : old) { release(&c, &model); }
        release(&c, &fresh);
        check(c.retry->gameplay_stats().inflight == 0, "loading drain does not strand permits");
    }
    {
        Context c;
        std::uint64_t read_count{};
        for (unsigned i = 0; i < 300; ++i) {
            Model model;
            c.terminal(model); release(&c, &model);
            TextureRetryEvent event;
            while (c.retry->take_event(event)) { ++read_count; }
        }
        check(read_count == 300 && c.retry->gameplay_stats().events_dropped == 0, "bounded event ring reuses drained storage");
    }
    std::puts("gameplay_policy=PASS backlog=38 concurrent_callers=16 queries=60800 max_inflight=4 repeated_failures=6 eventual_recovery=YES refs_leaked=0");
}
