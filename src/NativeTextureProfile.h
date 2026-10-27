// 原生纹理入口证据：只在启动时核对代码、虚表和引用归属，不按版本号放行。
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace dstorage_guard {
struct NativeTextureProfile {
    std::uintptr_t release, queue, destroy, ready, vtable, parse, finalize, manager;
    std::uintptr_t clear_task[2], work_dec[2], read_inc[2], release_ref;
    std::uintptr_t terminal_calls[4];
    std::uintptr_t destructor_vtable;
};

inline constexpr NativeTextureProfile native_texture_profiles[]{
    {0x24E070,
     0xB0CA3B0,
     0x12C250,
     0x12C3A0,
     0xE05AB80,
     0xAA377E0,
     0xAA38790,
     0x147C4330,
     {0xA4A7AAF, 0xA4A7341},
     {0xA4A7ACE, 0xA4A736A},
     {0xB0CA40A, 0xB0CA5C5},
     0x24E09D,
     {0xA4A7ADF, 0xA4A737B, 0xA4A71C0, 0xA4A79C6},
     0},
    {0x24DFF0,
     0xB0CB2A0,
     0x12C360,
     0x12C4B0,
     0xE07BD40,
     0xAA38630,
     0xAA395E0,
     0x147E5330,
     {0xA4A786F, 0xA4A7101},
     {0xA4A788E, 0xA4A712A},
     {0xB0CB2FA, 0xB0CB4B5},
     0x24E01D,
     {0xA4A789F, 0xA4A713B, 0xA4A6F80, 0xA4A7786},
     0x12C399},
};

inline bool texture_profile_read(const void *image, std::uintptr_t rva, void *out, std::size_t size,
                                 std::size_t image_size) {
    if (rva > image_size || size > image_size - rva) {
        return false;
    }
    SIZE_T read{};
    return ReadProcessMemory(GetCurrentProcess(), static_cast<const std::uint8_t *>(image) + rva,
                             out, size, &read) &&
           read == size;
}

template <std::size_t N>
inline bool texture_profile_code(const void *image, std::uintptr_t rva,
                                 const std::uint8_t (&expected)[N], std::size_t image_size) {
    std::uint8_t actual[N]{};
    return texture_profile_read(image, rva, actual, N, image_size) &&
           std::memcmp(actual, expected, N) == 0;
}

inline bool validate_native_texture_profile(const void *image, std::size_t image_size,
                                            const NativeTextureProfile &p) {
    constexpr std::uint8_t release[]{0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                     0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x48};
    constexpr std::uint8_t queue[]{0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83,
                                   0xEC, 0x30, 0x48, 0x89, 0xD7, 0x48, 0x89, 0xCE};
    constexpr std::uint8_t ready[]{0x8A, 0x41, 0x38, 0xC3};
    constexpr std::uint8_t destroy[]{0x56, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x89, 0xD7,
                                     0x48, 0x89, 0xCE, 0xE8, 0x20, 0,    0,    0};
    constexpr std::uint8_t clear_a[]{0x48, 0xC7, 0x45, 0x50, 0, 0, 0, 0};
    constexpr std::uint8_t clear_b[]{0x49, 0xC7, 0x44, 0x24, 0x50, 0, 0, 0, 0};
    constexpr std::uint8_t work_dec[]{0xF0, 0xFF, 0x8E, 0xC0, 0x0E, 0, 0};
    constexpr std::uint8_t read_inc[]{0xF0, 0xFF, 0x86, 0xAC, 0x0E, 0, 0};
    constexpr std::uint8_t release_ref[]{0xF0, 0xFF, 0x4A, 0x28};
    if (!texture_profile_code(image, p.release, release, image_size) ||
        !texture_profile_code(image, p.queue, queue, image_size) ||
        !texture_profile_code(image, p.ready, ready, image_size) ||
        !texture_profile_code(image, p.destroy, destroy, image_size) ||
        !texture_profile_code(image, p.clear_task[0], clear_a, image_size) ||
        !texture_profile_code(image, p.clear_task[1], clear_b, image_size) ||
        !texture_profile_code(image, p.release_ref, release_ref, image_size)) {
        return false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (!texture_profile_code(image, p.work_dec[i], work_dec, image_size) ||
            !texture_profile_code(image, p.read_inc[i], read_inc, image_size)) {
            return false;
        }
    }
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    for (auto rva : p.terminal_calls) {
        std::uint8_t call[5]{};
        if (!texture_profile_read(image, rva, call, sizeof(call), image_size) || call[0] != 0xE8) {
            return false;
        }
        std::int32_t displacement{};
        std::memcpy(&displacement, call + 1, sizeof(displacement));
        if (rva + 5 + static_cast<std::intptr_t>(displacement) != p.release) {
            return false;
        }
        // 四处 Release 必须仍使用同一 ResourceManager，而不是仅有相似调用指令。
        std::uint8_t manager[7]{};
        if (!texture_profile_read(image, rva - 10, manager, sizeof(manager), image_size) ||
            std::memcmp(manager, "\x48\x8b\x0d", 3) != 0) {
            return false;
        }
        std::memcpy(&displacement, manager + 3, sizeof(displacement));
        if (rva - 3 + static_cast<std::intptr_t>(displacement) != p.manager) {
            return false;
        }
    }
    std::uintptr_t methods[12]{}, manager{};
    if (!texture_profile_read(image, p.vtable, methods, sizeof(methods), image_size) ||
        methods[6] != base + p.destroy || methods[9] != base + p.parse ||
        methods[10] != base + p.ready || methods[11] != base + p.finalize ||
        !texture_profile_read(image, p.manager, &manager, sizeof(manager), image_size)) {
        return false;
    }
    if (p.destructor_vtable) {
        std::uint8_t binding[10]{};
        std::int32_t displacement{};
        if (!texture_profile_read(image, p.destructor_vtable, binding, sizeof(binding),
                                  image_size) ||
            std::memcmp(binding, "\x48\x8d\x05", 3) != 0 ||
            std::memcmp(binding + 7, "\x48\x89\x01", 3) != 0) {
            return false;
        }
        std::memcpy(&displacement, binding + 3, sizeof(displacement));
        if (p.destructor_vtable + 7 + static_cast<std::intptr_t>(displacement) != p.vtable) {
            return false;
        }
    }
    return true;
}

inline const NativeTextureProfile *find_native_texture_profile(const void *image) {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!texture_profile_read(image, 0, &dos, sizeof(dos), 0x10000) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x10000 ||
        !texture_profile_read(image, dos.e_lfanew, &nt, sizeof(nt), 0x20000) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return nullptr;
    }
    const NativeTextureProfile *found{};
    for (const auto &profile : native_texture_profiles) {
        if (validate_native_texture_profile(image, nt.OptionalHeader.SizeOfImage, profile)) {
            if (found) {
                return nullptr;
            }
            found = &profile;
        }
    }
    return found;
}
} // namespace dstorage_guard
