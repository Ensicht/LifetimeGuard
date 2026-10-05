// 有界失败纹理重读：保留代次、身份、四任务上限和退避，不增加轮询。
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace dstorage_guard {

struct TextureRetryGate {
    std::uint64_t generation{};
    bool active{};
    bool gameplay{};
};

struct TextureRetryResource {
    std::uintptr_t parsed{};
    std::uint64_t identity{};
    std::uint32_t references{};
    bool texture{};
    bool ready{};
    bool task{};
    bool runtime{};
    bool disposing{};

    bool detached() const {
        return texture && !ready && !task && !runtime && !disposing && parsed != 0 &&
               references != 0;
    }
};

// All callbacks operate on a resource already owned by the native caller.
// No callback may re-enter this object while holding one of its bucket locks.
// snapshot/gate/add_ref are bounded reads or an atomic reference increment.
struct TextureRetryBindings {
    TextureRetryGate (*gate)(){};
    TextureRetryResource (*snapshot)(void *){};
    bool (*manager_valid)(void *){};
    void (*add_ref)(void *){};
    void (*release)(void *, void *){};
    bool (*enqueue)(void *, void *){};
    bool (*wake)(void *){};
    void (*notify)(){};
    std::uint64_t (*clock_ticks)(){};
    std::uint32_t (*thread_id)(){};
    std::uint64_t (*milliseconds)(){};
};

struct TextureRetryStats {
    std::uint64_t receipts{};
    std::uint64_t queued{};
    std::uint64_t recovered{};
    std::uint64_t failed{};
    std::uint64_t cancelled{};
    std::uint64_t capacity_rejected{};
    std::uint64_t wake_failed{};
    std::uint64_t resident{};
};

struct TextureRetryEvent {
    std::uintptr_t resource{};
    std::uintptr_t manager{};
    std::uintptr_t parsed{};
    std::uint64_t identity{};
    std::uint64_t generation{};
    // 1 receipt, 2 queued, 3 recovered, 4 failed, 5 cancelled, 6 wake failure.
    std::uint32_t kind{};
    bool transferred_reference{};
    std::uint64_t qpc{};
    std::uint32_t thread_id{};
    bool gameplay{};
    std::uint32_t attempt{};
    std::uint32_t failures{};
    std::uint64_t retry_after_ms{};
};

struct GameplayRetryStats {
    std::uint64_t inflight{};
    std::uint64_t gameplay_queued{};
    std::uint64_t backoffs{};
    std::uint64_t events_dropped{};
};

class DetachedTextureRetry {
  public:
    static constexpr std::uint64_t gameplay_parallel_limit = 4;
    // Installed once before arming the adapter. Never change live bindings.
    explicit DetachedTextureRetry(TextureRetryBindings bindings) : bindings_(bindings) {}

    // Called ONLY after a verified native worker destroyed its task, cleared
    // the binding and balanced its work counter, but BEFORE its final Release.
    // true transfers that existing job reference to a new native read job.
    bool terminal(void *manager, void *resource) {
        const auto snapshot = bindings_.snapshot(resource);
        if (!snapshot.texture) {
            return false;
        }
        if (snapshot.ready && snapshot.runtime && resident_.load(std::memory_order_relaxed) == 0) {
            return false;
        }
        Ticket ticket{};
        {
            auto &bucket = bucket_for(resource);
            const std::lock_guard lock{bucket.lock};
            auto *entry = find(bucket, resource);
            if (snapshot.ready && snapshot.runtime) {
                if (entry != nullptr) {
                    if (entry->queued) {
                        finish_job(*entry);
                        recovered_.fetch_add(1, std::memory_order_relaxed);
                        event(*entry, 3);
                    }
                    erase(*entry);
                }
                return false;
            }
            // A reread can fail before it produces another parsed descriptor.
            if (entry != nullptr && entry->queued) {
                finish_job(*entry);
                failed_.fetch_add(1, std::memory_order_relaxed);
                entry->retry_failure = true;
                if (entry->failures < 4) {
                    ++entry->failures;
                }
                entry->retry_after_ms = now_ms() + backoff_ms(entry->failures);
                backoffs_.fetch_add(1, std::memory_order_relaxed);
                event(*entry, 4);
            }
            if (!retryable(snapshot, entry)) {
                return false;
            }
            if (entry == nullptr) {
                entry = empty(bucket);
                if (entry == nullptr) {
                    capacity_rejected_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                entry->resource = resource;
                resident_.fetch_add(1, std::memory_order_relaxed);
            }
            entry->manager = manager;
            entry->identity = snapshot.identity;
            entry->parsed = snapshot.parsed;
            entry->terminal = true;
            receipts_.fetch_add(1, std::memory_order_relaxed);
            event(*entry, 1);
            ticket = claim(*entry, snapshot, true);
        }
        return submit(ticket);
    }

    // Called only when the real texture ready getter returned false, the
    // recovery is allowed, and the ledger is nonempty. No scene scan.
    void queried(void *resource) {
        if (resident_.load(std::memory_order_relaxed) == 0) {
            return;
        }
        const auto gate = bindings_.gate();
        if (!gate.active || (gate.gameplay && inflight_.load(std::memory_order_relaxed) >=
                                                  gameplay_parallel_limit)) {
            return;
        }
        Ticket ticket{};
        {
            auto &bucket = bucket_for(resource);
            const std::lock_guard lock{bucket.lock};
            auto *entry = find(bucket, resource);
            if (entry == nullptr || !entry->terminal || entry->queued) {
                return;
            }
            if (gate.gameplay && now_ms() < entry->retry_after_ms) {
                return;
            }
            const auto snapshot = bindings_.snapshot(resource);
            if (entry->identity != snapshot.identity || entry->parsed != snapshot.parsed) {
                erase(*entry);
                return;
            }
            ticket = claim(*entry, snapshot, false);
        }
        submit(ticket);
    }

    // Invoked by the exact texture deleting destructor before memory reuse.
    // Receipts own scalar identities only, never resource references.
    void retired(void *resource) {
        if (resident_.load(std::memory_order_relaxed) == 0) {
            return;
        }
        auto &bucket = bucket_for(resource);
        const std::lock_guard lock{bucket.lock};
        if (auto *entry = find(bucket, resource)) {
            erase(*entry);
        }
    }

    TextureRetryStats stats() const {
        return {receipts_.load(),    queued_.load(),    recovered_.load(),
                failed_.load(),      cancelled_.load(), capacity_rejected_.load(),
                wake_failed_.load(), resident_.load()};
    }

    bool take_event(TextureRetryEvent &output) {
        const std::lock_guard lock{events_lock_};
        if (event_read_ == event_write_) {
            return false;
        }
        output = events_[event_read_++ % events_.size()];
        return true;
    }

    GameplayRetryStats gameplay_stats() const {
        return {inflight_.load(), gameplay_queued_.load(), backoffs_.load(),
                events_dropped_.load()};
    }

  private:
    struct Entry {
        void *resource{};
        void *manager{};
        std::uintptr_t parsed{};
        std::uint64_t identity{};
        std::uint64_t attempted_generation{};
        std::uint64_t event_generation{};
        bool terminal{};
        bool queued{};
        bool retry_failure{};
        bool gameplay{};
        std::uint32_t attempt{};
        std::uint32_t failures{};
        std::uint64_t retry_after_ms{};
    };
    struct Bucket {
        std::mutex lock;
        std::array<Entry, 32> entries{};
    };
    struct Ticket {
        void *resource{};
        void *manager{};
        std::uint64_t generation{};
        bool transfer{};
        bool gameplay{};
        TextureRetryEvent detail{};
    };

    Bucket &bucket_for(void *resource) {
        return buckets_[(reinterpret_cast<std::uintptr_t>(resource) >> 4U) % buckets_.size()];
    }
    static Entry *find(Bucket &bucket, void *resource) {
        for (auto &entry : bucket.entries) {
            if (entry.resource == resource) {
                return &entry;
            }
        }
        return nullptr;
    }
    static Entry *empty(Bucket &bucket) {
        return find(bucket, nullptr);
    }
    static bool retryable(const TextureRetryResource &snapshot, const Entry *entry) {
        if (snapshot.detached()) {
            return true;
        }
        // A verified reread may fail before allocating another descriptor.
        // Its worker receipt remains evidence; an arbitrary task=null is not.
        return entry != nullptr && entry->retry_failure && snapshot.texture &&
               snapshot.identity == entry->identity && !snapshot.ready && !snapshot.task &&
               !snapshot.runtime && !snapshot.disposing && snapshot.references != 0;
    }
    std::uint64_t now_ms() const {
        return bindings_.milliseconds ? bindings_.milliseconds() : 0;
    }
    static std::uint64_t backoff_ms(std::uint32_t failures) {
        return failures == 1 ? 250 : (failures == 2 ? 1000 : (failures == 3 ? 5000 : 30000));
    }
    bool reserve_job(bool gameplay) {
        if (!gameplay) {
            inflight_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        auto count = inflight_.load(std::memory_order_relaxed);
        while (count < gameplay_parallel_limit) {
            if (inflight_.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }
    void finish_job(Entry &entry) {
        if (!entry.queued) {
            return;
        }
        entry.queued = false;
        inflight_.fetch_sub(1, std::memory_order_relaxed);
    }
    void erase(Entry &entry) {
        finish_job(entry);
        entry = {};
        resident_.fetch_sub(1, std::memory_order_relaxed);
    }

    Ticket claim(Entry &entry, const TextureRetryResource &snapshot, bool transfer) {
        const auto gate = bindings_.gate();
        if (!gate.active || gate.generation == 0 || entry.queued || !entry.terminal ||
            (!gate.gameplay && entry.attempted_generation == gate.generation) ||
            (gate.gameplay &&
             (transfer || bindings_.milliseconds == nullptr || now_ms() < entry.retry_after_ms)) ||
            !retryable(snapshot, &entry) || (transfer && snapshot.references <= 1) ||
            !bindings_.manager_valid(entry.manager)) {
            return {};
        }
        if (!reserve_job(gate.gameplay)) {
            return {};
        }
        // Loading retains its once-per-generation rule. Gameplay uses a
        // bounded concurrent queue and failure backoff, never a new timer.
        if (!gate.gameplay) {
            entry.attempted_generation = gate.generation;
        }
        entry.event_generation = gate.generation;
        entry.gameplay = gate.gameplay;
        if (entry.attempt != UINT32_MAX) {
            ++entry.attempt;
        }
        entry.queued = true;
        entry.terminal = false;
        if (!transfer) {
            bindings_.add_ref(entry.resource);
        }
        return {entry.resource,
                entry.manager,
                gate.generation,
                transfer,
                gate.gameplay,
                {reinterpret_cast<std::uintptr_t>(entry.resource),
                 reinterpret_cast<std::uintptr_t>(entry.manager), entry.parsed, entry.identity,
                 gate.generation, 0, transfer, 0, 0, entry.gameplay, entry.attempt, entry.failures,
                 entry.retry_after_ms}};
    }

    bool submit(const Ticket &ticket) {
        if (ticket.resource == nullptr) {
            return false;
        }
        const auto gate = bindings_.gate();
        bool submitted = false;
        if (gate.active && gate.generation == ticket.generation &&
            gate.gameplay == ticket.gameplay && bindings_.manager_valid(ticket.manager)) {
            // Native read/parser/worker owns the job after enqueue returns.
            // Never call a finalizer or write readiness/descriptor fields here.
            submitted = bindings_.enqueue(ticket.manager, ticket.resource);
        }
        if (!submitted) {
            {
                auto &bucket = bucket_for(ticket.resource);
                const std::lock_guard lock{bucket.lock};
                if (auto *entry = find(bucket, ticket.resource)) {
                    finish_job(*entry);
                    entry->terminal = true;
                    if (ticket.gameplay) {
                        entry->retry_after_ms = now_ms() + 250;
                    }
                }
            }
            ticket_event(ticket, 5);
            cancelled_.fetch_add(1, std::memory_order_relaxed);
            if (!ticket.transfer) {
                bindings_.release(ticket.manager, ticket.resource);
            }
            return false;
        }
        queued_.fetch_add(1, std::memory_order_relaxed);
        if (ticket.gameplay) {
            gameplay_queued_.fetch_add(1, std::memory_order_relaxed);
        }
        ticket_event(ticket, 2);
        if (!bindings_.wake(ticket.manager)) {
            wake_failed_.fetch_add(1, std::memory_order_relaxed);
            ticket_event(ticket, 6);
        }
        // The worker may already have completed and destroyed the resource.
        // There must be no dereference of ticket.resource past enqueue.
        return true;
    }

    void event(const Entry &entry, std::uint32_t kind, bool transfer = false) {
        emit({reinterpret_cast<std::uintptr_t>(entry.resource),
              reinterpret_cast<std::uintptr_t>(entry.manager), entry.parsed, entry.identity,
              entry.event_generation, kind, transfer, 0, 0, entry.gameplay, entry.attempt,
              entry.failures, entry.retry_after_ms});
    }

    void ticket_event(const Ticket &ticket, std::uint32_t kind) {
        auto detail = ticket.detail;
        detail.kind = kind;
        emit(detail);
    }

    void emit(TextureRetryEvent detail) {
        const std::lock_guard lock{events_lock_};
        if (event_write_ - event_read_ == events_.size()) {
            events_dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (bindings_.clock_ticks != nullptr) {
            detail.qpc = bindings_.clock_ticks();
        }
        if (bindings_.thread_id != nullptr) {
            detail.thread_id = bindings_.thread_id();
        }
        events_[event_write_++ % events_.size()] = detail;
        if (bindings_.notify != nullptr) {
            bindings_.notify();
        }
    }

    TextureRetryBindings bindings_;
    std::array<Bucket, 128> buckets_{};
    std::atomic<std::uint64_t> receipts_{}, queued_{}, recovered_{}, failed_{}, cancelled_{};
    std::atomic<std::uint64_t> capacity_rejected_{}, wake_failed_{}, resident_{};
    std::atomic<std::uint64_t> inflight_{}, gameplay_queued_{}, backoffs_{}, events_dropped_{};
    std::mutex events_lock_;
    std::array<TextureRetryEvent, 96> events_{};
    std::size_t event_read_{}, event_write_{};
};

} // namespace dstorage_guard
