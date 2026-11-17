#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>

namespace {

using EnableRecoveryFn = bool (*)();
using InitializeFn = bool (*)(const void*);

} // namespace

int main() {
    const auto guard = LoadLibraryW(L"DStorageFileLifetimeGuard.dll");
    if (guard == nullptr) {
        std::printf("FAIL: LoadLibrary error=%lu\n", GetLastError());
        return 2;
    }
    const auto enable = reinterpret_cast<EnableRecoveryFn>(
        GetProcAddress(
            guard,
            "dstorage_file_lifetime_guard_enable_resource_recovery"));
    const auto initialize = reinterpret_cast<InitializeFn>(
        GetProcAddress(guard, "reframework_plugin_initialize"));
    if (enable == nullptr || initialize == nullptr) {
        std::printf("FAIL: missing guard export\n");
        return 3;
    }
    initialize(nullptr);
    const auto installed = enable();
    std::printf(
        "resource_recovery_unknown_host=%s installed=%d expected=0\n",
        !installed ? "PASS" : "FAIL", installed);
    Sleep(250);
    return installed ? 4 : 0;
}
