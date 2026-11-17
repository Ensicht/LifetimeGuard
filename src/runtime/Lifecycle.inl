// 加载生命周期与事件唤醒。普通加载和同场景黑屏分别结束，不扩大保护窗口。
void request_protection_report() {
    g_protection_report_requested.store(true, std::memory_order_release);
    if (g_report_event != nullptr) {
        SetEvent(g_report_event);
    }
}

void retain_install_detail(const char *text, std::size_t length);
void write_failure_detail(dstorage_guard::DiagnosticKind kind, const char *text,
                          std::size_t length);
void write_protection_report();

std::uint64_t query_qpc() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

void record_loading_gate_event(LoadingEventKind kind, LoadingEventSource source,
                               std::uint64_t generation) {
    const auto sequence = g_loading_event_next.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence > k_loading_event_capacity) {
        return;
    }
    auto &event = g_loading_events[sequence - 1];
    event.commit.store((sequence << 1U) | 1U, std::memory_order_release);
    event.qpc = query_qpc();
    event.generation = generation;
    event.active_leases = g_active_leases.load(std::memory_order_acquire);
    event.thread_id = GetCurrentThreadId();
    event.kind = static_cast<std::uint32_t>(kind);
    event.source = static_cast<std::uint32_t>(source);
    std::atomic_thread_fence(std::memory_order_release);
    event.commit.store(sequence << 1U, std::memory_order_release);
    if (g_report_event != nullptr) {
        SetEvent(g_report_event);
    }
}

// 开启一个加载代次；重叠起点由门控策略合并，不能重复增加代次。
void mark_loading_start(LoadingEventSource source, bool requires_fade_in) {
    std::uint64_t generation{};
    if (!g_loading_gate.mark_start(requires_fade_in, &generation)) {
        return;
    }
    record_loading_gate_event(LoadingEventKind::start, source, generation);
}

void mark_loading_end(LoadingEventSource source, bool is_fade_in) {
    std::uint64_t generation{};
    if (!g_loading_gate.mark_end(is_fade_in, &generation)) {
        return;
    }
    record_loading_gate_event(LoadingEventKind::end, source, generation);
}

void mark_loading_signal(LoadingEventSource source, bool is_fade_in) {
    std::uint64_t generation{};
    if (!g_loading_gate.mark_end_signal(is_fade_in, &generation)) {
        return;
    }
    record_loading_gate_event(LoadingEventKind::signal, source, generation);
}

int pre_environment_load(int, void **, REFrameworkTypeDefinitionHandle *, unsigned long long) {
    mark_loading_start(LoadingEventSource::environment_before, false);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void post_environment_load_end(void **, REFrameworkTypeDefinitionHandle, unsigned long long) {
    mark_loading_signal(LoadingEventSource::environment_end, false);
}

void post_player_load_end(void **, REFrameworkTypeDefinitionHandle, unsigned long long) {
    mark_loading_signal(LoadingEventSource::player_end, false);
}

int pre_fast_travel_load(int, void **, REFrameworkTypeDefinitionHandle *, unsigned long long) {
    if (!g_loading_gate.snapshot().active) {
        mark_loading_start(LoadingEventSource::fast_travel_setup, true);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void post_camera_fade_in(void **, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (g_loading_gate.snapshot().requires_fade_in) {
        mark_loading_end(LoadingEventSource::camera_fade_in, true);
    }
}

// 仅普通加载门开启时读取 Loading/Scene；快速旅行等待自己的淡入事件。
void post_update_motion_loading_gate() {
    auto snapshot = g_loading_gate.snapshot();
    if (!snapshot.active || snapshot.requires_fade_in || g_reframework_sdk == nullptr ||
        g_reframework_sdk->functions == nullptr) {
        return;
    }

    const auto *functions = g_reframework_sdk->functions;
    if (functions->get_vm_context == nullptr || functions->get_managed_singleton == nullptr ||
        functions->get_native_singleton == nullptr || g_game_flow_get_loading == nullptr) {
        return;
    }
    const auto vm_context = functions->get_vm_context();
    const auto game_flow = functions->get_managed_singleton("app.GameFlowManager");
    if (vm_context == nullptr || game_flow == nullptr) {
        return;
    }
    const auto loading = g_game_flow_get_loading(vm_context, game_flow);
    if (loading) {
        g_loading_gate.mark_loading_seen();
        return;
    }

    snapshot = g_loading_gate.snapshot();
    if (!snapshot.end_seen && !snapshot.saw_loading) {
        return;
    }
    auto *scene_manager = functions->get_native_singleton("via.SceneManager");
    if (scene_manager == nullptr) {
        return;
    }
    auto *scene =
        g_scene_get_current != nullptr ? g_scene_get_current(vm_context, scene_manager) : nullptr;
    auto *main_view = g_scene_get_main_view != nullptr
                          ? g_scene_get_main_view(vm_context, scene_manager)
                          : nullptr;
    std::uint64_t generation{};
    if (g_loading_gate.try_end_stable_scene(false, scene != nullptr || main_view != nullptr,
                                            &generation)) {
        record_loading_gate_event(LoadingEventKind::end, LoadingEventSource::stable_scene,
                                  generation);
    }
}

REFrameworkMethodHandle find_managed_method(const REFrameworkPluginInitializeParam *param,
                                            const char *type_name, const char *signature_name,
                                            const char *plain_name) {
    if (param == nullptr || param->sdk == nullptr || param->sdk->functions == nullptr ||
        param->sdk->tdb == nullptr || param->sdk->tdb->find_method == nullptr ||
        param->sdk->functions->get_tdb == nullptr) {
        return nullptr;
    }
    const auto tdb = param->sdk->functions->get_tdb();
    if (tdb == nullptr) {
        return nullptr;
    }
    auto method = param->sdk->tdb->find_method(tdb, type_name, signature_name);
    if (method == nullptr && plain_name != nullptr) {
        method = param->sdk->tdb->find_method(tdb, type_name, plain_name);
    }
    return method;
}

bool add_managed_hook(const REFrameworkPluginInitializeParam *param, const char *type_name,
                      const char *signature_name, const char *plain_name, REFPreHookFn pre,
                      REFPostHookFn post) {
    if (param == nullptr || param->sdk == nullptr || param->sdk->functions == nullptr ||
        param->sdk->functions->add_hook == nullptr) {
        return false;
    }
    const auto method = find_managed_method(param, type_name, signature_name, plain_name);
    if (method == nullptr) {
        return false;
    }
    return param->sdk->functions->add_hook(method, pre, post, false) != 0;
}

void *get_method_function(const REFrameworkPluginInitializeParam *param, const char *type_name,
                          const char *signature_name, const char *plain_name) {
    if (param == nullptr || param->sdk == nullptr || param->sdk->method == nullptr ||
        param->sdk->method->get_function == nullptr) {
        return nullptr;
    }
    const auto method = find_managed_method(param, type_name, signature_name, plain_name);
    return method != nullptr ? param->sdk->method->get_function(method) : nullptr;
}

std::uint32_t install_stable_scene_observer(const REFrameworkPluginInitializeParam *param) {
    if (param == nullptr || param->functions == nullptr ||
        param->functions->on_post_application_entry == nullptr || param->sdk == nullptr ||
        param->sdk->functions == nullptr) {
        return 0;
    }
    g_reframework_sdk = param->sdk;
    g_game_flow_get_loading = reinterpret_cast<GameFlowLoadingFn>(
        get_method_function(param, "app.GameFlowManager", "get_Loading()", "get_Loading"));
    g_scene_get_current = reinterpret_cast<NativeObjectGetterFn>(
        get_method_function(param, "via.SceneManager", "get_CurrentScene()", "get_CurrentScene"));
    g_scene_get_main_view = reinterpret_cast<NativeObjectGetterFn>(
        get_method_function(param, "via.SceneManager", "get_MainView()", "get_MainView"));

    std::uint32_t state{};
    if (g_game_flow_get_loading != nullptr) {
        state |= k_lifecycle_game_flow_loading;
    }
    if (g_scene_get_current != nullptr || g_scene_get_main_view != nullptr) {
        state |= k_lifecycle_scene_identity;
    }
    if ((state & (k_lifecycle_game_flow_loading | k_lifecycle_scene_identity)) ==
            (k_lifecycle_game_flow_loading | k_lifecycle_scene_identity) &&
        param->functions->on_post_application_entry("UpdateMotion",
                                                    &post_update_motion_loading_gate)) {
        state |= k_lifecycle_update_motion;
    }
    return state;
}

// 必须具备完整起止能力才放行保护；缺少结束边界时保持旁路。
bool install_loading_lifecycle_hooks(const REFrameworkPluginInitializeParam *param) {
    g_loading_gate.configure(false, false);
    std::uint32_t state{};

    const auto environment_start =
        add_managed_hook(param, "app.EnvironmentManager", "evSceneLoadBefore()",
                         "evSceneLoadBefore", &pre_environment_load, nullptr);
    if (environment_start) {
        state |= k_lifecycle_environment_start;
    }

    const auto environment_end =
        add_managed_hook(param, "app.EnvironmentManager", "evSceneLoadEnd()", "evSceneLoadEnd",
                         nullptr, &post_environment_load_end);
    if (environment_end) {
        state |= k_lifecycle_environment_end;
    }

    std::uint32_t player_end_count{};
    constexpr const char *player_end_methods[][2] = {
        {"evSceneLoadEnd()", "evSceneLoadEnd"},
        {"evSceneLoadEnd_FastTravel()", "evSceneLoadEnd_FastTravel"},
        {"evSceneLoadEnd_SceneTransition()", "evSceneLoadEnd_SceneTransition"},
        {"evSceneLoadEnd_ThroughJunction()", "evSceneLoadEnd_ThroughJunction"},
    };
    for (const auto &names : player_end_methods) {
        if (add_managed_hook(param, "app.PlayerManager", names[0], names[1], nullptr,
                             &post_player_load_end)) {
            ++player_end_count;
        }
    }
    if (player_end_count != 0) {
        state |= k_lifecycle_player_end;
    }

    const auto camera_fade = add_managed_hook(param, "app.CameraManager", "onSceneLoadFadeIn()",
                                              "onSceneLoadFadeIn", nullptr, &post_camera_fade_in);
    if (camera_fade) {
        state |= k_lifecycle_camera_fade_end;
        if (add_managed_hook(param, "app.mcFastTravel", "setupLoadingEvent()", "setupLoadingEvent",
                             &pre_fast_travel_load, nullptr)) {
            state |= k_lifecycle_fast_travel_start;
        }
    }

    state |= install_stable_scene_observer(param);

    g_lifecycle_hook_state.store(state, std::memory_order_release);
    const auto has_start = (state & k_lifecycle_environment_start) != 0;
    const auto has_end = (state & (k_lifecycle_environment_end | k_lifecycle_player_end)) != 0;
    const auto has_stable_scene_observer =
        (state & (k_lifecycle_update_motion | k_lifecycle_game_flow_loading |
                  k_lifecycle_scene_identity)) ==
        (k_lifecycle_update_motion | k_lifecycle_game_flow_loading | k_lifecycle_scene_identity);
    const auto has_black_screen_pair =
        (state & (k_lifecycle_fast_travel_start | k_lifecycle_camera_fade_end)) ==
        (k_lifecycle_fast_travel_start | k_lifecycle_camera_fade_end);
    const auto available =
        has_start && has_end && has_stable_scene_observer && has_black_screen_pair;
    g_loading_gate.configure(available, available);
    return available;
}
