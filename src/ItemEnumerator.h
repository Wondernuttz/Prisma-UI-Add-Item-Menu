#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ItemEnumerator
{
    // One row in our cache. Wide enough to power the UI without follow-up lookups.
    struct ItemRecord {
        uint32_t    rawFormID;     // full FormID with load-order index
        uint32_t    localFormID;   // mod-local FormID (loadOrder byte stripped)
        std::string editorID;      // can be empty on AE+ if EditorID DB isn't loaded
        std::string fullName;      // GetFullName(); items without a name are filtered out
        std::string pluginName;    // e.g. "Skyrim.esm"
        std::string typeStr;       // "WEAP" / "ARMO" / "POTN" / "POIS" / ...
        float       weight = 0.f;
        int32_t     value  = 0;
    };

    // Build (or rebuild) the cache. Called on kPostLoadGame and kNewGame. Threadsafe.
    void Refresh();

    // Pulled-out filter shape so PrismaBridge can deserialize JSON into it cleanly.
    struct QueryRequest {
        std::string typeFilter;       // "" = all
        std::string searchTerm;       // case-insensitive substring; empty matches anything
        std::string pluginFilter;     // "" = all
        bool        excludeVanilla = false;
        int         page     = 0;
        int         pageSize = 100;
    };

    struct QueryResponse {
        std::vector<ItemRecord> items;
        int totalMatched = 0;     // matching the filter, before pagination
        int totalAll     = 0;     // entire cache size (pre-filter)
    };

    QueryResponse Query(const QueryRequest& req);

    // List of all plugin filenames present in the cache (for the UI's source filter dropdown).
    std::vector<std::string> ListPlugins();

    // Plugin name + how many cacheable items it contributes. For the mod-list view
    // so users see "MyMod.esp — 248 items" before drilling in.
    struct PluginEntry {
        std::string name;
        int         itemCount = 0;
    };
    std::vector<PluginEntry> ListPluginsWithCounts(bool excludeVanilla);

    // Lookup live form by stable (pluginName, localFormID) pair. Returns nullptr if not found.
    // Used by the AddItem path — never trust raw FormID across load-order changes.
    RE::TESForm* FindFormByPluginAndLocalID(const std::string& pluginName, uint32_t localFormID);

    size_t CacheSize();
}
