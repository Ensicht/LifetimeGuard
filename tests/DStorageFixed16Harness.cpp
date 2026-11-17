#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHookFixed16.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

extern "C" void fixed16_test_fault(int);
extern "C" LONG fixed16_test_allocations();
extern "C" uintptr_t fixed16_test_ip(void*, uintptr_t, int);
namespace {
using Fn = std::uint64_t (*)(std::uint64_t, std::uint64_t);
Fn original;
std::atomic<std::uint64_t> hits{}, bad{};
std::atomic<bool> running{true};
void check(bool okay, const char* text) {
    if (!okay) { std::fprintf(stderr, "FAIL %s error=%lu\n", text, GetLastError()); std::exit(1); }
}
__declspec(noinline) std::uint64_t detour(std::uint64_t a, std::uint64_t b) {
    hits.fetch_add(1, std::memory_order_relaxed);
    return original(a, b) + 1;
}
constexpr std::uint8_t code[]{
    0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x55,0x53,0x48,0x83,0xEC,0x48,
    0x48,0x8B,0x05,0,0,0,0,
    0x48,0x8D,0x04,0x11, 0x48,0x83,0xC4,0x48,0x5B,0x5D,0x5F,0x5E,
    0x41,0x5C,0x41,0x5D,0x41,0x5E,0x41,0x5F,0xC3
};
void write(void* address, const void* data, std::size_t bytes) {
    DWORD old{}, ignored{};
    check(VirtualProtect(address, bytes, PAGE_EXECUTE_READWRITE, &old), "fixture writable");
    std::memcpy(address, data, bytes);
    VirtualProtect(address, bytes, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, bytes);
}
void unwind(void* trampoline) {
    constexpr unsigned offsets[]{0,2,4,6,8,9,10,11,12,16,26};
    for (unsigned index = 0; index < 11; ++index) {
        std::array<DWORD64, 128> stack{};
        auto* entry_sp = stack.data() + 100;
        *entry_sp = 0x1234567812345678ULL;
        DWORD64* sp = entry_sp;
        constexpr DWORD64 values[]{15,14,13,12,6,7,5,3};
        const auto pushed = index < 8 ? index : 8;
        for (unsigned i = 0; i < pushed; ++i) *--sp = values[i];
        if (index >= 9) sp -= 9;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_ALL;
        ctx.Rsp = reinterpret_cast<DWORD64>(sp);
        ctx.Rip = reinterpret_cast<DWORD64>(trampoline) + offsets[index];
        ctx.R15=15; ctx.R14=14; ctx.R13=13; ctx.R12=12; ctx.Rsi=6; ctx.Rdi=7; ctx.Rbp=5; ctx.Rbx=3;
        DWORD64 base{}, establisher{};
        void* handler{};
        const auto function = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        check(function != nullptr, "registered dynamic unwind table");
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, function, &ctx, &handler, &establisher, nullptr);
        if (ctx.Rsp != reinterpret_cast<DWORD64>(entry_sp + 1) || ctx.Rip != *entry_sp) {
            std::fprintf(stderr,"unwind index=%u offset=%u rsp=%llx expected_rsp=%llx rip=%llx expected_rip=%llx\n",
                index,offsets[index],ctx.Rsp,reinterpret_cast<DWORD64>(entry_sp+1),ctx.Rip,*entry_sp);
        }
        check(ctx.Rsp == reinterpret_cast<DWORD64>(entry_sp + 1) && ctx.Rip == *entry_sp,
            "unwind restores caller at every copied instruction boundary");
        check(ctx.R15==15 && ctx.R14==14 && ctx.R13==13 && ctx.R12==12 &&
            ctx.Rsi==6 && ctx.Rdi==7 && ctx.Rbp==5 && ctx.Rbx==3, "nonvolatile registers unwind");
    }
}
}
int main() {
    auto* target = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(0x140000000ULL),
        4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    check(target != nullptr, "bounded low-address synthetic code page");
    write(target, code, sizeof(code));
    auto call = reinterpret_cast<Fn>(target);
    check(call(5, 7) == 12, "native baseline");
    check(MH_Initialize() == MH_OK, "initialize");
    void* sentinel = reinterpret_cast<void*>(0x1234);
    for (int fault : {1,2,3}) {
        fixed16_test_fault(fault);
        auto status = MH_CreateHookStackPrologue16(target, reinterpret_cast<void*>(&detour), &sentinel);
        check(status == (fault==2 ? MH_ERROR_MEMORY_PROTECT : MH_ERROR_MEMORY_ALLOC), "injected allocation/protection/unwind failure");
        check(sentinel == reinterpret_cast<void*>(0x1234) && fixed16_test_allocations()==0, "failure neither publishes nor leaks");
        check(std::memcmp(target, code, 16)==0 && call(5,7)==12, "failure preserves native code");
    }
    fixed16_test_fault(0);
    const std::uint8_t foreign = 0x90;
    write(target, &foreign, 1);
    check(MH_CreateHookStackPrologue16(target, reinterpret_cast<void*>(&detour), &sentinel)==MH_ERROR_UNSUPPORTED_FUNCTION, "unknown/foreign prefix rejected");
    write(target, code, 1);
    write(target+16, &foreign, 1);
    check(MH_CreateHookStackPrologue16(target,reinterpret_cast<void*>(&detour),&sentinel)==MH_ERROR_UNSUPPORTED_FUNCTION,
        "RAX-live/changed continuation is rejected");
    write(target+16,code+16,1);
    check(MH_CreateHookStackPrologue16(target, reinterpret_cast<void*>(&detour), reinterpret_cast<void**>(&original))==MH_OK, "create far trampoline");
    auto* trampoline = reinterpret_cast<void*>(original);
    const auto distance = reinterpret_cast<std::uintptr_t>(trampoline) - reinterpret_cast<std::uintptr_t>(target);
    check(distance > 0x80000000ULL, "actual trampoline exceeds rel32 positive range");
    check(original(5,7)==12 && call(5,7)==12, "create does not enable");
    unwind(trampoline);
    for (unsigned offset : {0U,2U,4U,6U,8U,9U,10U,11U,12U,16U}) {
        const auto old_ip = reinterpret_cast<std::uintptr_t>(target)+offset;
        const auto new_ip = reinterpret_cast<std::uintptr_t>(trampoline)+offset;
        check(fixed16_test_ip(target,old_ip,1)==new_ip && fixed16_test_ip(target,new_ip,0)==old_ip, "freeze IP mappings");
    }
    check(fixed16_test_ip(target,reinterpret_cast<std::uintptr_t>(&detour),0)==0, "C++ detour is not treated as MinHook relay");
    check(fixed16_test_ip(target,reinterpret_cast<std::uintptr_t>(trampoline)+26,0)==reinterpret_cast<std::uintptr_t>(target)+16,"jump-register disable IP mapping");
    check(MH_CreateHookStackPrologue16(target,reinterpret_cast<void*>(&detour),&sentinel)==MH_ERROR_ALREADY_CREATED, "duplicate refused");
    write(target, &foreign, 1);
    check(MH_EnableHook(target)==MH_ERROR_UNSUPPORTED_FUNCTION && target[0]==foreign, "create/enable conflict preserves foreign patch");
    write(target,code,1);
    fixed16_test_fault(4);
    check(MH_EnableHook(target)==MH_ERROR_MEMORY_PROTECT && call(5,7)==12, "enable protection failure leaves native intact");
    fixed16_test_fault(0);
    check(MH_EnableHook(target)==MH_OK && call(5,7)==13, "absolute detour and original forwarding");
    std::array<std::uint8_t,16> our_patch{};
    std::memcpy(our_patch.data(),target,our_patch.size());
    write(target,&foreign,1);
    check(MH_DisableHook(target)==MH_ERROR_UNSUPPORTED_FUNCTION && target[0]==foreign,
        "disable does not overwrite a foreign replacement");
    write(target,our_patch.data(),our_patch.size());
    check(MH_DisableHook(target)==MH_OK && call(5,7)==12 && std::memcmp(target,code,16)==0, "exact 16-byte restore");
    std::vector<std::thread> workers;
    for (unsigned i=0;i<4;++i) workers.emplace_back([call] {
        unsigned calls{};
        while (running.load(std::memory_order_relaxed)) {
            auto value=call(5,7);
            if (value!=12 && value!=13) bad.fetch_add(1);
            if (++calls % 256 == 0) Sleep(1);
        }
    });
    for (unsigned i=0;i<100;++i) {
        check(MH_EnableHook(target)==MH_OK, "concurrent enable");
        Sleep(1);
        check(MH_DisableHook(target)==MH_OK, "concurrent disable");
    }
    running.store(false);
    for (auto& worker:workers) worker.join();
    check(bad.load()==0 && hits.load()>0, "concurrent native calls remain valid");
    check(MH_RemoveHook(target)==MH_OK && fixed16_test_allocations()==0, "remove frees far page");
    DWORD64 base{};
    check(RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(trampoline),&base,nullptr)==nullptr, "unwind table removed with page");
    check(MH_CreateHookStackPrologue16(target,reinterpret_cast<void*>(&detour),reinterpret_cast<void**>(&original))==MH_OK, "recreate");
    check(MH_EnableHook(target)==MH_OK && MH_Uninitialize()==MH_OK, "uninitialize enabled fallback");
    check(fixed16_test_allocations()==0 && call(5,7)==12, "uninitialize exact restore and no allocation leak");
    VirtualFree(target,0,MEM_RELEASE);
    std::printf("fixed16=PASS unwind_boundaries=11 ip_boundaries=11 concurrent_cycles=100 workers=4 detour_calls=%llu far_distance=%llu allocated_fixture_bytes=4096 game_process_access=NO\n",hits.load(),distance);
}
