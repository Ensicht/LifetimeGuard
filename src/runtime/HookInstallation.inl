 
template <std::size_t N>
void *find_exact_code(HMODULE module, const std::uint8_t (&pattern)[N], std::uintptr_t rva) {
    char mask[N + 1]{};
    std::memset(mask, 'x', N);
    return find_unique_executable_pattern(module, pattern, mask, N, rva);
}

void *find_task_submit_anchor(HMODULE module) {
    constexpr auto size = sizeof(dstorage_guard::task_submit_signature);
    char mask[size + 1]{};
    std::memset(mask, 'x', size);
    std::memset(mask, '?', 5);
    auto *submit = find_unique_executable_pattern(module, dstorage_guard::task_submit_signature,
                                                  mask, size, dstorage_guard::task_submit_rva);
    int status = 1;
    if (submit != nullptr && is_executable_address(submit)) {
        if (std::memcmp(submit, dstorage_guard::task_submit_signature, size) == 0) {
            status = 0;
        } else if (submit[0] == 0xE9) {
             
             
            std::int32_t relative{};
            std::memcpy(&relative, submit + 1, sizeof(relative));
            const auto origin = reinterpret_cast<std::uintptr_t>(submit);
            const auto target = origin + 5 + static_cast<std::intptr_t>(relative);
            if ((target < origin || target >= origin + size) &&
                is_readable_range(reinterpret_cast<void *>(target), 1) &&
                is_executable_address(reinterpret_cast<void *>(target))) {
                status = 2;
            }
        }
    }
    record_install_event(InstallComponent::resource, InstallStage::validate_task_submit, 0, submit,
                         nullptr, nullptr, status, 0);
    return status == 1 ? nullptr : submit;
}

void **find_task_ready_slot(HMODULE module, void *submit) {
    const auto *nt = get_nt_headers(module);
    if (nt == nullptr || submit == nullptr) {
        return nullptr;
    }
    auto *base = reinterpret_cast<std::uint8_t *>(module);
    const auto image_begin = reinterpret_cast<std::uintptr_t>(base);
    const auto image_end = image_begin + nt->OptionalHeader.SizeOfImage;
    void **found{};
    const auto *sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto &section = sections[index];
        if ((section.Characteristics & IMAGE_SCN_MEM_READ) == 0 ||
            (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
            continue;
        }
        const auto size = static_cast<std::size_t>(section.Misc.VirtualSize);
        if (section.VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
            size > nt->OptionalHeader.SizeOfImage - section.VirtualAddress) {
            return nullptr;
        }
        auto *start = base + section.VirtualAddress;
        for (std::size_t offset = 0; offset < size;) {
            MEMORY_BASIC_INFORMATION memory{};
            auto *cursor = start + offset;
            if (VirtualQuery(cursor, &memory, sizeof(memory)) != sizeof(memory)) {
                return nullptr;
            }
            const auto region_end =
                reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
            const auto section_begin = reinterpret_cast<std::uintptr_t>(start);
            if (region_end <= reinterpret_cast<std::uintptr_t>(cursor)) {
                return nullptr;
            }
            const auto limit =
                region_end - section_begin < size ? region_end - section_begin : size;
            const bool readable = memory.State == MEM_COMMIT && memory.Type == MEM_IMAGE &&
                                  memory.AllocationBase == module &&
                                  (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
            if (readable) {
                for (; offset + 13 * sizeof(void *) <= limit; offset += sizeof(void *)) {
                    auto **methods = reinterpret_cast<void **>(start + offset);
                    if (methods[8] != submit) {
                        continue;
                    }
                    bool valid = true;
                    for (unsigned entry = 0; entry < 13 && valid; ++entry) {
                        if (entry == 7) {
                            continue;
                        }  
                        const auto address = reinterpret_cast<std::uintptr_t>(methods[entry]);
                        valid = address >= image_begin && address < image_end &&
                                is_readable_range(methods[entry], 1) &&
                                is_executable_address(methods[entry]);
                    }
                    if (!valid) {
                        continue;
                    }
                    if (found != nullptr) {
                        return nullptr;
                    }
                    found = methods + 7;
                }
            }
            offset = (limit + sizeof(void *) - 1) / sizeof(void *) * sizeof(void *);
        }
    }
    return found;
}

struct NativePointerHookOperations {
    bool writable(void **slot, std::uint32_t &previous, std::uint32_t &error) {
        DWORD old{};
        const auto ok = VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old) != FALSE;
        error = ok ? 0U : GetLastError();
        previous = old;
        return ok;
    }
    bool restore(void **slot, std::uint32_t previous, std::uint32_t &error) {
        DWORD old{};
        const auto ok = VirtualProtect(slot, sizeof(void *), previous, &old) != FALSE;
        error = ok ? 0U : GetLastError();
        return ok;
    }
    void *exchange(void **slot, void *replacement, void *expected) {
        return InterlockedCompareExchangePointer(slot, replacement, expected);
    }
};

 
bool install_resource_vtable_fallback(HMODULE module, void *ready, void *texture_vtable,
                                      const std::uint8_t *ready_signature, std::size_t ready_size) {
    auto *worker = find_exact_code(module, dstorage_guard::resource_worker_signature,
                                   dstorage_guard::resource_worker_rva);
    record_install_event(InstallComponent::resource, InstallStage::validate_resource_worker, 0,
                         worker, nullptr, nullptr, worker ? 0 : 1, 0);
    if (worker == nullptr) {
        return false;
    }
    auto *submit = find_task_submit_anchor(module);
    if (submit == nullptr) {
        return false;
    }
    auto **slot = find_task_ready_slot(module, submit);
    record_install_event(InstallComponent::resource, InstallStage::locate_task_slot, 0, slot,
                         nullptr, ready, slot ? 0 : 1, 0);
    if (slot == nullptr || reinterpret_cast<std::uintptr_t>(slot) % alignof(void *) != 0) {
        return false;
    }
    const bool ready_unchanged = is_readable_range(ready, ready_size) &&
                                 std::memcmp(ready, ready_signature, ready_size) == 0;
    record_install_event(InstallComponent::resource, InstallStage::validate_target, 0, ready,
                         nullptr, nullptr, ready_unchanged ? 0 : 1, 0);
    if (!ready_unchanged) {
        return false;
    }

     
     
    g_original_resource_task_ready.store(reinterpret_cast<ResourceTaskReadyFn>(ready),
                                         std::memory_order_release);
    g_texture_vtable = texture_vtable;
    NativePointerHookOperations operations;
    auto *detour = reinterpret_cast<void *>(&hook_resource_task_ready);
    const auto result = dstorage_guard::install_pointer_hook(slot, ready, detour, operations);
    record_install_event(InstallComponent::resource, InstallStage::pointer_install, 0, slot, detour,
                         result.observed, static_cast<int>(result.status), result.protect_error);
    if (result.status != dstorage_guard::PointerHookStatus::protect_failed) {
        record_install_event(InstallComponent::resource, InstallStage::pointer_restore, 0, slot,
                             detour, ready, result.protection_restored ? 0 : 1,
                             result.restore_error);
    }
    if (result.status == dstorage_guard::PointerHookStatus::restore_failed ||
        result.rollback_restore_error != 0) {
        record_install_event(InstallComponent::resource, InstallStage::pointer_rollback, 0, slot,
                             detour, ready, result.rolled_back ? 0 : 1,
                             result.rollback_restore_error);
    }
    if (result.status != dstorage_guard::PointerHookStatus::installed ||
        !result.protection_restored) {
        return false;
    }
    g_resource_hook_mode.store(2, std::memory_order_release);
    g_resource_hook_armed.store(true, std::memory_order_release);
    g_resource_install_state.store(1, std::memory_order_release);
    return true;
}

 
bool install_resource_recovery_hook() {
    if (g_resource_install_state.load(std::memory_order_acquire) == 1) {
        return true;
    }
    if (!g_loading_gate.lifecycle_available()) {
        g_resource_install_state.store(7, std::memory_order_release);
        return false;
    }
    std::uint32_t expected{};
    if (!g_resource_install_state.compare_exchange_strong(expected, 2, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
        return expected == 1;
    }

    auto *module = GetModuleHandleW(nullptr);
    constexpr std::uint8_t ready_pattern[] = {
        0x80, 0xB9, 0x20, 0x01, 0x00, 0x00, 0x00, 0x74, 0x17, 0x48, 0x8B, 0x81, 0x28,
        0x01, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x0E, 0x48, 0x39, 0x81, 0x30, 0x01,
        0x00, 0x00, 0x0F, 0x94, 0xC0, 0xC3, 0x31, 0xC0, 0xC3, 0xB0, 0x01, 0xC3,
    };
    constexpr char ready_mask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    constexpr std::uint8_t finalizer_pattern[] = {
        0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xCE, 0x48, 0x8B, 0x05,
        0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x78, 0x18, 0x48, 0x8B, 0x51, 0x70, 0x48, 0x89, 0xF9,
    };
    constexpr char finalizer_mask[] = "xxxxxxxxxxxxxxx????xxxxxxxxxxx";
    static_assert(sizeof(ready_pattern) + 1 == sizeof(ready_mask));
    static_assert(sizeof(finalizer_pattern) + 1 == sizeof(finalizer_mask));

    auto *ready = find_unique_executable_pattern(module, ready_pattern, ready_mask,
                                                 sizeof(ready_pattern), 0xA4A6810);
    auto *finalizer = find_unique_executable_pattern(module, finalizer_pattern, finalizer_mask,
                                                     sizeof(finalizer_pattern), 0xAA38790);
    auto *texture_vtable = find_texture_vtable(module, finalizer, 0xE05AB80);
    record_install_event(InstallComponent::resource, InstallStage::locate_ready, 0, ready, nullptr,
                         nullptr, ready != nullptr ? 0 : 1, 0);
    record_install_event(InstallComponent::resource, InstallStage::locate_finalizer, 0, finalizer,
                         nullptr, nullptr, finalizer != nullptr ? 0 : 1, 0);
    record_install_event(InstallComponent::resource, InstallStage::locate_texture_vtable, 0,
                         texture_vtable, nullptr, nullptr, texture_vtable != nullptr ? 0 : 1, 0);
    if (ready == nullptr || finalizer == nullptr || texture_vtable == nullptr) {
        g_resource_install_state.store(3, std::memory_order_release);
        return false;
    }

    const auto saved_initialize_error = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const auto initialize_result = MH_Initialize();
    const auto initialize_error = GetLastError();
    SetLastError(saved_initialize_error);
    record_install_event(InstallComponent::resource, InstallStage::mh_initialize, 0, nullptr,
                         nullptr, nullptr, static_cast<int>(initialize_result), initialize_error);
    if (initialize_result != MH_OK && initialize_result != MH_ERROR_ALREADY_INITIALIZED) {
        g_resource_install_state.store(4, std::memory_order_release);
        return false;
    }
    void *original{};
    const auto saved_create_error = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const auto create_status =
        MH_CreateHook(ready, reinterpret_cast<void *>(&hook_resource_task_ready), &original);
    const auto create_error = GetLastError();
    SetLastError(saved_create_error);
    record_install_event(InstallComponent::resource, InstallStage::allocator_result, 0, ready,
                         nullptr, original, static_cast<int>(create_status), create_error);
    g_resource_create_hook_status.store(static_cast<int>(create_status), std::memory_order_release);
    record_install_event(InstallComponent::resource, InstallStage::mh_create, 0, ready,
                         reinterpret_cast<void *>(&hook_resource_task_ready), original,
                         static_cast<int>(create_status), create_error);
    if (create_status != MH_OK) {
        if (create_status == MH_ERROR_MEMORY_ALLOC &&
            install_resource_vtable_fallback(module, ready, texture_vtable, ready_pattern,
                                             sizeof(ready_pattern))) {
            return true;
        }
        g_resource_install_state.store(5, std::memory_order_release);
        return false;
    }
    g_original_resource_task_ready.store(reinterpret_cast<ResourceTaskReadyFn>(original),
                                         std::memory_order_release);
    g_texture_vtable = texture_vtable;
    const auto saved_enable_error = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const auto enable_status = MH_EnableHook(ready);
    const auto enable_error = GetLastError();
    SetLastError(saved_enable_error);
    g_resource_enable_hook_status.store(static_cast<int>(enable_status), std::memory_order_release);
    record_install_event(InstallComponent::resource, InstallStage::mh_enable, 0, ready,
                         reinterpret_cast<void *>(&hook_resource_task_ready), original,
                         static_cast<int>(enable_status), enable_error);
    if (enable_status != MH_OK) {
        const auto saved_remove_error = GetLastError();
        SetLastError(ERROR_SUCCESS);
        const auto remove_status = MH_RemoveHook(ready);
        const auto remove_error = GetLastError();
        SetLastError(saved_remove_error);
        record_install_event(InstallComponent::resource, InstallStage::mh_remove_rollback, 0, ready,
                             reinterpret_cast<void *>(&hook_resource_task_ready), original,
                             static_cast<int>(remove_status), remove_error);
        g_original_resource_task_ready.store(nullptr, std::memory_order_release);
        g_texture_vtable = nullptr;
        g_resource_install_state.store(6, std::memory_order_release);
        return false;
    }
    g_resource_hook_mode.store(1, std::memory_order_release);
    g_resource_hook_armed.store(true, std::memory_order_release);
    g_resource_install_state.store(1, std::memory_order_release);
    return true;
}

 
bool install_internal_hooks() {
    if (g_install_state.load(std::memory_order_acquire) == 1) {
        return true;
    }
    auto *module = reinterpret_cast<std::uint8_t *>(GetModuleHandleW(L"dstoragecore.dll"));
    if (module == nullptr) {
        return false;
    }
    std::uint32_t expected{};
    if (!g_install_state.compare_exchange_strong(expected, 2, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
        if (expected != 2) {
            return expected == 1;
        }
        const auto deadline = GetTickCount64() + 1000;
        do {
            const auto state = g_install_state.load(std::memory_order_acquire);
            if (state != 2) {
                return state == 1;
            }
            Sleep(1);
        } while (GetTickCount64() < deadline);
        return false;
    }
    constexpr std::uint8_t close_signature[] = {
        0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x49, 0x20,
    };
    constexpr std::uint8_t enqueue_signature[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x55, 0x48, 0x8B, 0xEC,
    };
    constexpr std::uint8_t ctor_signature[] = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0x28,
        0x02, 0x48, 0x8B, 0xD9, 0x0F, 0x28, 0x4A, 0x10,
    };
    constexpr std::uint8_t complete_signature[] = {
        0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00, 0x45,
        0x33, 0xC9, 0x4C, 0x8B, 0xC1, 0x83, 0x79, 0x70,
    };
    void *targets[] = {
        module + k_public_file_close_rva,
        module + k_public_enqueue_request_rva,
        module + k_request_ctor_rva,
        module + k_request_try_complete_rva,
    };
    const auto pe_valid = pe_matches(reinterpret_cast<HMODULE>(module));
    const bool targets_valid[] = {
        pe_valid && signature_matches(static_cast<std::uint8_t *>(targets[0]), close_signature,
                                      sizeof(close_signature)),
        pe_valid && signature_matches(static_cast<std::uint8_t *>(targets[1]), enqueue_signature,
                                      sizeof(enqueue_signature)),
        pe_valid && signature_matches(static_cast<std::uint8_t *>(targets[2]), ctor_signature,
                                      sizeof(ctor_signature)),
        pe_valid && signature_matches(static_cast<std::uint8_t *>(targets[3]), complete_signature,
                                      sizeof(complete_signature)),
    };
    for (std::size_t index = 0; index < 4; ++index) {
        record_install_event(InstallComponent::lifetime, InstallStage::validate_target,
                             static_cast<std::uint32_t>(index), targets[index], nullptr, nullptr,
                             targets_valid[index] ? 0 : 1, 0);
    }
    if (!pe_valid || !targets_valid[0] || !targets_valid[1] || !targets_valid[2] ||
        !targets_valid[3]) {
        g_install_state.store(3, std::memory_order_release);
        return false;
    }
    const auto saved_initialize_error = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const auto initialize_result = MH_Initialize();
    const auto initialize_error = GetLastError();
    SetLastError(saved_initialize_error);
    record_install_event(InstallComponent::lifetime, InstallStage::mh_initialize, 0, nullptr,
                         nullptr, nullptr, static_cast<int>(initialize_result), initialize_error);
    if (initialize_result != MH_OK && initialize_result != MH_ERROR_ALREADY_INITIALIZED) {
        g_install_state.store(4, std::memory_order_release);
        return false;
    }
    void *originals[4]{};
    void *hooks[] = {
        reinterpret_cast<void *>(&hook_public_file_close),
        reinterpret_cast<void *>(&hook_public_enqueue_request),
        reinterpret_cast<void *>(&hook_request_ctor),
        reinterpret_cast<void *>(&hook_request_try_complete),
    };
    std::size_t created{};
    for (; created < 4; ++created) {
        const auto saved_create_error = GetLastError();
        SetLastError(ERROR_SUCCESS);
        const auto create_status =
            MH_CreateHook(targets[created], hooks[created], &originals[created]);
        const auto create_error = GetLastError();
        SetLastError(saved_create_error);
        g_lifetime_create_hook_status.store(static_cast<int>(create_status),
                                            std::memory_order_release);
        g_lifetime_create_hook_index.store(static_cast<std::uint32_t>(created),
                                           std::memory_order_release);
        record_install_event(InstallComponent::lifetime, InstallStage::mh_create,
                             static_cast<std::uint32_t>(created), targets[created], hooks[created],
                             originals[created], static_cast<int>(create_status), create_error);
        if (create_status != MH_OK) {
            break;
        }
    }
    if (created != 4) {
        while (created > 0) {
            --created;
            const auto saved_remove_error = GetLastError();
            SetLastError(ERROR_SUCCESS);
            const auto remove_status = MH_RemoveHook(targets[created]);
            const auto remove_error = GetLastError();
            SetLastError(saved_remove_error);
            record_install_event(InstallComponent::lifetime, InstallStage::mh_remove_rollback,
                                 static_cast<std::uint32_t>(created), targets[created],
                                 hooks[created], originals[created],
                                 static_cast<int>(remove_status), remove_error);
        }
        g_install_state.store(5, std::memory_order_release);
        return false;
    }
    g_original_file_close.store(reinterpret_cast<PublicFileCloseFn>(originals[0]),
                                std::memory_order_release);
    g_original_enqueue.store(reinterpret_cast<PublicEnqueueRequestFn>(originals[1]),
                             std::memory_order_release);
    g_original_request_ctor.store(reinterpret_cast<RequestCtorFn>(originals[2]),
                                  std::memory_order_release);
    g_original_try_complete.store(reinterpret_cast<RequestTryCompleteFn>(originals[3]),
                                  std::memory_order_release);
    bool queued{};
    for (created = 0; created < 4; ++created) {
        const auto saved_queue_error = GetLastError();
        SetLastError(ERROR_SUCCESS);
        const auto queue_status = MH_QueueEnableHook(targets[created]);
        const auto queue_error = GetLastError();
        SetLastError(saved_queue_error);
        g_lifetime_queue_hook_status.store(static_cast<int>(queue_status),
                                           std::memory_order_release);
        record_install_event(InstallComponent::lifetime, InstallStage::mh_queue_enable,
                             static_cast<std::uint32_t>(created), targets[created], hooks[created],
                             originals[created], static_cast<int>(queue_status), queue_error);
        if (queue_status != MH_OK) {
            break;
        }
    }
    auto apply_status = static_cast<MH_STATUS>(k_mh_status_not_attempted);
    if (created == 4) {
        const auto saved_apply_error = GetLastError();
        SetLastError(ERROR_SUCCESS);
        apply_status = MH_ApplyQueued();
        const auto apply_error = GetLastError();
        SetLastError(saved_apply_error);
        g_lifetime_apply_hook_status.store(static_cast<int>(apply_status),
                                           std::memory_order_release);
        record_install_event(InstallComponent::lifetime, InstallStage::mh_apply_queued, 0, nullptr,
                             nullptr, nullptr, static_cast<int>(apply_status), apply_error);
    }
    queued = created == 4 && apply_status == MH_OK;
    if (!queued) {
        g_hooks_armed.store(false, std::memory_order_release);
        for (std::size_t index = 0; index < 4; ++index) {
            const auto saved_disable_error = GetLastError();
            SetLastError(ERROR_SUCCESS);
            const auto disable_status = MH_DisableHook(targets[index]);
            const auto disable_error = GetLastError();
            SetLastError(saved_disable_error);
            record_install_event(InstallComponent::lifetime, InstallStage::mh_disable_rollback,
                                 static_cast<std::uint32_t>(index), targets[index], hooks[index],
                                 originals[index], static_cast<int>(disable_status), disable_error);

            const auto saved_remove_error = GetLastError();
            SetLastError(ERROR_SUCCESS);
            const auto remove_status = MH_RemoveHook(targets[index]);
            const auto remove_error = GetLastError();
            SetLastError(saved_remove_error);
            record_install_event(InstallComponent::lifetime, InstallStage::mh_remove_rollback,
                                 static_cast<std::uint32_t>(index), targets[index], hooks[index],
                                 originals[index], static_cast<int>(remove_status), remove_error);
        }
        g_install_state.store(6, std::memory_order_release);
        return false;
    }
    g_loose_cleanup.initialize(GetModuleHandleW(nullptr), g_loose_cleanup_host.load());
    g_hooks_armed.store(true, std::memory_order_release);
    g_install_state.store(1, std::memory_order_release);
    return true;
}

#include "../NativeTextureRetryAdapter.inl"
