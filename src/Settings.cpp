#include "PCH.h"
#include "Settings.h"

namespace Settings
{
    namespace
    {
        // MCM owns the hotkey through this in-game global. Reading the INI is
        // unreliable under MO2's virtual filesystem and can resurrect stale keys.
        constexpr const char* kPluginName = "PrismaUIAddItemMenu.esp";
        constexpr RE::FormID  kToggleHotkeyGlobalID = 0x800;

        constexpr const char* kDefaultSettingsPath =
            "Data\\MCM\\Config\\PrismaUIAddItemMenu\\settings.ini";
        constexpr const char* kMcmUserSettingsPath =
            "Data\\MCM\\Settings\\PrismaUIAddItemMenu.ini";
        constexpr const char* kMo2OverwriteUserSettingsPath =
            "C:\\SkyrimVRmods\\overwrite\\MCM\\Settings\\PrismaUIAddItemMenu.ini";
        constexpr const char* kMo2OverwriteUserSettingsDevicePath =
            "\\\\?\\C:\\SkyrimVRmods\\overwrite\\MCM\\Settings\\PrismaUIAddItemMenu.ini";

        std::mutex                      g_mutex;
        Values                          g_values;
        std::filesystem::file_time_type g_lastWrite{};
        bool                            g_loaded = false;
        uint32_t                        g_lastLoggedGlobalHotkey = 0;
        bool                            g_loggedMissingGlobal = false;

        bool ReadHotkeyGlobal(uint32_t& out)
        {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler) {
                return false;
            }

            auto* form = dataHandler->LookupForm(kToggleHotkeyGlobalID, kPluginName);
            auto* global = form ? form->As<RE::TESGlobal>() : nullptr;
            if (!global) {
                if (!g_loggedMissingGlobal) {
                    logger::warn("Settings: hotkey global {}|{:X} is not loaded; keeping current hotkey",
                        kPluginName, kToggleHotkeyGlobalID);
                    g_loggedMissingGlobal = true;
                }
                return false;
            }

            const auto scancode = static_cast<int>(global->value + 0.5F);
            if (scancode <= 0) {
                logger::warn("Settings: hotkey global {}|{:X} has invalid value {}; keeping current hotkey",
                    kPluginName, kToggleHotkeyGlobalID, global->value);
                return false;
            }

            out = static_cast<uint32_t>(scancode);
            return true;
        }

        void RefreshHotkeyFromGlobal(Values& values)
        {
            uint32_t scancode = 0;
            if (!ReadHotkeyGlobal(scancode)) {
                return;
            }

            if (values.toggleScancode != scancode || g_lastLoggedGlobalHotkey != scancode) {
                logger::info("Settings: MCM global hotkey {}|{:X} = {} (0x{:X})",
                    kPluginName, kToggleHotkeyGlobalID, scancode, scancode);
                g_lastLoggedGlobalHotkey = scancode;
            }
            values.toggleScancode = scancode;
            g_loggedMissingGlobal = false;
        }

        // Tiny INI reader. Format:
        //   [Section]
        //   key = value
        // Comments start with ; or #. Whitespace tolerant.
        bool ParseIni(const std::filesystem::path& path, Values& out)
        {
            std::ifstream f(path);
            if (!f) {
                return false;
            }

            std::string line;
            std::string section;
            bool parsedSetting = false;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                const size_t start = line.find_first_not_of(" \t");
                if (start == std::string::npos) {
                    continue;
                }
                line = line.substr(start);

                if (line.empty() || line[0] == ';' || line[0] == '#') {
                    continue;
                }

                if (line.front() == '[') {
                    const auto end = line.find(']');
                    if (end != std::string::npos) {
                        section = line.substr(1, end - 1);
                    }
                    continue;
                }

                const auto eq = line.find('=');
                if (eq == std::string::npos) {
                    continue;
                }

                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);

                auto trim = [](std::string& s) {
                    const auto a = s.find_first_not_of(" \t");
                    const auto b = s.find_last_not_of(" \t");
                    s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
                };
                trim(key);
                trim(val);

                try {
                    if (section == "General") {
                        if (key == "iPageSize") {
                            out.pageSize = static_cast<uint32_t>(std::stoul(val));
                            parsedSetting = true;
                        } else if (key == "bPauseWhileOpen") {
                            out.pauseGameWhileOpen = (val == "1" || val == "true");
                            parsedSetting = true;
                        } else if (key == "bExcludeVanilla") {
                            out.excludeVanillaByDefault = (val == "1" || val == "true");
                            parsedSetting = true;
                        }
                    }
                } catch (const std::exception& e) {
                    logger::warn("Settings: ignored invalid value {}={} in {} ({})",
                        key, val, path.string(), e.what());
                }
            }
            return parsedSetting;
        }

        bool TryParseIni(const char* path, Values& out)
        {
            std::error_code ec;
            const bool exists = std::filesystem::exists(path, ec);
            const std::string existsSuffix = ec ? (" (" + ec.message() + ")") : "";
            logger::info("Settings: probing ini: {} exists={}{}", path, exists ? "true" : "false", existsSuffix);

            Values before = out;
            const bool parsed = ParseIni(path, out);
            if (parsed) {
                logger::info(
                    "Settings: loaded {} (hotkey 0x{:X}->0x{:X}, pageSize {}->{})",
                    path, before.toggleScancode, out.toggleScancode, before.pageSize, out.pageSize);
            } else {
                logger::info("Settings: ini not readable or not present: {}", path);
            }
            return parsed;
        }

        std::filesystem::file_time_type LatestSettingsWriteTime(std::error_code& ec)
        {
            std::filesystem::file_time_type latest{};
            for (const auto* path : {
                     kDefaultSettingsPath,
                     kMcmUserSettingsPath,
                     kMo2OverwriteUserSettingsPath,
                     kMo2OverwriteUserSettingsDevicePath
                 }) {
                if (!std::filesystem::exists(path, ec)) {
                    ec.clear();
                    continue;
                }

                const auto writeTime = std::filesystem::last_write_time(path, ec);
                if (!ec && writeTime > latest) {
                    latest = writeTime;
                }
                ec.clear();
            }
            return latest;
        }

        void NormalizeValues(Values& v)
        {
            if (v.toggleScancode == 0) {
                logger::warn("Settings: hotkey is invalid while MCM global is unavailable; falling back to F1");
                v.toggleScancode = 0x3B;
            }
        }
    }

    void Load()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        Values fresh;
        bool loadedAny = false;

        loadedAny = TryParseIni(kDefaultSettingsPath, fresh) || loadedAny;
        loadedAny = TryParseIni(kMcmUserSettingsPath, fresh) || loadedAny;
        loadedAny = TryParseIni(kMo2OverwriteUserSettingsPath, fresh) || loadedAny;
        loadedAny = TryParseIni(kMo2OverwriteUserSettingsDevicePath, fresh) || loadedAny;

        if (!loadedAny) {
            logger::info("Settings: no settings ini found, using built-in defaults");
        }

        RefreshHotkeyFromGlobal(fresh);
        NormalizeValues(fresh);

        std::error_code ec;
        g_lastWrite = LatestSettingsWriteTime(ec);
        g_values = fresh;
        g_loaded = true;
        logger::info("Settings: hotkey=0x{:X} pageSize={} pause={} excludeVanilla={}",
            g_values.toggleScancode, g_values.pageSize,
            g_values.pauseGameWhileOpen, g_values.excludeVanillaByDefault);
    }

    void ReloadIfChanged()
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_loaded) {
                RefreshHotkeyFromGlobal(g_values);

                std::error_code ec;
                const auto now = LatestSettingsWriteTime(ec);
                if (ec || now == g_lastWrite) {
                    return;
                }
            }
        }

        Load();
    }

    Values Get()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        RefreshHotkeyFromGlobal(g_values);
        return g_values;
    }

}
