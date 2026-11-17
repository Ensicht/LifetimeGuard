#include "../src/DetachedTextureRetry.h"
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {
using namespace dstorage_guard;
struct alignas(2048) Model {
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
bool wake(void*);
void notify();
void check(bool okay, const char* label) {
    if (!okay) { std::fprintf(stderr, "FAIL %s\n", label); std::exit(1); }
}
struct Context {
    bool active{true}, manager_ok{true}, enqueue_ok{true}, wake_ok{true};
    unsigned gate_calls{}, close_at{}, advance_at{}, manager_calls{}, replace_manager_at{};
    unsigned attempts{}, pins{}, drops{}, snapshots{}, notifications{};
    std::uint64_t generation{1};
    Model* job{};
    std::unique_ptr<DetachedTextureRetry> retry;
    Context() {
        current = this;
        retry = std::make_unique<DetachedTextureRetry>(TextureRetryBindings{
            &gate, &snapshot, &manager_valid, &add_ref, &release, &enqueue, &wake, &notify});
    }
    void terminal(Model& model) {
        if (!retry->terminal(this, &model)) { release(this, &model); }
    }
    void finish(bool success) {
        auto* model = job;
        check(model != nullptr, "job exists");
        job = nullptr;
        model->state.task = false;
        model->state.parsed += 64;
        model->state.runtime = model->state.ready = success;
        terminal(*model);
    }
};
TextureRetryGate gate() {
    auto& c = *current;
    ++c.gate_calls;
    if (c.gate_calls == c.close_at) { c.active = false; }
    if (c.gate_calls == c.advance_at) { ++c.generation; }
    return {c.generation, c.active};
}
TextureRetryResource snapshot(void* model) { ++current->snapshots; return static_cast<Model*>(model)->state; }
bool manager_valid(void* manager) {
    ++current->manager_calls;
    if (current->manager_calls == current->replace_manager_at) { current->manager_ok = false; }
    return manager == current && current->manager_ok;
}
void add_ref(void* model) { ++current->pins; ++static_cast<Model*>(model)->state.references; }
void release(void*, void* object) {
    auto& model = *static_cast<Model*>(object);
    check(model.state.references > 0, "reference decrement never underflows");
    ++current->drops;
    if (--model.state.references == 0) { current->retry->retired(object); }
}
bool enqueue(void* manager, void* object) {
    check(manager == current, "original resource manager preserved");
    ++current->attempts;
    if (!current->enqueue_ok) { return false; }
    check(current->job == nullptr, "no duplicate queued job");
    current->job = static_cast<Model*>(object);
    current->job->state.task = true;
    return true;
}
bool wake(void*) { return current->wake_ok; }
void notify() { ++current->notifications; }
}

int main() {
    for (unsigned negative = 0; negative < 11; ++negative) {
        Context c;
        Model model;
        switch (negative) {
        case 0: model.state.texture = false; break;
        case 1: model.state.task = true; break;
        case 2: model.state.ready = true; break;
        case 3: model.state.runtime = true; break;
        case 4: model.state.disposing = true; break;
        case 5: model.state.parsed = 0; break;
        case 6: model.state.references = 0; break;
        case 7: model.state.references = 1; break;
        case 8: c.active = false; break;
        case 9: c.generation = 0; break;
        case 10: c.manager_ok = false; break;
        }
        check(!c.retry->terminal(&c, &model), "negative evidence does not transfer job");
        check(c.attempts == 0 && c.pins == 0, "negative evidence performs no work");
    }
    for (unsigned mode = 0; mode < 5; ++mode) {
        Context c;
        Model model;
        const bool from_query = mode != 0;
        if (from_query) {
            c.active = false;
            c.terminal(model);
            check(model.state.references == 1, "receipt owns no reference");
            c.active = true;
            c.gate_calls = c.manager_calls = 0;
        }
        if (mode == 0) { c.close_at = 2; }
        if (mode == 1) { c.close_at = 3; }
        if (mode == 2) { c.advance_at = 3; }
        if (mode == 3) { c.replace_manager_at = 2; }
        if (mode == 4) { c.enqueue_ok = false; }
        if (from_query) { c.retry->queried(&model); } else { c.terminal(model); }
        check(model.state.references == 1 && c.retry->stats().cancelled == 1, "cancelled reservation releases exactly its reference");
        check(c.job == nullptr && c.retry->stats().queued == 0, "cancelled admission never owns a queued job");
        if (mode == 4) {
            for (unsigned i = 0; i < 10000; ++i) { c.retry->queried(&model); }
            check(c.attempts == 1 && c.pins == 1, "enqueue failure consumes this generation's attempt");
        }
        release(&c, &model);
        check(c.retry->stats().resident == 0, "cancelled resource record retires");
    }
    {
        Context c;
        Model model;
        c.wake_ok = false;
        c.terminal(model);
        check(c.retry->stats().wake_failed == 1 && model.state.references == 2, "notification failure does not release a submitted job");
        bool logged{};
        TextureRetryEvent entry{};
        while (c.retry->take_event(entry)) { logged |= entry.kind == 6; }
        check(logged, "notification failure has a distinct bounded event");
        c.finish(true);
        release(&c, &model);
        check(c.retry->stats().resident == 0, "notification failure followed by native completion drains");
    }
    {
        Context c;
        c.active = false;
        auto models = std::make_unique<Model[]>(33);
        for (unsigned i = 0; i < 33; ++i) { c.terminal(models[i]); }
        check(c.retry->stats().resident == 32 && c.retry->stats().capacity_rejected == 1, "bucket saturation rejects without eviction or retry reset");
        for (unsigned i = 0; i < 33; ++i) { release(&c, &models[i]); }
        check(c.retry->stats().resident == 0 && c.pins == 0, "saturation retains no resource references");
    }
    {
        Context c;
        Model model;
        c.active = false;
        c.terminal(model);
        ++model.state.identity;
        c.active = true;
        c.retry->queried(&model);
        check(c.attempts == 0 && c.retry->stats().resident == 0, "identity change discards receipt without requeue");
        release(&c, &model);
    }
    {
        Context c;
        c.active = false;
        for (unsigned i = 0; i < 1000; ++i) {
            Model model;
            c.terminal(model);
            release(&c, &model);
        }
        check(c.notifications == 96 && c.retry->stats().resident == 0 && c.attempts == 0, "diagnostics remain bounded across many failures");
    }
    std::puts("detached_boundaries=PASS negatives=11 admission_races=5 failed_enqueue_queries=10000 saturation=33 log_limit=96 retained_references=0");
}
