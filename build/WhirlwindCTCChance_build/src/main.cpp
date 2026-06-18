#include "extern/plugin.h"

#include <windows.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#ifndef WWCTC_DEBUG_LOG
#define WWCTC_DEBUG_LOG 0
#endif

namespace {

static constexpr uintptr_t RVA_PatchSite = 0x40AA35;
static constexpr uintptr_t RVA_AllowTarget = 0x40AA3B;
static constexpr uintptr_t RVA_SuppressTarget = 0x40AB7D;
static constexpr int WhirlwindSkillId = 151;
static constexpr uint32_t MaxTrackedSkillId = 4096;
static constexpr size_t InlineStateOffset = 0x1000;

static constexpr std::array<uint8_t, 6> ExpectedBytes {
    0x0F, 0x85, 0x42, 0x01, 0x00, 0x00
};

static constexpr std::array<uint8_t, 6> NopPatch {
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

static constexpr std::array<uint8_t, 6> DirectSuppressPatch {
    0xE9, 0x43, 0x01, 0x00, 0x00, 0x90
};

static constexpr const wchar_t* RelativeWwCtcPath = L"Data\\Global\\Excel\\wwctc.txt";
static constexpr const wchar_t* RelativeSkillFilterPath = L"Data\\Global\\Excel\\wwctcskills.txt";

struct InlineFilterState {
    uint32_t rng;
    uint32_t wwctc;
    uint32_t whitelistActive;
    uint32_t maxSkillIdExclusive;
    uint8_t allowTable[MaxTrackedSkillId];
};

enum class PatchMode {
    None,
    Nop100NoWhitelist,
    Suppress0,
    Inline,
};

struct SkillFilterConfig {
    bool whitelistActive = false;
    std::array<uint8_t, MaxTrackedSkillId> allowTable {};
};

static uintptr_t g_exeBase = 0;
static uint8_t* g_patchSite = nullptr;
static uint8_t* g_trampoline = nullptr;
static size_t g_trampolineSize = 0;
static InlineFilterState* g_filterState = nullptr;
static PatchMode g_patchMode = PatchMode::None;
static bool g_patchApplied = false;
static std::array<uint8_t, 6> g_originalBytes {};

static constexpr D2RLoaderPluginInfo PluginInfo {
    .apiVersion = D2RLOADER_PLUGIN_API_VERSION,
    .id         = "whirlwind-ctc-chance",
    .name       = "Whirlwind CTC Chance",
    .version    = "1.1.0",
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

static std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

static bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca + ('a' - 'A'));
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb + ('a' - 'A'));
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

static bool ParseInt(std::string_view text, int* out) noexcept {
    if (!out) {
        return false;
    }

    text = Trim(text);
    if (text.empty()) {
        return false;
    }

    int value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc() || result.ptr != last) {
        return false;
    }

    *out = value;
    return true;
}

static std::vector<std::string_view> SplitTabs(std::string_view line) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= line.size()) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string_view::npos) {
            parts.push_back(line.substr(start));
            break;
        }
        parts.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return parts;
}

static bool ReadFileBytes(const std::wstring& path, std::string* out) {
    if (!out) {
        return false;
    }

    out->clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return false;
    }

    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out->empty() ? TRUE : ReadFile(file, out->data(), static_cast<DWORD>(out->size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != out->size()) {
        out->clear();
        return false;
    }
    return true;
}

static std::wstring BuildModPath(const D2RLoaderPluginContext* ctx, const wchar_t* relativePath) {
    if (!ctx || !ctx->modDirectory || !ctx->modDirectory[0] || !relativePath) {
        return {};
    }

    std::wstring path = ctx->modDirectory;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    path += relativePath;
    return path;
}

static void StripUtf8Bom(std::string* bytes) {
    if (!bytes) {
        return;
    }
    if (bytes->size() >= 3 &&
        static_cast<unsigned char>((*bytes)[0]) == 0xEF &&
        static_cast<unsigned char>((*bytes)[1]) == 0xBB &&
        static_cast<unsigned char>((*bytes)[2]) == 0xBF) {
        bytes->erase(0, 3);
    }
}

static int ReadWwCtc(const D2RLoaderPluginContext* ctx) {
    const std::wstring path = BuildModPath(ctx, RelativeWwCtcPath);
    if (path.empty()) {
        return 100;
    }

    std::string bytes;
    if (!ReadFileBytes(path, &bytes)) {
        return 100;
    }

    StripUtf8Bom(&bytes);

    int skillIdCol = -1;
    int wwctcCol = -1;
    bool headerSeen = false;
    bool foundRow = false;
    bool invalidTargetRow = false;
    int parsedValue = 100;
    size_t pos = 0;

    while (pos <= bytes.size()) {
        const size_t lineStart = pos;
        size_t lineEnd = bytes.find_first_of("\r\n", lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = bytes.size();
            pos = bytes.size() + 1;
        } else {
            pos = lineEnd + 1;
            if (bytes[lineEnd] == '\r' && pos < bytes.size() && bytes[pos] == '\n') {
                ++pos;
            }
        }

        std::string_view line(bytes.data() + lineStart, lineEnd - lineStart);
        line = Trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const size_t comment = line.find('#');
        if (comment != std::string_view::npos) {
            line = Trim(line.substr(0, comment));
            if (line.empty()) {
                continue;
            }
        }

        std::vector<std::string_view> cols = SplitTabs(line);
        if (!headerSeen) {
            headerSeen = true;
            for (size_t i = 0; i < cols.size(); ++i) {
                const std::string_view name = Trim(cols[i]);
                if (EqualsIgnoreCase(name, "skillId")) {
                    skillIdCol = static_cast<int>(i);
                } else if (EqualsIgnoreCase(name, "wwctc")) {
                    wwctcCol = static_cast<int>(i);
                }
            }
            if (skillIdCol < 0 || wwctcCol < 0) {
                return 100;
            }
            continue;
        }

        const int needed = skillIdCol > wwctcCol ? skillIdCol : wwctcCol;
        if (static_cast<int>(cols.size()) <= needed) {
            continue;
        }

        int skillId = 0;
        int wwctc = 100;
        if (!ParseInt(cols[skillIdCol], &skillId) || skillId != WhirlwindSkillId) {
            continue;
        }
        if (!ParseInt(cols[wwctcCol], &wwctc)) {
            invalidTargetRow = true;
            continue;
        }

        if (wwctc < 0) {
            wwctc = 0;
        } else if (wwctc > 100) {
            wwctc = 100;
        }

        parsedValue = wwctc;
        foundRow = true;
        invalidTargetRow = false;
    }

    return (foundRow && !invalidTargetRow) ? parsedValue : 100;
}

static SkillFilterConfig ReadSkillFilter(const D2RLoaderPluginContext* ctx) {
    SkillFilterConfig config {};
    config.allowTable.fill(0);

    const std::wstring path = BuildModPath(ctx, RelativeSkillFilterPath);
    if (path.empty()) {
        return config;
    }

    std::string bytes;
    if (!ReadFileBytes(path, &bytes)) {
        return config;
    }

    StripUtf8Bom(&bytes);

    int skillIdCol = -1;
    int allowCol = -1;
    bool headerSeen = false;
    uint32_t allowedCount = 0;
    size_t pos = 0;

    while (pos <= bytes.size()) {
        const size_t lineStart = pos;
        size_t lineEnd = bytes.find_first_of("\r\n", lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = bytes.size();
            pos = bytes.size() + 1;
        } else {
            pos = lineEnd + 1;
            if (bytes[lineEnd] == '\r' && pos < bytes.size() && bytes[pos] == '\n') {
                ++pos;
            }
        }

        std::string_view line(bytes.data() + lineStart, lineEnd - lineStart);
        line = Trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const size_t comment = line.find('#');
        if (comment != std::string_view::npos) {
            line = Trim(line.substr(0, comment));
            if (line.empty()) {
                continue;
            }
        }

        std::vector<std::string_view> cols = SplitTabs(line);
        if (!headerSeen) {
            headerSeen = true;
            for (size_t i = 0; i < cols.size(); ++i) {
                const std::string_view name = Trim(cols[i]);
                if (EqualsIgnoreCase(name, "skillId")) {
                    skillIdCol = static_cast<int>(i);
                } else if (EqualsIgnoreCase(name, "allow")) {
                    allowCol = static_cast<int>(i);
                }
            }
            if (skillIdCol < 0 || allowCol < 0) {
                return config;
            }
            continue;
        }

        const int needed = skillIdCol > allowCol ? skillIdCol : allowCol;
        if (static_cast<int>(cols.size()) <= needed) {
            continue;
        }

        int skillId = -1;
        int allow = 0;
        if (!ParseInt(cols[skillIdCol], &skillId) || !ParseInt(cols[allowCol], &allow)) {
            continue;
        }
        if (skillId < 0 || skillId >= static_cast<int>(MaxTrackedSkillId)) {
            continue;
        }
        if (allow != 0 && allow != 1) {
            continue;
        }

        const size_t index = static_cast<size_t>(skillId);
        if (allow == 1 && config.allowTable[index] == 0) {
            config.allowTable[index] = 1;
            ++allowedCount;
        } else if (allow == 0 && config.allowTable[index] != 0) {
            config.allowTable[index] = 0;
            --allowedCount;
        }
    }

    config.whitelistActive = allowedCount != 0;
    return config;
}

static bool Rel32Fits(uintptr_t fromInstruction, uintptr_t toAddress) noexcept {
    const int64_t delta = static_cast<int64_t>(toAddress) - static_cast<int64_t>(fromInstruction + 5);
    return delta >= std::numeric_limits<int32_t>::min() && delta <= std::numeric_limits<int32_t>::max();
}

static void EmitU8(std::vector<uint8_t>& code, uint8_t value) {
    code.push_back(value);
}

static void EmitU32(std::vector<uint8_t>& code, uint32_t value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    code.insert(code.end(), bytes, bytes + sizeof(value));
}

static void EmitU64(std::vector<uint8_t>& code, uint64_t value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    code.insert(code.end(), bytes, bytes + sizeof(value));
}

static void EmitRel32(std::vector<uint8_t>& code, uint8_t opcode, uintptr_t instructionAddress, uintptr_t targetAddress) {
    code.push_back(opcode);
    const int64_t delta64 = static_cast<int64_t>(targetAddress) - static_cast<int64_t>(instructionAddress + 5);
    const int32_t delta = static_cast<int32_t>(delta64);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&delta);
    code.insert(code.end(), bytes, bytes + sizeof(delta));
}

static void EmitJccRel32(std::vector<uint8_t>& code, uint8_t conditionOpcode, uintptr_t instructionAddress, uintptr_t targetAddress) {
    code.push_back(0x0F);
    code.push_back(conditionOpcode);
    const int64_t delta64 = static_cast<int64_t>(targetAddress) - static_cast<int64_t>(instructionAddress + 6);
    const int32_t delta = static_cast<int32_t>(delta64);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&delta);
    code.insert(code.end(), bytes, bytes + sizeof(delta));
}

static void PatchRel32(std::vector<uint8_t>& code, uintptr_t base, size_t instructionOffset, size_t instructionLength, uintptr_t targetAddress) {
    const int64_t delta64 = static_cast<int64_t>(targetAddress) -
        static_cast<int64_t>(base + instructionOffset + instructionLength);
    const int32_t delta = static_cast<int32_t>(delta64);
    std::memcpy(code.data() + instructionOffset + instructionLength - 4, &delta, sizeof(delta));
}

static void EmitMovR11Imm64(std::vector<uint8_t>& code, uintptr_t value) {
    EmitU8(code, 0x49);
    EmitU8(code, 0xBB);
    EmitU64(code, static_cast<uint64_t>(value));
}

static void EmitMovEaxR8d(std::vector<uint8_t>& code) {
    const uint8_t bytes[] {0x44, 0x89, 0xC0};
    code.insert(code.end(), bytes, bytes + sizeof(bytes));
}

static void EmitCmpEaxImm32(std::vector<uint8_t>& code, uint32_t value) {
    EmitU8(code, 0x3D);
    EmitU32(code, value);
}

static void EmitMovEcxDwordR11Disp32(std::vector<uint8_t>& code, uint32_t disp) {
    EmitU8(code, 0x41);
    EmitU8(code, 0x8B);
    EmitU8(code, 0x8B);
    EmitU32(code, disp);
}

static void EmitMovEaxDwordR11Disp32(std::vector<uint8_t>& code, uint32_t disp) {
    EmitU8(code, 0x41);
    EmitU8(code, 0x8B);
    EmitU8(code, 0x83);
    EmitU32(code, disp);
}

static void EmitMovDwordR11Disp32Eax(std::vector<uint8_t>& code, uint32_t disp) {
    EmitU8(code, 0x41);
    EmitU8(code, 0x89);
    EmitU8(code, 0x83);
    EmitU32(code, disp);
}

static void EmitCmpDwordR11Disp8Imm8(std::vector<uint8_t>& code, uint8_t disp, uint8_t imm) {
    const uint8_t bytes[] {0x41, 0x83, 0x7B, disp, imm};
    code.insert(code.end(), bytes, bytes + sizeof(bytes));
}

static void EmitMovzxEcxByteR11RaxDisp32(std::vector<uint8_t>& code, uint32_t disp) {
    EmitU8(code, 0x41);
    EmitU8(code, 0x0F);
    EmitU8(code, 0xB6);
    EmitU8(code, 0x8C);
    EmitU8(code, 0x03);
    EmitU32(code, disp);
}

static void EmitRestoreAndJump(std::vector<uint8_t>& code, uintptr_t relayBase, uintptr_t targetAddress) {
    const uint8_t restore[] {
        0x41, 0x5B, // pop r11
        0x41, 0x58, // pop r8
        0x5A,       // pop rdx
        0x59,       // pop rcx
        0x58,       // pop rax
        0x9D        // popfq
    };
    code.insert(code.end(), restore, restore + sizeof(restore));
    const size_t jumpOffset = code.size();
    EmitRel32(code, 0xE9, relayBase + jumpOffset, targetAddress);
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

static bool BuildInlineRelay(uint8_t* base, std::vector<uint8_t>* out) {
    if (!base || !out || !g_filterState) {
        return false;
    }

    out->clear();
    out->reserve(512);

    const uintptr_t relayBase = reinterpret_cast<uintptr_t>(base);
    const uintptr_t stateAddress = reinterpret_cast<uintptr_t>(g_filterState);
    const uintptr_t allowTarget = g_exeBase + RVA_AllowTarget;
    const uintptr_t suppressTarget = g_exeBase + RVA_SuppressTarget;

    const uint32_t rngOffset = static_cast<uint32_t>(offsetof(InlineFilterState, rng));
    const uint32_t wwctcOffset = static_cast<uint32_t>(offsetof(InlineFilterState, wwctc));
    const uint32_t whitelistActiveOffset = static_cast<uint32_t>(offsetof(InlineFilterState, whitelistActive));
    const uint32_t allowTableOffset = static_cast<uint32_t>(offsetof(InlineFilterState, allowTable));

    const size_t originalNotTakenJz = out->size();
    EmitJccRel32(*out, 0x84, relayBase + originalNotTakenJz, 0);

    const uint8_t prologue[] {
        0x9C,       // pushfq
        0x50,       // push rax
        0x51,       // push rcx
        0x52,       // push rdx
        0x41, 0x50, // push r8
        0x41, 0x53  // push r11
    };
    out->insert(out->end(), prologue, prologue + sizeof(prologue));

    EmitMovR11Imm64(*out, stateAddress);

    const uint8_t movR8dR15d[] {0x45, 0x89, 0xF8}; // mov r8d,r15d
    out->insert(out->end(), movR8dR15d, movR8dR15d + sizeof(movR8dR15d));

    EmitCmpDwordR11Disp8Imm8(*out, static_cast<uint8_t>(whitelistActiveOffset), 0);
    const size_t noWhitelistJe = out->size();
    EmitJccRel32(*out, 0x84, relayBase + noWhitelistJe, 0);

    EmitMovEaxR8d(*out);
    EmitCmpEaxImm32(*out, MaxTrackedSkillId);
    const size_t whitelistRangeDenyJae = out->size();
    EmitJccRel32(*out, 0x83, relayBase + whitelistRangeDenyJae, 0);

    EmitMovzxEcxByteR11RaxDisp32(*out, allowTableOffset);
    const uint8_t testEcxEcx[] {0x85, 0xC9};
    out->insert(out->end(), testEcxEcx, testEcxEcx + sizeof(testEcxEcx));
    const size_t whitelistZeroDenyJz = out->size();
    EmitJccRel32(*out, 0x84, relayBase + whitelistZeroDenyJz, 0);

    const size_t chanceGate = out->size();
    EmitMovEcxDwordR11Disp32(*out, wwctcOffset);
    const uint8_t cmpEcx100[] {0x83, 0xF9, 0x64}; // cmp ecx,100
    out->insert(out->end(), cmpEcx100, cmpEcx100 + sizeof(cmpEcx100));
    const size_t chanceAllowNoRollJae = out->size();
    EmitJccRel32(*out, 0x83, relayBase + chanceAllowNoRollJae, 0);
    const uint8_t testEcxEcx2[] {0x85, 0xC9};
    out->insert(out->end(), testEcxEcx2, testEcxEcx2 + sizeof(testEcxEcx2));
    const size_t chanceSuppressNoRollJz = out->size();
    EmitJccRel32(*out, 0x84, relayBase + chanceSuppressNoRollJz, 0);

    EmitMovEaxDwordR11Disp32(*out, rngOffset);
    const uint8_t rngPath[] {
        0x69, 0xC0, 0x0D, 0x66, 0x19, 0x00, // imul eax,eax,1664525
        0x05, 0x5F, 0xF3, 0x6E, 0x3C        // add eax,1013904223
    };
    out->insert(out->end(), rngPath, rngPath + sizeof(rngPath));
    EmitMovDwordR11Disp32Eax(*out, rngOffset);

    const uint8_t rollPath[] {
        0x31, 0xD2,                         // xor edx,edx
        0xB9, 0x64, 0x00, 0x00, 0x00,       // mov ecx,100
        0xF7, 0xF1                          // div ecx
    };
    out->insert(out->end(), rollPath, rollPath + sizeof(rollPath));
    EmitMovEcxDwordR11Disp32(*out, wwctcOffset);
    const uint8_t cmpEdxEcx[] {0x39, 0xCA}; // cmp edx,ecx
    out->insert(out->end(), cmpEdxEcx, cmpEdxEcx + sizeof(cmpEdxEcx));
    const size_t rollAllowJb = out->size();
    EmitJccRel32(*out, 0x82, relayBase + rollAllowJb, 0);

    const size_t suppressPath = out->size();
    EmitRestoreAndJump(*out, relayBase, suppressTarget);

    const size_t allowPath = out->size();
    EmitRestoreAndJump(*out, relayBase, allowTarget);

    const size_t originalNotTakenAllow = out->size();
    const size_t originalAllowJmp = out->size();
    EmitRel32(*out, 0xE9, relayBase + originalAllowJmp, allowTarget);

    PatchRel32(*out, relayBase, originalNotTakenJz, 6, relayBase + originalNotTakenAllow);
    PatchRel32(*out, relayBase, noWhitelistJe, 6, relayBase + chanceGate);
    PatchRel32(*out, relayBase, whitelistRangeDenyJae, 6, relayBase + suppressPath);
    PatchRel32(*out, relayBase, whitelistZeroDenyJz, 6, relayBase + suppressPath);
    PatchRel32(*out, relayBase, chanceAllowNoRollJae, 6, relayBase + allowPath);
    PatchRel32(*out, relayBase, chanceSuppressNoRollJz, 6, relayBase + suppressPath);
    PatchRel32(*out, relayBase, rollAllowJb, 6, relayBase + allowPath);

    return Rel32Fits(relayBase + originalAllowJmp, allowTarget);
}

static bool VerifyOriginalBytes() noexcept {
    g_originalBytes = {};
    g_patchSite = reinterpret_cast<uint8_t*>(g_exeBase + RVA_PatchSite);

    const bool readOk = SafeRead(g_patchSite, g_originalBytes.data(), g_originalBytes.size());
    if (!readOk || std::memcmp(g_originalBytes.data(), ExpectedBytes.data(), ExpectedBytes.size()) != 0) {
        return false;
    }
    return true;
}

static bool InstallNop100NoWhitelist() noexcept {
    g_patchApplied = SafeWrite(g_patchSite, NopPatch.data(), NopPatch.size());
    if (g_patchApplied) {
        g_patchMode = PatchMode::Nop100NoWhitelist;
    }
    return g_patchApplied;
}

static bool InstallSuppress0() noexcept {
    g_patchApplied = SafeWrite(g_patchSite, DirectSuppressPatch.data(), DirectSuppressPatch.size());
    if (g_patchApplied) {
        g_patchMode = PatchMode::Suppress0;
    }
    return g_patchApplied;
}

static bool InstallInline(int wwctc, const SkillFilterConfig& filter) noexcept {
    constexpr size_t allocationSize = 0x4000;
    g_trampoline = AllocateNear(reinterpret_cast<uintptr_t>(g_patchSite), allocationSize);
    if (!g_trampoline) {
        return false;
    }

    std::memset(g_trampoline, 0, allocationSize);
    g_filterState = reinterpret_cast<InlineFilterState*>(g_trampoline + InlineStateOffset);
    g_filterState->wwctc = static_cast<uint32_t>(wwctc);
    g_filterState->whitelistActive = filter.whitelistActive ? 1u : 0u;
    g_filterState->maxSkillIdExclusive = MaxTrackedSkillId;
    g_filterState->rng = static_cast<uint32_t>(
        GetTickCount64() ^
        reinterpret_cast<uintptr_t>(g_trampoline) ^
        reinterpret_cast<uintptr_t>(g_patchSite) ^
        static_cast<uintptr_t>(g_exeBase));
    if (g_filterState->rng == 0) {
        g_filterState->rng = 0xA341316Cu;
    }
    std::memcpy(g_filterState->allowTable, filter.allowTable.data(), filter.allowTable.size());

    std::vector<uint8_t> code;
    if (!BuildInlineRelay(g_trampoline, &code) || code.empty() || code.size() >= InlineStateOffset) {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = nullptr;
        g_filterState = nullptr;
        return false;
    }

    std::memcpy(g_trampoline, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), g_trampoline, code.size());
    g_trampolineSize = code.size();

    if (!Rel32Fits(reinterpret_cast<uintptr_t>(g_patchSite), reinterpret_cast<uintptr_t>(g_trampoline))) {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = nullptr;
        g_filterState = nullptr;
        g_trampolineSize = 0;
        return false;
    }

    std::array<uint8_t, 6> patch {};
    patch[0] = 0xE9;
    const int64_t delta64 = reinterpret_cast<uintptr_t>(g_trampoline) - (reinterpret_cast<uintptr_t>(g_patchSite) + 5);
    const int32_t delta = static_cast<int32_t>(delta64);
    std::memcpy(patch.data() + 1, &delta, sizeof(delta));
    patch[5] = 0x90;

    g_patchApplied = SafeWrite(g_patchSite, patch.data(), patch.size());
    if (g_patchApplied) {
        g_patchMode = PatchMode::Inline;
    } else {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = nullptr;
        g_filterState = nullptr;
        g_trampolineSize = 0;
    }
    return g_patchApplied;
}

static bool RestorePatch() noexcept {
    bool restored = true;
    if (g_patchApplied && g_patchSite) {
        restored = SafeWrite(g_patchSite, g_originalBytes.data(), g_originalBytes.size());
    }
    if (g_trampoline) {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
    }

    g_trampoline = nullptr;
    g_trampolineSize = 0;
    g_filterState = nullptr;
    return restored;
}

} // namespace

D2RLOADER_PLUGIN_EXPORT const D2RLoaderPluginInfo* __cdecl D2RLoaderGetPluginInfo() noexcept {
    return &PluginInfo;
}

D2RLOADER_PLUGIN_EXPORT bool __cdecl D2RLoaderLoadHooks(const D2RLoaderPluginContext* ctx) noexcept {
    g_exeBase = 0;
    g_patchSite = nullptr;
    g_trampoline = nullptr;
    g_trampolineSize = 0;
    g_filterState = nullptr;
    g_patchMode = PatchMode::None;
    g_patchApplied = false;
    g_originalBytes = {};

    if (!ctx || ctx->apiVersion < D2RLOADER_PLUGIN_API_VERSION) {
        return false;
    }

    g_exeBase = ctx->exeBase;
    const int wwctc = ReadWwCtc(ctx);
    const SkillFilterConfig filter = ReadSkillFilter(ctx);

    if (!VerifyOriginalBytes()) {
        return false;
    }

    bool ok = false;
    if (!filter.whitelistActive && wwctc >= 100) {
        ok = InstallNop100NoWhitelist();
    } else if (!filter.whitelistActive && wwctc <= 0) {
        ok = InstallSuppress0();
    } else {
        ok = InstallInline(wwctc, filter);
    }

    return ok;
}

D2RLOADER_PLUGIN_EXPORT void __cdecl D2RLoaderUnload() noexcept {
    (void)RestorePatch();

    g_exeBase = 0;
    g_patchSite = nullptr;
    g_patchMode = PatchMode::None;
    g_patchApplied = false;
}
