 
bool install_iat_hook() {
    auto *image = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(nullptr));
    if (image == nullptr) {
        return false;
    }
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
    const auto import_rva =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (nt->Signature != IMAGE_NT_SIGNATURE || import_rva == 0) {
        return false;
    }
    auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(image + import_rva);
    for (; descriptor->Name != 0; ++descriptor) {
        const auto *module_name = reinterpret_cast<const char *>(image + descriptor->Name);
        if (_stricmp(module_name, "DSTORAGE.dll") != 0) {
            continue;
        }
        const auto names_available = descriptor->OriginalFirstThunk != 0;
        auto *names =
            names_available
                ? reinterpret_cast<IMAGE_THUNK_DATA64 *>(image + descriptor->OriginalFirstThunk)
                : nullptr;
        auto *addresses = reinterpret_cast<IMAGE_THUNK_DATA64 *>(image + descriptor->FirstThunk);
        const auto dstorage = GetModuleHandleW(L"dstorage.dll");
        const auto expected_factory =
            dstorage != nullptr ? GetProcAddress(dstorage, "DStorageGetFactory") : nullptr;
        for (std::size_t index = 0; addresses[index].u1.Function != 0; ++index) {
            bool match{};
            if (!names_available) {
                match =
                    expected_factory != nullptr &&
                    addresses[index].u1.Function == reinterpret_cast<ULONGLONG>(expected_factory);
            } else if (IMAGE_SNAP_BY_ORDINAL64(names[index].u1.Ordinal)) {
                match = IMAGE_ORDINAL64(names[index].u1.Ordinal) == 2;
            } else {
                const auto *import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(
                    image + names[index].u1.AddressOfData);
                match = std::strcmp(reinterpret_cast<const char *>(import->Name),
                                    "DStorageGetFactory") == 0;
            }
            if (!match) {
                continue;
            }
            auto *address = &addresses[index];
            extern  
                HRESULT WINAPI hook_dstorage_get_factory(REFIID, void **);
            const auto original = reinterpret_cast<DStorageGetFactoryFn>(address->u1.Function);
            DWORD old_protection{};
            if (!VirtualProtect(address, sizeof(*address), PAGE_READWRITE, &old_protection)) {
                return false;
            }
            g_original_get_factory.store(original, std::memory_order_release);
            const auto previous = InterlockedCompareExchangePointer(
                reinterpret_cast<void *volatile *>(&address->u1.Function),
                reinterpret_cast<void *>(&hook_dstorage_get_factory),
                reinterpret_cast<void *>(original));
            DWORD ignored{};
            VirtualProtect(address, sizeof(*address), old_protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), address, sizeof(*address));
            return previous == reinterpret_cast<void *>(original);
        }
    }
    return false;
}

HRESULT WINAPI hook_dstorage_get_factory(REFIID riid, void **output) {
    const auto original = g_original_get_factory.load(std::memory_order_acquire);
    if (original == nullptr) {
        return E_FAIL;
    }
    const auto result = original(riid, output);
    const auto install_sequence = g_install_event_next.load(std::memory_order_relaxed);
    if (g_loading_gate.lifecycle_available() && install_internal_hooks()) {
        install_resource_recovery_hook();
        install_texture_retry_hooks();
    }
    if (g_install_event_next.load(std::memory_order_relaxed) != install_sequence) {
        request_protection_report();
    }
    return result;
}

 
void pin_module() {
    HMODULE ignored{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&pin_module), &ignored);
}

 
void draw_status_ui(REFImGuiFrameCbData *data) {
    if (data == nullptr) {
        return;
    }
    const auto loading = g_loading_gate.snapshot();
    const auto retries = g_texture_retry.stats();
    const auto gameplay_retries = g_texture_retry.gameplay_stats();
    const dstorage_guard::GuardStatusSnapshot snapshot{
        g_initialize_state.load(std::memory_order_acquire),
        g_install_state.load(std::memory_order_acquire),
        g_resource_install_state.load(std::memory_order_acquire),
        g_resource_hook_mode.load(std::memory_order_acquire),
        g_iat_hook_state.load(std::memory_order_acquire),
        loading.lifecycle_available,
        loading.active,
        g_active_leases.load(std::memory_order_acquire),
        g_deferred_closes.load(std::memory_order_acquire),
        g_resource_recoveries.load(std::memory_order_acquire),
        g_resource_create_hook_status.load(std::memory_order_acquire),
        g_resource_enable_hook_status.load(std::memory_order_acquire),
        g_retry_install_state.load(std::memory_order_acquire),
        retries.queued,
        retries.recovered,
        retries.failed,
        g_retry_create_status.load(std::memory_order_acquire),
        g_retry_enable_status.load(std::memory_order_acquire),
        g_retry_gameplay_enabled.load(std::memory_order_relaxed),
        gameplay_retries.inflight,
        gameplay_retries.gameplay_queued,
        gameplay_retries.backoffs,
        gameplay_retries.events_dropped,
        g_loose_cleanup.caller() != 0 && g_hooks_armed.load(),
        g_loose_cleanup.paired(),
    };
    g_status_ui.draw(data->context, snapshot);
}

void install_status_ui(const REFrameworkPluginInitializeParam *param) {
    if (param == nullptr) {
        return;
    }
    if (param->functions == nullptr || param->functions->on_imgui_draw_ui == nullptr ||
        !g_status_ui.load(static_cast<HMODULE>(param->reframework_module))) {
        g_status_ui_state.store(2, std::memory_order_release);
        return;
    }
    g_status_ui.configure_settings(L"reframework\\data\\DStorageFileLifetimeGuard\\ui.ini");
    const bool registered = param->functions->on_imgui_draw_ui(&draw_status_ui);
    g_status_ui_state.store(registered ? 1U : 3U, std::memory_order_release);
}
