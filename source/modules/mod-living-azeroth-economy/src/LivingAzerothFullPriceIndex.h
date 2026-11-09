#ifndef LIVING_AZEROTH_FULL_PRICE_INDEX_H
#define LIVING_AZEROTH_FULL_PRICE_INDEX_H

#include "DatabaseEnv.h"
#include "Field.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "QueryResult.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace LivingAzeroth::Economy
{
inline constexpr char FullPriceSourceSha256[] =
    "675CEC6CD33E7108CD2E1E0228485D965407AB3D43B5C5347FF538C4EE6875F0";
inline constexpr char FullPriceCatalogSha256[] =
    "5E15BE2C976EE436B1B4BF520154EE9B565007045FF02441C79ADDF8A6CDBC77";

struct FullPriceMetric
{
    std::uint32_t ItemEntry = 0;
    std::uint32_t RecommendedStack = 0;
    std::uint64_t ReferenceUnitCopper = 0;
    std::uint64_t ModeledMinUnitCopper = 0;
    std::uint64_t ModeledMaxUnitCopper = 0;
    std::string Status;
    bool Active = false;
    bool AuctionSafe = false;
};

class FullPriceIndex
{
public:
    static FullPriceIndex& Instance()
    {
        static FullPriceIndex value;
        return value;
    }

    bool EnsureLoaded()
    {
        if (_attempted)
            return _ready;
        _attempted = true;

        QueryResult manifest = WorldDatabase.Query(
            "SELECT schema_version,source_csv_sha256,canonical_rows_sha256,"
            "candidate_rows,recommended_rows,review_rows,seasonal_rows "
            "FROM la_auction_price_catalog WHERE singleton_id=1");
        if (!manifest)
        {
            _error = "price_catalog_manifest_missing";
            return false;
        }

        Field* mf = manifest->Fetch();
        if (mf[0].Get<std::uint32_t>() != 1 ||
            mf[1].Get<std::string>() != FullPriceSourceSha256 ||
            mf[2].Get<std::string>() != FullPriceCatalogSha256 ||
            mf[3].Get<std::uint32_t>() != 14179 ||
            mf[4].Get<std::uint32_t>() != 11600 ||
            mf[5].Get<std::uint32_t>() != 2524 ||
            mf[6].Get<std::uint32_t>() != 55)
        {
            _error = "price_catalog_manifest_mismatch";
            return false;
        }

        QueryResult rows = WorldDatabase.Query(
            "SELECT item_entry,status,active,recommended_stack,reference_copper,"
            "modeled_min_unit_copper,modeled_max_unit_copper "
            "FROM la_auction_price_metric ORDER BY item_entry");
        if (!rows)
        {
            _error = "price_metric_rows_missing";
            return false;
        }

        do
        {
            Field* f = rows->Fetch();
            FullPriceMetric metric;
            metric.ItemEntry = f[0].Get<std::uint32_t>();
            metric.Status = f[1].Get<std::string>();
            metric.Active = f[2].Get<std::uint8_t>() != 0;
            metric.RecommendedStack = f[3].Get<std::uint32_t>();
            metric.ReferenceUnitCopper = f[4].Get<std::uint64_t>();
            metric.ModeledMinUnitCopper = f[5].Get<std::uint64_t>();
            metric.ModeledMaxUnitCopper = f[6].Get<std::uint64_t>();

            if (metric.ItemEntry == 0 || metric.RecommendedStack == 0 ||
                metric.ReferenceUnitCopper == 0 || metric.ModeledMinUnitCopper == 0 ||
                metric.ModeledMaxUnitCopper < metric.ModeledMinUnitCopper ||
                _rows.contains(metric.ItemEntry))
            {
                _error = "price_metric_row_invalid";
                return false;
            }

            ItemTemplate const* item = sObjectMgr->GetItemTemplate(metric.ItemEntry);
            metric.AuctionSafe = item &&
                !item->HasFlag(ITEM_FLAG_CONJURED) &&
                !item->HasFlag(ITEM_FLAG_IS_BOUND_TO_ACCOUNT) &&
                item->GetMaxStackSize() >= metric.RecommendedStack &&
                item->MaxCount <= 0 &&
                (item->Bonding == NO_BIND || item->Bonding == BIND_WHEN_EQUIPPED ||
                    item->Bonding == BIND_WHEN_USE) &&
                item->Duration == 0 && item->MinMoneyLoot == 0 && item->MaxMoneyLoot == 0;

            _rows.emplace(metric.ItemEntry, std::move(metric));
        } while (rows->NextRow());

        if (_rows.size() != 14179)
        {
            _error = "price_metric_row_count_mismatch";
            return false;
        }

        for (auto const& [entry, metric] : _rows)
        {
            (void)entry;
            if (metric.Active && metric.Status == "Recommended")
            {
                ++_activeRecommended;
                if (metric.AuctionSafe)
                    _approved.push_back(&metric);
                else
                    ++_unsafeRecommended;
            }
        }

        if (_activeRecommended != 11600 || _approved.size() != 11599 ||
            _unsafeRecommended != 1)
        {
            _error = "price_metric_approval_count_mismatch";
            return false;
        }

        _ready = true;
        return true;
    }

    FullPriceMetric const* Find(std::uint32_t itemEntry) const
    {
        auto const found = _rows.find(itemEntry);
        return found == _rows.end() ? nullptr : &found->second;
    }

    bool Approved(std::uint32_t itemEntry) const
    {
        FullPriceMetric const* metric = Find(itemEntry);
        return metric && metric->Active && metric->Status == "Recommended" &&
            metric->AuctionSafe;
    }

    std::uint64_t ProcurementUnitPrice(std::uint32_t itemEntry) const
    {
        FullPriceMetric const* metric = Find(itemEntry);
        if (!metric || !Approved(itemEntry))
            return 0;
        return std::max(metric->ReferenceUnitCopper, metric->ModeledMinUnitCopper);
    }

    std::vector<FullPriceMetric const*> const& ApprovedRows() const { return _approved; }
    std::string const& Error() const { return _error; }
    std::size_t TotalRows() const { return _rows.size(); }
    std::size_t ActiveRecommendedRows() const { return _activeRecommended; }
    std::size_t UnsafeRecommendedRows() const { return _unsafeRecommended; }

private:
    bool _attempted = false;
    bool _ready = false;
    std::string _error;
    std::map<std::uint32_t, FullPriceMetric> _rows;
    std::vector<FullPriceMetric const*> _approved;
    std::size_t _activeRecommended = 0;
    std::size_t _unsafeRecommended = 0;
};
}

#endif
