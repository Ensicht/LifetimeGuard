 
#pragma once
#include <cstdint>

namespace dstorage_guard {

 
 
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

}  
