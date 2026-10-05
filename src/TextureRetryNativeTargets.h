// 原生重读调用目标；保持稳定版结构布局，供生产适配器和离线宿主共用。
#pragma once
#include <cstdint>

namespace dstorage_guard {

// Production instances are constructed only after the exact native profile
// passes. Offline fixtures enter through a separate, initialization-gated ABI.
struct TextureRetryNativeTargets {
    void **texture_vtable{};
    void **manager_slot{};
    void *release_target{};
    void *read_queue{};
    void *delete_original{};
    void *ready_original{};
    std::uintptr_t terminal_callers[4]{};
    void (*offline_before_enable)(){};
};

} // namespace dstorage_guard
