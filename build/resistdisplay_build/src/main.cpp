#include "extern/plugin.h"

#include <windows.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

static constexpr uintptr_t RVA_ApplyResistanceToDamageDescriptor = 0x3B34A0;
static constexpr uintptr_t RVA_ResolveResistancePercent = 0x3B3080;
static constexpr uintptr_t RVA_UpdateMonsterHealthPanelBranch = 0x178800;
static constexpr uintptr_t RVA_ApplyInlineColorCode = 0x094890;
static constexpr uintptr_t RVA_StatGetter = 0x224720;
static constexpr uintptr_t RVA_GetUnitById = 0x06F710;
static constexpr uintptr_t RVA_ExcelContexts = 0x1E3A610;
static constexpr uintptr_t RVA_LocalPlayerUnitId = 0x1EEC7D4;
static constexpr uintptr_t RVA_FindDirectChildWidget = 0x64C5E0;
static constexpr uintptr_t RVA_FindRecursiveChildWidget = 0x64C690;
static constexpr uintptr_t RVA_CommitText = 0x0663E0;
static constexpr uintptr_t RVA_RefreshWidget = 0x65C690;
static constexpr uintptr_t RVA_ImmunityDescriptorTableReady = 0x095AC6;
static constexpr uintptr_t RVA_HpBarTextLengthCalc = 0x095D29;
static constexpr uintptr_t RVA_HpBarTextLengthNonEmpty = 0x095D32;
static constexpr uintptr_t RVA_HpBarTextLengthEmpty = 0x095D4C;

static constexpr uint32_t TargetUnitTypeMonster = 1;
static constexpr DWORD CacheTtlMs = 5000;
static constexpr DWORD ObserverFreshMs = 50;
static constexpr DWORD TtlFallbackMs = 200;
static constexpr uint64_t MonsterHealthSetInfoHash = 0x40ffba2d98a3ff97ull;
static constexpr uint64_t MonsterHealthClearInfoHash = 0x73013be2814ed07aull;
static constexpr uintptr_t StatRowsOffset = 0x1258;
static constexpr uintptr_t StatCountOffset = 0x1260;
static constexpr size_t StatRowSize = 0x144;
static constexpr int SunderBucketThreshold = 300;
static constexpr int SunderHoverFallbackResistance = 95;

enum class ElementIndex : size_t {
    Physical = 0,
    Magic,
    Fire,
    Lightning,
    Cold,
    Poison,
    Count,
};

struct ElementDef {
    ElementIndex index;
    int32_t targetResStat;
    int32_t localSunderStat;
    uint8_t colorId;
    char label;
    const char* name;
};

static constexpr std::array<ElementDef, static_cast<size_t>(ElementIndex::Count)> Elements {{
    { ElementIndex::Physical, 0x24, 0xC0, 0x04, 'D', "Physical" },
    { ElementIndex::Magic, 0x25, 0xC1, 0x08, 'M', "Magic" },
    { ElementIndex::Fire, 0x27, 0xBD, 0x01, 'F', "Fire" },
    { ElementIndex::Lightning, 0x29, 0xBE, 0x09, 'L', "Lightning" },
    { ElementIndex::Cold, 0x2B, 0xBB, 0x03, 'C', "Cold" },
    { ElementIndex::Poison, 0x2D, 0xBF, 0x02, 'P', "Poison" },
}};

static constexpr std::array<ElementIndex, static_cast<size_t>(ElementIndex::Count)> DisplayOrder {{
    ElementIndex::Fire,
    ElementIndex::Lightning,
    ElementIndex::Poison,
    ElementIndex::Cold,
    ElementIndex::Magic,
    ElementIndex::Physical,
}};

static constexpr std::array<uint8_t, 19> ExpectedResolverEntryBytes {
    0x48, 0x89, 0x6C, 0x24, 0x10,
    0x48, 0x89, 0x74, 0x24, 0x18,
    0x57,
    0x41, 0x56,
    0x41, 0x57,
    0x48, 0x83, 0xEC, 0x50,
};

static constexpr std::array<uint8_t, 9> ExpectedUiEntryBytes {
    0x80, 0xBD, 0xF0, 0x02, 0x00, 0x00, 0x00,
    0x74, 0x1A,
};

static constexpr std::array<uint8_t, 10> ExpectedImmunityOrderEntryBytes {
    0x45, 0x33, 0xFF,
    0x48, 0x8D, 0xB5, 0x34, 0x02, 0x00, 0x00,
};

static constexpr std::array<uint8_t, 20> ExpectedBranchEntryBytes {
    0x44, 0x88, 0x4C, 0x24, 0x20,
    0x4C, 0x89, 0x44, 0x24, 0x18,
    0x48, 0x89, 0x54, 0x24, 0x10,
    0x48, 0x89, 0x4C, 0x24, 0x08,
};

struct DamageDescriptor {
    uint32_t* damagePtr;      // +0x00
    int32_t targetResStat;    // +0x08
    int32_t capOrPairStat;    // +0x0C
    int32_t modifierStatB;    // +0x10
    int32_t modifierStatA;    // +0x14
    uint64_t unknown18;       // +0x18
    int32_t damageIndex;      // +0x20
    int32_t enabledFlag;      // +0x24
    uint64_t unknown28;       // +0x28
    void* labelOrMeta;        // +0x30
    uint8_t byteFlag38;       // +0x38
};

static_assert(sizeof(DamageDescriptor) <= 0x40);

struct TargetKey {
    uint32_t targetType;
    uint32_t unitId08;
    uint32_t classId;
};

struct ElementCacheSlot {
    int resolverPercent;
    uint32_t damageBefore;
    uint32_t damageAfter;
    int32_t targetResStat;
    int32_t damageIndex;
    DWORD lastSeenTick;
    bool nonzeroDamage;
    bool valid;
};

struct CacheEntry {
    TargetKey key;
    uint32_t targetMode;
    uintptr_t lastResolverUnit;
    std::array<ElementCacheSlot, static_cast<size_t>(ElementIndex::Count)> elements;
    bool valid;
};

struct UnitDebugFields {
    uint32_t type;
    uint32_t classId;
    uint32_t unitId08;
    uint32_t mode;
    uintptr_t ptr10;
    uintptr_t statOwner88;
};

struct TargetSnapshot {
    TargetKey key;
    uint32_t targetMode;
    uintptr_t targetUnit;
    uintptr_t targetPtr10;
    uintptr_t targetStatOwner88;
    uintptr_t localPlayerUnit;
    uintptr_t localPlayerStatOwner88;
    DWORD lastSeenTick;
    std::array<int, static_cast<size_t>(ElementIndex::Count)> rawValues;
    std::array<int, static_cast<size_t>(ElementIndex::Count)> localSunderBuckets;
    uint32_t rawMask;
    uint32_t localSunderMask;
    bool valid;
};

struct ResistanceRowWidgets {
    uintptr_t normal;
    uintptr_t compact;
    uintptr_t fallback;
    const char* normalLookup;
    const char* compactLookup;
    const char* fallbackLookup;
};

struct BranchTextState {
    uintptr_t nameWidget;
    uintptr_t uniqueWidget;
    uintptr_t additionalWidget;
    uint64_t nameTextLen;
    uint64_t uniqueTextLen;
    uint64_t additionalTextLen;
    bool nameLenValid;
    bool uniqueLenValid;
    bool additionalLenValid;
};

using ApplyResistanceFn = uint32_t(__fastcall *)(void* resolverContext, DamageDescriptor* descriptor, int flag);
using ResolveResistancePercentFn = int(__fastcall *)(void* resolverContext, DamageDescriptor* descriptor);
using ApplyInlineColorCodeFn = void(__fastcall *)(char* text, char colorId);
using StatGetterFn = int(__fastcall *)(uintptr_t statOwner, uint32_t encodedStat, uintptr_t statRecord);
using GetUnitByIdFn = uintptr_t(__fastcall *)(uint32_t unitId);
using UpdateMonsterHealthPanelBranchFn = void(__fastcall *)(uintptr_t panel, uintptr_t payload, uintptr_t branch, char targetAttached);
using FindChildWidgetFn = uintptr_t(__fastcall *)(uintptr_t parent, const char* name);
using CommitTextFn = uint64_t(__fastcall *)(uintptr_t textField, const char* text, uint64_t length);
using RefreshWidgetFn = void(__fastcall *)(uintptr_t widget);

static uintptr_t g_exeBase = 0;
static bool g_lockReady = false;
static CRITICAL_SECTION g_lock {};

static uint8_t* g_resolverPatchSite = nullptr;
static uint8_t* g_resolverRelay = nullptr;
static uint8_t* g_resolverTrampoline = nullptr;
static std::array<uint8_t, ExpectedResolverEntryBytes.size()> g_resolverOriginal {};
static bool g_resolverPatchApplied = false;
static ApplyResistanceFn g_originalApply = nullptr;

static uint8_t* g_uiPatchSite = nullptr;
static uint8_t* g_uiRelay = nullptr;
static uint8_t* g_uiTrampoline = nullptr;
static std::array<uint8_t, ExpectedUiEntryBytes.size()> g_uiOriginal {};
static bool g_uiPatchApplied = false;

static uint8_t* g_immunityOrderPatchSite = nullptr;
static uint8_t* g_immunityOrderRelay = nullptr;
static std::array<uint8_t, ExpectedImmunityOrderEntryBytes.size()> g_immunityOrderOriginal {};
static bool g_immunityOrderPatchApplied = false;

static uint8_t* g_branchPatchSite = nullptr;
static uint8_t* g_branchRelay = nullptr;
static uint8_t* g_branchTrampoline = nullptr;
static std::array<uint8_t, ExpectedBranchEntryBytes.size()> g_branchOriginal {};
static bool g_branchPatchApplied = false;
static UpdateMonsterHealthPanelBranchFn g_originalBranchUpdate = nullptr;

static std::array<CacheEntry, 32> g_cache {};
static size_t g_cacheNext = 0;
static TargetSnapshot g_currentTarget {};

static constexpr D2RLoaderPluginInfo PluginInfo {
    .apiVersion = D2RLOADER_PLUGIN_API_VERSION,
    .id         = "resistdisplay",
    .name       = "resistdisplay",
    .version    = "1.0.0",
    .author     = "yinyin333333",
    .flags      = D2RLoaderPluginFlag_None,
};

#define LogLine(...) ((void)0)

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

template <typename T>
static bool SafeReadValue(uintptr_t address, T* out) noexcept {
    return SafeRead(reinterpret_cast<const void*>(address), out, sizeof(T));
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

extern "C" void __fastcall ReorderImmunityDescriptorTable(uintptr_t frameBase) noexcept {
    if (!frameBase) {
        return;
    }

    struct ImmunityDescriptor {
        uint8_t bytes[0x20];
    };

    static constexpr size_t Count = 6;
    static constexpr std::array<size_t, Count> DesiredOrder {{
        2, // Fire
        3, // Lightning
        5, // Poison
        4, // Cold
        1, // Magic
        0, // Physical
    }};

    ImmunityDescriptor oldTable[Count] {};
    ImmunityDescriptor newTable[Count] {};
    auto* table = reinterpret_cast<ImmunityDescriptor*>(frameBase + 0x230);

    __try {
        std::memcpy(oldTable, table, sizeof(oldTable));
        for (size_t i = 0; i < Count; ++i) {
            newTable[i] = oldTable[DesiredOrder[i]];
        }
        std::memcpy(table, newTable, sizeof(newTable));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
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

static void EmitMovRaxImm64(std::vector<uint8_t>& code, uintptr_t value) {
    code.push_back(0x48);
    code.push_back(0xB8);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    code.insert(code.end(), bytes, bytes + sizeof(value));
}

static void EmitMovRcxImm64(std::vector<uint8_t>& code, uintptr_t value) {
    code.push_back(0x48);
    code.push_back(0xB9);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    code.insert(code.end(), bytes, bytes + sizeof(value));
}

static void EmitJeRel32(std::vector<uint8_t>& code, uintptr_t instructionAddress, uintptr_t targetAddress) {
    code.push_back(0x0F);
    code.push_back(0x84);
    const int64_t delta64 = static_cast<int64_t>(targetAddress) - static_cast<int64_t>(instructionAddress + 6);
    const int32_t delta = static_cast<int32_t>(delta64);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&delta);
    code.insert(code.end(), bytes, bytes + sizeof(delta));
}

static uintptr_t ContextUnit(void* resolverContext, size_t pointerIndex) noexcept {
    uintptr_t value = 0;
    SafeReadValue<uintptr_t>(reinterpret_cast<uintptr_t>(resolverContext) + pointerIndex * sizeof(uintptr_t), &value);
    return value;
}

static bool ReadMonsterFields(uintptr_t unit, uint32_t* type, uint32_t* classId, uint32_t* mode) noexcept {
    uint32_t unitType = 0;
    uint32_t unitClassId = 0;
    uint32_t unitMode = 0;
    if (!unit ||
        !SafeReadValue<uint32_t>(unit + 0x00, &unitType) ||
        !SafeReadValue<uint32_t>(unit + 0x04, &unitClassId) ||
        !SafeReadValue<uint32_t>(unit + 0x0C, &unitMode)) {
        return false;
    }

    if (type) {
        *type = unitType;
    }
    if (classId) {
        *classId = unitClassId;
    }
    if (mode) {
        *mode = unitMode;
    }
    return unitType == TargetUnitTypeMonster;
}

static UnitDebugFields ReadUnitDebugFields(uintptr_t unit) noexcept {
    UnitDebugFields fields {};
    if (!unit) {
        return fields;
    }
    SafeReadValue<uint32_t>(unit + 0x00, &fields.type);
    SafeReadValue<uint32_t>(unit + 0x04, &fields.classId);
    SafeReadValue<uint32_t>(unit + 0x08, &fields.unitId08);
    SafeReadValue<uint32_t>(unit + 0x0C, &fields.mode);
    SafeReadValue<uintptr_t>(unit + 0x10, &fields.ptr10);
    SafeReadValue<uintptr_t>(unit + 0x88, &fields.statOwner88);
    return fields;
}

static const ElementDef* ElementFromTargetResStat(int32_t targetResStat) noexcept {
    for (const ElementDef& element : Elements) {
        if (element.targetResStat == targetResStat) {
            return &element;
        }
    }
    return nullptr;
}

static const ElementDef& ElementByIndex(ElementIndex index) noexcept {
    return Elements[static_cast<size_t>(index)];
}

static TargetKey MakeTargetKey(const UnitDebugFields& fields) noexcept {
    return TargetKey {
        .targetType = fields.type,
        .unitId08 = fields.unitId08,
        .classId = fields.classId,
    };
}

static bool SameTargetKey(const TargetKey& a, const TargetKey& b) noexcept {
    return a.targetType == b.targetType && a.unitId08 == b.unitId08 && a.classId == b.classId;
}

static bool IsUsableTargetKey(const TargetKey& key) noexcept {
    return key.targetType == TargetUnitTypeMonster && key.unitId08 != 0;
}

static uintptr_t GetExcelContext(uintptr_t unit) noexcept {
    uint8_t index = 0;
    if (!SafeReadValue<uint8_t>(unit + 0x1BD, &index)) {
        return 0;
    }

    uintptr_t context = 0;
    SafeReadValue<uintptr_t>(g_exeBase + RVA_ExcelContexts + static_cast<uintptr_t>(index) * 16, &context);
    return context;
}

static uintptr_t GetStatRecord(uintptr_t unit, uint32_t statId) noexcept {
    const uintptr_t context = GetExcelContext(unit);
    if (!context) {
        return 0;
    }

    uint64_t count = 0;
    uintptr_t rows = 0;
    if (!SafeReadValue<uint64_t>(context + StatCountOffset, &count) ||
        !SafeReadValue<uintptr_t>(context + StatRowsOffset, &rows) ||
        rows == 0 ||
        statId >= count) {
        return 0;
    }

    return rows + static_cast<uintptr_t>(statId) * StatRowSize;
}

static int ReadUnitStat(uintptr_t unit, uint32_t statId, const char** status) noexcept {
    if (status) {
        *status = "ok";
    }
    if (!unit) {
        if (status) {
            *status = "unit=null";
        }
        return 0;
    }

    uintptr_t statOwner = 0;
    if (!SafeReadValue<uintptr_t>(unit + 0x88, &statOwner) || statOwner == 0) {
        if (status) {
            *status = "statOwner=null";
        }
        return 0;
    }

    const uintptr_t statRecord = GetStatRecord(unit, statId);
    if (!statRecord && status) {
        *status = "statRecord=null_r8=0";
    }

    const auto getter = reinterpret_cast<StatGetterFn>(g_exeBase + RVA_StatGetter);
    int value = 0;
    __try {
        value = getter(statOwner, statId << 16, statRecord);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (status) {
            *status = "exception";
        }
        value = 0;
    }
    return value;
}

static uintptr_t GetLocalPlayerUnit(const char** status) noexcept {
    if (status) {
        *status = "ok";
    }

    uint32_t localPlayerUnitId = 0;
    if (!SafeReadValue<uint32_t>(g_exeBase + RVA_LocalPlayerUnitId, &localPlayerUnitId)) {
        if (status) {
            *status = "localPlayerUnitIdReadFailed";
        }
        return 0;
    }

    const auto getUnitById = reinterpret_cast<GetUnitByIdFn>(g_exeBase + RVA_GetUnitById);
    uintptr_t unit = 0;
    __try {
        unit = getUnitById(localPlayerUnitId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (status) {
            *status = "exception";
        }
        return 0;
    }

    if (!unit && status) {
        *status = "unit=null";
    }
    return unit;
}

static int SafeResolveResistancePercent(void* resolverContext, DamageDescriptor* descriptor, const char** status) noexcept {
    if (status) {
        *status = "ok";
    }
    const auto resolver = reinterpret_cast<ResolveResistancePercentFn>(g_exeBase + RVA_ResolveResistancePercent);
    int value = 0;
    __try {
        value = resolver(resolverContext, descriptor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (status) {
            *status = "exception";
        }
        value = 0;
    }
    return value;
}

static void UpdateElementCache(
    uintptr_t targetUnit,
    const UnitDebugFields& fields,
    const ElementDef& element,
    const DamageDescriptor& descriptor,
    int resolverPercent,
    uint32_t before,
    uint32_t after) noexcept {
    const DWORD now = GetTickCount();
    const TargetKey key = MakeTargetKey(fields);
    if (!IsUsableTargetKey(key)) {
        LogLine(
            "cacheSkip targetUnit=0x%p targetType=%u targetClassId=%u targetUnitId08=%u element=%s reason=badTargetKey",
            reinterpret_cast<void*>(targetUnit),
            fields.type,
            fields.classId,
            fields.unitId08,
            element.name);
        return;
    }

    if (g_lockReady) {
        EnterCriticalSection(&g_lock);
    }

    CacheEntry* slot = nullptr;
    for (CacheEntry& entry : g_cache) {
        if (entry.valid && SameTargetKey(entry.key, key)) {
            slot = &entry;
            break;
        }
    }
    if (!slot) {
        slot = &g_cache[g_cacheNext % g_cache.size()];
        ++g_cacheNext;
        *slot = CacheEntry {};
        slot->key = key;
        slot->valid = true;
    }

    slot->key = key;
    slot->targetMode = fields.mode;
    slot->lastResolverUnit = targetUnit;
    slot->valid = true;

    ElementCacheSlot& elementSlot = slot->elements[static_cast<size_t>(element.index)];
    const bool shouldReplace = !elementSlot.valid || before > 0 || !elementSlot.nonzeroDamage;
    if (shouldReplace) {
        elementSlot = ElementCacheSlot {
            .resolverPercent = resolverPercent,
            .damageBefore = before,
            .damageAfter = after,
            .targetResStat = descriptor.targetResStat,
            .damageIndex = descriptor.damageIndex,
            .lastSeenTick = now,
            .nonzeroDamage = before > 0,
            .valid = true,
        };
    }

    if (g_lockReady) {
        LeaveCriticalSection(&g_lock);
    }

    LogLine(
        "cacheUpdate targetKey=(type=%u,unitId08=%u,classId=%u) replace=%s targetUnit=0x%p targetMode=%u targetPtr10=0x%p targetStatOwner88=0x%p element=%s targetResStat=0x%X resolverPercent=%d damageBefore=%u damageAfter=%u damageIndex=%d damageContext=%s tick=%lu",
        key.targetType,
        key.unitId08,
        key.classId,
        shouldReplace ? "yes" : "no",
        reinterpret_cast<void*>(targetUnit),
        fields.mode,
        reinterpret_cast<void*>(fields.ptr10),
        reinterpret_cast<void*>(fields.statOwner88),
        element.name,
        static_cast<unsigned>(descriptor.targetResStat),
        resolverPercent,
        before,
        after,
        descriptor.damageIndex,
        before > 0 ? "nonzero" : "zero",
        static_cast<unsigned long>(now));
}

static bool LookupCache(const TargetKey& key, CacheEntry* out, uint32_t* validMask) noexcept {
    const DWORD now = GetTickCount();
    bool found = false;
    uint32_t mask = 0;

    if (g_lockReady) {
        EnterCriticalSection(&g_lock);
    }

    for (CacheEntry& entry : g_cache) {
        if (!entry.valid || !SameTargetKey(entry.key, key)) {
            continue;
        }
        for (size_t i = 0; i < entry.elements.size(); ++i) {
            ElementCacheSlot& slot = entry.elements[i];
            if (!slot.valid) {
                continue;
            }
            if (now - slot.lastSeenTick > CacheTtlMs) {
                slot.valid = false;
                continue;
            }
            mask |= (1u << i);
        }
        if (mask != 0 && out) {
            *out = entry;
        }
        found = mask != 0;
        break;
    }

    if (g_lockReady) {
        LeaveCriticalSection(&g_lock);
    }

    if (validMask) {
        *validMask = mask;
    }
    return found;
}

static TargetSnapshot GetCurrentTargetSnapshot() noexcept {
    TargetSnapshot snapshot {};
    if (g_lockReady) {
        EnterCriticalSection(&g_lock);
    }
    snapshot = g_currentTarget;
    if (g_lockReady) {
        LeaveCriticalSection(&g_lock);
    }
    return snapshot;
}

static void SetCurrentTargetSnapshot(const TargetSnapshot& snapshot) noexcept {
    if (g_lockReady) {
        EnterCriticalSection(&g_lock);
    }
    g_currentTarget = snapshot;
    if (g_lockReady) {
        LeaveCriticalSection(&g_lock);
    }
}

static void SanitizeForLog(const char* input, char* output, size_t outputSize) noexcept {
    if (!output || outputSize == 0) {
        return;
    }
    output[0] = '\0';
    if (!input) {
        return;
    }

    size_t out = 0;
    __try {
        for (size_t in = 0; input[in] != '\0' && out + 1 < outputSize; ++in) {
            const unsigned char ch = static_cast<unsigned char>(input[in]);
            const char* replacement = nullptr;
            if (ch == '\n') {
                replacement = "\\n";
            } else if (ch == '\r') {
                replacement = "\\r";
            } else if (ch < 0x20 || ch == 0x7f) {
                replacement = ".";
            }

            if (replacement) {
                for (size_t i = 0; replacement[i] != '\0' && out + 1 < outputSize; ++i) {
                    output[out++] = replacement[i];
                }
            } else {
                output[out++] = static_cast<char>(ch);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output[0] = '\0';
        return;
    }
    output[out] = '\0';
}

static bool SafeApplyInlineColorCode(char* text, uint8_t colorId) noexcept {
    if (!text || text[0] == '\0' || !g_exeBase) {
        return false;
    }

    const auto applyColor = reinterpret_cast<ApplyInlineColorCodeFn>(g_exeBase + RVA_ApplyInlineColorCode);
    __try {
        applyColor(text, static_cast<char>(colorId));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static uintptr_t FindDirectChildWidget(uintptr_t parent, const char* name) noexcept {
    if (!parent || !name || !g_exeBase) {
        return 0;
    }

    const auto findChild = reinterpret_cast<FindChildWidgetFn>(g_exeBase + RVA_FindDirectChildWidget);
    uintptr_t widget = 0;
    __try {
        widget = findChild(parent, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        widget = 0;
    }
    return widget;
}

static uintptr_t FindRecursiveChildWidget(uintptr_t parent, const char* name) noexcept {
    if (!parent || !name || !g_exeBase) {
        return 0;
    }

    const auto findChild = reinterpret_cast<FindChildWidgetFn>(g_exeBase + RVA_FindRecursiveChildWidget);
    uintptr_t widget = 0;
    __try {
        widget = findChild(parent, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        widget = 0;
    }
    return widget;
}

static bool SetTextBoxWidgetText(uintptr_t widget, const char* text) noexcept {
    if (!widget || !text || !g_exeBase) {
        return false;
    }

    const auto commitText = reinterpret_cast<CommitTextFn>(g_exeBase + RVA_CommitText);
    const auto refreshWidget = reinterpret_cast<RefreshWidgetFn>(g_exeBase + RVA_RefreshWidget);
    const uint64_t length = static_cast<uint64_t>(std::strlen(text));

    __try {
        commitText(widget + 0x88, text, length);
        refreshWidget(widget);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ReadTextWidgetLength(uintptr_t widget, uint64_t* lengthOut) noexcept {
    if (lengthOut) {
        *lengthOut = 0;
    }
    if (!widget) {
        return false;
    }

    uint64_t length = 0;
    if (!SafeReadValue<uint64_t>(widget + 0x90, &length)) {
        return false;
    }
    if (lengthOut) {
        *lengthOut = length;
    }
    return true;
}

static uintptr_t FindWidgetDirectThenRecursive(uintptr_t branch, const char* name, const char** lookupMode) noexcept {
    if (lookupMode) {
        *lookupMode = "notFound";
    }
    const uintptr_t directWidget = FindDirectChildWidget(branch, name);
    const uintptr_t recursiveWidget = directWidget ? directWidget : FindRecursiveChildWidget(branch, name);
    if (directWidget && lookupMode) {
        *lookupMode = "direct";
    } else if (recursiveWidget && lookupMode) {
        *lookupMode = "recursive";
    }
    return directWidget ? directWidget : recursiveWidget;
}

static ResistanceRowWidgets FindResistanceRowWidgets(uintptr_t branch) noexcept {
    ResistanceRowWidgets widgets {};
    widgets.normalLookup = "notFound";
    widgets.compactLookup = "notFound";
    widgets.fallbackLookup = "notFound";
    widgets.normal = FindWidgetDirectThenRecursive(branch, "ResistanceRowNormal", &widgets.normalLookup);
    widgets.compact = FindWidgetDirectThenRecursive(branch, "ResistanceRowCompact", &widgets.compactLookup);
    widgets.fallback = FindWidgetDirectThenRecursive(branch, "ResistanceRow", &widgets.fallbackLookup);
    return widgets;
}

static BranchTextState ReadBranchTextState(uintptr_t branch) noexcept {
    BranchTextState state {};
    state.nameWidget = FindRecursiveChildWidget(branch, "Name");
    state.uniqueWidget = FindDirectChildWidget(branch, "Unique");
    state.additionalWidget = FindDirectChildWidget(branch, "Additional");
    state.nameLenValid = ReadTextWidgetLength(state.nameWidget, &state.nameTextLen);
    state.uniqueLenValid = ReadTextWidgetLength(state.uniqueWidget, &state.uniqueTextLen);
    state.additionalLenValid = ReadTextWidgetLength(state.additionalWidget, &state.additionalTextLen);
    return state;
}

static bool ClearRowWidget(uintptr_t widget, bool* clearedOut) noexcept {
    if (clearedOut) {
        *clearedOut = false;
    }
    if (!widget) {
        return false;
    }
    const bool cleared = SetTextBoxWidgetText(widget, "");
    if (clearedOut) {
        *clearedOut = cleared;
    }
    return true;
}

static bool BuildResistanceRow(
    const TargetSnapshot& snapshot,
    const CacheEntry* entry,
    uint32_t cacheValidMask,
    char* out,
    size_t outSize,
    uint32_t* validMaskOut,
    uint32_t* resolverMaskOut,
    uint32_t* sunderFallbackMaskOut,
    uint32_t* rawMaskOut) noexcept {
    if (!out || outSize == 0) {
        return false;
    }
    out[0] = '\0';
    if (validMaskOut) {
        *validMaskOut = 0;
    }
    if (resolverMaskOut) {
        *resolverMaskOut = 0;
    }
    if (sunderFallbackMaskOut) {
        *sunderFallbackMaskOut = 0;
    }
    if (rawMaskOut) {
        *rawMaskOut = 0;
    }

    out[0] = '\0';
    size_t used = std::strlen(out);

    for (ElementIndex displayIndex : DisplayOrder) {
        const ElementDef& element = ElementByIndex(displayIndex);
        const size_t index = static_cast<size_t>(element.index);
        char fragment[96] {};
        const uint32_t bit = 1u << index;
        const bool resolverCacheAvailable = entry && (cacheValidMask & bit) != 0 && entry->elements[index].valid;
        const bool rawAvailable = (snapshot.rawMask & bit) != 0;
        const bool localSunderAvailable = (snapshot.localSunderMask & bit) != 0;
        const int rawTargetRes = rawAvailable ? snapshot.rawValues[index] : 0;
        const int localSunderBucket = localSunderAvailable ? snapshot.localSunderBuckets[index] : 0;
        const int resolverPercent = resolverCacheAvailable ? entry->elements[index].resolverPercent : 0;
        const bool sunderHoverFallback =
            !resolverCacheAvailable &&
            rawAvailable &&
            localSunderAvailable &&
            rawTargetRes >= 100 &&
            localSunderBucket >= SunderBucketThreshold;

        const char* displaySource = "missing";
        int displayValue = 0;
        if (resolverCacheAvailable) {
            displaySource = "resolverCache";
            displayValue = resolverPercent;
            std::snprintf(fragment, sizeof(fragment), "%s%d", used > 0 ? " " : "", displayValue);
            if (validMaskOut) {
                *validMaskOut |= bit;
            }
            if (resolverMaskOut) {
                *resolverMaskOut |= bit;
            }
        } else if (sunderHoverFallback) {
            displaySource = "sunderHoverFallback95";
            displayValue = SunderHoverFallbackResistance;
            std::snprintf(fragment, sizeof(fragment), "%s%d", used > 0 ? " " : "", displayValue);
            if (validMaskOut) {
                *validMaskOut |= bit;
            }
            if (sunderFallbackMaskOut) {
                *sunderFallbackMaskOut |= bit;
            }
        } else if (rawAvailable) {
            displaySource = "rawFallback";
            displayValue = rawTargetRes;
            std::snprintf(fragment, sizeof(fragment), "%s%d", used > 0 ? " " : "", displayValue);
            if (validMaskOut) {
                *validMaskOut |= bit;
            }
            if (rawMaskOut) {
                *rawMaskOut |= bit;
            }
        } else {
            std::snprintf(fragment, sizeof(fragment), "%s?", used > 0 ? " " : "");
        }

        LogLine(
            "resistanceRowElement presentationTarget=ResistanceRowNative element=%s rawTargetRes=%d localSunderBucket=%d resolverCacheAvailable=%s resolverPercent=%d displaySource=%s displayValue=%d targetKey=(type=%u,unitId08=%u,classId=%u)",
            element.name,
            rawTargetRes,
            localSunderBucket,
            resolverCacheAvailable ? "yes" : "no",
            resolverPercent,
            displaySource,
            displayValue,
            snapshot.key.targetType,
            snapshot.key.unitId08,
            snapshot.key.classId);

        const bool colorApplied = SafeApplyInlineColorCode(fragment, element.colorId);

        const size_t fragmentLen = std::strlen(fragment);
        if (used + fragmentLen + 1 >= outSize) {
            out[0] = '\0';
            return false;
        }
        std::memcpy(out + used, fragment, fragmentLen + 1);
        used += fragmentLen;
        if (!colorApplied) {
            LogLine("colorCodeApply failed element=%s colorId=%u", element.name, static_cast<unsigned>(element.colorId));
        }
    }
    return used > 0;
}

static void UpdateResistanceRow(uintptr_t payload, uintptr_t branch, char targetAttached) noexcept {
    const ResistanceRowWidgets widgets = FindResistanceRowWidgets(branch);
    const bool anyRowWidget = widgets.normal || widgets.compact || widgets.fallback;
    if (!anyRowWidget) {
        LogLine(
            "resistanceRowSkip presentationTarget=ResistanceRowNative bindingSucceeded=no activeTarget=no useLayout=none normalRowUpdated=no compactRowUpdated=no normalRowCleared=no compactRowCleared=no additionalBufferModified=no normalLookup=%s compactLookup=%s fallbackLookup=%s branch=0x%p targetAttached=%u",
            widgets.normalLookup,
            widgets.compactLookup,
            widgets.fallbackLookup,
            reinterpret_cast<void*>(branch),
            static_cast<unsigned>(targetAttached != 0));
        return;
    }

    uint64_t payloadInfoHash = 0;
    const bool payloadHashOk = payload && SafeReadValue<uint64_t>(payload + 0x08, &payloadInfoHash);
    const bool clearPayload = payloadHashOk && payloadInfoHash == MonsterHealthClearInfoHash;

    const BranchTextState textState = ReadBranchTextState(branch);
    const DWORD now = GetTickCount();
    const TargetSnapshot snapshot = GetCurrentTargetSnapshot();
    const DWORD ageMs = snapshot.valid ? now - snapshot.lastSeenTick : 0xffffffffu;
    const bool nameEmpty = textState.nameLenValid && textState.nameTextLen == 0;
    const bool observerMissing = !snapshot.valid || !IsUsableTargetKey(snapshot.key);
    const bool observerStale = snapshot.valid && ageMs > ObserverFreshMs;
    const bool ttlExpired = snapshot.valid && ageMs > TtlFallbackMs;

    const char* clearReason = nullptr;
    if (clearPayload) {
        clearReason = "ClearInfo";
    } else if (nameEmpty) {
        clearReason = "nameEmpty";
    } else if (observerMissing) {
        clearReason = "noActiveTarget";
    } else if (observerStale) {
        clearReason = ttlExpired ? "ttlFallback" : "staleObserver";
    }

    const bool activeTarget = clearReason == nullptr;

    if (!activeTarget) {
        bool normalCleared = false;
        bool compactCleared = false;
        bool fallbackCleared = false;
        ClearRowWidget(widgets.normal, &normalCleared);
        ClearRowWidget(widgets.compact, &compactCleared);
        ClearRowWidget(widgets.fallback, &fallbackCleared);
        LogLine(
            "resistanceRowClear presentationTarget=ResistanceRowNative activeTarget=no observerFreshMs=%lu ttlMs=%lu lastTargetSeenAgeMs=%lu nameTextLen=%llu uniqueTextLen=%llu additionalTextLen=%llu useLayout=none normalRowUpdated=no compactRowUpdated=no fallbackRowUpdated=no normalRowCleared=%s compactRowCleared=%s fallbackRowCleared=%s clearReason=%s additionalBufferModified=no normalLookup=%s compactLookup=%s fallbackLookup=%s branch=0x%p targetAttached=%u payloadInfoHash=0x%llX targetKey=(type=%u,unitId08=%u,classId=%u)",
            static_cast<unsigned long>(ObserverFreshMs),
            static_cast<unsigned long>(TtlFallbackMs),
            static_cast<unsigned long>(ageMs),
            static_cast<unsigned long long>(textState.nameLenValid ? textState.nameTextLen : 0xffffffffffffffffull),
            static_cast<unsigned long long>(textState.uniqueLenValid ? textState.uniqueTextLen : 0xffffffffffffffffull),
            static_cast<unsigned long long>(textState.additionalLenValid ? textState.additionalTextLen : 0xffffffffffffffffull),
            normalCleared ? "yes" : "no",
            compactCleared ? "yes" : "no",
            fallbackCleared ? "yes" : "no",
            clearReason,
            widgets.normalLookup,
            widgets.compactLookup,
            widgets.fallbackLookup,
            reinterpret_cast<void*>(branch),
            static_cast<unsigned>(targetAttached != 0),
            static_cast<unsigned long long>(payloadHashOk ? payloadInfoHash : 0),
            snapshot.key.targetType,
            snapshot.key.unitId08,
            snapshot.key.classId);
        return;
    }

    CacheEntry entry {};
    uint32_t cacheValidMask = 0;
    const bool cacheHit = LookupCache(snapshot.key, &entry, &cacheValidMask);

    char row[384] {};
    uint32_t validMask = 0;
    uint32_t resolverDisplayMask = 0;
    uint32_t sunderFallbackMask = 0;
    uint32_t rawDisplayMask = 0;
    const bool rowBuilt = BuildResistanceRow(
        snapshot,
        cacheHit ? &entry : nullptr,
        cacheValidMask,
        row,
        sizeof(row),
        &validMask,
        &resolverDisplayMask,
        &sunderFallbackMask,
        &rawDisplayMask);

    if (!rowBuilt) {
        bool normalCleared = false;
        bool compactCleared = false;
        bool fallbackCleared = false;
        ClearRowWidget(widgets.normal, &normalCleared);
        ClearRowWidget(widgets.compact, &compactCleared);
        ClearRowWidget(widgets.fallback, &fallbackCleared);
        LogLine(
            "resistanceRowClear presentationTarget=ResistanceRowNative activeTarget=yes observerFreshMs=%lu ttlMs=%lu lastTargetSeenAgeMs=%lu nameTextLen=%llu uniqueTextLen=%llu additionalTextLen=%llu useLayout=none normalRowUpdated=no compactRowUpdated=no fallbackRowUpdated=no normalRowCleared=%s compactRowCleared=%s fallbackRowCleared=%s clearReason=stringBuildFailed additionalBufferModified=no normalLookup=%s compactLookup=%s fallbackLookup=%s branch=0x%p targetAttached=%u cacheHit=%s validMask=0x%02X resolverDisplayMask=0x%02X sunderFallbackMask=0x%02X rawDisplayMask=0x%02X targetKey=(type=%u,unitId08=%u,classId=%u)",
            static_cast<unsigned long>(ObserverFreshMs),
            static_cast<unsigned long>(TtlFallbackMs),
            static_cast<unsigned long>(ageMs),
            static_cast<unsigned long long>(textState.nameLenValid ? textState.nameTextLen : 0xffffffffffffffffull),
            static_cast<unsigned long long>(textState.uniqueLenValid ? textState.uniqueTextLen : 0xffffffffffffffffull),
            static_cast<unsigned long long>(textState.additionalLenValid ? textState.additionalTextLen : 0xffffffffffffffffull),
            normalCleared ? "yes" : "no",
            compactCleared ? "yes" : "no",
            fallbackCleared ? "yes" : "no",
            widgets.normalLookup,
            widgets.compactLookup,
            widgets.fallbackLookup,
            reinterpret_cast<void*>(branch),
            static_cast<unsigned>(targetAttached != 0),
            cacheHit ? "yes" : "no",
            validMask,
            resolverDisplayMask,
            sunderFallbackMask,
            rawDisplayMask,
            snapshot.key.targetType,
            snapshot.key.unitId08,
            snapshot.key.classId);
        return;
    }

    const bool uniqueLineHasText = textState.uniqueLenValid && textState.uniqueTextLen > 0;
    const bool useNormalLayout = uniqueLineHasText;
    uintptr_t targetRow = useNormalLayout ? widgets.normal : widgets.compact;
    const char* targetLookup = useNormalLayout ? widgets.normalLookup : widgets.compactLookup;
    bool usingFallback = false;
    if (!targetRow) {
        targetRow = widgets.fallback;
        targetLookup = widgets.fallbackLookup;
        usingFallback = targetRow != 0;
    }

    bool normalCleared = false;
    bool compactCleared = false;
    bool fallbackCleared = false;
    bool normalUpdated = false;
    bool compactUpdated = false;
    bool fallbackUpdated = false;

    if (targetRow) {
        const bool updated = SetTextBoxWidgetText(targetRow, row);
        if (usingFallback) {
            fallbackUpdated = updated;
        } else if (useNormalLayout) {
            normalUpdated = updated;
        } else {
            compactUpdated = updated;
        }
    }

    if (!useNormalLayout || usingFallback) {
        ClearRowWidget(widgets.normal, &normalCleared);
    }
    if (useNormalLayout || usingFallback) {
        ClearRowWidget(widgets.compact, &compactCleared);
    }
    if (!usingFallback) {
        ClearRowWidget(widgets.fallback, &fallbackCleared);
    }

    char rowSanitized[768] {};
    SanitizeForLog(row, rowSanitized, sizeof(rowSanitized));
    LogLine(
        "resistanceRowUpdate presentationTarget=ResistanceRowNative activeTarget=yes observerFreshMs=%lu ttlMs=%lu lastTargetSeenAgeMs=%lu nameTextLen=%llu uniqueTextLen=%llu additionalTextLen=%llu useLayout=%s normalRowUpdated=%s compactRowUpdated=%s fallbackRowUpdated=%s normalRowCleared=%s compactRowCleared=%s fallbackRowCleared=%s clearReason=none additionalBufferModified=no targetLookup=%s normalLookup=%s compactLookup=%s fallbackLookup=%s branch=0x%p targetRow=0x%p targetAttached=%u cacheHit=%s validMask=0x%02X resolverDisplayMask=0x%02X sunderFallbackMask=0x%02X rawDisplayMask=0x%02X outputOrder=Fire,Lightning,Poison,Cold,Magic,Physical labels=no targetKey=(type=%u,unitId08=%u,classId=%u) targetUnit=0x%p targetMode=%u targetPtr10=0x%p targetStatOwner88=0x%p localPlayerUnit=0x%p localPlayerStatOwner88=0x%p finalRowString=\"%s\"",
        static_cast<unsigned long>(ObserverFreshMs),
        static_cast<unsigned long>(TtlFallbackMs),
        static_cast<unsigned long>(ageMs),
        static_cast<unsigned long long>(textState.nameLenValid ? textState.nameTextLen : 0xffffffffffffffffull),
        static_cast<unsigned long long>(textState.uniqueLenValid ? textState.uniqueTextLen : 0xffffffffffffffffull),
        static_cast<unsigned long long>(textState.additionalLenValid ? textState.additionalTextLen : 0xffffffffffffffffull),
        useNormalLayout ? "normal" : "compact",
        normalUpdated ? "yes" : "no",
        compactUpdated ? "yes" : "no",
        fallbackUpdated ? "yes" : "no",
        normalCleared ? "yes" : "no",
        compactCleared ? "yes" : "no",
        fallbackCleared ? "yes" : "no",
        targetLookup,
        widgets.normalLookup,
        widgets.compactLookup,
        widgets.fallbackLookup,
        reinterpret_cast<void*>(branch),
        reinterpret_cast<void*>(targetRow),
        static_cast<unsigned>(targetAttached != 0),
        cacheHit ? "yes" : "no",
        validMask,
        resolverDisplayMask,
        sunderFallbackMask,
        rawDisplayMask,
        snapshot.key.targetType,
        snapshot.key.unitId08,
        snapshot.key.classId,
        reinterpret_cast<void*>(snapshot.targetUnit),
        snapshot.targetMode,
        reinterpret_cast<void*>(snapshot.targetPtr10),
        reinterpret_cast<void*>(snapshot.targetStatOwner88),
        reinterpret_cast<void*>(snapshot.localPlayerUnit),
        reinterpret_cast<void*>(snapshot.localPlayerStatOwner88),
        rowSanitized);
}

extern "C" void __fastcall MonsterHealthBranchHook(uintptr_t panel, uintptr_t payload, uintptr_t branch, char targetAttached) noexcept {
    if (g_originalBranchUpdate) {
        __try {
            g_originalBranchUpdate(panel, payload, branch, targetAttached);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogLine("branchHook original exception panel=0x%p payload=0x%p branch=0x%p targetAttached=%u", reinterpret_cast<void*>(panel), reinterpret_cast<void*>(payload), reinterpret_cast<void*>(branch), static_cast<unsigned>(targetAttached != 0));
        }
    }

    UpdateResistanceRow(payload, branch, targetAttached);
}

extern "C" uint32_t __fastcall ApplyResistanceHook(void* resolverContext, DamageDescriptor* descriptor, int flag) noexcept {
    DamageDescriptor beforeDesc {};
    bool descriptorOk = false;
    uint32_t damageBefore = 0;
    if (descriptor) {
        descriptorOk = SafeRead(descriptor, &beforeDesc, sizeof(beforeDesc));
        if (descriptorOk && beforeDesc.damagePtr) {
            SafeReadValue<uint32_t>(reinterpret_cast<uintptr_t>(beforeDesc.damagePtr), &damageBefore);
        }
    }

    const ElementDef* element = descriptorOk ? ElementFromTargetResStat(beforeDesc.targetResStat) : nullptr;
    int resolverPercent = 0;
    const char* resolverStatus = "skipped";
    if (descriptorOk && element) {
        resolverPercent = SafeResolveResistancePercent(resolverContext, descriptor, &resolverStatus);
    }

    uint32_t result = 0;
    if (g_originalApply) {
        __try {
            result = g_originalApply(resolverContext, descriptor, flag);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogLine("resolverHook original exception context=0x%p descriptor=0x%p", resolverContext, descriptor);
            result = 0;
        }
    }

    if (!descriptorOk || !element) {
        return result;
    }

    uint32_t damageAfter = 0;
    if (beforeDesc.damagePtr) {
        SafeReadValue<uint32_t>(reinterpret_cast<uintptr_t>(beforeDesc.damagePtr), &damageAfter);
    }

    const uintptr_t targetUnit = ContextUnit(resolverContext, 3);
    const UnitDebugFields fields = ReadUnitDebugFields(targetUnit);
    if (fields.type != TargetUnitTypeMonster) {
        return result;
    }

    if (std::strcmp(resolverStatus, "ok") == 0) {
        UpdateElementCache(targetUnit, fields, *element, beforeDesc, resolverPercent, damageBefore, damageAfter);
    } else {
        LogLine(
            "cacheSkip targetUnit=0x%p targetType=%u targetClassId=%u targetUnitId08=%u targetMode=%u element=%s resolverPercent=%d resolverStatus=%s damageBefore=%u damageAfter=%u reason=resolverNotOk",
            reinterpret_cast<void*>(targetUnit),
            fields.type,
            fields.classId,
            fields.unitId08,
            fields.mode,
            element->name,
            resolverPercent,
            resolverStatus,
            damageBefore,
            damageAfter);
    }

    return result;
}

extern "C" void __fastcall UiAppendHook(uintptr_t targetUnit, char* /*buffer*/) noexcept {
    const UnitDebugFields fields = ReadUnitDebugFields(targetUnit);
    const TargetKey key = MakeTargetKey(fields);
    if (!IsUsableTargetKey(key)) {
        LogLine(
            "targetObserverSkip presentationTarget=ResistanceRowNative observerOnly=yes additionalBufferModified=no targetUnit=0x%p targetType=%u targetClassId=%u targetUnitId08=%u reason=badTargetKey",
            reinterpret_cast<void*>(targetUnit),
            fields.type,
            fields.classId,
            fields.unitId08);
        return;
    }

    TargetSnapshot snapshot {};
    snapshot.key = key;
    snapshot.targetMode = fields.mode;
    snapshot.targetUnit = targetUnit;
    snapshot.targetPtr10 = fields.ptr10;
    snapshot.targetStatOwner88 = fields.statOwner88;
    snapshot.lastSeenTick = GetTickCount();
    snapshot.valid = true;

    for (const ElementDef& element : Elements) {
        const size_t index = static_cast<size_t>(element.index);
        const char* rawStatus = "ok";
        const int rawValue = ReadUnitStat(targetUnit, static_cast<uint32_t>(element.targetResStat), &rawStatus);
        snapshot.rawValues[index] = rawValue;
        snapshot.rawMask |= (1u << index);
        if (std::strcmp(rawStatus, "ok") != 0 && std::strcmp(rawStatus, "statRecord=null_r8=0") != 0) {
            LogLine("targetObserverRawRead element=%s stat=0x%X status=%s value=%d", element.name, static_cast<unsigned>(element.targetResStat), rawStatus, rawValue);
        }
    }

    const char* localPlayerStatus = "ok";
    snapshot.localPlayerUnit = GetLocalPlayerUnit(&localPlayerStatus);
    if (snapshot.localPlayerUnit) {
        SafeReadValue<uintptr_t>(snapshot.localPlayerUnit + 0x88, &snapshot.localPlayerStatOwner88);
        for (const ElementDef& element : Elements) {
            const size_t index = static_cast<size_t>(element.index);
            const char* localStatus = "ok";
            const int localValue = ReadUnitStat(snapshot.localPlayerUnit, static_cast<uint32_t>(element.localSunderStat), &localStatus);
            snapshot.localSunderBuckets[index] = localValue;
            snapshot.localSunderMask |= (1u << index);
            if (std::strcmp(localStatus, "ok") != 0 && std::strcmp(localStatus, "statRecord=null_r8=0") != 0) {
                LogLine(
                    "targetObserverLocalSunderRead element=%s stat=0x%X status=%s value=%d localPlayerUnit=0x%p",
                    element.name,
                    static_cast<unsigned>(element.localSunderStat),
                    localStatus,
                    localValue,
                    reinterpret_cast<void*>(snapshot.localPlayerUnit));
            }
        }
    } else {
        LogLine("targetObserverLocalPlayerRead status=%s localPlayerUnit=0x%p", localPlayerStatus, reinterpret_cast<void*>(snapshot.localPlayerUnit));
    }

    SetCurrentTargetSnapshot(snapshot);
    LogLine(
        "targetObserver presentationTarget=ResistanceRowNative observerOnly=yes rawFallbackEnabled=yes sunderHoverFallbackEnabled=yes additionalBufferModified=no targetKey=(type=%u,unitId08=%u,classId=%u) targetUnit=0x%p targetMode=%u targetPtr10=0x%p targetStatOwner88=0x%p localPlayerUnit=0x%p localPlayerStatOwner88=0x%p localPlayerStatus=%s rawMask=0x%02X localSunderMask=0x%02X rawFire=%d rawLightning=%d rawPoison=%d rawCold=%d rawMagic=%d rawPhysical=%d localFire0xBD=%d localLightning0xBE=%d localPoison0xBF=%d localCold0xBB=%d localMagic0xC1=%d localPhysical0xC0=%d tick=%lu",
        key.targetType,
        key.unitId08,
        key.classId,
        reinterpret_cast<void*>(targetUnit),
        fields.mode,
        reinterpret_cast<void*>(fields.ptr10),
        reinterpret_cast<void*>(fields.statOwner88),
        reinterpret_cast<void*>(snapshot.localPlayerUnit),
        reinterpret_cast<void*>(snapshot.localPlayerStatOwner88),
        localPlayerStatus,
        snapshot.rawMask,
        snapshot.localSunderMask,
        snapshot.rawValues[static_cast<size_t>(ElementIndex::Fire)],
        snapshot.rawValues[static_cast<size_t>(ElementIndex::Lightning)],
        snapshot.rawValues[static_cast<size_t>(ElementIndex::Poison)],
        snapshot.rawValues[static_cast<size_t>(ElementIndex::Cold)],
        snapshot.rawValues[static_cast<size_t>(ElementIndex::Magic)],
        snapshot.rawValues[static_cast<size_t>(ElementIndex::Physical)],
        snapshot.localSunderBuckets[static_cast<size_t>(ElementIndex::Fire)],
        snapshot.localSunderBuckets[static_cast<size_t>(ElementIndex::Lightning)],
        snapshot.localSunderBuckets[static_cast<size_t>(ElementIndex::Poison)],
        snapshot.localSunderBuckets[static_cast<size_t>(ElementIndex::Cold)],
        snapshot.localSunderBuckets[static_cast<size_t>(ElementIndex::Magic)],
        snapshot.localSunderBuckets[static_cast<size_t>(ElementIndex::Physical)],
        static_cast<unsigned long>(snapshot.lastSeenTick));
}

static bool BuildResolverTrampoline(uint8_t* trampoline) noexcept {
    if (!trampoline) {
        return false;
    }
    std::vector<uint8_t> code;
    code.reserve(64);
    code.insert(code.end(), g_resolverOriginal.begin(), g_resolverOriginal.end());
    EmitJmpRel32(code, reinterpret_cast<uintptr_t>(trampoline) + code.size(), g_exeBase + RVA_ApplyResistanceToDamageDescriptor + g_resolverOriginal.size());
    std::memcpy(trampoline, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), trampoline, code.size());
    g_originalApply = reinterpret_cast<ApplyResistanceFn>(trampoline);
    return true;
}

static bool BuildResolverRelay(uint8_t* relay) noexcept {
    if (!relay) {
        return false;
    }
    std::vector<uint8_t> code;
    code.reserve(32);
    EmitMovRaxImm64(code, reinterpret_cast<uintptr_t>(&ApplyResistanceHook));
    code.push_back(0xFF);
    code.push_back(0xE0);
    std::memcpy(relay, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), relay, code.size());
    return true;
}

static bool BuildUiRelay(uint8_t* relay) noexcept {
    if (!relay) {
        return false;
    }
    std::vector<uint8_t> code;
    code.reserve(128);

    const uint8_t save[] {
        0x9C,
        0x50,
        0x51,
        0x52,
        0x41, 0x50,
        0x41, 0x51,
        0x41, 0x52,
        0x41, 0x53,
        0x48, 0x83, 0xEC, 0x20,
    };
    code.insert(code.end(), save, save + sizeof(save));
    const uint8_t args[] {
        0x4C, 0x89, 0xF1,
        0x48, 0x8D, 0x95, 0xF0, 0x02, 0x00, 0x00,
    };
    code.insert(code.end(), args, args + sizeof(args));
    EmitMovRaxImm64(code, reinterpret_cast<uintptr_t>(&UiAppendHook));
    const uint8_t callRestore[] {
        0xFF, 0xD0,
        0x48, 0x83, 0xC4, 0x20,
        0x41, 0x5B,
        0x41, 0x5A,
        0x41, 0x59,
        0x41, 0x58,
        0x5A,
        0x59,
        0x58,
        0x9D,
    };
    code.insert(code.end(), callRestore, callRestore + sizeof(callRestore));
    const uint8_t originalCmp[] {
        0x80, 0xBD, 0xF0, 0x02, 0x00, 0x00, 0x00,
    };
    code.insert(code.end(), originalCmp, originalCmp + sizeof(originalCmp));
    EmitJeRel32(code, reinterpret_cast<uintptr_t>(relay) + code.size(), g_exeBase + RVA_HpBarTextLengthEmpty);
    EmitJmpRel32(code, reinterpret_cast<uintptr_t>(relay) + code.size(), g_exeBase + RVA_HpBarTextLengthNonEmpty);

    std::memcpy(relay, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), relay, code.size());
    return true;
}

static bool BuildImmunityOrderRelay(uint8_t* relay) noexcept {
    if (!relay) {
        return false;
    }
    std::vector<uint8_t> code;
    code.reserve(128);

    const uint8_t save[] {
        0x9C,
        0x50,
        0x51,
        0x52,
        0x41, 0x50,
        0x41, 0x51,
        0x41, 0x52,
        0x41, 0x53,
        0x48, 0x83, 0xEC, 0x20,
        0x48, 0x89, 0xE9,
    };
    code.insert(code.end(), save, save + sizeof(save));
    EmitMovRaxImm64(code, reinterpret_cast<uintptr_t>(&ReorderImmunityDescriptorTable));
    const uint8_t callRestore[] {
        0xFF, 0xD0,
        0x48, 0x83, 0xC4, 0x20,
        0x41, 0x5B,
        0x41, 0x5A,
        0x41, 0x59,
        0x41, 0x58,
        0x5A,
        0x59,
        0x58,
        0x9D,
    };
    code.insert(code.end(), callRestore, callRestore + sizeof(callRestore));
    code.insert(code.end(), g_immunityOrderOriginal.begin(), g_immunityOrderOriginal.end());
    EmitJmpRel32(code, reinterpret_cast<uintptr_t>(relay) + code.size(), g_exeBase + RVA_ImmunityDescriptorTableReady + g_immunityOrderOriginal.size());

    std::memcpy(relay, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), relay, code.size());
    return true;
}

static bool BuildBranchTrampoline(uint8_t* trampoline) noexcept {
    if (!trampoline) {
        return false;
    }
    std::vector<uint8_t> code;
    code.reserve(64);
    code.insert(code.end(), g_branchOriginal.begin(), g_branchOriginal.end());
    EmitJmpRel32(code, reinterpret_cast<uintptr_t>(trampoline) + code.size(), g_exeBase + RVA_UpdateMonsterHealthPanelBranch + g_branchOriginal.size());
    std::memcpy(trampoline, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), trampoline, code.size());
    g_originalBranchUpdate = reinterpret_cast<UpdateMonsterHealthPanelBranchFn>(trampoline);
    return true;
}

static bool BuildBranchRelay(uint8_t* relay) noexcept {
    if (!relay) {
        return false;
    }
    std::vector<uint8_t> code;
    code.reserve(32);
    EmitMovRaxImm64(code, reinterpret_cast<uintptr_t>(&MonsterHealthBranchHook));
    code.push_back(0xFF);
    code.push_back(0xE0);
    std::memcpy(relay, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), relay, code.size());
    return true;
}

template <size_t N>
static bool VerifyOriginal(uintptr_t rva, uint8_t** site, std::array<uint8_t, N>& original, const std::array<uint8_t, N>& expected) noexcept {
    *site = reinterpret_cast<uint8_t*>(g_exeBase + rva);
    if (!SafeRead(*site, original.data(), original.size())) {
        return false;
    }
    return std::memcmp(original.data(), expected.data(), expected.size()) == 0;
}

static bool InstallPatch(uint8_t* site, uint8_t* relay, size_t patchLength) noexcept {
    const uintptr_t patchSite = reinterpret_cast<uintptr_t>(site);
    const uintptr_t relayAddress = reinterpret_cast<uintptr_t>(relay);
    if (!Rel32Fits(patchSite, relayAddress)) {
        return false;
    }

    std::vector<uint8_t> patch(patchLength, 0x90);
    patch[0] = 0xE9;
    const int64_t delta64 = static_cast<int64_t>(relayAddress) - static_cast<int64_t>(patchSite + 5);
    const int32_t delta = static_cast<int32_t>(delta64);
    std::memcpy(patch.data() + 1, &delta, sizeof(delta));
    return SafeWrite(site, patch.data(), patch.size());
}

static bool InstallResolverHook() noexcept {
    const bool originalOk = VerifyOriginal(RVA_ApplyResistanceToDamageDescriptor, &g_resolverPatchSite, g_resolverOriginal, ExpectedResolverEntryBytes);
    LogLine("resolver original bytes match=%s address=0x%p patchLength=%zu", originalOk ? "yes" : "no", g_resolverPatchSite, g_resolverOriginal.size());
    if (!originalOk) {
        return false;
    }

    g_resolverRelay = AllocateNear(reinterpret_cast<uintptr_t>(g_resolverPatchSite), 4096);
    if (!g_resolverRelay) {
        return false;
    }
    std::memset(g_resolverRelay, 0xCC, 4096);
    g_resolverTrampoline = g_resolverRelay + 0x80;

    if (!BuildResolverRelay(g_resolverRelay) || !BuildResolverTrampoline(g_resolverTrampoline)) {
        return false;
    }
    g_resolverPatchApplied = InstallPatch(g_resolverPatchSite, g_resolverRelay, g_resolverOriginal.size());
    return g_resolverPatchApplied;
}

static bool InstallUiHook() noexcept {
    const bool originalOk = VerifyOriginal(RVA_HpBarTextLengthCalc, &g_uiPatchSite, g_uiOriginal, ExpectedUiEntryBytes);
    LogLine("ui original bytes match=%s address=0x%p patchLength=%zu", originalOk ? "yes" : "no", g_uiPatchSite, g_uiOriginal.size());
    if (!originalOk) {
        return false;
    }

    g_uiRelay = AllocateNear(reinterpret_cast<uintptr_t>(g_uiPatchSite), 4096);
    if (!g_uiRelay) {
        return false;
    }
    std::memset(g_uiRelay, 0xCC, 4096);
    g_uiTrampoline = g_uiRelay;

    if (!BuildUiRelay(g_uiRelay)) {
        return false;
    }
    g_uiPatchApplied = InstallPatch(g_uiPatchSite, g_uiRelay, g_uiOriginal.size());
    return g_uiPatchApplied;
}

static bool InstallImmunityOrderHook() noexcept {
    const bool originalOk = VerifyOriginal(RVA_ImmunityDescriptorTableReady, &g_immunityOrderPatchSite, g_immunityOrderOriginal, ExpectedImmunityOrderEntryBytes);
    LogLine("immunity order original bytes match=%s address=0x%p patchLength=%zu", originalOk ? "yes" : "no", g_immunityOrderPatchSite, g_immunityOrderOriginal.size());
    if (!originalOk) {
        return false;
    }

    g_immunityOrderRelay = AllocateNear(reinterpret_cast<uintptr_t>(g_immunityOrderPatchSite), 4096);
    if (!g_immunityOrderRelay) {
        return false;
    }
    std::memset(g_immunityOrderRelay, 0xCC, 4096);

    if (!BuildImmunityOrderRelay(g_immunityOrderRelay)) {
        return false;
    }
    g_immunityOrderPatchApplied = InstallPatch(g_immunityOrderPatchSite, g_immunityOrderRelay, g_immunityOrderOriginal.size());
    return g_immunityOrderPatchApplied;
}

static bool InstallBranchHook() noexcept {
    const bool originalOk = VerifyOriginal(RVA_UpdateMonsterHealthPanelBranch, &g_branchPatchSite, g_branchOriginal, ExpectedBranchEntryBytes);
    LogLine("branch original bytes match=%s address=0x%p patchLength=%zu", originalOk ? "yes" : "no", g_branchPatchSite, g_branchOriginal.size());
    if (!originalOk) {
        return false;
    }

    g_branchRelay = AllocateNear(reinterpret_cast<uintptr_t>(g_branchPatchSite), 4096);
    if (!g_branchRelay) {
        return false;
    }
    std::memset(g_branchRelay, 0xCC, 4096);
    g_branchTrampoline = g_branchRelay + 0x80;

    if (!BuildBranchRelay(g_branchRelay) || !BuildBranchTrampoline(g_branchTrampoline)) {
        return false;
    }
    g_branchPatchApplied = InstallPatch(g_branchPatchSite, g_branchRelay, g_branchOriginal.size());
    return g_branchPatchApplied;
}

static bool RestoreHooks() noexcept {
    bool ok = true;
    if (g_uiPatchApplied && g_uiPatchSite) {
        ok = SafeWrite(g_uiPatchSite, g_uiOriginal.data(), g_uiOriginal.size()) && ok;
    }
    if (g_immunityOrderPatchApplied && g_immunityOrderPatchSite) {
        ok = SafeWrite(g_immunityOrderPatchSite, g_immunityOrderOriginal.data(), g_immunityOrderOriginal.size()) && ok;
    }
    if (g_resolverPatchApplied && g_resolverPatchSite) {
        ok = SafeWrite(g_resolverPatchSite, g_resolverOriginal.data(), g_resolverOriginal.size()) && ok;
    }
    if (g_branchPatchApplied && g_branchPatchSite) {
        ok = SafeWrite(g_branchPatchSite, g_branchOriginal.data(), g_branchOriginal.size()) && ok;
    }
    if (g_uiRelay) {
        VirtualFree(g_uiRelay, 0, MEM_RELEASE);
    }
    if (g_resolverRelay) {
        VirtualFree(g_resolverRelay, 0, MEM_RELEASE);
    }
    if (g_immunityOrderRelay) {
        VirtualFree(g_immunityOrderRelay, 0, MEM_RELEASE);
    }
    if (g_branchRelay) {
        VirtualFree(g_branchRelay, 0, MEM_RELEASE);
    }
    g_uiRelay = nullptr;
    g_uiTrampoline = nullptr;
    g_resolverRelay = nullptr;
    g_resolverTrampoline = nullptr;
    g_immunityOrderRelay = nullptr;
    g_branchRelay = nullptr;
    g_branchTrampoline = nullptr;
    g_originalApply = nullptr;
    g_originalBranchUpdate = nullptr;
    g_uiPatchApplied = false;
    g_immunityOrderPatchApplied = false;
    g_resolverPatchApplied = false;
    g_branchPatchApplied = false;
    return ok;
}

} // namespace

D2RLOADER_PLUGIN_EXPORT const D2RLoaderPluginInfo* __cdecl D2RLoaderGetPluginInfo() noexcept {
    return &PluginInfo;
}

D2RLOADER_PLUGIN_EXPORT bool __cdecl D2RLoaderLoadHooks(const D2RLoaderPluginContext* ctx) noexcept {
    g_exeBase = 0;
    g_cache = {};
    g_cacheNext = 0;
    g_currentTarget = {};

    InitializeCriticalSection(&g_lock);
    g_lockReady = true;

    if (!ctx || ctx->apiVersion < D2RLOADER_PLUGIN_API_VERSION) {
        LogLine("plugin load failed: invalid D2RLoader context");
        return false;
    }

    g_exeBase = ctx->exeBase;
    LogLine(
        "plugin loaded activeMod=%s modDirectory=%ls exeBase=0x%p presentationTarget=ResistanceRowNative resolverHookEnabled=yes targetObserverRawFallbackEnabled=yes resistanceRowNativeEnabled=yes sunderHoverFallbackEnabled=yes resolverHookRVA=0x%llX resolverRVA=0x%llX uiObserverHookRVA=0x%llX branchHookRVA=0x%llX getUnitByIdRVA=0x%llX localPlayerUnitIdRVA=0x%llX observerFreshMs=%lu ttlFallbackMs=%lu directLookupRVA=0x%llX recursiveLookupRVA=0x%llX commitTextRVA=0x%llX refreshWidgetRVA=0x%llX additionalBufferModified=no outputOrder=Fire,Lightning,Poison,Cold,Magic,Physical labels=no logPath=%ls",
        ctx->activeMod ? ctx->activeMod : "",
        ctx->modDirectory ? ctx->modDirectory : L"",
        reinterpret_cast<void*>(g_exeBase),
        static_cast<unsigned long long>(RVA_ApplyResistanceToDamageDescriptor),
        static_cast<unsigned long long>(RVA_ResolveResistancePercent),
        static_cast<unsigned long long>(RVA_HpBarTextLengthCalc),
        static_cast<unsigned long long>(RVA_UpdateMonsterHealthPanelBranch),
        static_cast<unsigned long long>(RVA_GetUnitById),
        static_cast<unsigned long long>(RVA_LocalPlayerUnitId),
        static_cast<unsigned long>(ObserverFreshMs),
        static_cast<unsigned long>(TtlFallbackMs),
        static_cast<unsigned long long>(RVA_FindDirectChildWidget),
        static_cast<unsigned long long>(RVA_FindRecursiveChildWidget),
        static_cast<unsigned long long>(RVA_CommitText),
        static_cast<unsigned long long>(RVA_RefreshWidget),
        LogPath);

    const bool resolverInstalled = InstallResolverHook();
    const bool uiInstalled = resolverInstalled && InstallUiHook();
    const bool immunityOrderInstalled = uiInstalled && InstallImmunityOrderHook();
    const bool branchInstalled = immunityOrderInstalled && InstallBranchHook();
    LogLine(
        "hook install presentationTarget=ResistanceRowNative resolverHookEnabled=%s targetObserverRawFallbackEnabled=%s resistanceRowNativeEnabled=%s sunderHoverFallbackEnabled=yes resolver=%s resolverRelay=0x%p resolverTrampoline=0x%p uiObserver=%s uiRelay=0x%p branch=%s branchRelay=0x%p branchTrampoline=0x%p additionalBufferModified=no",
        resolverInstalled ? "yes" : "no",
        uiInstalled ? "yes" : "no",
        branchInstalled ? "yes" : "no",
        resolverInstalled ? "yes" : "no",
        g_resolverRelay,
        g_resolverTrampoline,
        uiInstalled ? "yes" : "no",
        g_uiRelay,
        branchInstalled ? "yes" : "no",
        g_branchRelay,
        g_branchTrampoline);

    if (!resolverInstalled || !uiInstalled || !immunityOrderInstalled || !branchInstalled) {
        RestoreHooks();
        if (g_lockReady) {
            g_lockReady = false;
            DeleteCriticalSection(&g_lock);
        }
        return false;
    }

    return true;
}

D2RLOADER_PLUGIN_EXPORT void __cdecl D2RLoaderUnload() noexcept {
    RestoreHooks();

    if (g_lockReady) {
        g_lockReady = false;
        DeleteCriticalSection(&g_lock);
    }

    g_exeBase = 0;
}
