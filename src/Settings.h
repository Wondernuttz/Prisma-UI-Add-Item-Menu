#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>

namespace Settings
{
    // Runtime settings. The hotkey comes from the MCM-backed game global;
    // the INI is only a fallback place for non-hotkey tuning values.
    struct Values {
        // DirectInput scancode (matches keymap values written by MCM Helper).
        // Default = F1 (0x3B). 0 is treated as invalid and falls back to F1.
        uint32_t toggleScancode = 0x3B;

        // Pagination caps the JS-side DOM size. Tune if Ultralight chokes on big lists.
        uint32_t pageSize = 100;

        // When true, pause the game while the menu is focused (closer to AIM behavior).
        bool pauseGameWhileOpen = false;

        // When true, items from Skyrim.esm/Update.esm are excluded by default.
        bool excludeVanillaByDefault = false;
    };

    void Load();            // read INI tuning and the MCM hotkey global
    void ReloadIfChanged(); // refresh global hotkey and reload INI tuning if changed

    Values Get();          // copy of current values (locked read)
}
