# Prisma UI AddItem Menu

An AddItemMenu-style browser for Skyrim built on top of [PrismaUI](https://github.com/PrismaUI-SKSE/framework).

Open a Prisma UI panel, browse every loaded plugin that contributes items, drill into a plugin, search and filter its records, then add items, learn spell tomes, or consume usable items directly from the menu.

## Features

- PrismaUI-based item browser for Skyrim VR/SE/AE SKSE setups.
- Plugin list showing every loaded file that contributes supported item records.
- Per-plugin item view with search, pagination, and category tabs.
- Supported categories include weapons, armor, potions, ingredients, books, misc items, ammo, soul gems, and keys.
- Direct actions for normal inventory items, spell tomes, potions, food, and ingredients.
- Quantity presets for x1, x10, x100, and x1000.
- Optional quantity lock for repeated bulk adds.
- Inline item type, weight, and gold value columns.
- MCM Helper hotkey configuration backed by an ESP global value.
- No Papyrus gameplay scripting for the add-item flow.

## Requirements

- Skyrim Script Extender for the target game version.
- PrismaUI.
- MCM Helper.
- Address Library for SKSE Plugins.

## Usage

Install the mod, enable the plugin, and configure the menu hotkey in MCM. Press the hotkey in game to open the Prisma UI panel.

The first view lists plugins. Select a plugin to open its item list. Use the category tabs and search box to narrow the results, pick a quantity, then select an item action.

When an action succeeds, the selection moves to the next item instead of removing the item from the list. If quantity lock is disabled, the quantity resets to x1 after each successful action.

## Building

This project is a CMake/CommonLibSSE-NG SKSE plugin.

```sh
cmake --preset vs2026
cmake --build build/vs2026 --config Release
```

The compiled plugin is written to the CMake build output. When configured with `-DPIM_DEPLOY=ON`, the DLL is copied into `mod/Data/SKSE/Plugins/`.

## Project Layout

| Path | Purpose |
| --- | --- |
| `src/` | Native SKSE plugin code. |
| `mod/Data/PrismaUI/views/PrismaUIAddItemMenu/` | PrismaUI HTML/CSS/JS view. |
| `mod/Data/Interface/PrismaUIAddItemMenu/` | Mirrored UI files for compatibility/testing. |
| `mod/Data/MCM/Config/PrismaUIAddItemMenu/` | MCM Helper configuration. |
| `mod/Data/PrismaUIAddItemMenu.esp` | Plugin file containing the MCM-backed hotkey global. |

## Notes

`src/PrismaUI_API.h` is a vendored consumer API header from PrismaUI. Keep it in sync with upstream PrismaUI when its interface changes.

## License

MIT. See [LICENSE](LICENSE).

## Credits

- PrismaUI-SKSE for the rendering framework.
- powerof3 for MCM Helper.
- The original AddItemMenu and AddItemMenu-NG projects for proving the workflow Skyrim players expect.
