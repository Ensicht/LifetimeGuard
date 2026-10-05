#pragma once
#include "MinHook.h"

#ifdef __cplusplus
extern "C" {
#endif
// Not a general relocator. Unknown prologues are rejected without modification.
MH_STATUS WINAPI MH_CreateHookStackPrologue16(LPVOID target, LPVOID detour, LPVOID *original);
#ifdef __cplusplus
}
#endif
