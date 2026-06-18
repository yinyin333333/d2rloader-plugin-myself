#include "extern/plugin.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace {

static constexpr uintptr_t RVA_InvalidTextureBlockSource = 0x840C2C;
static constexpr uintptr_t RVA_DAT_GlobalInvalidTexture = 0x1E71A00;
static constexpr uintptr_t RVA_DAT_GlobalInvalid3DTexture = 0x1E71A08;
static constexpr uintptr_t RVA_DAT_GlobalInvalidTerrainChunkTexture = 0x1E719F8;
static constexpr uintptr_t RVA_DAT_GlobalInvalidTerrainNoiseTexture = 0x1E719F0;

static constexpr std::array<uint8_t, 3> ExpectedBytes {
    0x49, 0x8B, 0xD5,
};

static constexpr std::array<uint8_t, 3> PatchBytes {
    0x48, 0x8B, 0xD7,
};

static uintptr_t g_exeBase = 0;
static uint8_t* g_patchSite = nullptr;
static bool g_patchApplied = false;
static bool g_logReady = false;
static CRITICAL_SECTION g_logLock {};

static constexpr D2RLoaderPluginInfo PluginInfo {
    .apiVersion = D2RLOADER_PLUGIN_API_VERSION,
    .id         = "texture_placeholder_memory_patch_test",
    .name       = "TexturePlaceholderMemoryPatchTest",
    .version    = "0.1.0",
    .author     = "yinyin333333",
    .flags      = D2RLoaderPluginFlag_None,
};

static void AppendLog(const char*, ...) noexcept {
}

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

static uintptr_t ReadPointer(uintptr_t address) noexcept {
    uintptr_t value = 0;
    SafeRead(reinterpret_cast<const void*>(address), &value, sizeof(value));
    return value;
}

static void LogInvalidTextureGlobals(const char* phase) noexcept {
    if (!g_exeBase) {
        return;
    }

    const uintptr_t addr2d = g_exeBase + RVA_DAT_GlobalInvalidTexture;
    const uintptr_t addr3d = g_exeBase + RVA_DAT_GlobalInvalid3DTexture;
    const uintptr_t addrChunk = g_exeBase + RVA_DAT_GlobalInvalidTerrainChunkTexture;
    const uintptr_t addrNoise = g_exeBase + RVA_DAT_GlobalInvalidTerrainNoiseTexture;

    AppendLog(
        "event=invalid_globals phase=%s DAT_141e71a00_slot=0x%p value=0x%p DAT_141e71a08_slot=0x%p value=0x%p DAT_141e719f8_slot=0x%p value=0x%p DAT_141e719f0_slot=0x%p value=0x%p",
        phase,
        reinterpret_cast<void*>(addr2d),
        reinterpret_cast<void*>(ReadPointer(addr2d)),
        reinterpret_cast<void*>(addr3d),
        reinterpret_cast<void*>(ReadPointer(addr3d)),
        reinterpret_cast<void*>(addrChunk),
        reinterpret_cast<void*>(ReadPointer(addrChunk)),
        reinterpret_cast<void*>(addrNoise),
        reinterpret_cast<void*>(ReadPointer(addrNoise)));
}

static bool InstallPatch() noexcept {
    g_patchSite = reinterpret_cast<uint8_t*>(g_exeBase + RVA_InvalidTextureBlockSource);

    std::array<uint8_t, ExpectedBytes.size()> current {};
    if (!SafeRead(g_patchSite, current.data(), current.size())) {
        AppendLog("event=patch_install result=failed reason=read_failed site=0x%p", g_patchSite);
        return false;
    }

    if (current != ExpectedBytes) {
        AppendLog(
            "event=patch_install result=skipped reason=byte_mismatch site=0x%p current=%02X%02X%02X expected=%02X%02X%02X",
            g_patchSite,
            current[0],
            current[1],
            current[2],
            ExpectedBytes[0],
            ExpectedBytes[1],
            ExpectedBytes[2]);
        return false;
    }

    g_patchApplied = SafeWrite(g_patchSite, PatchBytes.data(), PatchBytes.size());
    AppendLog(
        "event=patch_install result=%s site=0x%p rva=0x%llX original=%02X%02X%02X patch=%02X%02X%02X meaning=invalid_texture_blocks_use_zero_RDI_instead_of_checker_seed_R13",
        g_patchApplied ? "applied" : "failed",
        g_patchSite,
        static_cast<unsigned long long>(RVA_InvalidTextureBlockSource),
        ExpectedBytes[0],
        ExpectedBytes[1],
        ExpectedBytes[2],
        PatchBytes[0],
        PatchBytes[1],
        PatchBytes[2]);
    return g_patchApplied;
}

static void RestorePatch() noexcept {
    bool restored = false;
    if (g_patchApplied && g_patchSite) {
        restored = SafeWrite(g_patchSite, ExpectedBytes.data(), ExpectedBytes.size());
    }
    AppendLog("event=patch_restore applied_previously=%s restored=%s", g_patchApplied ? "yes" : "no", restored ? "yes" : "no");
    g_patchApplied = false;
    g_patchSite = nullptr;
}

static void ResetLog() noexcept {
}

} // namespace

D2RLOADER_PLUGIN_EXPORT const D2RLoaderPluginInfo* __cdecl D2RLoaderGetPluginInfo() noexcept {
    return &PluginInfo;
}

D2RLOADER_PLUGIN_EXPORT bool __cdecl D2RLoaderLoadHooks(const D2RLoaderPluginContext* ctx) noexcept {
    InitializeCriticalSection(&g_logLock);
    g_logReady = true;
    ResetLog();

    if (!ctx || ctx->apiVersion < D2RLOADER_PLUGIN_API_VERSION || !ctx->exeBase) {
        AppendLog("event=plugin_load result=failed reason=invalid_context");
        g_logReady = false;
        DeleteCriticalSection(&g_logLock);
        return false;
    }

    g_exeBase = ctx->exeBase;
    AppendLog(
        "event=plugin_load result=begin exeBase=0x%p activeMod=%s patchRVA=0x%llX targetDATs=DAT_141e71a00,DAT_141e71a08,DAT_141e719f8,DAT_141e719f0",
        reinterpret_cast<void*>(g_exeBase),
        ctx->activeMod ? ctx->activeMod : "",
        static_cast<unsigned long long>(RVA_InvalidTextureBlockSource));

    LogInvalidTextureGlobals("before_patch");

    if (!InstallPatch()) {
        LogInvalidTextureGlobals("patch_not_applied");
        g_exeBase = 0;
        g_logReady = false;
        DeleteCriticalSection(&g_logLock);
        return false;
    }

    LogInvalidTextureGlobals("after_patch");
    AppendLog("event=plugin_load result=success");
    return true;
}

D2RLOADER_PLUGIN_EXPORT void __cdecl D2RLoaderUnload() noexcept {
    LogInvalidTextureGlobals("before_unload");
    RestorePatch();
    LogInvalidTextureGlobals("after_restore");

    g_exeBase = 0;
    if (g_logReady) {
        g_logReady = false;
        DeleteCriticalSection(&g_logLock);
    }
}
