#include "extern/plugin.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

static constexpr uintptr_t RVA_MenuRowBuilder = 0xD2A3B0;
static constexpr uintptr_t RVA_PcListStart = 0x1669A90;
static constexpr uintptr_t RVA_PcListEnd = 0x1669BA0;
static constexpr uintptr_t RVA_HotkeyTable = 0x1986300;
static constexpr uintptr_t RVA_DirectionBytes = 0x1EEC848;
static constexpr uintptr_t RVA_CBE80VectorInjectSite = 0x0CC1DF;

static constexpr uint32_t Action_ForceMove = 0x3B;
static constexpr uint32_t Action_MoveUp = 0x3F;
static constexpr uint32_t Action_MoveRight = 0x40;
static constexpr uint32_t Action_MoveDown = 0x41;
static constexpr uint32_t Action_MoveLeft = 0x42;
static constexpr size_t OriginalPcCount = 68;
static constexpr size_t ClonePcCount = OriginalPcCount + 4;

static constexpr std::array<uint8_t, 3> Expected_MenuRowBuilder {0x48, 0x8B, 0xC4};
static constexpr std::array<uint8_t, 9> Expected_CBE80VectorInjectSite {
    0xF3, 0x0F, 0x10, 0x8C, 0x3E, 0x30, 0x0D, 0x00, 0x00
};

static constexpr std::array<uint32_t, OriginalPcCount> ExpectedPcList {
    0x00, 0x01, 0x3C, 0x02, 0x36, 0x03, 0x04, 0x06, 0x44, 0x45,
    0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
    0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x27, 0x28,
    0x45, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x2C, 0x45, 0x05, 0x22,
    0x23, 0x24, 0x3B, 0x25, 0x43, 0x2B, 0x45, 0x07, 0x08, 0x09,
    0x0A, 0x0B, 0x2D, 0x45, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
    0x21, 0x37, 0x45, 0x3D, 0x3E, 0x45, 0x26, 0x29
};

enum class HookKind : uint8_t {
    MenuRowBuilder,
    VectorInject,
};

struct HookPatch {
    HookKind kind;
    uintptr_t rva;
    size_t instructionLength;
    uint8_t original;
    bool installed;
};

struct DirectionSnapshot {
    uint8_t up;
    uint8_t right;
    uint8_t down;
    uint8_t left;
};

struct VectorSlots {
    float rawX;
    float rawY;
    float processedX;
    float processedY;
};

static const D2RLoaderPluginContext* g_ctx = nullptr;
static uintptr_t g_exeBase = 0;
static void* g_vectoredHandler = nullptr;
static std::array<uint32_t, OriginalPcCount> g_originalPcList {};
static std::array<uint32_t, ClonePcCount> g_clonedPcList {};
static bool g_cloneReady = false;

static HookPatch g_menuRowBuilder {HookKind::MenuRowBuilder, RVA_MenuRowBuilder, 3, 0, false};
static HookPatch g_vectorInject {HookKind::VectorInject, RVA_CBE80VectorInjectSite, 9, 0, false};
static std::array<HookPatch*, 2> g_hooks {&g_menuRowBuilder, &g_vectorInject};

static constexpr D2RLoaderPluginInfo PluginInfo {
    .apiVersion = D2RLOADER_PLUGIN_API_VERSION,
    .id         = "d2r-wasd-cbe80-vector-inject-controller-safe-v2",
    .name       = "D2R WASD CBE80 Vector Inject Controller Safe V2",
    .version    = "1.0.2-test",
    .author     = "yinyin333333",
    .flags      = D2RLoaderPluginFlag_None,
};

static bool SafeRead(const void* src, void* dst, size_t n) noexcept {
    if (!src || !dst || n == 0) return false;
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
static bool SafeReadValue(uintptr_t address, T& out) noexcept {
    return SafeRead(reinterpret_cast<const void*>(address), &out, sizeof(out));
}

static bool SafeStoreFloat(uintptr_t address, float value) noexcept {
    if (!address) return false;
    __try {
        *reinterpret_cast<float*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <size_t N>
static bool VerifyBytes(uintptr_t rva, const std::array<uint8_t, N>& expected) noexcept {
    std::array<uint8_t, N> actual {};
    return SafeRead(reinterpret_cast<const void*>(g_exeBase + rva), actual.data(), actual.size()) &&
           actual == expected;
}

static bool VerifyHotkeyEntry(uint32_t actionId, uintptr_t expectedDownRva, uintptr_t expectedUpRva) noexcept {
    const uintptr_t entry = g_exeBase + RVA_HotkeyTable + static_cast<uintptr_t>(actionId) * 0x18;
    uintptr_t down = 0;
    uintptr_t up = 0;
    return SafeReadValue(entry, down) &&
           SafeReadValue(entry + 8, up) &&
           down == g_exeBase + expectedDownRva &&
           up == g_exeBase + expectedUpRva;
}

static bool VerifyOriginalPcList() noexcept {
    const uintptr_t start = g_exeBase + RVA_PcListStart;
    const uintptr_t end = g_exeBase + RVA_PcListEnd;
    return (end - start) == OriginalPcCount * sizeof(uint32_t) &&
           SafeRead(reinterpret_cast<const void*>(start), g_originalPcList.data(), OriginalPcCount * sizeof(uint32_t)) &&
           g_originalPcList == ExpectedPcList;
}

static bool VerifyAll() noexcept {
    return VerifyBytes(RVA_MenuRowBuilder, Expected_MenuRowBuilder) &&
           VerifyBytes(RVA_CBE80VectorInjectSite, Expected_CBE80VectorInjectSite) &&
           VerifyOriginalPcList() &&
           VerifyHotkeyEntry(Action_MoveUp, 0x138880, 0x138850) &&
           VerifyHotkeyEntry(Action_MoveRight, 0x1388E0, 0x1388B0) &&
           VerifyHotkeyEntry(Action_MoveDown, 0x138940, 0x138910) &&
           VerifyHotkeyEntry(Action_MoveLeft, 0x1389A0, 0x138970);
}

static bool BuildClone() noexcept {
    size_t out = 0;
    bool inserted = false;
    for (size_t i = 0; i < OriginalPcCount; ++i) {
        g_clonedPcList[out++] = g_originalPcList[i];
        if (!inserted && g_originalPcList[i] == Action_ForceMove) {
            g_clonedPcList[out++] = Action_MoveUp;
            g_clonedPcList[out++] = Action_MoveRight;
            g_clonedPcList[out++] = Action_MoveDown;
            g_clonedPcList[out++] = Action_MoveLeft;
            inserted = true;
        }
    }
    g_cloneReady = inserted && out == ClonePcCount;
    return g_cloneReady;
}

static bool InstallHook(HookPatch& hook) noexcept {
    const uintptr_t target = g_exeBase + hook.rva;
    if (!SafeReadValue(target, hook.original)) return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(target), 1, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    *reinterpret_cast<uint8_t*>(target) = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), 1);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(target), 1, oldProtect, &ignored);
    hook.installed = true;
    return true;
}

static void RestoreHook(HookPatch& hook) noexcept {
    if (!hook.installed || !g_exeBase) return;
    const uintptr_t target = g_exeBase + hook.rva;
    DWORD oldProtect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(target), 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        *reinterpret_cast<uint8_t*>(target) = hook.original;
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), 1);
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(target), 1, oldProtect, &ignored);
    }
    hook.installed = false;
}

static HookPatch* FindHook(uintptr_t exceptionAddress, uintptr_t rip) noexcept {
    for (HookPatch* hook : g_hooks) {
        if (!hook || !hook->installed) continue;
        const uintptr_t target = g_exeBase + hook->rva;
        if (exceptionAddress == target || rip == target) return hook;
    }
    return nullptr;
}

static bool ReadDirections(DirectionSnapshot& dirs) noexcept {
    return SafeRead(reinterpret_cast<const void*>(g_exeBase + RVA_DirectionBytes), &dirs, sizeof(dirs));
}

static bool HasWasdDirection(const DirectionSnapshot& dirs) noexcept {
    return dirs.up != 0 || dirs.right != 0 || dirs.down != 0 || dirs.left != 0;
}

static void ComputeWasdVector(const DirectionSnapshot& dirs, VectorSlots& out) noexcept {
    out.rawX = (dirs.right ? 1.0f : 0.0f) - (dirs.left ? 1.0f : 0.0f);
    out.rawY = (dirs.up ? 1.0f : 0.0f) - (dirs.down ? 1.0f : 0.0f);
    if (out.rawX != 0.0f && out.rawY != 0.0f) {
        constexpr float InvSqrt2 = 0.7071067811865476f;
        out.rawX *= InvSqrt2;
        out.rawY *= InvSqrt2;
    }
    constexpr float InvSqrt2 = 0.7071067811865476f;
    out.processedX = (out.rawX - out.rawY) * InvSqrt2;
    out.processedY = -(out.rawX + out.rawY) * InvSqrt2;
}

static bool WriteVectorSlots(uintptr_t base, const VectorSlots& slots) noexcept {
    return SafeStoreFloat(base + 0xD2C, slots.rawX) &&
           SafeStoreFloat(base + 0xD30, slots.rawY) &&
           SafeStoreFloat(base + 0xD34, slots.processedX) &&
           SafeStoreFloat(base + 0xD38, slots.processedY);
}

static void SetXmmLowFloat(M128A& xmm, float value) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    xmm.Low = (xmm.Low & 0xFFFFFFFF00000000ull) | bits;
}

static void HandleVectorInject(CONTEXT* ctx, uintptr_t target, const HookPatch& hook) noexcept {
    const uintptr_t vectorBase = static_cast<uintptr_t>(ctx->Rdi) + static_cast<uintptr_t>(ctx->Rsi);
    DirectionSnapshot dirs {};
    if (ReadDirections(dirs) && HasWasdDirection(dirs)) {
        VectorSlots slots {};
        ComputeWasdVector(dirs, slots);
        WriteVectorSlots(vectorBase, slots);
    }

    float xmm1Value = 0.0f;
    SafeReadValue(vectorBase + 0xD30, xmm1Value);
    SetXmmLowFloat(ctx->Xmm1, xmm1Value);
    ctx->Rip = target + hook.instructionLength;
}

static LONG CALLBACK HookHandler(EXCEPTION_POINTERS* info) noexcept {
    if (!info || !info->ExceptionRecord || !info->ContextRecord || !g_exeBase) return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT* ctx = info->ContextRecord;
    const uintptr_t rip = static_cast<uintptr_t>(ctx->Rip);
    const uintptr_t exceptionAddress = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    HookPatch* hook = FindHook(exceptionAddress, rip);
    if (!hook) return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t target = g_exeBase + hook->rva;
    switch (hook->kind) {
    case HookKind::MenuRowBuilder: {
        const uintptr_t originalStart = g_exeBase + RVA_PcListStart;
        const uintptr_t originalEnd = g_exeBase + RVA_PcListEnd;
        if (static_cast<uintptr_t>(ctx->Rdx) == originalStart &&
            static_cast<uintptr_t>(ctx->R8) == originalEnd &&
            g_cloneReady) {
            ctx->Rdx = reinterpret_cast<DWORD64>(g_clonedPcList.data());
            ctx->R8 = reinterpret_cast<DWORD64>(g_clonedPcList.data() + g_clonedPcList.size());
        }
        ctx->Rax = ctx->Rsp;
        ctx->Rip = target + hook->instructionLength;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    case HookKind::VectorInject:
        HandleVectorInject(ctx, target, *hook);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool InstallAllHooks() noexcept {
    for (HookPatch* hook : g_hooks) {
        if (!InstallHook(*hook)) return false;
    }
    return true;
}

static void RestoreAllHooks() noexcept {
    for (auto it = g_hooks.rbegin(); it != g_hooks.rend(); ++it) RestoreHook(**it);
}

} // namespace

D2RLOADER_PLUGIN_EXPORT const D2RLoaderPluginInfo* __cdecl D2RLoaderGetPluginInfo() noexcept {
    return &PluginInfo;
}

D2RLOADER_PLUGIN_EXPORT bool __cdecl D2RLoaderLoadHooks(const D2RLoaderPluginContext* ctx) noexcept {
    g_ctx = ctx;
    g_exeBase = ctx ? ctx->exeBase : 0;
    g_cloneReady = false;
    g_vectoredHandler = nullptr;
    for (HookPatch* hook : g_hooks) {
        hook->installed = false;
        hook->original = 0;
    }

    if (!ctx || ctx->apiVersion < D2RLOADER_PLUGIN_API_VERSION || ctx->exeBase == 0) return false;
    if (!VerifyAll()) return false;
    if (!BuildClone()) return false;

    g_vectoredHandler = AddVectoredExceptionHandler(1, HookHandler);
    if (!g_vectoredHandler) return false;
    if (!InstallAllHooks()) {
        RestoreAllHooks();
        RemoveVectoredExceptionHandler(g_vectoredHandler);
        g_vectoredHandler = nullptr;
        return false;
    }
    return true;
}

D2RLOADER_PLUGIN_EXPORT void __cdecl D2RLoaderUnload() noexcept {
    RestoreAllHooks();
    if (g_vectoredHandler) {
        RemoveVectoredExceptionHandler(g_vectoredHandler);
        g_vectoredHandler = nullptr;
    }
    g_ctx = nullptr;
    g_exeBase = 0;
    g_cloneReady = false;
}
