/*
 * Living Azeroth AI4 deterministic auction desired-state planner.
 */

#ifndef LIVING_AZEROTH_ECONOMY_PLAN_H
#define LIVING_AZEROTH_ECONOMY_PLAN_H

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace LivingAzeroth::Economy
{
struct CatalogRow
{
    std::string_view band;
    std::uint32_t itemEntry;
    std::string_view itemName;
    std::uint32_t stack;
    std::uint32_t startBid;
    std::uint32_t buyout;
};

struct BusinessKey
{
    std::uint8_t market;
    std::uint32_t itemEntry;
    std::uint32_t stack;
    std::uint32_t startBid;
    std::uint32_t buyout;

    bool operator==(BusinessKey const&) const = default;

    bool operator<(BusinessKey const& other) const
    {
        return std::tie(market, itemEntry, stack, startBid, buyout) <
            std::tie(other.market, other.itemEntry, other.stack, other.startBid, other.buyout);
    }
};

struct ObservedAuction
{
    BusinessKey key;
    bool hasBid = false;
    std::uint32_t auctionId = 0;
};

struct ReconcilePlan
{
    bool ok = false;
    std::string error;
    std::size_t desiredCount = 0;
    std::size_t existingCount = 0;
    std::vector<BusinessKey> creates;
    std::vector<std::uint32_t> retirementCandidates;
    std::vector<std::uint32_t> retires;
};

inline constexpr std::array<std::uint8_t, 3> StaticMarkets = { 2, 6, 7 };

inline constexpr std::array<CatalogRow, 300> StaticCatalog = {{
#include "LivingAzerothStaticCatalog.inc"
}};

inline constexpr std::string_view StaticCatalogSha256 =
    "E442EC7840045A1271B1B350EFC883B6B5C4AB4E2A4FBD75C85C3B28B6752BBD";

inline constexpr bool StaticCatalogContains(std::uint32_t itemEntry)
{
    for (CatalogRow const& row : StaticCatalog)
        if (row.itemEntry == itemEntry)
            return true;
    return false;
}

inline constexpr std::uint64_t StaticCatalogUnitPrice(std::uint32_t itemEntry)
{
    for (CatalogRow const& row : StaticCatalog)
        if (row.itemEntry == itemEntry && row.stack != 0)
            return row.buyout / row.stack;
    return 0;
}

inline std::string ValidateCatalog(std::span<CatalogRow const> catalog)
{
    if (catalog.size() != 300)
        return "catalog_row_count_must_equal_300";

    std::set<std::uint32_t> entries;
    std::map<std::string_view, std::size_t> bands;
    std::map<std::string_view, std::size_t> const expectedBands = {
        { "Bags", 25 }, { "Cloth", 20 }, { "Leather", 20 },
        { "MetalStone", 30 }, { "Herbs", 30 }, { "Enchanting", 25 },
        { "Elemental", 25 }, { "Engineering", 25 }, { "Jewelcrafting", 30 },
        { "Recipes", 30 }, { "Consumables", 25 }, { "Cooking", 15 }
    };

    for (CatalogRow const& row : catalog)
    {
        if (row.itemEntry == 0 || row.itemName.empty())
            return "catalog_item_identity_invalid";

        if (!entries.insert(row.itemEntry).second)
            return "catalog_item_entry_duplicate";

        if (!expectedBands.contains(row.band))
            return "catalog_band_invalid";

        ++bands[row.band];

        if (row.stack == 0)
            return "catalog_stack_must_be_positive";

        if (row.startBid == 0 || row.startBid >= row.buyout)
            return "catalog_price_order_invalid";
    }

    for (auto const& [band, expected] : expectedBands)
    {
        if (bands[band] != expected)
            return "catalog_band_count_mismatch";
    }

    return {};
}

inline std::vector<BusinessKey> BuildDesiredMatrix(std::span<CatalogRow const> catalog)
{
    std::vector<BusinessKey> desired;
    desired.reserve(catalog.size() * StaticMarkets.size());

    for (std::uint8_t market : StaticMarkets)
    {
        for (CatalogRow const& row : catalog)
        {
            desired.push_back({
                market,
                row.itemEntry,
                row.stack,
                row.startBid,
                row.buyout
            });
        }
    }

    return desired;
}

inline ReconcilePlan BuildReconcilePlan(
    std::span<CatalogRow const> catalog,
    std::span<ObservedAuction const> observed)
{
    ReconcilePlan plan;
    plan.error = ValidateCatalog(catalog);
    if (!plan.error.empty())
        return plan;

    std::vector<BusinessKey> desired = BuildDesiredMatrix(catalog);
    std::set<BusinessKey> desiredKeys(desired.begin(), desired.end());
    plan.desiredCount = desiredKeys.size();

    if (desired.size() != 900 || desiredKeys.size() != 900)
    {
        plan.error = "desired_matrix_must_have_900_unique_keys";
        return plan;
    }

    std::set<BusinessKey> observedKeys;
    for (ObservedAuction const& auction : observed)
    {
        if (auction.hasBid)
        {
            plan.error = "synthetic_auction_has_bid";
            return plan;
        }

        if (!desiredKeys.contains(auction.key))
        {
            plan.error = "unexpected_synthetic_auction";
            return plan;
        }

        if (!observedKeys.insert(auction.key).second)
        {
            plan.error = "duplicate_synthetic_business_key";
            return plan;
        }
    }

    plan.existingCount = observedKeys.size();
    for (BusinessKey const& key : desired)
    {
        if (!observedKeys.contains(key))
            plan.creates.push_back(key);
    }

    plan.ok = true;
    return plan;
}
}

#endif
