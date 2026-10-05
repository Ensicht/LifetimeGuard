// 指针安装事务：失败只恢复自己拥有的槽位，避免覆盖其他插件更新。
#pragma once
#include <cstdint>

namespace dstorage_guard {
enum class PointerHookStatus : std::uint32_t {
    installed,
    protect_failed,
    conflict,
    restore_failed
};

struct PointerHookResult {
    PointerHookStatus status{PointerHookStatus::protect_failed};
    std::uint32_t old_protection{};
    std::uint32_t protect_error{};
    std::uint32_t restore_error{};
    std::uint32_t rollback_restore_error{};
    void *observed{};
    bool rolled_back{};
    bool protection_restored{};
};

// The caller publishes the original before this transaction, and keeps it
// valid on failure because a racing reader may already have fetched detour.
template <class Operations>
PointerHookResult install_pointer_hook(void **slot, void *original, void *detour,
                                       Operations &operations) {
    PointerHookResult result;
    if (!operations.writable(slot, result.old_protection, result.protect_error)) {
        return result;
    }
    result.observed = operations.exchange(slot, detour, original);
    result.status =
        result.observed == original ? PointerHookStatus::installed : PointerHookStatus::conflict;
    if (operations.restore(slot, result.old_protection, result.restore_error)) {
        result.protection_restored = true;
        return result;
    }
    if (result.status == PointerHookStatus::installed) {
        result.rolled_back = operations.exchange(slot, original, detour) == detour;
        result.status = PointerHookStatus::restore_failed;
    }
    result.protection_restored =
        operations.restore(slot, result.old_protection, result.rollback_restore_error);
    return result;
}
} // namespace dstorage_guard
