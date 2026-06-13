#include "PCH.h"
#include "ItemEnumerator.h"

namespace ItemEnumerator
{
    namespace
    {
        std::shared_mutex          g_mutex;
        std::vector<ItemRecord>    g_cache;
        std::vector<size_t>        g_nameOrder;  // cache indices sorted by lowercase name

        // Helper: type-tag + walk for one form type. Templated on RE form class.
        // Skipped silently if the form has no display name (e.g. dummy items).
        template <class TForm>
        void AppendOne(std::vector<ItemRecord>& out, TForm* form, const char* typeStr)
        {
            if (!form) return;

            const char* name = form->GetFullName();
            if (!name || !*name) return;        // unnamed = engine internal, skip
            if (form->IsDeleted()) return;
            if (form->formFlags & RE::TESForm::RecordFlags::kDeleted) return;

            ItemRecord rec;
            rec.rawFormID   = form->GetFormID();
            rec.localFormID = rec.rawFormID & 0x00FFFFFF;
            rec.fullName    = name;
            rec.typeStr     = typeStr;

            if (auto* file = form->GetFile(0)) {
                rec.pluginName = file->fileName;
            }

            if constexpr (std::is_base_of_v<RE::TESBoundObject, TForm>) {
                auto* bound = static_cast<RE::TESBoundObject*>(form);
                rec.weight = bound->GetWeight();
                rec.value  = bound->GetGoldValue();
            }

            out.emplace_back(std::move(rec));
        }

        template <class TForm>
        void Append(std::vector<ItemRecord>& out, const char* typeStr)
        {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return;

            const auto& arr = dh->GetFormArray<TForm>();
            out.reserve(out.size() + arr.size());

            for (auto* form : arr) {
                AppendOne(out, form, typeStr);
            }
        }

        void AppendAlchemy(std::vector<ItemRecord>& out)
        {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return;

            const auto& arr = dh->GetFormArray<RE::AlchemyItem>();
            out.reserve(out.size() + arr.size());

            for (auto* form : arr) {
                if (!form) continue;
                const char* type = form->IsFood() ? "FOOD" : (form->IsPoison() ? "POIS" : "POTN");
                AppendOne(out, form, type);
            }
        }

        void AppendBooks(std::vector<ItemRecord>& out)
        {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return;

            const auto& arr = dh->GetFormArray<RE::TESObjectBOOK>();
            out.reserve(out.size() + arr.size());

            for (auto* form : arr) {
                if (!form) continue;
                AppendOne(out, form, form->TeachesSpell() ? "STOM" : "BOOK");
            }
        }
    }

    void Refresh()
    {
        std::vector<ItemRecord> next;
        next.reserve(20000);  // typical heavy modlist lands 50-100k; saves rehashing

        // Order matches what the UI tab order will display.
        Append<RE::TESObjectWEAP> (next, "WEAP");
        Append<RE::TESObjectARMO> (next, "ARMO");
        AppendAlchemy(next);
        Append<RE::IngredientItem>(next, "INGR");
        AppendBooks(next);
        Append<RE::TESObjectMISC> (next, "MISC");
        Append<RE::TESAmmo>       (next, "AMMO");
        Append<RE::TESSoulGem>    (next, "SLGM");
        Append<RE::TESKey>        (next, "KEYM");
        // SpellItem is intentionally not added by default — spells don't use AddItem;
        // they need AddSpell. If we add a Spells tab later it gets its own enumerator
        // and its own action handler.

        // Name-sorted index, built once: queries page alphabetically (SkyUI-style),
        // so "All Mods" interleaves every plugin instead of fronting load order.
        // The cache itself stays in load order for the plugin-list views.
        std::vector<size_t> order(next.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::vector<std::string> keys(next.size());
        for (size_t i = 0; i < next.size(); ++i) {
            keys[i] = next[i].fullName;
            std::transform(keys[i].begin(), keys[i].end(), keys[i].begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        std::sort(order.begin(), order.end(),
            [&keys](size_t a, size_t b) { return keys[a] < keys[b]; });

        const auto count = next.size();
        {
            std::unique_lock<std::shared_mutex> w(g_mutex);
            g_cache.swap(next);
            g_nameOrder.swap(order);
        }
        logger::info("ItemEnumerator: cached {} forms", count);
    }

    QueryResponse Query(const QueryRequest& req)
    {
        QueryResponse out;
        std::shared_lock<std::shared_mutex> r(g_mutex);

        out.totalAll = static_cast<int>(g_cache.size());

        // Lowercase the search term once.
        std::string needle = req.searchTerm;
        std::transform(needle.begin(), needle.end(), needle.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        auto lowerName = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };

        auto matchRank = [](const std::string& lname, const std::string& n) {
            if (n.empty()) return 0;
            const auto pos = lname.find(n);
            if (pos == std::string::npos) return 99;
            if (pos == 0) return 0;
            if (pos > 0 && !std::isalnum(static_cast<unsigned char>(lname[pos - 1]))) return 1;
            return 2;
        };

        // First pass: count + collect indices that match. Counting full matches lets
        // the UI render "showing 100 of 4128" reliably independent of pagination.
        struct Match {
            size_t index;
            int rank;
            std::string lname;
        };
        std::vector<Match> matched;
        matched.reserve(g_cache.size() / 4);

        // Walk in name order so unsearched queries page alphabetically across all plugins
        for (size_t si = 0; si < g_cache.size(); ++si) {
            const size_t i = (si < g_nameOrder.size()) ? g_nameOrder[si] : si;
            const auto& it = g_cache[i];

            if (!req.typeFilter.empty() && it.typeStr != req.typeFilter)   continue;
            if (!req.pluginFilter.empty() && it.pluginName != req.pluginFilter) continue;

            if (req.excludeVanilla) {
                static constexpr std::string_view kVanilla[] = {
                    "Skyrim.esm", "Update.esm",
                    "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm",
                    "ccBGSSSE001-Fish.esm", "ccBGSSSE025-AdvDSGS.esm",
                    "ccQDRSSE001-SurvivalMode.esl",
                };
                bool isVanilla = false;
                for (auto v : kVanilla) {
                    if (it.pluginName == v) { isVanilla = true; break; }
                }
                if (isVanilla) continue;
            }

            std::string lname;
            int rank = 0;
            if (!needle.empty()) {
                lname = lowerName(it.fullName);
                rank = matchRank(lname, needle);
                if (rank == 99) continue;
            }

            matched.push_back({ i, rank, std::move(lname) });
        }

        if (!needle.empty()) {
            std::stable_sort(matched.begin(), matched.end(), [](const Match& a, const Match& b) {
                if (a.rank != b.rank) return a.rank < b.rank;
                return a.lname < b.lname;
            });
        }

        out.totalMatched = static_cast<int>(matched.size());

        // Slice for pagination.
        const int pageSize = std::max(1, req.pageSize);
        const int page     = std::max(0, req.page);
        const int begin    = page * pageSize;
        const int end      = std::min<int>(begin + pageSize, out.totalMatched);

        if (begin < end) {
            out.items.reserve(end - begin);
            for (int i = begin; i < end; ++i) {
                out.items.push_back(g_cache[matched[i].index]);
            }
        }
        return out;
    }

    std::vector<std::string> ListPlugins()
    {
        std::shared_lock<std::shared_mutex> r(g_mutex);
        std::vector<std::string> out;
        out.reserve(64);

        // Insertion-order preserving uniqueness — first time we see a plugin name,
        // add it. Mod authors expect their .esp grouped near load-order top, this
        // is good enough for a dropdown without sorting.
        for (const auto& it : g_cache) {
            bool seen = false;
            for (const auto& p : out) if (p == it.pluginName) { seen = true; break; }
            if (!seen) out.push_back(it.pluginName);
        }
        return out;
    }

    std::vector<PluginEntry> ListPluginsWithCounts(bool excludeVanilla)
    {
        static constexpr std::string_view kVanilla[] = {
            "Skyrim.esm", "Update.esm",
            "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm",
            "ccBGSSSE001-Fish.esm", "ccBGSSSE025-AdvDSGS.esm",
            "ccQDRSSE001-SurvivalMode.esl",
        };

        std::shared_lock<std::shared_mutex> r(g_mutex);

        // Map preserves insertion order (load order) by tracking first-seen index.
        std::vector<PluginEntry> out;
        out.reserve(128);
        std::unordered_map<std::string, size_t> idx;

        for (const auto& it : g_cache) {
            if (excludeVanilla) {
                bool isV = false;
                for (auto v : kVanilla) if (it.pluginName == v) { isV = true; break; }
                if (isV) continue;
            }

            auto found = idx.find(it.pluginName);
            if (found == idx.end()) {
                idx.emplace(it.pluginName, out.size());
                out.push_back({ it.pluginName, 1 });
            } else {
                out[found->second].itemCount++;
            }
        }
        return out;
    }

    RE::TESForm* FindFormByPluginAndLocalID(const std::string& pluginName, uint32_t localFormID)
    {
        uint32_t rawFormID = 0;
        {
            std::shared_lock<std::shared_mutex> r(g_mutex);
            for (const auto& it : g_cache) {
                if (it.pluginName == pluginName && it.localFormID == localFormID) {
                    rawFormID = it.rawFormID;
                    break;
                }
            }
        }

        if (rawFormID) {
            if (auto* form = RE::TESForm::LookupByID(rawFormID)) {
                return form;
            }
            logger::warn("ItemEnumerator: cached form disappeared plugin='{}' local={:08X} raw={:08X}",
                         pluginName, localFormID, rawFormID);
        }

        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return nullptr;
        // Fallback only. The cache lookup above is the primary path.

        // LookupForm handles ESL load-order math internally — it's the right call.
        return dh->LookupForm(localFormID, pluginName);
    }

    size_t CacheSize()
    {
        std::shared_lock<std::shared_mutex> r(g_mutex);
        return g_cache.size();
    }
}
