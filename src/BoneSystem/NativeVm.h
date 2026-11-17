#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace stock_vm {
// These private entry points are valid only for the SHA-256-pinned 26.2.10.2 DLL.
constexpr uintptr_t state_rva = 0xb6290;
constexpr uintptr_t load_rva = 0x243a0;
constexpr uintptr_t pcall_rva = 0x24280;
constexpr uintptr_t settop_rva = 0x23870;
constexpr uintptr_t index_rva = 0x23800;
constexpr int registry_index = -1001000;

inline bool readable(const void *address, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(address, &info, sizeof(info)))
        return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    auto start = reinterpret_cast<uintptr_t>(address);
    auto end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start <= end && size <= end - start;
}

template <class T> inline T read(const void *object, size_t offset) {
    T result{};
    std::memcpy(&result, static_cast<const unsigned char *>(object) + offset, sizeof(T));
    return result;
}

inline int top(void *state) {
    if (!readable(state, 0xb0))
        return -1;
    auto ci = read<void *>(state, 0x20);
    if (!readable(ci, 16))
        return -1;
    auto begin = read<uintptr_t>(ci, 0);
    auto end = read<uintptr_t>(state, 0x10);
    if (end < begin + 16 || (end - begin) % 16 || end - begin > 0x100000)
        return -1;
    return static_cast<int>((end - begin) / 16 - 1);
}

inline std::string error_text(void *state) {
    auto end = read<uintptr_t>(state, 0x10);
    if (!readable(reinterpret_cast<void *>(end - 16), 16))
        return "unreadable Lua error";
    auto value = reinterpret_cast<void *>(end - 16);
    if ((read<uint8_t>(value, 8) & 15) != 4)
        return "non-string Lua error";
    auto str = read<void *>(value, 0);
    if (!readable(str, 24))
        return "unreadable Lua string";
    size_t length = read<uint8_t>(str, 8) == 4 ? read<uint8_t>(str, 11) : read<size_t>(str, 16);
    if (length > 2048)
        length = 2048;
    auto bytes = static_cast<const char *>(str) + 24;
    if (!readable(bytes, length))
        return "unreadable Lua string payload";
    return std::string(bytes, length);
}

inline bool run(uintptr_t module, void *state, const char *code, size_t size, bool pass_registry,
                std::string &error) {
    int saved_top = top(state);
    if (saved_top < 0) {
        error = "private Lua stack layout rejected";
        return false;
    }
    using Load = int (*)(void *, uintptr_t, void *, const char *, const char *);
    using Call = int (*)(void *, int, uintptr_t, int);
    using SetTop = void (*)(void *, int);
    using Index = const void *(*)(void *, int);
    struct Buffer {
        const char *data;
        size_t size;
    } buffer{code, size};
    auto load = reinterpret_cast<Load>(module + load_rva);
    auto call = reinterpret_cast<Call>(module + pcall_rva);
    auto settop = reinterpret_cast<SetTop>(module + settop_rva);
    auto index = reinterpret_cast<Index>(module + index_rva);
    int status = load(state, 0, &buffer, "@LifetimeGuard/BoneSystem/Bootstrap.lua", "t");
    if (!status && pass_registry) {
        auto ci = read<void *>(state, 0x20);
        auto end = read<uintptr_t>(state, 0x10);
        auto limit = read<uintptr_t>(ci, 8);
        auto registry = index(state, registry_index);
        if (limit < end || limit - end < 16 || !readable(registry, 16) ||
            !readable(reinterpret_cast<void *>(end), 16) ||
            (read<uint8_t>(registry, 8) & 15) != 5) {
            settop(state, saved_top);
            error = "private Lua registry/stack capacity rejected";
            return false;
        }
        // Equivalent to lua_pushvalue(L, LUA_REGISTRYINDEX); no registry/global mutation.
        std::memcpy(reinterpret_cast<void *>(end), registry, 16);
        end += 16;
        std::memcpy(static_cast<unsigned char *>(state) + 0x10, &end, sizeof(end));
    }
    if (!status)
        status = call(state, pass_registry ? 1 : 0, 0, 0);
    if (status)
        error = error_text(state);
    settop(state, saved_top);
    if (top(state) != saved_top) {
        error = "Lua stack restoration failed";
        return false;
    }
    return status == 0;
}
} // namespace stock_vm
