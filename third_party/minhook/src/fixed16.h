/* Optional LifetimeGuard fallback; shares hook.c's lock, freeze and IP mapping.
 * Only the validated push/push/sub prologue below can use an arbitrary address.
 * All ordinary MinHook entries continue using the original rel32 path. */
#if !defined(_M_X64) && !defined(__x86_64__)
#error Fixed16 requires x64
#endif

static const UINT8 kFixed16Prologue[16] = {
    0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,
    0x56,0x57,0x55,0x53,0x48,0x83,0xEC,0x48
};
static const UINT8 kFixed16OldIPs[11] = {0,2,4,6,8,9,10,11,12,16,16};
static const UINT8 kFixed16NewIPs[11] = {0,2,4,6,8,9,10,11,12,16,26};
static const UINT8 kFixed16Next[3] = {0x48,0x8B,0x05};

static BOOL Fixed16Readable(LPVOID address, SIZE_T length)
{
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return FALSE;
    return (ULONG_PTR)address >= (ULONG_PTR)info.BaseAddress &&
        length <= info.RegionSize &&
        (ULONG_PTR)address - (ULONG_PTR)info.BaseAddress <= info.RegionSize - length;
}

static VOID Fixed16Jump(LPBYTE destination, LPVOID target)
{
    const UINT8 jump[6] = {0xFF,0x25,0,0,0,0};
    memcpy(destination, jump, sizeof(jump));
    memcpy(destination + 6, &target, sizeof(target));
}

static VOID FreeFixed16(LPVOID buffer)
{
    RtlDeleteFunctionTable((PRUNTIME_FUNCTION)((LPBYTE)buffer + 64));
    VirtualFree(buffer, 0, MEM_RELEASE);
}

static MH_STATUS CreateFixed16(LPVOID target, LPVOID detour, LPVOID *original)
{
    LPBYTE buffer;
    PHOOK_ENTRY hook;
    PRUNTIME_FUNCTION function;
    DWORD previous;
    // FF25 would be mistaken for a tail epilogue by RtlVirtualUnwind. Use a
    // register jump instead. The required next instruction overwrites RAX
    // without reading it; no live incoming register or flags are changed.
    const UINT8 unwind[24] = {
        1,16,9,0, 16,0x82, 12,0x30, 11,0x50, 10,0x70, 9,0x60,
        8,0xC0, 6,0xD0, 4,0xE0, 2,0xF0, 0,0
    };
    if (!IsExecutableAddress(target) || !IsExecutableAddress(detour) ||
        !Fixed16Readable(target, 23))
        return MH_ERROR_NOT_EXECUTABLE;
    if (memcmp(target, kFixed16Prologue, sizeof(kFixed16Prologue)) != 0 ||
        memcmp((LPBYTE)target + 16, kFixed16Next, sizeof(kFixed16Next)) != 0)
        return MH_ERROR_UNSUPPORTED_FUNCTION;
    buffer = (LPBYTE)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (buffer == NULL)
        return MH_ERROR_MEMORY_ALLOC;
    memcpy(buffer, kFixed16Prologue, sizeof(kFixed16Prologue));
    buffer[16] = 0x48;
    buffer[17] = 0xB8;
    {
        LPVOID continuation = (LPBYTE)target + 16;
        memcpy(buffer + 18, &continuation, sizeof(continuation));
    }
    buffer[26] = 0xFF;
    buffer[27] = 0xE0;
    function = (PRUNTIME_FUNCTION)(buffer + 64);
    function->BeginAddress = 0;
    function->EndAddress = 28;
    function->UnwindData = 80;
    memcpy(buffer + 80, unwind, sizeof(unwind));
    if (!VirtualProtect(buffer, 4096, PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), buffer, 28))
    {
        VirtualFree(buffer, 0, MEM_RELEASE);
        return MH_ERROR_MEMORY_PROTECT;
    }
    if (!RtlAddFunctionTable(function, 1, (DWORD64)buffer))
    {
        VirtualFree(buffer, 0, MEM_RELEASE);
        return MH_ERROR_MEMORY_ALLOC;
    }
    hook = AddHookEntry();
    if (hook == NULL)
    {
        FreeFixed16(buffer);
        return MH_ERROR_MEMORY_ALLOC;
    }
    memset(hook, 0, sizeof(*hook));
    hook->fixed16 = TRUE;
    hook->pTarget = target;
    hook->pDetour = detour;
    hook->pTrampoline = buffer;
    memcpy(hook->backup, kFixed16Prologue, sizeof(kFixed16Prologue));
    hook->nIP = ARRAYSIZE(kFixed16OldIPs);
    memcpy(hook->oldIPs, kFixed16OldIPs, sizeof(kFixed16OldIPs));
    memcpy(hook->newIPs, kFixed16NewIPs, sizeof(kFixed16NewIPs));
    if (original != NULL)
        *original = buffer;
    return MH_OK;
}

static MH_STATUS EnableFixed16(PHOOK_ENTRY hook, BOOL enable)
{
    UINT8 patch[16];
    DWORD previous, ignored;
    memset(patch, 0x90, sizeof(patch));
    Fixed16Jump(patch, hook->pDetour);
    // Recheck after thread freeze; do not overwrite another hook installed
    // between Create and Enable, or remove a foreign replacement on Disable.
    if (!Fixed16Readable(hook->pTarget, 23))
        return MH_ERROR_MEMORY_PROTECT;
    if (memcmp(hook->pTarget, enable ? hook->backup : patch, sizeof(patch)) != 0 ||
        memcmp((LPBYTE)hook->pTarget + 16, kFixed16Next, sizeof(kFixed16Next)) != 0)
        return MH_ERROR_UNSUPPORTED_FUNCTION;
    if (!VirtualProtect(hook->pTarget, sizeof(patch), PAGE_EXECUTE_READWRITE, &previous))
        return MH_ERROR_MEMORY_PROTECT;
    memcpy(hook->pTarget, enable ? patch : hook->backup, sizeof(patch));
    VirtualProtect(hook->pTarget, sizeof(patch), previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), hook->pTarget, sizeof(patch));
    hook->isEnabled = enable;
    hook->queueEnable = enable;
    return MH_OK;
}
