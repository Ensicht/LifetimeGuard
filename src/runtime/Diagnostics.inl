// 日志格式、限额与历史轮换。此处只由既有报告路径调用，不增加热路径写盘。
void ensure_report_directory() {
    CreateDirectoryW(L"reframework", nullptr);
    CreateDirectoryW(L"reframework\\data", nullptr);
    CreateDirectoryW(L"reframework\\data\\DStorageFileLifetimeGuard", nullptr);
}

void append_report(const char *text, std::size_t length) {
    ensure_report_directory();
    const auto file = CreateFileW(L"reframework\\data\\DStorageFileLifetimeGuard\\events.log",
                                  FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written{};
    WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
}

void report_history_path(unsigned index, wchar_t *output, std::size_t output_count) {
    if (output == nullptr || output_count == 0) {
        return;
    }
    if (index == 0) {
        wcscpy_s(output, output_count, L"reframework\\data\\DStorageFileLifetimeGuard\\events.log");
        return;
    }
    swprintf_s(output, output_count, L"reframework\\data\\DStorageFileLifetimeGuard\\events.%u.log",
               index);
}

void note_report_rotation_failure(DWORD error) {
    ++g_report_rotation_failures;
    g_report_rotation_last_error = error;
}

void rotate_report_file(const wchar_t *source, const wchar_t *destination) {
    const auto attributes = GetFileAttributesW(source);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            note_report_rotation_failure(error);
        }
        if (!DeleteFileW(destination)) {
            const auto delete_error = GetLastError();
            if (delete_error != ERROR_FILE_NOT_FOUND && delete_error != ERROR_PATH_NOT_FOUND) {
                note_report_rotation_failure(delete_error);
            }
        }
        return;
    }
    if (MoveFileExW(source, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return;
    }
    const auto move_error = GetLastError();
    if (CopyFileW(source, destination, FALSE)) {
        if (!DeleteFileW(source)) {
            const auto delete_error = GetLastError();
            if (delete_error != ERROR_FILE_NOT_FOUND) {
                note_report_rotation_failure(delete_error);
            }
        }
        return;
    }
    const auto copy_error = GetLastError();
    note_report_rotation_failure(copy_error != ERROR_SUCCESS ? copy_error : move_error);
}

// 事件和故障日志按同一会话轮换；无故障留下空位，不能把不同启动对齐。
void rotate_report_history() {
    ensure_report_directory();
    wchar_t source[MAX_PATH]{};
    wchar_t destination[MAX_PATH]{};
    for (unsigned index = k_report_history_count; index > 0; --index) {
        report_history_path(index - 1, source, MAX_PATH);
        report_history_path(index, destination, MAX_PATH);
        rotate_report_file(source, destination);
    }
    for (unsigned index = k_report_history_count; index > 0; --index) {
        if (index == 1) {
            wcscpy_s(source, L"reframework\\data\\DStorageFileLifetimeGuard\\failures.log");
        } else {
            swprintf_s(source, L"reframework\\data\\DStorageFileLifetimeGuard\\failures.%u.log",
                       index - 1);
        }
        swprintf_s(destination, L"reframework\\data\\DStorageFileLifetimeGuard\\failures.%u.log",
                   index);
        rotate_report_file(source, destination);
    }
    report_history_path(0, destination, MAX_PATH);
    const auto file = CreateFileW(destination, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    } else {
        note_report_rotation_failure(GetLastError());
    }
}

void write_event(const DeferredCloseEvent &event, std::uint64_t sequence) {
    char buffer[12288]{};
    auto used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "deferred_close seq=%llu qpc=%llu tid=%u wrapper=0x%016llX "
        "internal=0x%016llX outstanding=%u\r\n",
        static_cast<unsigned long long>(sequence), static_cast<unsigned long long>(event.qpc),
        event.thread_id, static_cast<unsigned long long>(event.wrapper),
        static_cast<unsigned long long>(event.internal_file), event.outstanding));
    for (std::size_t index = 0; index < event.stack_count && used < sizeof(buffer); ++index) {
        MEMORY_BASIC_INFORMATION memory{};
        const auto address = reinterpret_cast<void *>(event.stack[index]);
        const auto queried = VirtualQuery(address, &memory, sizeof(memory));
        char module_path[MAX_PATH]{};
        const auto module =
            queried == sizeof(memory) ? reinterpret_cast<HMODULE>(memory.AllocationBase) : nullptr;
        if (module != nullptr) {
            GetModuleFileNameA(module, module_path, MAX_PATH);
        }
        const auto base = reinterpret_cast<std::uint64_t>(module);
        const auto added = std::snprintf(
            buffer + used, sizeof(buffer) - used,
            "stack[%zu]=0x%016llX module_base=0x%016llX rva=0x%llX "
            "module=\"%s\"\r\n",
            index, static_cast<unsigned long long>(event.stack[index]),
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(base != 0 ? event.stack[index] - base : 0),
            module_path);
        if (added <= 0) {
            break;
        }
        used += static_cast<std::size_t>(added);
    }
    append_report(buffer, used < sizeof(buffer) ? used : sizeof(buffer));
}

void write_replay_event(const DeferredCloseEvent &event, std::uint64_t sequence) {
    char buffer[512]{};
    const auto length =
        std::snprintf(buffer, sizeof(buffer),
                      "replayed_close seq=%llu qpc=%llu tid=%u wrapper=0x%016llX "
                      "internal=0x%016llX outstanding=0\r\n",
                      static_cast<unsigned long long>(sequence),
                      static_cast<unsigned long long>(event.replay_qpc), event.replay_thread_id,
                      static_cast<unsigned long long>(event.wrapper),
                      static_cast<unsigned long long>(event.internal_file));
    if (length > 0) {
        append_report(buffer, static_cast<std::size_t>(length) < sizeof(buffer)
                                  ? static_cast<std::size_t>(length)
                                  : sizeof(buffer));
    }
}

void write_resource_event(const ResourceRecoveryEvent &event, std::uint64_t sequence) {
    const auto decision = static_cast<dstorage_guard::ResourceRecoveryDecision>(event.decision);
    char buffer[1536]{};
    const auto length = std::snprintf(
        buffer, sizeof(buffer),
        "resource_completion_mismatch seq=%llu qpc=%llu tid=%u "
        "task=0x%016llX resource=0x%016llX expected=%llu completed=%llu "
        "decision=%s generation=%llu scope=loading native_complete=%u completion_seen=%u "
        "path_present=%u readable=%u texture=%u path_matches=%u attached=%u ready=%u "
        "parsed_present=%u parsed_readable=%u runtime_present=%u outcome=%s\r\n",
        static_cast<unsigned long long>(sequence), static_cast<unsigned long long>(event.qpc),
        event.thread_id, static_cast<unsigned long long>(event.task),
        static_cast<unsigned long long>(event.resource),
        static_cast<unsigned long long>(event.expected_bytes),
        static_cast<unsigned long long>(event.completed_bytes),
        dstorage_guard::recovery_decision_name(decision),
        static_cast<unsigned long long>(event.generation), event.snapshot.native_complete,
        event.snapshot.completion_callback_seen, event.snapshot.resource_path_present,
        event.snapshot.resource_readable, event.snapshot.texture_resource,
        event.snapshot.resource_path_matches, event.snapshot.resource_attached,
        event.snapshot.resource_ready, event.snapshot.parsed_texture_present,
        event.snapshot.parsed_texture_readable, event.snapshot.runtime_texture_present,
        dstorage_guard::is_resource_recovery(decision) ? "repair_dispatched_not_verified"
                                                       : "rejected_by_safety_policy");
    if (length > 0) {
        append_report(buffer, static_cast<std::size_t>(length) < sizeof(buffer)
                                  ? static_cast<std::size_t>(length)
                                  : sizeof(buffer));
        write_failure_detail(dstorage_guard::DiagnosticKind::resource, buffer,
                             static_cast<std::size_t>(length) < sizeof(buffer)
                                 ? static_cast<std::size_t>(length)
                                 : sizeof(buffer) - 1);
    }
}

const char *loading_event_source_name(LoadingEventSource source) {
    switch (source) {
    case LoadingEventSource::environment_before:
        return "app.EnvironmentManager.evSceneLoadBefore";
    case LoadingEventSource::environment_end:
        return "app.EnvironmentManager.evSceneLoadEnd";
    case LoadingEventSource::player_end:
        return "app.PlayerManager.evSceneLoadEnd";
    case LoadingEventSource::fast_travel_setup:
        return "app.mcFastTravel.setupLoadingEvent";
    case LoadingEventSource::camera_fade_in:
        return "app.CameraManager.onSceneLoadFadeIn";
    case LoadingEventSource::offline_test:
        return "offline_test";
    case LoadingEventSource::stable_scene:
        return "GameFlowManager.Loading=false+live_SceneManager";
    }
    return "unknown";
}

void write_loading_event(const LoadingGateEvent &event, std::uint64_t sequence) {
    const auto kind = static_cast<LoadingEventKind>(event.kind);
    const auto source = static_cast<LoadingEventSource>(event.source);
    char buffer[768]{};
    const auto length = std::snprintf(
        buffer, sizeof(buffer),
        "loading_gate_%s seq=%llu qpc=%llu tid=%u generation=%llu "
        "source=%s active_leases=%llu\r\n",
        kind == LoadingEventKind::start ? "start"
                                        : (kind == LoadingEventKind::signal ? "signal" : "end"),
        static_cast<unsigned long long>(sequence), static_cast<unsigned long long>(event.qpc),
        event.thread_id, static_cast<unsigned long long>(event.generation),
        loading_event_source_name(source), static_cast<unsigned long long>(event.active_leases));
    if (length > 0) {
        append_report(buffer, static_cast<std::size_t>(length) < sizeof(buffer)
                                  ? static_cast<std::size_t>(length)
                                  : sizeof(buffer));
    }
}

const char *install_component_name(InstallComponent component) {
    switch (component) {
    case InstallComponent::texture_retry:
        return "texture_retry";
    case InstallComponent::lifetime:
        return "lifetime";
    case InstallComponent::resource:
        return "resource";
    }
    return "unknown";
}

const char *install_stage_name(InstallStage stage) {
    switch (stage) {
    case InstallStage::locate_ready:
        return "locate_ready";
    case InstallStage::locate_finalizer:
        return "locate_finalizer";
    case InstallStage::locate_texture_vtable:
        return "locate_texture_vtable";
    case InstallStage::validate_target:
        return "validate_target";
    case InstallStage::mh_initialize:
        return "mh_initialize";
    case InstallStage::mh_create:
        return "mh_create";
    case InstallStage::mh_queue_enable:
        return "mh_queue_enable";
    case InstallStage::mh_apply_queued:
        return "mh_apply_queued";
    case InstallStage::mh_enable:
        return "mh_enable";
    case InstallStage::mh_disable_rollback:
        return "mh_disable_rollback";
    case InstallStage::mh_remove_rollback:
        return "mh_remove_rollback";
    case InstallStage::allocator_result:
        return "allocator_result";
    case InstallStage::validate_resource_worker:
        return "validate_resource_worker";
    case InstallStage::locate_task_slot:
        return "locate_task_slot";
    case InstallStage::pointer_install:
        return "pointer_install";
    case InstallStage::pointer_restore:
        return "pointer_restore";
    case InstallStage::pointer_rollback:
        return "pointer_rollback";
    case InstallStage::validate_task_submit:
        return "validate_task_submit";
    }
    return "unknown";
}

bool install_stage_uses_mh_status(InstallStage stage) {
    return stage == InstallStage::mh_initialize || stage == InstallStage::mh_create ||
           stage == InstallStage::mh_queue_enable || stage == InstallStage::mh_apply_queued ||
           stage == InstallStage::mh_enable || stage == InstallStage::mh_disable_rollback ||
           stage == InstallStage::mh_remove_rollback;
}

const char *mh_status_name(int status) {
    switch (static_cast<MH_STATUS>(status)) {
    case MH_UNKNOWN:
        return "MH_UNKNOWN";
    case MH_OK:
        return "MH_OK";
    case MH_ERROR_ALREADY_INITIALIZED:
        return "MH_ERROR_ALREADY_INITIALIZED";
    case MH_ERROR_NOT_INITIALIZED:
        return "MH_ERROR_NOT_INITIALIZED";
    case MH_ERROR_ALREADY_CREATED:
        return "MH_ERROR_ALREADY_CREATED";
    case MH_ERROR_NOT_CREATED:
        return "MH_ERROR_NOT_CREATED";
    case MH_ERROR_ENABLED:
        return "MH_ERROR_ENABLED";
    case MH_ERROR_DISABLED:
        return "MH_ERROR_DISABLED";
    case MH_ERROR_NOT_EXECUTABLE:
        return "MH_ERROR_NOT_EXECUTABLE";
    case MH_ERROR_UNSUPPORTED_FUNCTION:
        return "MH_ERROR_UNSUPPORTED_FUNCTION";
    case MH_ERROR_MEMORY_ALLOC:
        return "MH_ERROR_MEMORY_ALLOC";
    case MH_ERROR_MEMORY_PROTECT:
        return "MH_ERROR_MEMORY_PROTECT";
    case MH_ERROR_MODULE_NOT_FOUND:
        return "MH_ERROR_MODULE_NOT_FOUND";
    case MH_ERROR_FUNCTION_NOT_FOUND:
        return "MH_ERROR_FUNCTION_NOT_FOUND";
    }
    return "MH_STATUS_UNRECOGNIZED";
}

void utf8_from_wide(const wchar_t *input, char *output, std::size_t output_count) {
    if (output == nullptr || output_count == 0) {
        return;
    }
    output[0] = '\0';
    if (input == nullptr || input[0] == L'\0') {
        return;
    }
    const auto converted = WideCharToMultiByte(CP_UTF8, 0, input, -1, output,
                                               static_cast<int>(output_count), nullptr, nullptr);
    if (converted == 0) {
        output[0] = '\0';
    }
    output[output_count - 1] = '\0';
}

void module_path_from_base(std::uint64_t base, char *output, std::size_t output_count) {
    if (output == nullptr || output_count == 0) {
        return;
    }
    output[0] = '\0';
    if (base == 0) {
        return;
    }
    wchar_t wide_path[MAX_PATH]{};
    if (GetModuleFileNameW(reinterpret_cast<HMODULE>(base), wide_path, MAX_PATH) == 0) {
        return;
    }
    utf8_from_wide(wide_path, output, output_count);
}

std::uint64_t module_base_for_address(std::uint64_t address) {
    if (address == 0) {
        return 0;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<void *>(address), &memory, sizeof(memory)) !=
        sizeof(memory)) {
        return 0;
    }
    return reinterpret_cast<std::uint64_t>(memory.AllocationBase);
}

void format_win32_error(std::uint32_t error, char *output, std::size_t output_count) {
    if (output == nullptr || output_count == 0) {
        return;
    }
    output[0] = '\0';
    if (error == ERROR_SUCCESS) {
        strcpy_s(output, output_count, "ERROR_SUCCESS");
        return;
    }
    wchar_t wide_message[256]{};
    const auto length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, wide_message,
        static_cast<DWORD>(sizeof(wide_message) / sizeof(wide_message[0])), nullptr);
    if (length == 0) {
        std::snprintf(output, output_count, "WIN32_ERROR_%u", error);
        return;
    }
    for (std::size_t index = 0; wide_message[index] != L'\0'; ++index) {
        if (wide_message[index] == L'\r' || wide_message[index] == L'\n' ||
            wide_message[index] == L'"') {
            wide_message[index] = L' ';
        }
    }
    utf8_from_wide(wide_message, output, output_count);
}

void write_install_event(const InstallEvent &event, std::uint64_t sequence) {
    const auto component = static_cast<InstallComponent>(event.component);
    const auto stage = static_cast<InstallStage>(event.stage);
    if (stage == InstallStage::allocator_result) {
        const auto &a = event.allocation;
        char text[1536]{};
        const auto length = std::snprintf(
            text, sizeof(text),
            "hook_allocator seq=%llu qpc=%llu origin=0x%llX min=0x%llX max=0x%llX "
            "queries=%llu free_candidates=%llu attempts=%llu failures=%llu reused=%llu "
            "last_candidate=0x%llX query_error=%u allocation_error=%u "
            "last_region_base=0x%llX last_region_size=%llu last_region_state=0x%X\r\n",
            static_cast<unsigned long long>(sequence), static_cast<unsigned long long>(event.qpc),
            static_cast<unsigned long long>(a.origin), static_cast<unsigned long long>(a.minimum),
            static_cast<unsigned long long>(a.maximum), static_cast<unsigned long long>(a.queries),
            static_cast<unsigned long long>(a.free_candidates),
            static_cast<unsigned long long>(a.attempts),
            static_cast<unsigned long long>(a.allocation_failures),
            static_cast<unsigned long long>(a.reused),
            static_cast<unsigned long long>(a.last_candidate), a.query_error, a.allocation_error,
            static_cast<unsigned long long>(a.last_region_base),
            static_cast<unsigned long long>(a.last_region_size), a.last_region_state);
        if (length > 0 && static_cast<std::size_t>(length) < sizeof(text)) {
            append_report(text, length);
            retain_install_detail(text, length);
        }
        return;
    }
    const auto target_base = event.module_base;
    const auto redirect_base = module_base_for_address(event.redirect);
    std::uint64_t redirect_final = event.redirect;
    std::uint8_t redirect_code_bytes[k_install_code_bytes]{};
    std::uint32_t redirect_code_size{};
    if (is_readable_range(reinterpret_cast<void *>(event.redirect), k_install_code_bytes)) {
        std::memcpy(redirect_code_bytes, reinterpret_cast<void *>(event.redirect),
                    k_install_code_bytes);
        redirect_code_size = static_cast<std::uint32_t>(k_install_code_bytes);
        const auto next = decode_redirect(reinterpret_cast<void *>(event.redirect),
                                          redirect_code_bytes, k_install_code_bytes);
        if (next != 0) {
            redirect_final = next;
        }
    }
    const auto redirect_final_base = module_base_for_address(redirect_final);
    const auto detour_base = module_base_for_address(event.detour);
    char target_module[MAX_PATH * 3]{};
    char redirect_module[MAX_PATH * 3]{};
    char redirect_final_module[MAX_PATH * 3]{};
    char detour_module[MAX_PATH * 3]{};
    module_path_from_base(target_base, target_module, sizeof(target_module));
    module_path_from_base(redirect_base, redirect_module, sizeof(redirect_module));
    module_path_from_base(redirect_final_base, redirect_final_module,
                          sizeof(redirect_final_module));
    module_path_from_base(detour_base, detour_module, sizeof(detour_module));

    char code[k_install_code_bytes * 2 + 1]{};
    auto code_used = std::size_t{};
    for (std::size_t index = 0; index < event.code_size && index < k_install_code_bytes; ++index) {
        const auto added = std::snprintf(code + code_used, sizeof(code) - code_used, "%02X",
                                         static_cast<unsigned>(event.code[index]));
        if (added <= 0) {
            break;
        }
        code_used += static_cast<std::size_t>(added);
    }
    if (code_used == 0) {
        strcpy_s(code, sizeof(code), "-");
    }
    char redirect_code[k_install_code_bytes * 2 + 1]{};
    auto redirect_code_used = std::size_t{};
    for (std::size_t index = 0; index < redirect_code_size && index < k_install_code_bytes;
         ++index) {
        const auto added = std::snprintf(redirect_code + redirect_code_used,
                                         sizeof(redirect_code) - redirect_code_used, "%02X",
                                         static_cast<unsigned>(redirect_code_bytes[index]));
        if (added <= 0) {
            break;
        }
        redirect_code_used += static_cast<std::size_t>(added);
    }
    if (redirect_code_used == 0) {
        strcpy_s(redirect_code, sizeof(redirect_code), "-");
    }

    const char *status_name{};
    if (install_stage_uses_mh_status(stage)) {
        status_name = mh_status_name(event.status);
    } else if (stage == InstallStage::validate_task_submit) {
        status_name = event.status == 0 ? "original_entry"
                                        : (event.status == 2 ? "compatible_rel32_entry"
                                                             : "unrecognized_entry_or_body");
    } else if (stage == InstallStage::validate_target) {
        status_name = event.status == 0 ? "matched" : "mismatch";
    } else if (stage == InstallStage::pointer_install) {
        constexpr const char *names[] = {"installed", "protect_failed", "conflict",
                                         "restore_failed"};
        status_name = event.status >= 0 && event.status < 4 ? names[event.status] : "unknown";
    } else if (stage == InstallStage::pointer_restore || stage == InstallStage::pointer_rollback) {
        status_name = event.status == 0 ? "restored" : "failed";
    } else {
        status_name = event.status == 0 ? "found" : "missing";
    }
    char win32_message[256]{};
    format_win32_error(event.win32_error, win32_message, sizeof(win32_message));

    char buffer[8192]{};
    const auto length = std::snprintf(
        buffer, sizeof(buffer),
        "hook_install seq=%llu qpc=%llu tid=%u component=%s stage=%s "
        "index=%u status=%d status_name=%s win32_error=%u "
        "win32_message=\"%s\" target=0x%016llX "
        "target_module_base=0x%016llX target_rva=0x%llX "
        "target_protect=0x%08X target_module=\"%s\" bytes=%s "
        "redirect=0x%016llX redirect_module_base=0x%016llX "
        "redirect_rva=0x%llX redirect_module=\"%s\" "
        "redirect_bytes=%s redirect_final=0x%016llX "
        "redirect_final_module_base=0x%016llX "
        "redirect_final_rva=0x%llX redirect_final_module=\"%s\" "
        "detour=0x%016llX detour_module_base=0x%016llX "
        "detour_rva=0x%llX detour_module=\"%s\" "
        "original=0x%016llX\r\n",
        static_cast<unsigned long long>(sequence), static_cast<unsigned long long>(event.qpc),
        event.thread_id, install_component_name(component), install_stage_name(stage), event.index,
        event.status, status_name, event.win32_error, win32_message,
        static_cast<unsigned long long>(event.target), static_cast<unsigned long long>(target_base),
        static_cast<unsigned long long>(
            target_base != 0 && event.target >= target_base ? event.target - target_base : 0),
        event.protection, target_module, code, static_cast<unsigned long long>(event.redirect),
        static_cast<unsigned long long>(redirect_base),
        static_cast<unsigned long long>(redirect_base != 0 && event.redirect >= redirect_base
                                            ? event.redirect - redirect_base
                                            : 0),
        redirect_module, redirect_code, static_cast<unsigned long long>(redirect_final),
        static_cast<unsigned long long>(redirect_final_base),
        static_cast<unsigned long long>(redirect_final_base != 0 &&
                                                redirect_final >= redirect_final_base
                                            ? redirect_final - redirect_final_base
                                            : 0),
        redirect_final_module, static_cast<unsigned long long>(event.detour),
        static_cast<unsigned long long>(detour_base),
        static_cast<unsigned long long>(
            detour_base != 0 && event.detour >= detour_base ? event.detour - detour_base : 0),
        detour_module, static_cast<unsigned long long>(event.original));
    if (length > 0) {
        retain_install_detail(buffer, static_cast<std::size_t>(length) < sizeof(buffer)
                                          ? static_cast<std::size_t>(length)
                                          : sizeof(buffer) - 1);
        append_report(buffer, static_cast<std::size_t>(length) < sizeof(buffer)
                                  ? static_cast<std::size_t>(length)
                                  : sizeof(buffer));
    }
}

void write_texture_retry_events() {
    dstorage_guard::TextureRetryEvent event{};
    while (g_texture_retry.take_event(event)) {
        char text[768]{};
        const auto length = std::snprintf(
            text, sizeof(text),
            "texture_retry qpc=%llu thread=%u kind=%u resource=0x%llX manager=0x%llX parsed=0x%llX "
            "identity=0x%llX generation=%llu transfer=%u scope=%s attempt=%u failures=%u "
            "retry_after_ms=%llu "
            "kind_name=%s\r\n",
            static_cast<unsigned long long>(event.qpc), event.thread_id, event.kind,
            static_cast<unsigned long long>(event.resource),
            static_cast<unsigned long long>(event.manager),
            static_cast<unsigned long long>(event.parsed),
            static_cast<unsigned long long>(event.identity),
            static_cast<unsigned long long>(event.generation),
            event.transferred_reference ? 1U : 0U, event.gameplay ? "gameplay" : "loading",
            event.attempt, event.failures, static_cast<unsigned long long>(event.retry_after_ms),
            event.kind == 1   ? "failed_texture_receipt"
            : event.kind == 2 ? "queued"
            : event.kind == 3 ? "native_ready_and_runtime_recovered"
            : event.kind == 4 ? "native_retry_failed"
            : event.kind == 5 ? "cancelled_before_enqueue"
            : event.kind == 6 ? "queue_wake_failed"
                              : "unknown");
        if (length > 0 && static_cast<std::size_t>(length) < sizeof(text)) {
            append_report(text, length);
            if (event.kind >= 4) {
                write_failure_detail(dstorage_guard::DiagnosticKind::retry, text, length);
            }
        }
    }
}

#include "../GuardFailureReport.inl"
