// ==WindhawkMod==
// @id           native-four-column-windows-snap-layout
// @name         Native Four-Column Windows Snap Layout
// @description  Adds an extra native Windows 11 Snap Layout with four equal vertical columns
// @version      1.0
// @author       Hoffelhas
// @github       https://github.com/Hoffelhas
// @include      explorer.exe
// @architecture amd64
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Native Four-Column Windows Snap Layout

Adds an extra native Windows 11 Snap Layout with four equal vertical columns:

`| 25% | 25% | 25% | 25% |`

The layout is integrated into Windows' own Snap Layout UI and appears in:

- **Win+Z**
- **Maximize-button hover**
- **Snap Bar** when dragging a window to the top of the screen

Because the mod extends the native Snap Layout model, snapping continues to use
Windows' normal Snap Assist and Snap Groups behavior.

## Compatibility

This mod relies on undocumented Windows internals and private symbols from
`SnapLayout.dll`. Windows updates can change these implementation details.

The mod contains structural checks before modifying a layout. If the expected
native layout structure isn't found, it leaves that layout set unchanged rather
than patching an unknown structure.

Developed and tested on Windows 11 build **26200.9445** on x86-64.

If a Windows update changes the relevant private symbols, Windhawk may be unable
to load the mod until it is updated.
*/
// ==/WindhawkModReadme==

// Source code is published under the MIT License.

#include <windhawk_utils.h>
#include <windows.h>
#include <cstring>

namespace {

constexpr SIZE_T kSnapLayoutSize = 0x50;
constexpr SIZE_T kSnapZoneSize = 0x38;
constexpr SIZE_T kNativeLayoutCount = 6;
constexpr SIZE_T kCustomLayoutCount = 7;
constexpr int kNativePickerHeight = 244;
constexpr int kExtraPickerHeight = 80;

struct RawVector {
    void* first;
    void* last;
    void* end;
};

// The Snap Bar asks SnapModel::Layouts() more than once while loading. These
// thread-local flags let us distinguish that path from the Win+Z/maximize
// flyout path and avoid appending the custom layout more than once per load.
thread_local bool g_inSnapBarLoad = false;
thread_local bool g_snapBarCustomAdded = false;

// Set after the normal flyout path has successfully appended the custom layout.
// This prevents changing the flyout height if our structural checks fail.
bool g_flyoutCustomAdded = false;

// -----------------------------------------------------------------------------
// Native functions resolved from SnapLayout.dll
// -----------------------------------------------------------------------------

using Layouts_t = RawVector* (__cdecl*)(void* thisPtr, RawVector* returnBuffer);
Layouts_t Layouts_Original = nullptr;

using EmplaceLayout_t = void* (__cdecl*)(void* vectorThis,
                                        const void* sourceLayout);
EmplaceLayout_t EmplaceLayout_Original = nullptr;

using PickerHeight_t = int (__cdecl*)(void* thisPtr, int* value);
PickerHeight_t PickerHeight_Original = nullptr;

using SnapBarLoadLayouts_t = void (__cdecl*)(void* thisPtr,
                                             double scale,
                                             int options,
                                             bool flag);
SnapBarLoadLayouts_t SnapBarLoadLayouts_Original = nullptr;

// This pass-through hook exists so Windhawk resolves the private vector helper
// for us and exposes a callable original/trampoline pointer.
void* __cdecl EmplaceLayout_Hook(void* vectorThis, const void* sourceLayout) {
    return EmplaceLayout_Original(vectorThis, sourceLayout);
}

int __cdecl PickerHeight_Hook(void* thisPtr, int* value) {
    int hr = PickerHeight_Original(thisPtr, value);

    // The additional flyout item occupies one more row. Only adjust the exact
    // picker height observed for the supported layout structure, and only after
    // the custom flyout layout was actually added.
    if (hr == 0 && value && g_flyoutCustomAdded &&
        *value == kNativePickerHeight) {
        *value += kExtraPickerHeight;
    }

    return hr;
}

void __cdecl SnapBarLoadLayouts_Hook(void* thisPtr,
                                     double scale,
                                     int options,
                                     bool flag) {
    const bool previousInSnapBarLoad = g_inSnapBarLoad;
    const bool previousCustomAdded = g_snapBarCustomAdded;

    g_inSnapBarLoad = true;
    g_snapBarCustomAdded = false;

    SnapBarLoadLayouts_Original(thisPtr, scale, options, flag);

    g_inSnapBarLoad = previousInSnapBarLoad;
    g_snapBarCustomAdded = previousCustomAdded;
}

// -----------------------------------------------------------------------------
// Raw SnapLayout/SnapZone helpers
// -----------------------------------------------------------------------------

SIZE_T GetVectorCount(const RawVector* vec, SIZE_T elementSize) {
    if (!vec || !vec->first || !vec->last || elementSize == 0) {
        return 0;
    }

    const auto* first = reinterpret_cast<const BYTE*>(vec->first);
    const auto* last = reinterpret_cast<const BYTE*>(vec->last);

    if (last < first) {
        return 0;
    }

    const SIZE_T bytes = static_cast<SIZE_T>(last - first);
    if (bytes % elementSize != 0) {
        return 0;
    }

    return bytes / elementSize;
}

SIZE_T GetZoneCount(const void* layout) {
    if (!layout) {
        return 0;
    }

    void* first = nullptr;
    void* last = nullptr;

    memcpy(&first, reinterpret_cast<const BYTE*>(layout) + 0x28, sizeof(first));
    memcpy(&last, reinterpret_cast<const BYTE*>(layout) + 0x30, sizeof(last));

    if (!first || !last) {
        return 0;
    }

    const auto* firstBytes = reinterpret_cast<const BYTE*>(first);
    const auto* lastBytes = reinterpret_cast<const BYTE*>(last);

    if (lastBytes < firstBytes) {
        return 0;
    }

    const SIZE_T bytes = static_cast<SIZE_T>(lastBytes - firstBytes);
    if (bytes % kSnapZoneSize != 0) {
        return 0;
    }

    return bytes / kSnapZoneSize;
}

void WriteU32(void* base, SIZE_T offset, unsigned int value) {
    memcpy(reinterpret_cast<BYTE*>(base) + offset, &value, sizeof(value));
}

bool IsExpectedSourceLayout(const void* layout) {
    if (!layout) {
        return false;
    }

    unsigned int columns = 0;
    unsigned int rows = 0;

    memcpy(&columns,
           reinterpret_cast<const BYTE*>(layout) + 0x20,
           sizeof(columns));
    memcpy(&rows,
           reinterpret_cast<const BYTE*>(layout) + 0x24,
           sizeof(rows));

    return columns == 2 && rows == 2 && GetZoneCount(layout) == 4;
}

// Convert a deep-copied native 2x2/four-zone layout into four equal vertical
// columns while preserving the native GridUnitType stored in every SnapZone.
bool PatchToFourColumns(void* layout) {
    if (!IsExpectedSourceLayout(layout)) {
        return false;
    }

    void* zoneFirst = nullptr;
    void* zoneLast = nullptr;

    memcpy(&zoneFirst,
           reinterpret_cast<BYTE*>(layout) + 0x28,
           sizeof(zoneFirst));
    memcpy(&zoneLast,
           reinterpret_cast<BYTE*>(layout) + 0x30,
           sizeof(zoneLast));

    if (!zoneFirst || !zoneLast) {
        return false;
    }

    const SIZE_T zoneBytes =
        reinterpret_cast<BYTE*>(zoneLast) -
        reinterpret_cast<BYTE*>(zoneFirst);

    if (zoneBytes != 4 * kSnapZoneSize) {
        return false;
    }

    // SnapLayout grid: four columns, one row.
    WriteU32(layout, 0x20, 4);
    WriteU32(layout, 0x24, 1);

    // SnapZone layout (relevant fields):
    // +0x20 OriginColumn
    // +0x24 OriginRow
    // +0x28 ColumnSpan
    // +0x2C RowSpan
    // +0x30 GridUnitType (preserved)
    for (unsigned int i = 0; i < 4; i++) {
        BYTE* zone =
            reinterpret_cast<BYTE*>(zoneFirst) + i * kSnapZoneSize;

        WriteU32(zone, 0x20, i);
        WriteU32(zone, 0x24, 0);
        WriteU32(zone, 0x28, 1);
        WriteU32(zone, 0x2C, 1);
    }

    return true;
}

bool CloneAndAppendFourColumn(RawVector* vec, SIZE_T sourceIndex) {
    if (!vec || !EmplaceLayout_Original) {
        return false;
    }

    const SIZE_T count = GetVectorCount(vec, kSnapLayoutSize);
    if (count != kNativeLayoutCount || sourceIndex >= count) {
        return false;
    }

    BYTE* source =
        reinterpret_cast<BYTE*>(vec->first) + sourceIndex * kSnapLayoutSize;

    if (!IsExpectedSourceLayout(source)) {
        return false;
    }

    // Use SnapLayout.dll's own std::vector insertion helper. SnapLayout owns
    // non-trivial data (including std::wstring and vector<SnapZone>), so a raw
    // memcpy clone would duplicate ownership pointers and be unsafe.
    void* newLayout = EmplaceLayout_Original(vec, source);
    if (!newLayout) {
        return false;
    }

    if (GetVectorCount(vec, kSnapLayoutSize) != kCustomLayoutCount) {
        return false;
    }

    return PatchToFourColumns(newLayout);
}

// -----------------------------------------------------------------------------
// SnapModel::Layouts hook
// -----------------------------------------------------------------------------

RawVector* __cdecl Layouts_Hook(void* thisPtr, RawVector* returnBuffer) {
    RawVector* result = Layouts_Original(thisPtr, returnBuffer);

    if (!returnBuffer) {
        return result;
    }

    if (g_inSnapBarLoad) {
        // Snap Bar ordering observed on the supported build:
        // 0: 2x1/2 zones, 1: 3x1/2 zones, 2: 2x2/3 zones,
        // 3: 2x2/4 zones, 4: 3x1/3 zones, 5: 4x1/3 zones.
        // Element 3 is therefore the four-zone source to clone.
        if (!g_snapBarCustomAdded &&
            CloneAndAppendFourColumn(returnBuffer, 3)) {
            g_snapBarCustomAdded = true;
        }

        return result;
    }

    // Win+Z / maximize-hover ordering observed on the supported build:
    // element 4 is the native 2x2/four-zone source layout.
    if (CloneAndAppendFourColumn(returnBuffer, 4)) {
        g_flyoutCustomAdded = true;
    }

    return result;
}

}  // namespace

BOOL Wh_ModInit() {
    HMODULE module = GetModuleHandleW(L"SnapLayout.dll");

    if (!module) {
        // On current Windows 11 builds SnapLayout.dll is part of the Client.Core
        // system app. Loading it here allows symbol hooks to be installed even
        // if Explorer hasn't loaded the module yet.
        module = LoadLibraryExW(
            L"C:\\WINDOWS\\SystemApps\\MicrosoftWindows.Client.Core_cw5n1h2txyewy\\SnapLayout.dll",
            nullptr,
            LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    if (!module) {
        Wh_Log(L"Failed to load SnapLayout.dll");
        return FALSE;
    }

    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {
            {
                LR"(public: class std::vector<struct SnapLayout,class std::allocator<struct SnapLayout> > __cdecl SnapModel::Layouts(void)const )",
            },
            &Layouts_Original,
            Layouts_Hook,
        },
        {
            {
                LR"(private: struct SnapLayout & __cdecl std::vector<struct SnapLayout,class std::allocator<struct SnapLayout> >::_Emplace_one_at_back<struct SnapLayout const &>(struct SnapLayout const &))",
            },
            &EmplaceLayout_Original,
            EmplaceLayout_Hook,
        },
        {
            {
                LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::SnapLayout::implementation::SnapLayoutPickerViewModel,struct winrt::SnapLayout::ISnapLayoutPickerViewModel>::get_PickerHeight(int *))",
            },
            &PickerHeight_Original,
            PickerHeight_Hook,
        },
        {
            {
                LR"(public: void __cdecl winrt::SnapLayout::implementation::SnapBarViewModel::LoadLayouts(double,enum winrt::SnapLayout::SnapModelOptions,bool))",
            },
            &SnapBarLoadLayouts_Original,
            SnapBarLoadLayouts_Hook,
        },
    };

    if (!WindhawkUtils::HookSymbols(module, hooks, ARRAYSIZE(hooks))) {
        Wh_Log(L"Failed to resolve one or more SnapLayout.dll symbols");
        return FALSE;
    }

    return TRUE;
}

void Wh_ModUninit() {
}
