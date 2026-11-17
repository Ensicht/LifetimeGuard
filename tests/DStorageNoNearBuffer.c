// Fault-injection object used ONLY in the separately named offline DLL.
// It simulates an unavailable near window without reserving gigabytes.
#define AllocateBuffer UnusedRealAllocateBuffer
#include "../third_party/minhook/src/buffer.c"
#undef AllocateBuffer
LPVOID AllocateBuffer(LPVOID origin)
{
    (void)origin;
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}
