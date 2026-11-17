#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../src/NativeTaskHookSpec.h"

namespace {

struct ImageView {
    const std::uint8_t* bytes{};
    std::size_t size{};
    const IMAGE_NT_HEADERS64* nt{};
    const IMAGE_SECTION_HEADER* sections{};
};

const std::uint8_t* rva_to_pointer(
    const ImageView& image, std::uint32_t rva, std::size_t size) {
    if (rva < image.nt->OptionalHeader.SizeOfHeaders &&
        static_cast<std::size_t>(rva) + size <= image.size) {
        return image.bytes + rva;
    }
    for (unsigned index = 0;
         index < image.nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = image.sections[index];
        const auto section_size = section.Misc.VirtualSize >
                section.SizeOfRawData
            ? section.Misc.VirtualSize
            : section.SizeOfRawData;
        if (rva < section.VirtualAddress ||
            rva - section.VirtualAddress > section_size ||
            size > section_size - (rva - section.VirtualAddress)) {
            continue;
        }
        const auto file_offset = static_cast<std::size_t>(
            section.PointerToRawData) + (rva - section.VirtualAddress);
        return file_offset <= image.size && size <= image.size - file_offset
            ? image.bytes + file_offset
            : nullptr;
    }
    return nullptr;
}

bool masked_match(
    const std::uint8_t* address, const std::uint8_t* pattern,
    const char* mask, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        if (mask[index] == 'x' && address[index] != pattern[index]) {
            return false;
        }
    }
    return true;
}

std::size_t count_executable_pattern(
    const ImageView& image, const std::uint8_t* pattern,
    const char* mask, std::size_t pattern_size) {
    std::size_t count{};
    for (unsigned section_index = 0;
         section_index < image.nt->FileHeader.NumberOfSections;
         ++section_index) {
        const auto& section = image.sections[section_index];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            section.SizeOfRawData < pattern_size) {
            continue;
        }
        const auto* start = image.bytes + section.PointerToRawData;
        for (std::size_t offset = 0;
             offset + pattern_size <= section.SizeOfRawData; ++offset) {
            count += masked_match(
                start + offset, pattern, mask, pattern_size);
        }
    }
    return count;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::printf("usage: DStorageResourceSignatureVerifier <game-exe>\n");
        return 2;
    }
    const auto file = CreateFileW(
        argv[1], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::printf("FAIL: CreateFile error=%lu\n", GetLastError());
        return 3;
    }
    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0) {
        CloseHandle(file);
        return 4;
    }
    const auto mapping = CreateFileMappingW(
        file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        CloseHandle(file);
        std::printf("FAIL: CreateFileMapping error=%lu\n", GetLastError());
        return 5;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(MapViewOfFile(
        mapping, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(mapping);
    CloseHandle(file);
    if (bytes == nullptr) {
        std::printf("FAIL: MapViewOfFile error=%lu\n", GetLastError());
        return 5;
    }
    const auto size = static_cast<std::size_t>(file_size.QuadPart);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) >
            size) {
        UnmapViewOfFile(bytes);
        return 6;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        bytes + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        UnmapViewOfFile(bytes);
        return 7;
    }
    const ImageView image{
        bytes, size, nt, IMAGE_FIRST_SECTION(nt)};

    constexpr std::uint8_t ready_pattern[] = {
        0x80, 0xB9, 0x20, 0x01, 0x00, 0x00, 0x00, 0x74,
        0x17, 0x48, 0x8B, 0x81, 0x28, 0x01, 0x00, 0x00,
        0x48, 0x85, 0xC0, 0x74, 0x0E, 0x48, 0x39, 0x81,
        0x30, 0x01, 0x00, 0x00, 0x0F, 0x94, 0xC0, 0xC3,
        0x31, 0xC0, 0xC3, 0xB0, 0x01, 0xC3,
    };
    constexpr char ready_mask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    constexpr std::uint8_t finalizer_pattern[] = {
        0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC,
        0x28, 0x48, 0x89, 0xCE, 0x48, 0x8B, 0x05, 0x00,
        0x00, 0x00, 0x00, 0x48, 0x8B, 0x78, 0x18, 0x48,
        0x8B, 0x51, 0x70, 0x48, 0x89, 0xF9,
    };
    constexpr char finalizer_mask[] = "xxxxxxxxxxxxxxx????xxxxxxxxxxx";
    constexpr std::uint8_t worker_pattern[] = {
        0x48, 0x8B, 0x4D, 0x50, 0x48, 0x8B, 0x01, 0x45,
        0x31, 0xED, 0x31, 0xD2, 0xFF, 0x50, 0x38, 0x84,
        0xC0, 0x74, 0x0D, 0x48, 0x8B, 0x45, 0x00, 0x48,
        0x89, 0xE9, 0xFF, 0x50, 0x58, 0x41, 0x89, 0xC5,
        0x48, 0x8B, 0x4D, 0x50, 0x48, 0x85, 0xC9, 0x74,
        0x12, 0x48, 0x8B, 0x01, 0xBA, 0x01, 0x00, 0x00,
        0x00, 0xFF, 0x10, 0x48, 0xC7, 0x45, 0x50, 0x00,
        0x00, 0x00, 0x00, 0x45, 0x84, 0xED, 0x74, 0x06,
        0xC6, 0x45, 0x38, 0x01,
    };
    constexpr char worker_mask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    static_assert(sizeof(ready_pattern) + 1 == sizeof(ready_mask));
    static_assert(sizeof(finalizer_pattern) + 1 == sizeof(finalizer_mask));
    static_assert(sizeof(worker_pattern) + 1 == sizeof(worker_mask));

    // 此离线审查覆盖两份已确认的布局；生产入口另外执行完整结构授权。
    const bool current = nt->OptionalHeader.SizeOfImage == 0x20DDA000;
    const auto ready_rva = current ? 0xA4A65D0U : 0xA4A6810U;
    const auto finalizer_rva = current ? 0xAA395E0U : 0xAA38790U;
    const auto worker_rva = current ? 0xA4A783CU : 0xA4A7A7CU;
    const auto texture_vtable_rva = current ? 0xE07BD40U : 0xE05AB80U;
    const auto texture_ready_rva = current ? 0x12C4B0U : 0x12C3A0U;
    const auto task_table_rva = current ? 0xDFD32B0U : dstorage_guard::task_vtable_rva;
    const auto submit_rva = current ? 0xA6DA0U : dstorage_guard::task_submit_rva;
    const auto* ready = rva_to_pointer(image, ready_rva, sizeof(ready_pattern));
    const auto* finalizer = rva_to_pointer(
        image, finalizer_rva, sizeof(finalizer_pattern));
    const auto* worker = rva_to_pointer(
        image, worker_rva, sizeof(worker_pattern));
    const auto* vtable = rva_to_pointer(image, texture_vtable_rva, 0x60);
    const auto ready_count = count_executable_pattern(
        image, ready_pattern, ready_mask, sizeof(ready_pattern));
    const auto finalizer_count = count_executable_pattern(
        image, finalizer_pattern, finalizer_mask,
        sizeof(finalizer_pattern));
    const auto worker_count = count_executable_pattern(
        image, worker_pattern, worker_mask, sizeof(worker_pattern));
    const auto preferred_ready_ok = ready != nullptr && masked_match(
        ready, ready_pattern, ready_mask, sizeof(ready_pattern));
    const auto preferred_finalizer_ok = finalizer != nullptr && masked_match(
        finalizer, finalizer_pattern, finalizer_mask,
        sizeof(finalizer_pattern));
    const auto preferred_worker_ok = worker != nullptr && masked_match(
        worker, worker_pattern, worker_mask, sizeof(worker_pattern));
    std::uint64_t vtable_finalizer{};
    std::uint64_t vtable_ready{};
    if (vtable != nullptr) {
        std::memcpy(&vtable_ready, vtable + 0x50, sizeof(vtable_ready));
        std::memcpy(&vtable_finalizer, vtable + 0x58, sizeof(vtable_finalizer));
    }
    const auto expected_base = nt->OptionalHeader.ImageBase;
    const auto vtable_ok = vtable_ready == expected_base + texture_ready_rva &&
        vtable_finalizer == expected_base + finalizer_rva;
    const auto* task_vtable = rva_to_pointer(image, task_table_rva, 13 * sizeof(void*));
    const auto* submit = rva_to_pointer(image, submit_rva, sizeof(dstorage_guard::task_submit_signature));
    std::uint64_t task_ready{}, task_submit{};
    if (task_vtable) {
        std::memcpy(&task_ready, task_vtable + 0x38, sizeof(task_ready));
        std::memcpy(&task_submit, task_vtable + 0x40, sizeof(task_submit));
    }
    std::size_t task_tables{};
    std::size_t ready_data_references{}, near_calls_or_jumps{};
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        const auto& section = image.sections[s];
        if (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            const auto* code = bytes + section.PointerToRawData;
            for (std::size_t offset = 0; offset + 5 <= section.SizeOfRawData; ++offset) {
                if (code[offset] != 0xE8 && code[offset] != 0xE9) { continue; }
                std::int32_t relative{};
                std::memcpy(&relative, code + offset + 1, sizeof(relative));
                const auto destination = static_cast<std::int64_t>(section.VirtualAddress) + offset + 5 + relative;
                near_calls_or_jumps += destination == ready_rva;
            }
        }
        if (!(section.Characteristics & IMAGE_SCN_MEM_READ) || (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) { continue; }
        const auto* start = bytes + section.PointerToRawData;
        for (std::size_t offset = 0; offset + sizeof(void*) <= section.SizeOfRawData; offset += sizeof(void*)) {
            std::uint64_t value{};
            std::memcpy(&value, start + offset, sizeof(value));
            ready_data_references += value == expected_base + ready_rva;
        }
        for (std::size_t offset = 0; offset + 13 * sizeof(void*) <= section.SizeOfRawData; offset += sizeof(void*)) {
            std::uint64_t candidate_ready{}, candidate_submit{};
            std::memcpy(&candidate_ready, start + offset + 0x38, sizeof(candidate_ready));
            std::memcpy(&candidate_submit, start + offset + 0x40, sizeof(candidate_submit));
            task_tables += candidate_ready == expected_base + ready_rva &&
                candidate_submit == expected_base + submit_rva;
        }
    }
    const bool task_ok = task_ready == expected_base + ready_rva &&
        task_submit == expected_base + submit_rva && task_tables == 1 &&
        submit && std::memcmp(submit, dstorage_guard::task_submit_signature, sizeof(dstorage_guard::task_submit_signature)) == 0;
    std::printf("task_vtable_unique=%zu task_slot_38_ready=%d task_slot_40_submit=%d worker_virtual_call=%d fallback_static_gate=%s\n",
        task_tables, task_ready == expected_base + ready_rva, task_submit == expected_base + submit_rva,
        preferred_worker_ok, task_ok ? "PASS" : "FAIL");
    std::printf("ready_aligned_data_references=%zu ready_E8_E9_candidates=%zu\n", ready_data_references, near_calls_or_jumps);
    const auto passed = task_ok && ready_data_references == 1 && near_calls_or_jumps == 0 && preferred_ready_ok && preferred_finalizer_ok &&
        preferred_worker_ok && ready_count == 1 &&
        finalizer_count == 1 && worker_count == 1 && vtable_ok;
    std::printf(
        "resource_signature_verifier=%s ready_count=%zu finalizer_count=%zu "
        "worker_count=%zu preferred_ready=%d preferred_finalizer=%d "
        "preferred_worker=%d vtable=%d\n",
        passed ? "PASS" : "FAIL", ready_count, finalizer_count, worker_count,
        preferred_ready_ok, preferred_finalizer_ok, preferred_worker_ok,
        vtable_ok);
    UnmapViewOfFile(bytes);
    return passed ? 0 : 8;
}
