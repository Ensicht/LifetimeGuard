 
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
#include "PointerHookTransaction.h"
#include "GuardStatusUi.h"
#include "DetachedTextureRetry.h"
#include "TextureRetryNativeTargets.h"
#include "GuardFailurePolicy.h"
#include "LooseTextureCleanup.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>

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
}  

#include "Exports.inl"
