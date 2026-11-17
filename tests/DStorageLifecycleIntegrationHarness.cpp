#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reframework/API.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "MockGuardStatusHost.h"

namespace {

enum class MethodId : std::uintptr_t {
    environment_start = 1,
    environment_end,
    player_end,
    player_fast_end,
    player_transition_end,
    player_junction_end,
    camera_fade,
    fast_travel_start,
    game_flow_loading,
    scene_current,
    scene_main_view,
    count,
};

struct HookCapture {
    REFPreHookFn pre{};
    REFPostHookFn post{};
};

struct LoadingLifecycleStats {
    std::uint32_t version{};
    std::uint32_t lifecycle_state{};
    std::uint32_t active{};
    std::uint32_t requires_fade_in{};
    std::uint32_t end_seen{};
    std::uint32_t saw_loading{};
    std::uint64_t generation{};
};

using InitializeFn = bool (*)(const REFrameworkPluginInitializeParam*);
using GetLoadingStatsFn = bool (*)(LoadingLifecycleStats*, std::size_t);

std::array<HookCapture, static_cast<std::size_t>(MethodId::count)> g_hooks{};
REFOnPostApplicationEntryCb g_update_motion{};
REFOnImGuiDrawUICb g_draw_ui{};
unsigned g_frame_registrations{};
unsigned g_ui_registrations{};
bool register_draw_ui(REFOnImGuiDrawUICb cb) {
    ++g_ui_registrations;
    g_draw_ui = cb;
    return cb != nullptr;
}
bool reject_frame_callback(REFOnImGuiFrameCb) { ++g_frame_registrations; return false; }
bool g_loading{};
void* g_current_scene{};
void* g_main_view{};
std::uint64_t g_vm_context_reads{};
std::uint64_t g_managed_singleton_reads{};
std::uint64_t g_native_singleton_reads{};
std::uint64_t g_loading_reads{};
std::uint64_t g_scene_reads{};
int g_dummy_tdb{};
int g_dummy_vm{};
int g_dummy_game_flow{};
int g_dummy_scene_manager{};
int g_dummy_scene{};

REFrameworkMethodHandle method_handle(MethodId id) {
    return reinterpret_cast<REFrameworkMethodHandle>(
        static_cast<std::uintptr_t>(id));
}

std::size_t method_index(REFrameworkMethodHandle method) {
    return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(method));
}

bool name_is(const char* value, const char* signature, const char* plain) {
    return value != nullptr &&
        (std::strcmp(value, signature) == 0 || std::strcmp(value, plain) == 0);
}

REFrameworkMethodHandle fake_find_method(
    REFrameworkTDBHandle, const char* type_name, const char* method_name) {
    if (type_name == nullptr || method_name == nullptr) {
        return nullptr;
    }
    if (std::strcmp(type_name, "app.EnvironmentManager") == 0) {
        if (name_is(method_name, "evSceneLoadBefore()", "evSceneLoadBefore")) {
            return method_handle(MethodId::environment_start);
        }
        if (name_is(method_name, "evSceneLoadEnd()", "evSceneLoadEnd")) {
            return method_handle(MethodId::environment_end);
        }
    }
    if (std::strcmp(type_name, "app.PlayerManager") == 0) {
        if (name_is(method_name, "evSceneLoadEnd()", "evSceneLoadEnd")) {
            return method_handle(MethodId::player_end);
        }
        if (name_is(
                method_name, "evSceneLoadEnd_FastTravel()",
                "evSceneLoadEnd_FastTravel")) {
            return method_handle(MethodId::player_fast_end);
        }
        if (name_is(
                method_name, "evSceneLoadEnd_SceneTransition()",
                "evSceneLoadEnd_SceneTransition")) {
            return method_handle(MethodId::player_transition_end);
        }
        if (name_is(
                method_name, "evSceneLoadEnd_ThroughJunction()",
                "evSceneLoadEnd_ThroughJunction")) {
            return method_handle(MethodId::player_junction_end);
        }
    }
    if (std::strcmp(type_name, "app.CameraManager") == 0 &&
        name_is(method_name, "onSceneLoadFadeIn()", "onSceneLoadFadeIn")) {
        return method_handle(MethodId::camera_fade);
    }
    if (std::strcmp(type_name, "app.mcFastTravel") == 0 &&
        name_is(method_name, "setupLoadingEvent()", "setupLoadingEvent")) {
        return method_handle(MethodId::fast_travel_start);
    }
    if (std::strcmp(type_name, "app.GameFlowManager") == 0 &&
        name_is(method_name, "get_Loading()", "get_Loading")) {
        return method_handle(MethodId::game_flow_loading);
    }
    if (std::strcmp(type_name, "via.SceneManager") == 0) {
        if (name_is(method_name, "get_CurrentScene()", "get_CurrentScene")) {
            return method_handle(MethodId::scene_current);
        }
        if (name_is(method_name, "get_MainView()", "get_MainView")) {
            return method_handle(MethodId::scene_main_view);
        }
    }
    return nullptr;
}

bool fake_get_loading(
    REFrameworkVMContextHandle, REFrameworkManagedObjectHandle) {
    ++g_loading_reads;
    return g_loading;
}

void* fake_get_current_scene(REFrameworkVMContextHandle, void*) {
    ++g_scene_reads;
    return g_current_scene;
}

void* fake_get_main_view(REFrameworkVMContextHandle, void*) {
    ++g_scene_reads;
    return g_main_view;
}

void* fake_get_method_function(REFrameworkMethodHandle method) {
    switch (static_cast<MethodId>(method_index(method))) {
    case MethodId::game_flow_loading:
        return reinterpret_cast<void*>(&fake_get_loading);
    case MethodId::scene_current:
        return reinterpret_cast<void*>(&fake_get_current_scene);
    case MethodId::scene_main_view:
        return reinterpret_cast<void*>(&fake_get_main_view);
    default:
        return nullptr;
    }
}

REFrameworkTDBHandle fake_get_tdb() {
    return reinterpret_cast<REFrameworkTDBHandle>(&g_dummy_tdb);
}

REFrameworkVMContextHandle fake_get_vm_context() {
    ++g_vm_context_reads;
    return reinterpret_cast<REFrameworkVMContextHandle>(&g_dummy_vm);
}

REFrameworkManagedObjectHandle fake_get_managed_singleton(const char* name) {
    ++g_managed_singleton_reads;
    return name != nullptr && std::strcmp(name, "app.GameFlowManager") == 0
        ? reinterpret_cast<REFrameworkManagedObjectHandle>(&g_dummy_game_flow)
        : nullptr;
}

void* fake_get_native_singleton(const char* name) {
    ++g_native_singleton_reads;
    return name != nullptr && std::strcmp(name, "via.SceneManager") == 0
        ? &g_dummy_scene_manager
        : nullptr;
}

unsigned int fake_add_hook(
    REFrameworkMethodHandle method, REFPreHookFn pre, REFPostHookFn post,
    bool) {
    const auto index = method_index(method);
    if (index == 0 || index >= g_hooks.size()) {
        return 0;
    }
    g_hooks[index] = {pre, post};
    return 1;
}

bool fake_on_post_application_entry(
    const char* name, REFOnPostApplicationEntryCb callback) {
    if (name == nullptr || std::strcmp(name, "UpdateMotion") != 0) {
        return false;
    }
    g_update_motion = callback;
    return callback != nullptr;
}

bool invoke_pre(MethodId id) {
    const auto callback = g_hooks[static_cast<std::size_t>(id)].pre;
    if (callback == nullptr) {
        return false;
    }
    void* args[1]{};
    REFrameworkTypeDefinitionHandle types[1]{};
    return callback(0, args, types, 0) == REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

bool invoke_post(MethodId id) {
    const auto callback = g_hooks[static_cast<std::size_t>(id)].post;
    if (callback == nullptr) {
        return false;
    }
    void* result{};
    callback(&result, nullptr, 0);
    return true;
}

bool expect(bool value, const char* label, unsigned& failures) {
    std::printf("%s: %s\n", value ? "PASS" : "FAIL", label);
    if (!value) {
        ++failures;
    }
    return value;
}

LoadingLifecycleStats read_stats(
    GetLoadingStatsFn getter, unsigned& failures) {
    LoadingLifecycleStats stats{};
    expect(getter != nullptr && getter(&stats, sizeof(stats)),
        "read lifecycle snapshot", failures);
    return stats;
}

} // namespace

int main() {
    unsigned failures{};
    const auto guard = LoadLibraryW(L"DStorageFileLifetimeGuard.dll");
    if (!expect(guard != nullptr, "load guard DLL", failures)) {
        return 1;
    }
    const auto initialize = reinterpret_cast<InitializeFn>(
        GetProcAddress(guard, "reframework_plugin_initialize"));
    const auto get_loading_stats = reinterpret_cast<GetLoadingStatsFn>(
        GetProcAddress(
            guard, "dstorage_file_lifetime_guard_get_loading_stats"));
    if (!expect(initialize != nullptr, "resolve initialize export", failures) ||
        !expect(get_loading_stats != nullptr, "resolve lifecycle export", failures)) {
        return 1;
    }

    REFrameworkPluginFunctions plugin_functions{};
    plugin_functions.on_post_application_entry =
        &fake_on_post_application_entry;
    plugin_functions.on_imgui_draw_ui = &register_draw_ui;
    plugin_functions.on_imgui_frame = &reject_frame_callback;
    REFrameworkSDKFunctions sdk_functions{};
    sdk_functions.get_tdb = &fake_get_tdb;
    sdk_functions.get_vm_context = &fake_get_vm_context;
    sdk_functions.get_managed_singleton = &fake_get_managed_singleton;
    sdk_functions.get_native_singleton = &fake_get_native_singleton;
    sdk_functions.add_hook = &fake_add_hook;
    REFrameworkTDB tdb{};
    tdb.find_method = &fake_find_method;
    REFrameworkTDBMethod method{};
    method.get_function = &fake_get_method_function;
    REFrameworkSDKData sdk{};
    sdk.functions = &sdk_functions;
    sdk.tdb = &tdb;
    sdk.method = &method;
    REFrameworkPluginVersion version{1, 15, 0, "MHWilds"};
    REFrameworkPluginInitializeParam param{};
    param.version = &version;
    param.functions = &plugin_functions;
    param.sdk = &sdk;
    param.reframework_module = GetModuleHandleW(nullptr);

    const auto initialized = initialize(&param);
    std::printf(
        "INFO: initialize returned %s (status UI remains available without DirectStorage)\n",
        initialized ? "true" : "false");
    expect(initialized && g_draw_ui != nullptr && g_frame_registrations == 0,
        "UI registers only draw-ui, including failed protection startup", failures);
    expect(initialize(&param) && g_ui_registrations == 1,
        "diagnostic-only initialization is idempotent without duplicate UI hooks", failures);
    const auto test_detours = reinterpret_cast<bool (*)(void**, void**, void**)>(GetProcAddress(
        guard, "dstorage_file_lifetime_guard_test_retry_detours"));
    void *test_ready = &g_dummy_scene, *test_release = &g_dummy_scene, *test_delete = &g_dummy_scene;
    initialize(nullptr);
    expect(test_detours != nullptr && !test_detours(&test_ready, &test_release, &test_delete) &&
        test_ready == &g_dummy_scene && test_release == &g_dummy_scene && test_delete == &g_dummy_scene,
        "game initialization cannot enable offline retry ABI, including later null reinitialization", failures);
    const auto test_gameplay = reinterpret_cast<bool (*)(bool, std::uint64_t)>(GetProcAddress(
        guard, "dstorage_file_lifetime_guard_test_gameplay_retry"));
    expect(test_gameplay != nullptr && !test_gameplay(true, 1000) && !test_gameplay(false, 2000),
        "game initialization rejects gameplay test clock and test switch", failures);
    REFImGuiFrameCbData ui_data{};
    ui_data.context = &mock_guard_ui::context_token;
    if (g_draw_ui != nullptr) { g_draw_ui(&ui_data); }
    expect(mock_guard_ui::contains("保护安装失败"),
        "production callback reports actual failed protection", failures);

    auto stats = read_stats(get_loading_stats, failures);
    expect(stats.version == 9, "production version is 9", failures);
    expect(stats.lifecycle_state == 0xFFU,
        "all eight lifecycle capabilities registered", failures);
    expect(stats.active == 1 && stats.requires_fade_in == 0,
        "bootstrap ordinary gate starts active", failures);
    expect(g_update_motion != nullptr, "UpdateMotion observer registered", failures);

    g_loading = false;
    g_current_scene = &g_dummy_scene;
    expect(invoke_post(MethodId::environment_end),
        "bootstrap end signal callback available", failures);
    g_update_motion();
    stats = read_stats(get_loading_stats, failures);
    expect(stats.active == 0, "bootstrap closes on stable scene", failures);

    expect(invoke_pre(MethodId::environment_start),
        "ordinary start callback available", failures);
    g_loading = true;
    g_update_motion();
    expect(invoke_post(MethodId::player_end),
        "Player load-end callback available", failures);
    stats = read_stats(get_loading_stats, failures);
    expect(stats.active == 1 && stats.end_seen == 1 && stats.saw_loading == 1,
        "Player end records evidence but does not close", failures);

    g_update_motion();
    stats = read_stats(get_loading_stats, failures);
    expect(stats.active == 1, "Loading true keeps ordinary gate active", failures);
    g_loading = false;
    g_current_scene = nullptr;
    g_main_view = nullptr;
    g_update_motion();
    stats = read_stats(get_loading_stats, failures);
    expect(stats.active == 1, "missing live scene keeps gate active", failures);
    g_current_scene = &g_dummy_scene;
    g_update_motion();
    stats = read_stats(get_loading_stats, failures);
    expect(stats.active == 0, "stable live scene closes ordinary gate", failures);

    expect(invoke_pre(MethodId::environment_start),
        "second ordinary route starts", failures);
    g_loading = true;
    g_update_motion();
    g_loading = false;
    g_update_motion();
    stats = read_stats(get_loading_stats, failures);
    expect(stats.active == 0,
        "Loading true-to-false evidence closes without Player callback", failures);

    const auto calls_before_fast = g_vm_context_reads;
    expect(invoke_pre(MethodId::fast_travel_start),
        "same-scene black-screen start callback available", failures);
    stats = read_stats(get_loading_stats, failures);
    expect(stats.active == 1 && stats.requires_fade_in == 1,
        "same-scene route requires camera fade", failures);
    expect(invoke_post(MethodId::player_fast_end),
        "early fast-travel Player end callback available", failures);
    g_loading = false;
    g_current_scene = &g_dummy_scene;
    g_update_motion();
    stats = read_stats(get_loading_stats, failures);
    expect(stats.active == 1 && stats.end_seen == 0,
        "stable-scene observer cannot close fade route", failures);
    expect(g_vm_context_reads == calls_before_fast,
        "fade route performs no GameFlow polling", failures);
    expect(invoke_post(MethodId::camera_fade),
        "camera fade callback available", failures);
    stats = read_stats(get_loading_stats, failures);
    expect(stats.active == 0, "camera fade closes same-scene route", failures);

    const auto generation_before_seamless = stats.generation;
    const auto vm_reads_before_idle = g_vm_context_reads;
    for (unsigned i = 0; i < 100000; ++i) {
        g_update_motion();
    }
    stats = read_stats(get_loading_stats, failures);
    expect(stats.active == 0 && stats.generation == generation_before_seamless,
        "seamless idle updates do not create a loading generation", failures);
    expect(g_vm_context_reads == vm_reads_before_idle,
        "inactive UpdateMotion path performs zero REFramework reads", failures);
    const auto ui_reads_before = g_vm_context_reads + g_managed_singleton_reads +
        g_native_singleton_reads + g_loading_reads + g_scene_reads;
    mock_guard_ui::reset();
    for (unsigned i = 0; i < 1000 && g_draw_ui != nullptr; ++i) {
        g_draw_ui(&ui_data);
    }
    expect(ui_reads_before == g_vm_context_reads + g_managed_singleton_reads +
        g_native_singleton_reads + g_loading_reads + g_scene_reads,
        "1000 expanded production UI draws perform zero SDK reads", failures);
    expect(mock_guard_ui::pops == 1000 && mock_guard_ui::wraps == 1000 &&
        mock_guard_ui::unwraps == 1000, "UI stacks are balanced", failures);

    std::printf(
        "SUMMARY failures=%u vm=%llu managed=%llu native=%llu "
        "loading=%llu scene=%llu generation=%llu\n",
        failures,
        static_cast<unsigned long long>(g_vm_context_reads),
        static_cast<unsigned long long>(g_managed_singleton_reads),
        static_cast<unsigned long long>(g_native_singleton_reads),
        static_cast<unsigned long long>(g_loading_reads),
        static_cast<unsigned long long>(g_scene_reads),
        static_cast<unsigned long long>(stats.generation));
    return failures == 0 ? 0 : 2;
}
