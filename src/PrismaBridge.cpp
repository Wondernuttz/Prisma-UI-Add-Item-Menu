#include "PCH.h"
#include "PrismaBridge.h"
#include "PrismaUI_API.h"
#include "ItemEnumerator.h"
#include "Settings.h"
#include "RE/A/ActorEquipManager.h"
#include "RE/B/BGSDefaultObjectManager.h"
#include "RE/M/Misc.h"

#ifdef GetObject
#undef GetObject
#endif

namespace PrismaBridge
{
    namespace
    {
        PRISMA_UI_API::IVPrismaUI1* g_api  = nullptr;
        PrismaView                  g_view = 0;
        std::atomic<bool>           g_domReady{false};
        std::atomic<bool>           g_menuOpen{false};

        // Path is relative to Data/PrismaUI/views, which PrismaUI exposes as file:///views/.
        constexpr const char* kViewHtmlPath = "PrismaUIAddItemMenu/index.html";

        // ── JSON helpers (tiny — we only emit/parse small stuff) ───────────────
        // Avoiding nlohmann/json to keep build deps to zero. Output only escapes
        // the characters that would break a JS string literal in our generated
        // calls; if a mod author smuggles bytes 0x00-0x1F into a TESForm name,
        // they get filtered.
        std::string EscapeJsonString(const std::string& s)
        {
            std::string out;
            out.reserve(s.size() + 8);
            for (unsigned char c : s) {
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b";  break;
                    case '\f': out += "\\f";  break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if (c < 0x20) { /* drop */ }
                        else          { out += static_cast<char>(c); }
                        break;
                }
            }
            return out;
        }

        // Very small JSON value lookup: scans for "key":<value> patterns.
        // Good enough for the shape JS sends us; do NOT use as a general parser.
        std::string PullJsonString(const std::string& json, const std::string& key)
        {
            const std::string needle = "\"" + key + "\"";
            auto p = json.find(needle);
            if (p == std::string::npos) return {};
            p = json.find(':', p + needle.size());
            if (p == std::string::npos) return {};
            // skip whitespace + opening quote
            while (p < json.size() && (json[p] == ':' || std::isspace((unsigned char)json[p]))) ++p;
            if (p >= json.size() || json[p] != '"') return {};
            ++p;
            std::string out;
            while (p < json.size() && json[p] != '"') {
                if (json[p] == '\\' && p + 1 < json.size()) {
                    char esc = json[p + 1];
                    switch (esc) {
                        case '"': out += '"'; break; case '\\': out += '\\'; break;
                        case 'n': out += '\n'; break; case 't': out += '\t'; break;
                        case 'r': out += '\r'; break;
                        default: out += esc; break;
                    }
                    p += 2;
                } else {
                    out += json[p++];
                }
            }
            return out;
        }

        long long PullJsonInt(const std::string& json, const std::string& key, long long fallback = 0)
        {
            const std::string needle = "\"" + key + "\"";
            auto p = json.find(needle);
            if (p == std::string::npos) return fallback;
            p = json.find(':', p + needle.size());
            if (p == std::string::npos) return fallback;
            ++p;
            while (p < json.size() && std::isspace((unsigned char)json[p])) ++p;
            char* endp = nullptr;
            long long v = std::strtoll(json.c_str() + p, &endp, 10);
            if (endp == json.c_str() + p) return fallback;
            return v;
        }

        bool PullJsonBool(const std::string& json, const std::string& key, bool fallback = false)
        {
            const std::string needle = "\"" + key + "\"";
            auto p = json.find(needle);
            if (p == std::string::npos) return fallback;
            p = json.find(':', p + needle.size());
            if (p == std::string::npos) return fallback;
            ++p;
            while (p < json.size() && std::isspace((unsigned char)json[p])) ++p;
            if (json.compare(p, 4, "true")  == 0) return true;
            if (json.compare(p, 5, "false") == 0) return false;
            return fallback;
        }

        std::string Snip(const std::string& s, size_t maxLen = 240)
        {
            if (s.size() <= maxLen) return s;
            return s.substr(0, maxLen) + "...";
        }

        std::string SerializeQueryResponse(const ItemEnumerator::QueryResponse& r)
        {
            std::ostringstream ss;
            ss << "{\"totalMatched\":" << r.totalMatched
               << ",\"totalAll\":"     << r.totalAll
               << ",\"items\":[";
            for (size_t i = 0; i < r.items.size(); ++i) {
                const auto& it = r.items[i];
                if (i) ss << ',';
                ss << "{"
                   << "\"name\":\""    << EscapeJsonString(it.fullName)   << "\","
                   << "\"plugin\":\""  << EscapeJsonString(it.pluginName) << "\","
                   << "\"type\":\""    << it.typeStr                      << "\","
                   << "\"localId\":"   << it.localFormID                  << ","
                   << "\"weight\":"    << it.weight                       << ","
                   << "\"value\":"     << it.value
                   << "}";
            }
            ss << "]}";
            return ss.str();
        }

        std::string SerializePluginList(const std::vector<std::string>& v)
        {
            std::ostringstream ss;
            ss << "[";
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) ss << ',';
                ss << "\"" << EscapeJsonString(v[i]) << "\"";
            }
            ss << "]";
            return ss.str();
        }

        std::string SerializePluginEntries(const std::vector<ItemEnumerator::PluginEntry>& v)
        {
            std::ostringstream ss;
            ss << "[";
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) ss << ',';
                ss << "{"
                   << "\"name\":\""  << EscapeJsonString(v[i].name) << "\","
                   << "\"count\":"   << v[i].itemCount
                   << "}";
            }
            ss << "]";
            return ss.str();
        }

        std::string SerializeAddResult(bool ok, const std::string& message, bool closeMenu = false)
        {
            std::ostringstream ss;
            ss << "{\"ok\":" << (ok ? "true" : "false")
               << ",\"closeMenu\":" << (closeMenu ? "true" : "false")
               << ",\"message\":\"" << EscapeJsonString(message) << "\"}";
            return ss.str();
        }

        struct ActionResult {
            bool ok = false;
            bool closeMenu = false;
            std::string message;
        };

        // Push a plain string back to JS. Caller handles escaping if it's JSON.
        void InvokeJSCallback(const std::string& jsFunc, const std::string& argJson)
        {
            if (!g_api || !g_view) {
                logger::warn("PrismaBridge: dropping JS callback {} because view/API is not ready", jsFunc);
                return;
            }
            logger::info("PrismaBridge: C++ -> JS {} ({} bytes)", jsFunc, argJson.size());
            // We use InteropCall — fastest path; argument is a single string.
            g_api->InteropCall(g_view, jsFunc.c_str(), argJson.c_str());
        }

        const char* PickAddSuccessSound(RE::TESForm* form)
        {
            if (!form) {
                return "ITMGenericUp";
            }

            switch (form->GetFormType()) {
                case RE::FormType::Weapon:
                    return "ITMGenericWeaponUp";
                case RE::FormType::Armor:
                    return "ITMGenericArmorUp";
                case RE::FormType::Book:
                    return "ITMGenericBookUp";
                case RE::FormType::Ammo:
                    return "ITMArrowsUp";
                case RE::FormType::KeyMaster:
                    return "ITMKeyUp";
                case RE::FormType::AlchemyItem:
                    if (auto* alch = form->As<RE::AlchemyItem>()) {
                        return alch->IsPoison() ? "ITMGenericUp" : "ITMPotionUp";
                    }
                    return "ITMPotionUp";
                case RE::FormType::SoulGem:
                    return "ITMGenericUp";
                case RE::FormType::Misc:
                    return "ITMGenericUp";
                default:
                    return "ITMGenericUp";
            }
        }

        void PlayAddResultSound(bool ok, RE::TESForm* form = nullptr)
        {
            const char* sound = ok ? PickAddSuccessSound(form) : "UIMenuCancel";
            RE::PlaySound(sound);
            logger::info("AddItem: played UI sound '{}'", sound);
        }

        const RE::BGSEquipSlot* GetDefaultEquipSlot(RE::DefaultObjectID id)
        {
            auto* defaults = RE::BGSDefaultObjectManager::GetSingleton();
            if (!defaults) {
                return nullptr;
            }

            auto slot = defaults->GetObject<RE::BGSEquipSlot>(id);
            return slot ? *slot : nullptr;
        }

        std::string HotkeyName(uint32_t scan)
        {
            switch (scan) {
                case 0x01: return "Esc";
                case 0x02: return "1";
                case 0x03: return "2";
                case 0x04: return "3";
                case 0x05: return "4";
                case 0x06: return "5";
                case 0x07: return "6";
                case 0x08: return "7";
                case 0x09: return "8";
                case 0x0A: return "9";
                case 0x0B: return "0";
                case 0x0C: return "-";
                case 0x0D: return "=";
                case 0x0E: return "Backspace";
                case 0x0F: return "Tab";
                case 0x10: return "Q";
                case 0x11: return "W";
                case 0x12: return "E";
                case 0x13: return "R";
                case 0x14: return "T";
                case 0x15: return "Y";
                case 0x16: return "U";
                case 0x17: return "I";
                case 0x18: return "O";
                case 0x19: return "P";
                case 0x1A: return "[";
                case 0x1B: return "]";
                case 0x1C: return "Enter";
                case 0x1D: return "Left Ctrl";
                case 0x1E: return "A";
                case 0x1F: return "S";
                case 0x20: return "D";
                case 0x21: return "F";
                case 0x22: return "G";
                case 0x23: return "H";
                case 0x24: return "J";
                case 0x25: return "K";
                case 0x26: return "L";
                case 0x27: return ";";
                case 0x28: return "'";
                case 0x29: return "`";
                case 0x2A: return "Left Shift";
                case 0x2B: return "\\";
                case 0x2C: return "Z";
                case 0x2D: return "X";
                case 0x2E: return "C";
                case 0x2F: return "V";
                case 0x30: return "B";
                case 0x31: return "N";
                case 0x32: return "M";
                case 0x33: return ",";
                case 0x34: return ".";
                case 0x35: return "/";
                case 0x36: return "Right Shift";
                case 0x37: return "Numpad *";
                case 0x38: return "Left Alt";
                case 0x39: return "Space";
                case 0x3A: return "Caps Lock";
                case 0x3B: return "F1";
                case 0x3C: return "F2";
                case 0x3D: return "F3";
                case 0x3E: return "F4";
                case 0x3F: return "F5";
                case 0x40: return "F6";
                case 0x41: return "F7";
                case 0x42: return "F8";
                case 0x43: return "F9";
                case 0x44: return "F10";
                case 0x45: return "Num Lock";
                case 0x46: return "Scroll Lock";
                case 0x57: return "F11";
                case 0x58: return "F12";
                case 0xC7: return "Home";
                case 0xC9: return "Page Up";
                case 0xCF: return "End";
                case 0xD1: return "Page Down";
                case 0xD2: return "Insert";
                case 0xD3: return "Delete";
                default:
                    {
                        std::ostringstream ss;
                        ss << "0x" << std::hex << std::uppercase << scan;
                        return ss.str();
                    }
            }
        }

        std::string SerializeHotkeyResult(bool ok, uint32_t scancode, const std::string& message)
        {
            std::ostringstream ss;
            ss << "{\"ok\":" << (ok ? "true" : "false")
               << ",\"scancode\":" << scancode
               << ",\"name\":\"" << EscapeJsonString(HotkeyName(scancode)) << "\","
               << "\"message\":\"" << EscapeJsonString(message) << "\"}";
            return ss.str();
        }

        // ── JS → C++ listeners ─────────────────────────────────────────────────
        void OnJSQuery(const char* arg)
        {
            if (!arg) return;
            std::string s(arg);

            ItemEnumerator::QueryRequest req;
            req.typeFilter     = PullJsonString(s, "type");
            req.searchTerm     = PullJsonString(s, "search");
            req.pluginFilter   = PullJsonString(s, "plugin");
            req.excludeVanilla = PullJsonBool  (s, "excludeVanilla", false);
            req.page           = static_cast<int>(PullJsonInt(s, "page", 0));
            req.pageSize       = static_cast<int>(PullJsonInt(s, "pageSize", 100));

            logger::info("PrismaBridge: JS -> C++ query plugin='{}' type='{}' search='{}' page={} pageSize={}",
                         req.pluginFilter, req.typeFilter, Snip(req.searchTerm, 80), req.page, req.pageSize);
            auto resp = ItemEnumerator::Query(req);
            logger::info("PrismaBridge: query result {} items, totalMatched={}, totalAll={}",
                         resp.items.size(), resp.totalMatched, resp.totalAll);
            InvokeJSCallback("__prismaUIAddItem_onQueryResult",
                             SerializeQueryResponse(resp));
        }

        void OnJSGetPlugins(const char* arg)
        {
            // Argument is JSON: {"excludeVanilla": bool}
            const std::string s = arg ? arg : "";
            const bool excludeVanilla = PullJsonBool(s, "excludeVanilla", false);

            logger::info("PrismaBridge: JS -> C++ getPlugins excludeVanilla={} raw='{}'",
                         excludeVanilla, Snip(s));
            auto entries = ItemEnumerator::ListPluginsWithCounts(excludeVanilla);
            logger::info("PrismaBridge: plugin list result {} plugins", entries.size());
            InvokeJSCallback("__prismaUIAddItem_onPluginList",
                             SerializePluginEntries(entries));
        }

        void OnJSAddItem(const char* arg)
        {
            if (!arg) return;
            std::string s(arg);

            const std::string plugin   = PullJsonString(s, "plugin");
            const uint32_t    localId  = static_cast<uint32_t>(PullJsonInt(s, "localId"));
            logger::info("PrismaBridge: JS -> C++ addItem plugin='{}' localId={:08X} raw='{}'",
                         plugin, localId, Snip(s));

            // HARD CLAMP — engine stores per-item count as int16 in ExtraCount
            // for most form types. Counts above 32767 wrap to negative values
            // and silently corrupt container state (negative inventory counts,
            // weird gold totals). The JS layer snaps to this same cap on blur
            // and on Add; this is defense-in-depth in case JS state is ever
            // bypassed (custom Prisma builds, scripted callers, etc.).
            constexpr int32_t kMaxCount = 32767;
            int32_t requested = static_cast<int32_t>(PullJsonInt(s, "count", 1));
            const int32_t count = std::clamp(requested, 1, kMaxCount);
            if (requested != count) {
                logger::warn("AddItem: count {} clamped to {} (engine int16 ceiling)",
                             requested, count);
            }

            // Form lookup must happen on the main thread; AddObjectToContainer too.
            SKSE::GetTaskInterface()->AddTask([plugin, localId, count]() {
                auto* form = ItemEnumerator::FindFormByPluginAndLocalID(plugin, localId);
                if (!form) {
                    logger::warn("AddItem: form not found ({}/{:08X})", plugin, localId);
                    PlayAddResultSound(false);
                    InvokeJSCallback("__prismaUIAddItem_onAddResult",
                                     SerializeAddResult(false, "Form not found"));
                    return;
                }
                auto* bound = form->As<RE::TESBoundObject>();
                if (!bound) {
                    logger::warn("AddItem: form {} is not a bound object", form->GetName());
                    PlayAddResultSound(false);
                    InvokeJSCallback("__prismaUIAddItem_onAddResult",
                                     SerializeAddResult(false, "Selected form is not an inventory object"));
                    return;
                }
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player) {
                    logger::warn("AddItem: player singleton not available");
                    PlayAddResultSound(false);
                    InvokeJSCallback("__prismaUIAddItem_onAddResult",
                                     SerializeAddResult(false, "Player not available"));
                    return;
                }

                player->AddObjectToContainer(bound, nullptr, count, nullptr);
                PlayAddResultSound(true, form);
                logger::info("AddItem: +{} {} ({}/{:08X})", count, bound->GetName(), plugin, localId);
                std::ostringstream msg;
                msg << "Added " << count << " x " << bound->GetName();
                InvokeJSCallback("__prismaUIAddItem_onAddResult",
                                 SerializeAddResult(true, msg.str()));
            });
        }

        ActionResult RunDirectAction(RE::TESForm* form, int32_t count)
        {
            if (!form) {
                PlayAddResultSound(false);
                return { false, false, "Form not found" };
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                PlayAddResultSound(false);
                return { false, false, "Player not available" };
            }

            auto* bound = form->As<RE::TESBoundObject>();

            if (auto* book = form->As<RE::TESObjectBOOK>()) {
                if (book->TeachesSpell()) {
                    auto* spell = book->GetSpell();
                    if (!spell) {
                        PlayAddResultSound(false);
                        return { false, false, "Spell tome has no spell" };
                    }
                    const bool learned = player->AddSpell(spell);
                    PlayAddResultSound(true, form);
                    std::ostringstream msg;
                    msg << (learned ? "Learned " : "Already knows ") << spell->GetFullName();
                    return { true, false, msg.str() };
                }

                PlayAddResultSound(false);
                return { false, false, "Books can be added to inventory, not opened from this menu" };
            }

            if (!bound) {
                PlayAddResultSound(false);
                return { false, false, "Selected form is not usable" };
            }

            constexpr int32_t kMaxCount = 32767;
            count = std::clamp(count, 1, kMaxCount);

            if (auto* alch = form->As<RE::AlchemyItem>()) {
                player->AddObjectToContainer(bound, nullptr, 1, nullptr);
                if (alch->IsPoison()) {
                    PlayAddResultSound(true, form);
                    std::ostringstream msg;
                    msg << "Added poison " << bound->GetName();
                    return { true, false, msg.str() };
                }

                const bool drank = player->DrinkPotion(alch, nullptr);
                PlayAddResultSound(drank, form);
                std::ostringstream msg;
                msg << (alch->IsFood() ? (drank ? "Ate " : "Could not eat ") : (drank ? "Drank " : "Could not drink "))
                    << bound->GetName();
                return { drank, false, msg.str() };
            }

            switch (form->GetFormType()) {
                case RE::FormType::Weapon:
                    player->AddObjectToContainer(bound, nullptr, count, nullptr);
                    PlayAddResultSound(true, form);
                    {
                        std::ostringstream msg;
                        msg << "Added " << count << " x " << bound->GetName();
                        return { true, false, msg.str() };
                    }

                case RE::FormType::Armor:
                case RE::FormType::Ammo:
                    player->AddObjectToContainer(bound, nullptr, count, nullptr);
                    PlayAddResultSound(true, form);
                    {
                        std::ostringstream msg;
                        msg << "Added " << count << " x " << bound->GetName();
                        return { true, false, msg.str() };
                    }

                case RE::FormType::Ingredient:
                    player->AddObjectToContainer(bound, nullptr, 1, nullptr);
                    PlayAddResultSound(true, form);
                    {
                        std::ostringstream msg;
                        msg << "Added ingredient " << bound->GetName();
                        return { true, false, msg.str() };
                    }

                default:
                    PlayAddResultSound(false);
                    return { false, false, "No direct action for this item" };
            }
        }

        void OnJSUseItem(const char* arg)
        {
            if (!arg) return;
            std::string s(arg);

            const std::string plugin  = PullJsonString(s, "plugin");
            const uint32_t    localId = static_cast<uint32_t>(PullJsonInt(s, "localId"));
            int32_t requested = static_cast<int32_t>(PullJsonInt(s, "count", 1));
            logger::info("PrismaBridge: JS -> C++ useItem plugin='{}' localId={:08X} raw='{}'",
                         plugin, localId, Snip(s));

            SKSE::GetTaskInterface()->AddTask([plugin, localId, requested]() {
                auto* form = ItemEnumerator::FindFormByPluginAndLocalID(plugin, localId);
                const ActionResult result = RunDirectAction(form, requested);
                logger::info("UseItem: {} ({}/{:08X})", result.message, plugin, localId);
                InvokeJSCallback("__prismaUIAddItem_onAddResult",
                                 SerializeAddResult(result.ok, result.message, result.closeMenu));
            });
        }

        void OnJSCloseMenu(const char* /*arg*/)
        {
            logger::info("PrismaBridge: JS -> C++ close");
            CloseMenu();
        }

        void OnJSDebug(const char* arg)
        {
            logger::info("PrismaBridge: JS debug: {}", Snip(arg ? arg : ""));
        }

        // ── DOM ready callback ─────────────────────────────────────────────────
        void OnDomReady(PrismaView /*v*/)
        {
            g_domReady = true;
            logger::info("PrismaBridge: DOM ready");
            const auto sets = Settings::Get();
            InvokeJSCallback("__prismaUIAddItem_onHotkeyResult",
                             SerializeHotkeyResult(true, sets.toggleScancode, "Current hotkey"));
        }
    }

    // Lazy view creation. We acquire the API pointer eagerly (cheap, just a
    // GetProcAddress + interface cast) but DEFER CreateView until the user
    // actually presses the hotkey for the first time. PrismaVRTest follows
    // the same pattern, and for good reason: at kPostLoad time PrismaUI's
    // D3D device isn't created yet, the Ultralight renderer doesn't exist,
    // and CreateView either queues forever or blocks the load chain. Calling
    // it later — after the game has rendered its first frame and PrismaUI's
    // D3D Present hook has fired — is reliably safe.
    static bool EnsureViewCreated()
    {
        if (g_view) return true;
        if (!g_api)  return false;

        logger::info("PrismaBridge: creating view (lazy, on first menu open)");
        g_view = g_api->CreateView(kViewHtmlPath, &OnDomReady);
        if (!g_view) {
            logger::error("PrismaBridge: CreateView failed for {}", kViewHtmlPath);
            return false;
        }

        g_api->Hide(g_view);
        g_api->RegisterJSListener(g_view, "__prismaUIAddItem_query",      &OnJSQuery);
        g_api->RegisterJSListener(g_view, "__prismaUIAddItem_getPlugins", &OnJSGetPlugins);
        g_api->RegisterJSListener(g_view, "__prismaUIAddItem_addItem",    &OnJSAddItem);
        g_api->RegisterJSListener(g_view, "__prismaUIAddItem_useItem",    &OnJSUseItem);
        g_api->RegisterJSListener(g_view, "__prismaUIAddItem_close",      &OnJSCloseMenu);
        g_api->RegisterJSListener(g_view, "__prismaUIAddItem_debug",      &OnJSDebug);
        logger::info("PrismaBridge: view {} created, listeners registered", g_view);
        return true;
    }

    bool Initialize()
    {
        if (g_api) return true;

        g_api = static_cast<PRISMA_UI_API::IVPrismaUI1*>(
            PRISMA_UI_API::RequestPluginAPI(PRISMA_UI_API::InterfaceVersion::V1));
        if (!g_api) {
            logger::error("PrismaBridge: PrismaUI API not available — is PrismaUI installed?");
            return false;
        }
        logger::info("PrismaBridge: PrismaUI API acquired (view will be created on first hotkey press)");
        return true;
    }

    void OpenMenu()
    {
        if (!g_api) {
            logger::info("PrismaBridge::OpenMenu: PrismaUI API not acquired; retrying acquisition");
            if (!Initialize()) {
                logger::warn("PrismaBridge::OpenMenu: PrismaUI API still not available");
                return;
            }
        }

        // First open of this session — create the view. Cheap on subsequent opens.
        if (!EnsureViewCreated()) return;

        const auto sets = Settings::Get();
        g_api->Show(g_view);
        g_menuOpen = true;
        g_api->Focus(g_view, sets.pauseGameWhileOpen, /*disableFocusMenu=*/true);
        logger::info("PrismaBridge: menu opened (domReady={}, pause={}, excludeVanillaDefault={})",
                     g_domReady.load(), sets.pauseGameWhileOpen, sets.excludeVanillaByDefault);

        // Tell the page to reset to the mod-list view and re-query. Each menu
        // open behaves like a fresh "directory" — even if the page lifetime
        // spans multiple opens, state resets to a known good place.
        if (g_domReady) {
            g_api->InteropCall(g_view, "__prismaUIAddItem_onMenuOpen",
                               sets.excludeVanillaByDefault ? "true" : "false");
            InvokeJSCallback("__prismaUIAddItem_onHotkeyResult",
                             SerializeHotkeyResult(true, sets.toggleScancode, "Current hotkey"));
        } else {
            logger::info("PrismaBridge: menu open happened before DOM ready; JS load retry will request plugin list");
        }
    }

    void CloseMenu()
    {
        g_menuOpen = false;
        if (!g_api || !g_view) return;

        g_api->Unfocus(g_view);
        g_api->Hide(g_view);
        logger::info("PrismaBridge: menu closed (view hidden, kept alive)");
    }

    bool IsMenuOpen()
    {
        return g_api && g_view && g_menuOpen.load();
    }

    void Shutdown()
    {
        g_menuOpen = false;
        if (g_api && g_view) {
            g_api->Destroy(g_view);
            g_view = 0;
        }
        g_api = nullptr;
    }
}
