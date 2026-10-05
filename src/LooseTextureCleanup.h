 
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace dstorage_guard {

 
struct CleanupBlock {
    std::uintptr_t rva;
    const char *hex;
};
inline constexpr CleanupBlock cleanup_blocks[] = {
    {0xCF03328, "4885c07442488d0d5c11a607ff1536da7101488b064885c0741a488b0d2f11a607488b5108807a1900"
                "4889ce7420807e19007438488d0d2d11a6074883c4205e48ff25f9db7101904883c4205ec3"},
    {0xCF03376, "4889ce4c8d421048394220480f43f24c0f43c2498b10807a190074e7ebc2483b462072c24839ce74bd"
                "488b4e28488b01ff5018488b4e28488b01ff5010488d0dbe10a6074889f2e8ce010000eb98"},
    {0xCF03590, "e9bb3e6e12"},
    {0x1F5E7456, "488b421080781900750e4889c6488b008078190074f4eb164889d0488b7008807e19007509483b461"
                 "04889f074ede8a7c07fed4889c7488b48284885c9740e48c7472800000000488b01ff50104889f9e8"
                 "859ea1e04889f04883c4285f5ec3"},
    {0x5178DF, "48890e488d0d8fcb4414488d7c24304889fa4989f0e8379e170b488b3f488b4f28488b7424284839f17"
               "4264885f6740d488b064889f1ff5008488b4f28488977284885c97406488b01ff5010488b4c24284885"
               "c9740f48c744242800000000488b01ff501040b701e96affffff"},
};
inline constexpr std::uintptr_t cleanup_close_return_rva = 0xCF033A9;
inline constexpr std::uint32_t cleanup_game_timestamp = 0x6A7D2E58;
inline constexpr std::uint32_t cleanup_game_size = 0x2079C000;
struct CleanupHostProfile {
    std::uint32_t timestamp, size;
    std::uintptr_t rva;
    const char *hex;
};
inline constexpr CleanupHostProfile cleanup_hosts[] = {
    {0x6A391892, 0x161C000, 0xEACB0,
     "40534883ec20488bd9e8c28effff488b10488b826805000080780800743b488b8b600100004885c9742f4883bb580"
     "1000000742548897c2430488d3de8e71b01488bd7e8f84a6d004885c074074889bb58010000488b7c24304883c420"
     "5bc3"},
    {0x69D60B74, 0x15D1000, 0xB8FF0,
     "40534883ec20488bd9e882b0ffff488b10488b826805000080780800743b488b8b600100004885c9742f4883bb580"
     "1000000742548897c2430488d3d68501b01488bd7e82c6a6d004885c074074889bb58010000488b7c24304883c420"
     "5bc3"},
    {0x6AA2FE9B, 0x162D000, 0xEE650,
     "40534883ec20488bd9e85291ffff488b10488b826805000080780800743b488b8b600100004885c9742f4883bb580"
     "1000000742548897c2430488d3de8821c01488bd7e8e8a96d004885c074074889bb58010000488b7c24304883c420"
     "5bc3"},
    {0x6AA53BAF, 0x162E000, 0xEE650,
     "40534883ec20488bd9e85291ffff488b10488b826805000080780800743b488b8b600100004885c9742f4883bb580"
     "1000000742548897c2430488d3de8921c01488bd7e868b26d004885c074074889bb58010000488b7c24304883c420"
     "5bc3"},
};

inline unsigned cleanup_nibble(char c) {
    return c <= '9' ? c - '0' : c - 'a' + 10;
}
inline bool cleanup_read(const void *p, void *out, std::size_t size) {
    SIZE_T read{};
    return p && ReadProcessMemory(GetCurrentProcess(), p, out, size, &read) && read == size;
}

 
inline std::uint32_t validate_cleanup_image(const void *image) {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    const auto *base = static_cast<const std::uint8_t *>(image);
    if (!cleanup_read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || dos.e_lfanew > 0x10000 ||
        !cleanup_read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return 1;
    }
    if (nt.FileHeader.TimeDateStamp != cleanup_game_timestamp ||
        nt.OptionalHeader.SizeOfImage != cleanup_game_size) {
        return 2;
    }
    for (unsigned index = 0; index < sizeof(cleanup_blocks) / sizeof(cleanup_blocks[0]); ++index) {
        const auto &block = cleanup_blocks[index];
        std::uint8_t bytes[256]{};
        const auto length = std::strlen(block.hex) / 2;
        if (length > sizeof(bytes) || !cleanup_read(base + block.rva, bytes, length)) {
            return 10 + index;
        }
        for (std::size_t i = 0; i < length; ++i) {
            const auto expected =
                (cleanup_nibble(block.hex[i * 2]) << 4U) | cleanup_nibble(block.hex[i * 2 + 1]);
            if (bytes[i] != expected) {
                return 10 + index;
            }
        }
    }
    return 0;
}

inline std::uint32_t validate_cleanup_host(const void *image) {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    const auto *base = static_cast<const std::uint8_t *>(image);
    if (!cleanup_read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || dos.e_lfanew > 0x10000 ||
        !cleanup_read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) {
        return 30;
    }
    for (const auto &profile : cleanup_hosts) {
        if (nt.FileHeader.TimeDateStamp != profile.timestamp ||
            nt.OptionalHeader.SizeOfImage != profile.size) {
            continue;
        }
        std::uint8_t bytes[128]{};
        const auto length = std::strlen(profile.hex) / 2;
        if (!cleanup_read(base + profile.rva, bytes, length)) {
            return 31;
        }
        for (std::size_t i = 0; i < length; ++i) {
            if (bytes[i] != ((cleanup_nibble(profile.hex[i * 2]) << 4U) |
                             cleanup_nibble(profile.hex[i * 2 + 1]))) {
                return 31;
            }
        }
        return 0;
    }
    return 32;
}

inline bool is_loose_texture_path(const wchar_t *path, std::size_t length) noexcept {
    if (!path || length < 6 || length > 32767) {
        return false;
    }
     
    auto end = length;
    while (end && path[end - 1] >= L'0' && path[end - 1] <= L'9') {
        --end;
    }
    return end != length && end >= 5 && path[end - 5] == L'.' &&
           (path[end - 4] == L't' || path[end - 4] == L'T') &&
           (path[end - 3] == L'e' || path[end - 3] == L'E') &&
           (path[end - 2] == L'x' || path[end - 2] == L'X') && path[end - 1] == L'.';
}

class LooseTextureCleanup {
  public:
    void initialize(const void *image, const void *host) noexcept {
        auto failure = validate_cleanup_image(image);
        if (!failure) {
            failure = validate_cleanup_host(host);
        }
        failure_.store(failure, std::memory_order_relaxed);
        if (!failure) {
            caller_.store(reinterpret_cast<std::uintptr_t>(image) + cleanup_close_return_rva,
                          std::memory_order_release);
        } else {
            caller_.store(0, std::memory_order_release);
        }
    }

     
    bool pair(void *wrapper, std::uintptr_t caller) noexcept {
        const auto expected = caller_.load(std::memory_order_acquire);
        if (!expected || caller != expected || !wrapper) {
            return false;
        }
         
         
        const auto *file = *reinterpret_cast<const std::uint8_t *const *>(
            static_cast<const std::uint8_t *>(wrapper) + 0x20);
        if (!file) {
            return false;
        }
        const auto length = *reinterpret_cast<const std::size_t *>(file + 0x20);
        const auto capacity = *reinterpret_cast<const std::size_t *>(file + 0x28);
        if (length > capacity || length > 32767) {
            return false;
        }
        const auto *path = capacity < 8 ? reinterpret_cast<const wchar_t *>(file + 0x10)
                                        : *reinterpret_cast<const wchar_t *const *>(file + 0x10);
        if (!is_loose_texture_path(path, length)) {
            return false;
        }
        using Ref = ULONG (*)(void *);
        const auto table = *reinterpret_cast<void ***>(wrapper);
         
         
        reinterpret_cast<Ref>(table[1])(wrapper);
        paired_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::uintptr_t caller() const noexcept {
        return caller_.load(std::memory_order_acquire);
    }
    std::uint32_t failure() const noexcept {
        return failure_.load(std::memory_order_relaxed);
    }
    std::uint64_t paired() const noexcept {
        return paired_.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<std::uintptr_t> caller_{};
    std::atomic<std::uint32_t> failure_{1};
    std::atomic<std::uint64_t> paired_{};
};
}  
