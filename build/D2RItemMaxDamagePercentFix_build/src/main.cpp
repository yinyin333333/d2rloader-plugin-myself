#include "extern/plugin.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

static constexpr uintptr_t RVA_PatchSite = 0x225C02;
static constexpr uintptr_t RVA_Return = 0x225C11;
static constexpr uint32_t EncodedStatItemMaxDamagePercent = 0x00110000;

static constexpr std::array<uint8_t, 13> ExpectedBytes {
    0x33, 0xED,                         // xor ebp, ebp
    0x83, 0x7E, 0x08, 0x04,             // cmp dword ptr [rsi+8], 4
    0x0F, 0x45, 0x6C, 0x24, 0x34,       // cmovnz ebp, dword ptr [rsp+34h]
    0xEB, 0x02,                         // jmp +2
};

static uintptr_t g_exeBase = 0;
static uint8_t* g_patchSite = nullptr;
static uint8_t* g_relay = nullptr;
static std::array<uint8_t, ExpectedBytes.size()> g_original {};
static bool g_patchApplied = false;

static constexpr D2RLoaderPluginInfo PluginInfo {
    .apiVersion = D2RLOADER_PLUGIN_API_VERSION,
    .id         = "d2r-item-maxdamage-percent-fix",
    .name       = "D2R Item MaxDamage Percent Fix",
    .version    = "0.1.0",
    .author     = "yinyin333333",
    .flags      = D2RLoaderPluginFlag_None,
};

static bool SafeRead(const void* src, void* dst, size_t n) noexcept {
    if (!src || !dst || n == 0) {
        return false;
    }

    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeWrite(void* dst, const void* src, size_t n) noexcept {
    if (!dst || !src || n == 0) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    std::memcpy(dst, src, n);

    DWORD ignored = 0;
    VirtualProtect(dst, n, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

static bool Rel32Fits(uintptr_t fromInstruction, uintptr_t toAddress) noexcept {
    const int64_t delta = static_cast<int64_t>(toAddress) - static_cast<int64_t>(fromInstruction + 5);
    return delta >= std::numeric_limits<int32_t>::min() && delta <= std::numeric_limits<int32_t>::max();
}

static uint8_t* AllocateNear(uintptr_t target, size_t size) noexcept {
    SYSTEM_INFO info {};
    GetSystemInfo(&info);

    const uintptr_t granularity = info.dwAllocationGranularity ? info.dwAllocationGranularity : 0x10000;
    const uintptr_t minAddress = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t maxAddress = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t alignedTarget = target & ~(granularity - 1);
    const uintptr_t maxDistance = 0x7fff0000ull;

    for (uintptr_t distance = 0; distance <= maxDistance; distance += granularity) {
        const uintptr_t down = alignedTarget >= distance ? alignedTarget - distance : 0;
        if (down >= minAddress && Rel32Fits(target, down)) {
            void* memory = VirtualAlloc(reinterpret_cast<void*>(down), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (memory) {
                return static_cast<uint8_t*>(memory);
            }
        }

        const uintptr_t up = alignedTarget + distance;
        if (up >= alignedTarget && up <= maxAddress && Rel32Fits(target, up)) {
            void* memory = VirtualAlloc(reinterpret_cast<void*>(up), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (memory) {
                return static_cast<uint8_t*>(memory);
            }
        }
    }

    return nullptr;
}

static void EmitJmpRel32(std::vector<uint8_t>& code, uintptr_t instructionAddress, uintptr_t targetAddress) {
    code.push_back(0xE9);
    const int64_t delta64 = static_cast<int64_t>(targetAddress) - static_cast<int64_t>(instructionAddress + 5);
    const int32_t delta = static_cast<int32_t>(delta64);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&delta);
    code.insert(code.end(), bytes, bytes + sizeof(delta));
}

static bool BuildRelay(uint8_t* relay) noexcept {
    if (!relay) {
        return false;
    }

    std::vector<uint8_t> code;
    code.reserve(64);

    const uintptr_t relayBase = reinterpret_cast<uintptr_t>(relay);
    const uintptr_t returnAddress = g_exeBase + RVA_Return;

    code.insert(code.end(), {
        0x33, 0xED,                         // xor ebp, ebp
        0x83, 0x7E, 0x08, 0x04,             // cmp dword ptr [rsi+8], 4
    });

    const size_t ownerNotItemJne = code.size();
    code.insert(code.end(), { 0x75, 0x00 }); // jne set_original_value

    code.insert(code.end(), {
        0x81, 0xBC, 0x24, 0x88, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x11, 0x00,             // cmp dword ptr [rsp+88h], 00110000h
    });

    const size_t notMaxPercentJne = code.size();
    code.insert(code.end(), { 0x75, 0x00 }); // jne done

    const size_t setOriginalValue = code.size();
    code.insert(code.end(), {
        0x8B, 0x6C, 0x24, 0x34,             // mov ebp, dword ptr [rsp+34h]
    });

    const size_t done = code.size();
    code[ownerNotItemJne + 1] = static_cast<uint8_t>(setOriginalValue - (ownerNotItemJne + 2));
    code[notMaxPercentJne + 1] = static_cast<uint8_t>(done - (notMaxPercentJne + 2));

    EmitJmpRel32(code, relayBase + code.size(), returnAddress);

    std::memcpy(relay, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), relay, code.size());
    return true;
}

static bool InstallPatch() noexcept {
    g_patchSite = reinterpret_cast<uint8_t*>(g_exeBase + RVA_PatchSite);

    if (!SafeRead(g_patchSite, g_original.data(), g_original.size())) {
        return false;
    }

    if (g_original != ExpectedBytes) {
        return false;
    }

    g_relay = AllocateNear(reinterpret_cast<uintptr_t>(g_patchSite), 4096);
    if (!g_relay) {
        return false;
    }
    std::memset(g_relay, 0xCC, 4096);

    if (!BuildRelay(g_relay)) {
        return false;
    }

    std::array<uint8_t, ExpectedBytes.size()> patch {};
    patch.fill(0x90);
    patch[0] = 0xE9;

    const uintptr_t patchSite = reinterpret_cast<uintptr_t>(g_patchSite);
    const uintptr_t relayAddress = reinterpret_cast<uintptr_t>(g_relay);
    const int64_t delta64 = static_cast<int64_t>(relayAddress) - static_cast<int64_t>(patchSite + 5);
    if (delta64 < std::numeric_limits<int32_t>::min() || delta64 > std::numeric_limits<int32_t>::max()) {
        return false;
    }

    const int32_t delta = static_cast<int32_t>(delta64);
    std::memcpy(patch.data() + 1, &delta, sizeof(delta));

    g_patchApplied = SafeWrite(g_patchSite, patch.data(), patch.size());
    return g_patchApplied;
}

static void RestorePatch() noexcept {
    if (g_patchApplied && g_patchSite) {
        SafeWrite(g_patchSite, g_original.data(), g_original.size());
    }

    if (g_relay) {
        VirtualFree(g_relay, 0, MEM_RELEASE);
    }

    g_patchSite = nullptr;
    g_relay = nullptr;
    g_patchApplied = false;
}

} // namespace

D2RLOADER_PLUGIN_EXPORT const D2RLoaderPluginInfo* __cdecl D2RLoaderGetPluginInfo() noexcept {
    return &PluginInfo;
}

D2RLOADER_PLUGIN_EXPORT bool __cdecl D2RLoaderLoadHooks(const D2RLoaderPluginContext* ctx) noexcept {
    if (!ctx || ctx->apiVersion < D2RLOADER_PLUGIN_API_VERSION || !ctx->exeBase) {
        return false;
    }

    g_exeBase = ctx->exeBase;
    if (!InstallPatch()) {
        RestorePatch();
        g_exeBase = 0;
        return false;
    }

    return true;
}

D2RLOADER_PLUGIN_EXPORT void __cdecl D2RLoaderUnload() noexcept {
    RestorePatch();
    g_exeBase = 0;
}
