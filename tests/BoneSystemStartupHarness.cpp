// Run the packaged entry in a disposable Lua host, never in the game.
#include <windows.h>
#include <reframework/API.h>

#include <cstdarg>
#include <cstdio>
#include <string>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "BoneSystem/NativeVm.h"

namespace {
uintptr_t stock_base{};
int (*start_entry)(lua_State *);
int lock_depth{}, lock_count{};
std::string last_log;

void capture(const char *format, ...) {
    char buffer[2048]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    last_log = buffer;
}

void lock_lua() {
    ++lock_depth;
    ++lock_count;
}

void unlock_lua() {
    --lock_depth;
}

int probe(lua_State *state) {
    lock_count = 0;
    last_log.clear();
    // The Python driver pins the stock fixture before permitting private-state access.
    if (stock_base) {
        auto slot = reinterpret_cast<void **>(stock_base + stock_vm::state_rva);
        auto previous = *slot;
        *slot = state;
        start_entry(nullptr);
        *slot = previous;
    } else {
        start_entry(state);
    }
    lua_pushlstring(state, last_log.data(), last_log.size());
    lua_pushinteger(state, lock_depth);
    lua_pushinteger(state, lock_count);
    return 3;
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 5) {
        return 2;
    }
    const std::string mode = argv[3];
    if (std::string(argv[2]) != "-") {
        auto stock = LoadLibraryA(argv[2]);
        if (!stock) {
            return 3;
        }
        if (mode == "ready" || mode == "unmatched-lookup") {
            stock_base = reinterpret_cast<uintptr_t>(stock);
        }
    }
    auto guard = LoadLibraryA(argv[1]);
    if (!guard) {
        return 4;
    }
    auto initialize = reinterpret_cast<REFPluginInitializeFn>(
        GetProcAddress(guard, "reframework_plugin_initialize"));
    start_entry = reinterpret_cast<int (*)(lua_State *)>(
        GetProcAddress(guard, "lifetimeguard_bonesystem_start"));
    if (!initialize || !start_entry) {
        return 5;
    }
    REFrameworkPluginVersion version{1, 15, 0, nullptr};
    REFrameworkPluginFunctions functions{};
    functions.log_info = functions.log_error = capture;
    functions.lock_lua = lock_lua;
    functions.unlock_lua = unlock_lua;
    REFrameworkPluginInitializeParam parameter{nullptr, &version, &functions, nullptr, nullptr};
    // The offline host does not supply a DirectStorage/SDK environment.
    initialize(&parameter);

    for (int generation = 1; generation <= 2; ++generation) {
        auto state = luaL_newstate();
        luaL_openlibs(state);
        lua_register(state, "probe_bonesystem", probe);
        lua_pushstring(state, argv[3]);
        lua_setglobal(state, "case_mode");
        lua_pushstring(state, argv[4]);
        lua_setglobal(state, "owner_key");
        const int failed = luaL_dofile(state, "Startup.lua");
        if (failed) {
            std::fprintf(stderr, "%s\n", lua_tostring(state, -1));
            lua_close(state);
            return 1;
        }
        lua_close(state);
        std::printf("PASS %s, generation %d\n", argv[3], generation);
    }
    return 0;
}
