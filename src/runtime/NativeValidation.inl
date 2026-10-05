 
bool pe_matches(HMODULE module) {
    if (module == nullptr) {
        return false;
    }
    const auto *base = reinterpret_cast<const std::uint8_t *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE &&
           nt->FileHeader.TimeDateStamp == k_dstoragecore_timestamp &&
           nt->OptionalHeader.SizeOfImage == k_dstoragecore_image_size;
}

bool signature_matches(const std::uint8_t *address, const std::uint8_t *expected,
                       std::size_t size) {
    return std::memcmp(address, expected, size) == 0;
}

bool masked_signature_matches(const std::uint8_t *address, const std::uint8_t *expected,
                              const char *mask, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        if (mask[index] == 'x' && address[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

const IMAGE_NT_HEADERS64 *get_nt_headers(HMODULE module) {
    if (module == nullptr) {
        return nullptr;
    }
    const auto *base = reinterpret_cast<const std::uint8_t *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

bool is_readable_range(const void *address, std::size_t size) {
    if (address == nullptr || size == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto region_end =
        reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return start <= region_end && size <= region_end - start;
}

bool is_executable_address(const void *address) {
    MEMORY_BASIC_INFORMATION memory{};
    if (address == nullptr || VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT) {
        return false;
    }
    const auto protection = memory.Protect & 0xFFU;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

std::uint64_t decode_redirect(const void *target, const std::uint8_t *code, std::size_t size) {
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    if (target == nullptr || code == nullptr || size == 0) {
        return 0;
    }
    if (size >= 5 && code[0] == 0xE9) {
        std::int32_t displacement{};
        std::memcpy(&displacement, code + 1, sizeof(displacement));
        return static_cast<std::uint64_t>(address + 5 + displacement);
    }
    if (size >= 2 && code[0] == 0xEB) {
        const auto displacement = static_cast<std::int8_t>(code[1]);
        return static_cast<std::uint64_t>(address + 2 + displacement);
    }
    if (size >= 6 && code[0] == 0xFF && code[1] == 0x25) {
        std::int32_t displacement{};
        std::memcpy(&displacement, code + 2, sizeof(displacement));
        const auto slot = reinterpret_cast<const void *>(address + 6 + displacement);
        if (is_readable_range(slot, sizeof(void *))) {
            void *destination{};
            std::memcpy(&destination, slot, sizeof(destination));
            return reinterpret_cast<std::uint64_t>(destination);
        }
    }
    if (size >= 12 && code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) {
        std::uint64_t destination{};
        std::memcpy(&destination, code + 2, sizeof(destination));
        return destination;
    }
    if (size >= 13 && code[0] == 0x49 && code[1] == 0xBB && code[10] == 0x41 && code[11] == 0xFF &&
        code[12] == 0xE3) {
        std::uint64_t destination{};
        std::memcpy(&destination, code + 2, sizeof(destination));
        return destination;
    }
    return 0;
}

void record_install_event(InstallComponent component, InstallStage stage, std::uint32_t index,
                          void *target, void *detour, void *original, int status,
                          std::uint32_t win32_error) {
    const auto sequence = g_install_event_next.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence > k_install_event_capacity) {
        return;
    }
    auto &event = g_install_events[sequence - 1];
    event.commit.store((sequence << 1U) | 1U, std::memory_order_release);
    event.qpc = query_qpc();
    event.target = reinterpret_cast<std::uint64_t>(target);
    event.detour = reinterpret_cast<std::uint64_t>(detour);
    event.original = reinterpret_cast<std::uint64_t>(original);
    event.thread_id = GetCurrentThreadId();
    event.component = static_cast<std::uint32_t>(component);
    event.stage = static_cast<std::uint32_t>(stage);
    event.index = index;
    event.status = status;
    event.win32_error = win32_error;
    event.module_base = 0;
    event.redirect = 0;
    event.protection = 0;
    event.code_size = 0;
    if (stage == InstallStage::allocator_result) {
        MinHookGetAllocationDiagnostics(&event.allocation);
    }

    MEMORY_BASIC_INFORMATION memory{};
    if (target != nullptr && VirtualQuery(target, &memory, sizeof(memory)) == sizeof(memory)) {
        event.module_base = reinterpret_cast<std::uint64_t>(memory.AllocationBase);
        event.protection = memory.Protect;
    }
    if (is_readable_range(target, k_install_code_bytes)) {
        std::memcpy(event.code, target, k_install_code_bytes);
        event.code_size = static_cast<std::uint32_t>(k_install_code_bytes);
        event.redirect = decode_redirect(target, event.code, k_install_code_bytes);
    }

    std::atomic_thread_fence(std::memory_order_release);
    event.commit.store(sequence << 1U, std::memory_order_release);
    if (g_report_event != nullptr) {
        SetEvent(g_report_event);
    }
}

std::uint8_t *find_unique_executable_pattern(HMODULE module, const std::uint8_t *pattern,
                                             const char *mask, std::size_t pattern_size,
                                             std::uintptr_t preferred_rva) {
    auto *base = reinterpret_cast<std::uint8_t *>(module);
    const auto *nt = get_nt_headers(module);
    if (base == nullptr || nt == nullptr) {
        return nullptr;
    }
    if (preferred_rva != 0 && preferred_rva + pattern_size <= nt->OptionalHeader.SizeOfImage) {
        auto *preferred = base + preferred_rva;
        if (masked_signature_matches(preferred, pattern, mask, pattern_size)) {
            return preferred;
        }
    }

    auto *sections = IMAGE_FIRST_SECTION(nt);
    std::uint8_t *found{};
    for (unsigned section_index = 0; section_index < nt->FileHeader.NumberOfSections;
         ++section_index) {
        const auto &section = sections[section_index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        const auto section_size = static_cast<std::size_t>(
            section.Misc.VirtualSize != 0 ? section.Misc.VirtualSize : section.SizeOfRawData);
        if (section_size < pattern_size) {
            continue;
        }
        auto *start = base + section.VirtualAddress;
        for (std::size_t offset = 0; offset + pattern_size <= section_size; ++offset) {
            auto *candidate = start + offset;
            if (!masked_signature_matches(candidate, pattern, mask, pattern_size)) {
                continue;
            }
            if (found != nullptr) {
                return nullptr;
            }
            found = candidate;
        }
    }
    return found;
}

bool validate_texture_vtable(void *vtable, void *texture_finalizer) {
    if (!is_readable_range(vtable, 0x68)) {
        return false;
    }
    auto **methods = static_cast<void **>(vtable);
    auto *ready_method = static_cast<const std::uint8_t *>(methods[10]);
    return methods[11] == texture_finalizer && is_executable_address(methods[9]) &&
           is_executable_address(methods[11]) && is_readable_range(ready_method, 4) &&
           ready_method[0] == 0x8A && ready_method[1] == 0x41 && ready_method[2] == 0x38 &&
           ready_method[3] == 0xC3;
}

void *find_texture_vtable(HMODULE module, void *texture_finalizer, std::uintptr_t preferred_rva) {
    if (texture_finalizer == nullptr) {
        return nullptr;
    }
    auto *base = reinterpret_cast<std::uint8_t *>(module);
    const auto *nt = get_nt_headers(module);
    if (base == nullptr || nt == nullptr) {
        return nullptr;
    }
    if (preferred_rva != 0 && preferred_rva + 0x68 <= nt->OptionalHeader.SizeOfImage) {
        auto *preferred = base + preferred_rva;
        if (validate_texture_vtable(preferred, texture_finalizer)) {
            return preferred;
        }
    }

    auto *sections = IMAGE_FIRST_SECTION(nt);
    void *found{};
    for (unsigned section_index = 0; section_index < nt->FileHeader.NumberOfSections;
         ++section_index) {
        const auto &section = sections[section_index];
        if ((section.Characteristics & IMAGE_SCN_MEM_READ) == 0 ||
            (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
            continue;
        }
        const auto section_size = static_cast<std::size_t>(
            section.Misc.VirtualSize != 0 ? section.Misc.VirtualSize : section.SizeOfRawData);
        if (section_size < 0x68) {
            continue;
        }
        auto *start = base + section.VirtualAddress;
        for (std::size_t offset = 0; offset + 0x68 <= section_size; offset += alignof(void *)) {
            auto *candidate = start + offset;
            if (*reinterpret_cast<void **>(candidate + 0x58) != texture_finalizer) {
                continue;
            }
            if (!validate_texture_vtable(candidate, texture_finalizer)) {
                continue;
            }
            if (found != nullptr) {
                return nullptr;
            }
            found = candidate;
        }
    }
    return found;
}
