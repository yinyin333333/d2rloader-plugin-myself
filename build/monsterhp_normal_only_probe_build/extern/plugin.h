#pragma once

#include <cstdint>

#define D2RLOADER_PLUGIN_API_VERSION 1u
#define D2RLOADER_PLUGIN_EXPORT extern "C" __declspec(dllexport)

enum D2RLoaderPluginFlags : uint32_t {
    D2RLoaderPluginFlag_None          = 0,
    D2RLoaderPluginFlag_ModScopedOnly = 1u << 0,
};

enum D2RLoaderPluginLoadScope : uint32_t {
    D2RLoaderPluginLoadScope_Mod    = 1,
    D2RLoaderPluginLoadScope_Global = 2,
};

struct D2RLoaderPluginInfo {
    uint32_t apiVersion;
    const char* id;
    const char* name;
    const char* version;
    const char* author;
    uint32_t flags;
};

struct D2RLoaderPluginContext {
    uint32_t apiVersion;
    D2RLoaderPluginLoadScope loadScope;
    uintptr_t exeBase;
    const char* activeMod;
    const wchar_t* modDirectory;
    void (*logInfo)(const char* message);
    void (*logWarn)(const char* message);
    void (*logError)(const char* message);
};
