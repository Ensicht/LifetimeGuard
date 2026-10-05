// 加载门状态机：保留普通加载、快速旅行、嵌套起点和代次隔离的稳定版规则。
#pragma once

#include <atomic>
#include <cstdint>

namespace dstorage_guard {

struct LoadingGateSnapshot {
    bool lifecycle_available{};
    bool active{};
    bool requires_fade_in{};
    bool bootstrap{};
    bool end_seen{};
    bool saw_loading{};
    std::uint64_t generation{};
};

class LoadingGate {
  public:
    // The DLL can be exercised by the standalone DirectStorage harness before
    // a REFramework API exists. Real plugin initialization reconfigures this
    // state after installing the managed lifecycle hooks.
    LoadingGate() noexcept : state_(k_generation_step | k_available | k_active | k_bootstrap) {}

    void configure(bool lifecycle_available, bool active) noexcept {
        std::uint64_t state{};
        if (lifecycle_available) {
            state |= k_available;
        }
        if (lifecycle_available && active) {
            state |= k_generation_step | k_active | k_bootstrap;
        }
        state_.store(state, std::memory_order_release);
    }

    bool mark_start(bool requires_fade_in, std::uint64_t *generation = nullptr) noexcept {
        auto state = state_.load(std::memory_order_acquire);
        for (;;) {
            if ((state & k_available) == 0) {
                return false;
            }
            auto next = state;
            if ((state & k_active) == 0) {
                next += k_generation_step;
                next |= k_active;
                next &= ~(k_bootstrap | k_end_seen | k_saw_loading);
            } else if ((state & k_bootstrap) != 0) {
                // Adopt the first real lifecycle signal without leaving an
                // unprotected gap during the game's initial boot loading.
                next &= ~(k_bootstrap | k_end_seen | k_saw_loading);
            }
            if (requires_fade_in) {
                next |= k_requires_fade_in;
            }
            if (next == state) {
                if (generation != nullptr) {
                    *generation = state >> k_generation_shift;
                }
                return false;
            }
            if (state_.compare_exchange_weak(state, next, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                if (generation != nullptr) {
                    *generation = next >> k_generation_shift;
                }
                return true;
            }
        }
    }

    // Player/Environment load-end is lifecycle evidence, not a stable release
    // boundary. Same-Scene black-screen routes deliberately ignore it.
    bool mark_end_signal(bool is_fade_in, std::uint64_t *generation = nullptr) noexcept {
        auto state = state_.load(std::memory_order_acquire);
        for (;;) {
            if ((state & (k_available | k_active)) != (k_available | k_active)) {
                return false;
            }
            if ((state & k_requires_fade_in) != 0 && !is_fade_in) {
                return false;
            }
            const auto next = state | k_end_seen;
            if (next == state) {
                if (generation != nullptr) {
                    *generation = state >> k_generation_shift;
                }
                return false;
            }
            if (state_.compare_exchange_weak(state, next, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                if (generation != nullptr) {
                    *generation = next >> k_generation_shift;
                }
                return true;
            }
        }
    }

    bool mark_loading_seen() noexcept {
        auto state = state_.load(std::memory_order_acquire);
        for (;;) {
            if ((state & (k_available | k_active)) != (k_available | k_active) ||
                (state & k_requires_fade_in) != 0) {
                return false;
            }
            const auto next = state | k_saw_loading;
            if (next == state) {
                return false;
            }
            if (state_.compare_exchange_weak(state, next, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                return true;
            }
        }
    }

    // Mirrors the stable ordinary-load BSFN/AVM release semantics. The caller
    // supplies the game-owned observations so this policy remains testable and
    // contains no managed-object access itself.
    bool try_end_stable_scene(bool loading, bool scene_valid,
                              std::uint64_t *generation = nullptr) noexcept {
        if (loading || !scene_valid) {
            return false;
        }
        auto state = state_.load(std::memory_order_acquire);
        for (;;) {
            if ((state & (k_available | k_active)) != (k_available | k_active) ||
                (state & k_requires_fade_in) != 0 || (state & (k_end_seen | k_saw_loading)) == 0) {
                return false;
            }
            const auto next =
                state & ~(k_active | k_requires_fade_in | k_bootstrap | k_end_seen | k_saw_loading);
            if (state_.compare_exchange_weak(state, next, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                if (generation != nullptr) {
                    *generation = next >> k_generation_shift;
                }
                return true;
            }
        }
    }

    bool mark_end(bool is_fade_in, std::uint64_t *generation = nullptr) noexcept {
        auto state = state_.load(std::memory_order_acquire);
        for (;;) {
            if ((state & (k_available | k_active)) != (k_available | k_active)) {
                return false;
            }
            if ((state & k_requires_fade_in) != 0 && !is_fade_in) {
                return false;
            }
            const auto next =
                state & ~(k_active | k_requires_fade_in | k_bootstrap | k_end_seen | k_saw_loading);
            if (state_.compare_exchange_weak(state, next, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                if (generation != nullptr) {
                    *generation = next >> k_generation_shift;
                }
                return true;
            }
        }
    }

    bool accepts_new_work() const noexcept {
        const auto state = state_.load(std::memory_order_acquire);
        return (state & (k_available | k_active)) == (k_available | k_active);
    }

    bool lifecycle_available() const noexcept {
        return (state_.load(std::memory_order_acquire) & k_available) != 0;
    }

    LoadingGateSnapshot snapshot() const noexcept {
        const auto state = state_.load(std::memory_order_acquire);
        return LoadingGateSnapshot{
            (state & k_available) != 0,        (state & k_active) != 0,
            (state & k_requires_fade_in) != 0, (state & k_bootstrap) != 0,
            (state & k_end_seen) != 0,         (state & k_saw_loading) != 0,
            state >> k_generation_shift,
        };
    }

  private:
    static constexpr std::uint64_t k_available = 1ULL << 0U;
    static constexpr std::uint64_t k_active = 1ULL << 1U;
    static constexpr std::uint64_t k_requires_fade_in = 1ULL << 2U;
    static constexpr std::uint64_t k_bootstrap = 1ULL << 3U;
    static constexpr std::uint64_t k_end_seen = 1ULL << 4U;
    static constexpr std::uint64_t k_saw_loading = 1ULL << 5U;
    static constexpr std::uint32_t k_generation_shift = 8U;
    static constexpr std::uint64_t k_generation_step = 1ULL << k_generation_shift;

    std::atomic<std::uint64_t> state_{};
};

} // namespace dstorage_guard
