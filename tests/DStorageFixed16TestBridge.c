#include <windows.h>
#include <stdint.h>

static int fault;
static LONG allocations;
void fixed16_test_fault(int value) { fault = value; }
LONG fixed16_test_allocations(void) { return allocations; }
static LPVOID test_alloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect)
{
    LPVOID result;
    if (fault == 1) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    result = VirtualAlloc(address, size, type, protect);
    if (result != NULL) ++allocations;
    return result;
}
static BOOL test_free(LPVOID address, SIZE_T size, DWORD type)
{
    BOOL result = VirtualFree(address, size, type);
    if (result) --allocations;
    return result;
}
static BOOL test_protect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old)
{
    if (fault == 2 || fault == 4) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return VirtualProtect(address, size, protect, old);
}
static BOOLEAN test_add(PRUNTIME_FUNCTION function, DWORD count, DWORD64 base)
{
    if (fault == 3) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    return RtlAddFunctionTable(function, count, base);
}
#define VirtualAlloc test_alloc
#define VirtualFree test_free
#define VirtualProtect test_protect
#define RtlAddFunctionTable test_add
#define MINHOOK_ENABLE_FIXED16_FALLBACK
#include "../third_party/minhook/src/hook.c"

uintptr_t fixed16_test_ip(void *target, uintptr_t ip, int enabling)
{
    UINT pos = FindHookEntry(target);
    if (pos == INVALID_HOOK_POS) return 0;
    return enabling ? FindNewIP(&g_hooks.pItems[pos], ip) : FindOldIP(&g_hooks.pItems[pos], ip);
}
