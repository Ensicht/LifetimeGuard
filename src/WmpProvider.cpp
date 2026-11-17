// Include the shared core with the WMP provider's export and diagnostics policy.
#define LG_WMP_PROVIDER
#include "DStorageFileLifetimeGuard.cpp"
#include <psapi.h>
#include <mutex>

namespace wmp_lg {
std::mutex mutex;
REFrameworkPluginInitializeParam host{};
bool admitted = false, attempted = false, desired = false;
const char *reason = "provider_not_initialized";
HANDLE ownership = nullptr;

bool memory(const void *address, std::size_t size, bool executable = false) {
    if (!address || !size)
        return false;
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(address, &m, sizeof(m)) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto region = reinterpret_cast<std::uintptr_t>(m.BaseAddress);
    if (begin < region || size > m.RegionSize || begin - region > m.RegionSize - size)
        return false;
    return !executable || (m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                        PAGE_EXECUTE_WRITECOPY));
}
template <class F> bool function(F value) {
    return memory(reinterpret_cast<const void *>(value), 1, true);
}

bool capability(const REFrameworkPluginInitializeParam *p) {
    if (!memory(p, sizeof(*p)) || !memory(p->version, sizeof(*p->version)) ||
        p->version->major != 1 || p->version->minor < 15 ||
        !memory(p->functions, sizeof(*p->functions)) || !memory(p->sdk, sizeof(*p->sdk)))
        return false;
    const auto *s = p->sdk;
    if (!memory(s->functions, sizeof(*s->functions)) || !memory(s->tdb, sizeof(*s->tdb)) ||
        !memory(s->method, sizeof(*s->method)))
        return false;
    const auto *f = s->functions;
    return function(p->functions->on_post_application_entry) && function(f->add_hook) &&
           function(f->get_tdb) && function(f->get_vm_context) &&
           function(f->get_managed_singleton) && function(f->get_native_singleton) &&
           function(s->tdb->find_method) && function(s->method->get_function);
}

// Export inspection also catches renamed standalone DLLs. Unknown enumeration fails closed.
bool other_provider(HMODULE *found = nullptr, bool *known = nullptr) {
    if (found)
        *found = nullptr;
    if (known)
        *known = true;
    HMODULE modules[2048]{};
    DWORD needed{};
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed) ||
        needed > sizeof(modules)) {
        if (known)
            *known = false;
        return true;
    }
    for (std::size_t i = 0; i < needed / sizeof(HMODULE); ++i) {
        if (modules[i] == g_module)
            continue;
        if (GetProcAddress(modules[i], "dstorage_file_lifetime_guard_get_stats") ||
            GetProcAddress(modules[i], "wmp_lifetimeguard_start")) {
            if (found)
                *found = modules[i];
            return true;
        }
    }
    if (GetFileAttributesW(L"reframework\\plugins\\DStorageFileLifetimeGuard.dll") !=
        INVALID_FILE_ATTRIBUTES)
        return true;
    const auto error = GetLastError();
    const bool uncertain = error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
    if (known && uncertain)
        *known = false;
    return uncertain;
}

bool acquire() {
    if (ownership)
        return true;
    wchar_t name[96]{};
    std::swprintf(name, 96, L"Local\\WMPLifetimeGuardOwner_%lu", GetCurrentProcessId());
    HANDLE handle = CreateMutexW(nullptr, FALSE, name);
    const DWORD error = GetLastError();
    if (!handle || error == ERROR_ALREADY_EXISTS) {
        if (handle)
            CloseHandle(handle);
        return false;
    }
    ownership = handle; // Keep ownership through Lua reset and pending native callbacks.
    return true;
}

void start() {
    desired = true;
    if (attempted)
        return;
    if (!admitted) {
        reason = "ref_api_or_consumed_capability_unavailable";
        return;
    }
    if (other_provider()) {
        reason = "independent_or_other_provider_present_no_hooks";
        return;
    }
    if (!acquire()) {
        reason = "another_wmp_owner_present_no_hooks";
        return;
    }
    attempted = true; // Partial registration cannot be safely replayed.
    core_initialize(&host);
    reason = "native_initialization_attempted";
}
void close() {
    desired = false;
    reason = attempted ? "native_callbacks_retained_until_process_restart" : "not_started";
}
bool active() {
    return g_hooks_armed.load() || g_resource_hook_armed.load() || g_retry_armed.load();
}
const char *status() {
    if (attempted && !desired)
        return "restart_required";
    if (!desired)
        return "disabled";
    if (!admitted)
        return "unavailable";
    if (!attempted)
        return "blocked";
    if (!g_loading_gate.lifecycle_available())
        return "incompatible";
    if (g_hooks_armed.load() && g_resource_hook_armed.load() && g_retry_armed.load() &&
        g_loose_cleanup.caller())
        return "working";
    if (active())
        return "partial";
    if (g_iat_hook_state.load() == 1 && g_install_state.load() == 0)
        return "waiting_for_factory";
    return "incompatible";
}

bool request_id(char (&id)[192]) {
    FILE *file = std::fopen(
        "reframework/data/WildsMakeoverProject/Compatibility/LifetimeGuard.request", "rb");
    if (!file)
        return false;
    const auto count = std::fread(id, 1, sizeof(id), file);
    const bool ok = !std::ferror(file) && std::feof(file) && count > 0 && count < sizeof(id);
    std::fclose(file);
    if (!ok)
        return false;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = id[i];
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            c != ':' && c != ' ' && c != '-')
            return false;
    }
    return true;
}

// Only receipt() reads these counters. No callback, timer, scene scan or native UI is added.
struct JsonFields {
    FILE *file;
    void number(const char *key, std::uint64_t value) const {
        std::fprintf(file, "\"%s\":%llu,", key, static_cast<unsigned long long>(value));
    }
    void code(const char *key, int value) const {
        std::fprintf(file, "\"%s\":%d,", key, value);
    }
    void flag(const char *key, bool value) const {
        std::fprintf(file, "\"%s\":%s,", key, value ? "true" : "false");
    }
};

void own_runtime(FILE *file) {
    const JsonFields out{file};
    const auto loading = g_loading_gate.snapshot();
    const auto retries = g_texture_retry.stats();
    const auto gameplay = g_texture_retry.gameplay_stats();
    out.number("initialize_state", g_initialize_state.load());
    out.number("file_state", g_install_state.load());
    out.number("resource_state", g_resource_install_state.load());
    out.number("resource_mode", g_resource_hook_mode.load());
    out.number("iat_state", g_iat_hook_state.load());
    out.flag("lifecycle_available", loading.lifecycle_available);
    out.flag("loading", loading.active);
    out.number("active_leases", g_active_leases.load());
    out.number("deferred_closes", g_deferred_closes.load());
    out.number("texture_recoveries", g_resource_recoveries.load());
    out.code("resource_create_status", g_resource_create_hook_status.load());
    out.code("resource_enable_status", g_resource_enable_hook_status.load());
    out.number("retry_state", g_retry_install_state.load());
    out.number("retry_queued", retries.queued);
    out.number("retry_recovered", retries.recovered);
    out.number("retry_failed", retries.failed);
    out.code("retry_create_status", g_retry_create_status.load());
    out.code("retry_enable_status", g_retry_enable_status.load());
    out.flag("gameplay_retry", g_retry_gameplay_enabled.load());
    out.number("retry_inflight", gameplay.inflight);
    out.number("gameplay_queued", gameplay.gameplay_queued);
    out.number("retry_backoffs", gameplay.backoffs);
    out.number("retry_events_dropped", gameplay.events_dropped);
    out.flag("loose_cleanup_ready", g_loose_cleanup.caller() && g_hooks_armed.load());
    out.number("loose_cleanup_pairs", g_loose_cleanup.paired());
    out.number("guarded_requests", g_guarded_requests.load());
    out.number("completed_requests", g_completed_requests.load());
    out.number("replayed_closes", g_replayed_closes.load());
    out.number("unpaired_requests", g_unpaired_requests.load());
    out.number("registry_collisions", g_registry_collisions.load());
    out.number("lifecycle_state", g_lifecycle_hook_state.load());
    out.number("loading_generation", loading.generation);
    out.number("cleanup_profile_failure", g_loose_cleanup.failure());
    std::fputs("\"available\":true", file);
}

template <class F> F read_export(HMODULE module, const char *name) {
    const auto fn = reinterpret_cast<F>(GetProcAddress(module, name));
    return function(fn) ? fn : nullptr;
}

bool external_runtime(FILE *file, HMODULE module) {
    // Retain only a module already loaded by its owner; never load or initialize it.
    HMODULE retained{};
    if (!module || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                       reinterpret_cast<LPCWSTR>(module), &retained)) {
        std::fputs("\"available\":false", file);
        return false;
    }
    const auto stats = read_export<bool (*)(GuardStats *, std::size_t)>(
        retained, "dstorage_file_lifetime_guard_get_stats");
    GuardStats s{};
    const bool valid = stats && stats(&s, sizeof(s)) && s.version == k_version;
    if (valid) {
        const JsonFields out{file};
        out.number("file_state", s.install_state);
        out.number("guarded_requests", s.guarded_requests);
        out.number("completed_requests", s.completed_requests);
        out.number("active_leases", s.active_leases);
        out.number("deferred_closes", s.deferred_closes);
        out.number("replayed_closes", s.replayed_closes);
        out.number("unpaired_requests", s.unpaired_requests);
        out.number("registry_collisions", s.registry_collisions);
        const auto loading = read_export<bool (*)(LoadingLifecycleStats *, std::size_t)>(
            retained, "dstorage_file_lifetime_guard_get_loading_stats");
        LoadingLifecycleStats l{};
        if (loading && loading(&l, sizeof(l)) && l.version == k_version) {
            constexpr auto stable = k_lifecycle_update_motion | k_lifecycle_game_flow_loading |
                                    k_lifecycle_scene_identity | k_lifecycle_fast_travel_start |
                                    k_lifecycle_camera_fade_end;
            out.flag("lifecycle_available", (l.lifecycle_state & k_lifecycle_environment_start) &&
                                                (l.lifecycle_state & (k_lifecycle_environment_end |
                                                                      k_lifecycle_player_end)) &&
                                                (l.lifecycle_state & stable) == stable);
            out.flag("loading", l.active != 0);
            out.number("lifecycle_state", l.lifecycle_state);
            out.number("loading_generation", l.generation);
        }
        const auto retry = read_export<bool (*)(dstorage_guard::TextureRetryStats *, std::size_t,
                                                std::uint32_t *)>(
            retained, "dstorage_file_lifetime_guard_get_texture_retry_stats");
        dstorage_guard::TextureRetryStats r{};
        std::uint32_t state{};
        if (retry && retry(&r, sizeof(r), &state)) {
            out.number("retry_state", state);
            out.number("retry_queued", r.queued);
            out.number("retry_recovered", r.recovered);
            out.number("retry_failed", r.failed);
        }
        const auto gameplay =
            read_export<bool (*)(dstorage_guard::GameplayRetryStats *, std::size_t)>(
                retained, "dstorage_file_lifetime_guard_get_gameplay_retry_stats");
        dstorage_guard::GameplayRetryStats g{};
        if (gameplay && gameplay(&g, sizeof(g))) {
            out.number("retry_inflight", g.inflight);
            out.number("gameplay_queued", g.gameplay_queued);
            out.number("retry_backoffs", g.backoffs);
            out.number("retry_events_dropped", g.events_dropped);
        }
        const auto pairs = read_export<std::uint64_t (*)()>(
            retained, "dstorage_file_lifetime_guard_cleanup_pairs");
        if (pairs)
            out.number("loose_cleanup_pairs", pairs());
    }
    FreeLibrary(retained);
    std::fprintf(file, "\"available\":%s", valid ? "true" : "false");
    return valid;
}

bool receipt(const char *id) {
    constexpr auto path =
        "reframework/data/WildsMakeoverProject/Compatibility/LifetimeGuard.response.json";
    constexpr auto temp =
        "reframework/data/WildsMakeoverProject/Compatibility/LifetimeGuard.response.tmp";
    FILE *file = std::fopen(temp, "wb");
    if (!file)
        return false;
    HMODULE external{};
    bool known = true;
    const bool present = !attempted && other_provider(&external, &known);
    const auto observed_status = !known ? "outcome_unknown" : present ? "external_owner" : status();
    const auto observed_reason = !known ? "provider_inventory_unknown_no_hooks"
                                 : present
                                     ? (external ? "external_provider_loaded_wmp_yielded"
                                                 : "external_provider_file_present_not_loaded")
                                     : reason;
    const auto loading = g_loading_gate.snapshot();
    const int written = std::fprintf(
        file,
        "{\"schema\":1,\"provider\":\"wmp_lifetimeguard\",\"id\":\"%s\",\"status\":\"%s\",\"message\":\"%s\","
        "\"requested_enabled\":%s,\"active\":%s,\"restart_required\":%s,\"dll_loaded\":true,\"unloaded\":false,"
        "\"initialization_attempted\":%s,\"source_version\":\"" LG_BUILD_TEXT
        "\",\"file_armed\":%s,\"resource_armed\":%s,"
        "\"retry_armed\":%s,\"loose_ready\":%s,\"lifecycle_available\":%s,\"loading\":%s,\"active_leases\":%llu,"
        "\"install_state\":%u,\"resource_install_state\":%u,\"retry_install_state\":%u,\"cleanup_profile_failure\":%u,",
        id, observed_status, observed_reason, desired ? "true" : "false",
        active() ? "true" : "false", attempted && !desired ? "true" : "false",
        attempted ? "true" : "false", g_hooks_armed.load() ? "true" : "false",
        g_resource_hook_armed.load() ? "true" : "false", g_retry_armed.load() ? "true" : "false",
        g_loose_cleanup.caller() && g_hooks_armed.load() ? "true" : "false",
        loading.lifecycle_available ? "true" : "false", loading.active ? "true" : "false",
        static_cast<unsigned long long>(g_active_leases.load()), g_install_state.load(),
        g_resource_install_state.load(), g_retry_install_state.load(), g_loose_cleanup.failure());
    std::fprintf(
        file,
        "\"status_schema\":2,\"runtime_source\":\"%s\",\"runtime_complete\":%s,"
        "\"external_present\":%s,\"external_loaded\":%s,\"activity_unknown\":%s,\"runtime\":{",
        !known    ? "unknown"
        : present ? "standalone"
                  : "wmp",
        !present && known ? "true" : "false", present ? "true" : "false",
        external ? "true" : "false", !known ? "true" : "false");
    if (present)
        external_runtime(file, external);
    else if (known)
        own_runtime(file);
    else
        std::fputs("\"available\":false", file);
    std::fputs("}}", file);
    const bool flushed = written > 0 && !std::ferror(file) && std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    return flushed && closed &&
           MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
int command(unsigned op) {
    std::lock_guard<std::mutex> lock(mutex);
    char id[192]{};
    if (!request_id(id))
        return 0;
    if (op == 1)
        start();
    if (op == 2)
        close();
    receipt(id); // A missing/stale receipt is independently rejected by Lua.
    return 0;
}
} // namespace wmp_lg

extern "C" __declspec(dllexport) void
reframework_plugin_required_version(REFrameworkPluginVersion *v) {
    if (v) {
        v->major = 1;
        v->minor = 15;
        v->patch = 0;
        v->game_name = nullptr;
    }
}
extern "C" __declspec(dllexport) bool
reframework_plugin_initialize(const REFrameworkPluginInitializeParam *p) {
#ifdef LG_BONE_SYSTEM_COMPANION
    lifetimeguard::bonesystem::bind(p);
#endif
    static_cast<void>(
        &install_status_ui); // Retained core helper, deliberately never registered in WMP.
    std::lock_guard<std::mutex> lock(wmp_lg::mutex);
    if (wmp_lg::admitted)
        return true;
    wmp_lg::admitted = wmp_lg::capability(p);
    if (wmp_lg::admitted) {
        wmp_lg::host = *p;
        wmp_lg::reason = "provider_ready_no_hooks";
    }
    pin_module();
    return true; // Remain queryable when capabilities are absent; do not install hooks.
}
extern "C" __declspec(dllexport) int wmp_lifetimeguard_start(lua_State *) {
    return wmp_lg::command(1);
}
extern "C" __declspec(dllexport) int wmp_lifetimeguard_close(lua_State *) {
    return wmp_lg::command(2);
}
extern "C" __declspec(dllexport) int wmp_lifetimeguard_snapshot(lua_State *) {
    return wmp_lg::command(0);
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD why, LPVOID reserved) {
    return core_dll_main(module, why, reserved);
}

#ifdef LG_BONE_SYSTEM_COMPANION
extern "C" __declspec(dllexport) int lifetimeguard_bonesystem_start(lua_State *state) {
    lifetimeguard::bonesystem::start(
        state, "reframework/autorun/WMPOptimizationPatches/BoneSystem/Bootstrap.lua", true);
    return 0;
}
#endif
