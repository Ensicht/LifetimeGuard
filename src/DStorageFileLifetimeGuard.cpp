// LifetimeGuard: stable implementation assembled as one translation unit.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <MinHook.h>
#include <MinHookFixed16.h>
#include <MinHookAllocationDiagnostics.h>
#include <reframework/API.h>

#include "LoadingGatePolicy.h"
#include "ResourceRecoveryPolicy.h"
#include "NativeTaskHookSpec.h"
#include "NativeTextureProfile.h"
#include "PointerHookTransaction.h"
#include "GuardStatusUi.h"
#include "DetachedTextureRetry.h"
#include "TextureRetryNativeTargets.h"
#include "GuardFailurePolicy.h"
#include "LooseTextureCleanup.h"

#ifdef LG_BONE_SYSTEM_COMPANION
#include "BoneSystem/Bridge.h"
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>

// The WMP facade shares the core, with its own diagnostics directory and public exports.
#ifdef LG_WMP_PROVIDER
#define LG_CORE_EXPORT
#define LG_REPORT_DIR L"reframework\\data\\WMPLifetimeGuard"
#define reframework_plugin_initialize core_initialize
#define reframework_plugin_required_version core_required_version
#define DllMain core_dll_main
#else
#define LG_CORE_EXPORT __declspec(dllexport)
#define LG_REPORT_DIR L"reframework\\data\\DStorageFileLifetimeGuard"
#endif

namespace {
#include "runtime/State.inl"
#include "runtime/Lifecycle.inl"
#include "runtime/FileLifetime.inl"
#include "runtime/NativeValidation.inl"
#include "runtime/ResourceRecovery.inl"
#include "runtime/HookInstallation.inl"
#include "runtime/Diagnostics.inl"
#include "runtime/ReportWorker.inl"
#include "runtime/Startup.inl"
} // namespace

#include "Exports.inl"

#undef LG_CORE_EXPORT
#undef LG_REPORT_DIR
#ifdef LG_WMP_PROVIDER
#undef reframework_plugin_initialize
#undef reframework_plugin_required_version
#undef DllMain
#endif

#if defined(LG_BONE_SYSTEM_COMPANION) && !defined(LG_WMP_PROVIDER)
extern "C" __declspec(dllexport) int lifetimeguard_bonesystem_start(lua_State *state) {
    lifetimeguard::bonesystem::start(
        state, "reframework/autorun/LifetimeGuard/BoneSystem/Bootstrap.lua", false);
    return 0;
}
#endif
