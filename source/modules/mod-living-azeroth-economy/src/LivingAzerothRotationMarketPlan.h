#ifndef LIVING_AZEROTH_ROTATION_MARKET_PLAN_H
#define LIVING_AZEROTH_ROTATION_MARKET_PLAN_H

#include "LivingAzerothEconomyPlan.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace LivingAzeroth::Economy
{
struct RotationMetric
{
    std::uint32_t itemEntry = 0;
    std::uint32_t stack = 0;
    std::uint64_t referenceUnit = 0;
    std::uint64_t modeledMinUnit = 0;
    std::uint64_t modeledMaxUnit = 0;
    bool professionItem = false;
    // 0 = ordinary/base coverage, 1 = stackable profession commodity,
    // 2 = low-tier stackable profession commodity.
    std::uint8_t densityClass = 0;
};

struct RotationPlan
{
    bool ok = false;
    std::string error;
    std::size_t selectedItems = 0;
    std::size_t allianceListings = 0;
    std::size_t hordeListings = 0;
    std::size_t neutralListings = 0;
    std::vector<BusinessKey> desired;
};

inline std::uint64_t RotationScore(std::uint32_t entry, std::uint64_t epoch)
{
    std::uint64_t value = static_cast<std::uint64_t>(entry) ^
        (epoch + 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

inline std::uint32_t CheckedPrice(std::uint64_t unit, std::uint32_t stack)
{
    if (unit == 0 || stack == 0 || unit > std::numeric_limits<std::uint32_t>::max() / stack)
        return 0;
    return static_cast<std::uint32_t>(unit * stack);
}

inline BusinessKey MakeRotationKey(
    std::uint8_t market, RotationMetric const& row, std::uint32_t variant)
{
    std::uint32_t stack = row.stack;
    std::uint64_t unit = std::clamp(
        std::max(row.referenceUnit, row.modeledMinUnit),
        row.modeledMinUnit,
        row.modeledMaxUnit);
    if (variant == 1)
    {
        if (stack > 1)
            stack = std::max<std::uint32_t>(1, stack / 2);
        else
            unit = std::min(row.modeledMaxUnit, unit + std::max<std::uint64_t>(1, unit / 20));
    }

    std::uint32_t buyout = CheckedPrice(unit, stack);
    if (variant == 1 && row.stack == 1 && buyout == CheckedPrice(
            std::clamp(std::max(row.referenceUnit, row.modeledMinUnit),
                row.modeledMinUnit, row.modeledMaxUnit), row.stack))
    {
        if (buyout < std::numeric_limits<std::uint32_t>::max())
            ++buyout;
    }
    std::uint32_t startBid = buyout > 1 ? std::max<std::uint32_t>(1, buyout * 9 / 10) : 0;
    return { market, row.itemEntry, stack, startBid, buyout };
}

inline std::vector<BusinessKey> MakeDenseRotationKeys(
    std::uint8_t market, RotationMetric const& row, std::size_t requested)
{
    std::vector<std::uint32_t> stacks;
    for (std::uint32_t value : {
        1U,
        std::max<std::uint32_t>(1, (row.stack + 3) / 4),
        std::max<std::uint32_t>(1, (row.stack + 1) / 2),
        std::max<std::uint32_t>(1, (row.stack * 3 + 3) / 4),
        row.stack })
    {
        if (std::find(stacks.begin(), stacks.end(), value) == stacks.end())
            stacks.push_back(value);
    }

    std::uint64_t const baseUnit = std::clamp(
        std::max(row.referenceUnit, row.modeledMinUnit),
        row.modeledMinUnit,
        row.modeledMaxUnit);
    std::vector<std::uint64_t> units;
    for (std::uint32_t percent : { 0U, 2U, 5U, 8U })
    {
        std::uint64_t const increase = percent == 0 ? 0 :
            std::max<std::uint64_t>(1, baseUnit * percent / 100);
        std::uint64_t const value = std::min(row.modeledMaxUnit, baseUnit + increase);
        if (std::find(units.begin(), units.end(), value) == units.end())
            units.push_back(value);
    }

    std::vector<BusinessKey> result;
    std::set<BusinessKey> unique;
    for (std::uint32_t bidPercent : { 80U, 85U, 90U, 95U })
    {
        for (std::uint64_t unit : units)
        {
            for (std::uint32_t stack : stacks)
            {
                std::uint32_t const buyout = CheckedPrice(unit, stack);
                std::uint32_t const startBid = buyout > 1 ?
                    std::max<std::uint32_t>(1, buyout * bidPercent / 100) : 0;
                BusinessKey const key{ market, row.itemEntry, stack, startBid, buyout };
                if (buyout > 1 && startBid < buyout && unique.insert(key).second)
                    result.push_back(key);
                if (result.size() == requested)
                    return result;
            }
        }
    }
    result.clear();
    return result;
}

inline constexpr std::size_t RotationExpirationBucketCount = 96;
inline constexpr std::uint32_t RotationExpirationBaseSeconds = 24U * 60U * 60U;
inline constexpr std::uint32_t RotationExpirationBucketSeconds = 15U * 60U;

inline constexpr std::uint32_t RotationExpirationSeconds(std::size_t desiredIndex)
{
    return RotationExpirationBaseSeconds +
        static_cast<std::uint32_t>(desiredIndex % RotationExpirationBucketCount) *
            RotationExpirationBucketSeconds;
}

inline RotationPlan BuildRotationMarketPlan(
    std::span<RotationMetric const> metrics,
    std::uint32_t tier,
    std::uint64_t epoch)
{
    RotationPlan plan;
    std::size_t selected = tier == 3000 ? 1200 : tier == 6000 ? 2400 :
        tier == 9000 ? 7200 : tier == 11000 ? 9000 : 0;
    std::size_t secondary = tier == 3000 ? 300 : tier == 6000 ? 600 :
        tier == 9000 ? 900 : 1000;
    if (selected == 0)
    {
        plan.error = "listing_tier_must_equal_3000_6000_9000_or_11000";
        return plan;
    }

    std::set<std::uint32_t> entries;
    std::vector<RotationMetric> ordered(metrics.begin(), metrics.end());
    for (RotationMetric const& row : ordered)
    {
        if (row.itemEntry == 0 || row.stack == 0 || row.referenceUnit == 0 ||
            row.modeledMinUnit == 0 || row.modeledMaxUnit < row.modeledMinUnit ||
            row.densityClass > 2 || (!row.professionItem && row.densityClass != 0) ||
            !entries.insert(row.itemEntry).second)
        {
            plan.error = "rotation_metric_invalid";
            return plan;
        }
    }
    if (ordered.size() < selected)
    {
        plan.error = "rotation_metric_coverage_insufficient";
        return plan;
    }

    std::sort(ordered.begin(), ordered.end(), [epoch](auto const& left, auto const& right)
    {
        auto const l = std::pair{ RotationScore(left.itemEntry, epoch), left.itemEntry };
        auto const r = std::pair{ RotationScore(right.itemEntry, epoch), right.itemEntry };
        return l < r;
    });

    if (tier == 11000)
    {
        constexpr std::size_t AllianceTarget = 9000;
        constexpr std::size_t SecondaryTarget = 1000;
        std::size_t const professionCount = static_cast<std::size_t>(std::count_if(
            ordered.begin(), ordered.end(), [](RotationMetric const& row)
            {
                return row.professionItem;
            }));
        std::size_t const lowCount = static_cast<std::size_t>(std::count_if(
            ordered.begin(), ordered.end(), [](RotationMetric const& row)
            {
                return row.densityClass == 2;
            }));
        std::size_t const otherCount = static_cast<std::size_t>(std::count_if(
            ordered.begin(), ordered.end(), [](RotationMetric const& row)
            {
                return row.densityClass == 1;
            }));
        if (professionCount != 4332 || lowCount != 284 || otherCount != 825)
        {
            plan.error = "dense_profession_catalog_mismatch";
            return plan;
        }

        plan.desired.reserve(tier);
        std::set<std::uint32_t> allianceItems;
        for (RotationMetric const& row : ordered)
        {
            if (!row.professionItem)
                continue;
            std::size_t const count = row.densityClass == 2 ? 8 :
                row.densityClass == 1 ? 4 : 1;
            if (count == 1)
                plan.desired.push_back(MakeRotationKey(2, row, 0));
            else
            {
                std::vector<BusinessKey> keys = MakeDenseRotationKeys(2, row, count);
                if (keys.size() != count)
                {
                    plan.error = "dense_rotation_variants_insufficient";
                    return plan;
                }
                plan.desired.insert(plan.desired.end(), keys.begin(), keys.end());
            }
            allianceItems.insert(row.itemEntry);
        }
        for (RotationMetric const& row : ordered)
        {
            if (plan.desired.size() >= AllianceTarget)
                break;
            if (!row.professionItem)
            {
                plan.desired.push_back(MakeRotationKey(2, row, 0));
                allianceItems.insert(row.itemEntry);
            }
        }
        if (plan.desired.size() != AllianceTarget || ordered.size() < SecondaryTarget)
        {
            plan.error = "dense_profession_market_capacity_insufficient";
            return plan;
        }
        for (std::size_t index = 0; index < SecondaryTarget; ++index)
        {
            plan.desired.push_back(MakeRotationKey(6, ordered[index], 0));
            plan.desired.push_back(MakeRotationKey(7, ordered[index], 0));
        }

        std::set<BusinessKey> unique(plan.desired.begin(), plan.desired.end());
        for (BusinessKey const& key : plan.desired)
        {
            if (key.stack == 0 || key.startBid == 0 || key.startBid >= key.buyout)
            {
                plan.error = "rotation_business_key_invalid";
                return plan;
            }
            plan.allianceListings += key.market == 2;
            plan.hordeListings += key.market == 6;
            plan.neutralListings += key.market == 7;
        }
        if (plan.desired.size() != tier || unique.size() != tier ||
            plan.allianceListings != AllianceTarget ||
            plan.hordeListings != SecondaryTarget ||
            plan.neutralListings != SecondaryTarget)
        {
            plan.error = "dense_profession_rotation_distribution_mismatch";
            return plan;
        }
        plan.selectedItems = allianceItems.size();
        plan.ok = true;
        return plan;
    }

    if (tier == 9000)
    {
        constexpr std::size_t AllianceTarget = 7200;
        constexpr std::size_t SecondaryTarget = 900;
        std::size_t const professionCount = static_cast<std::size_t>(std::count_if(
            ordered.begin(), ordered.end(), [](RotationMetric const& row)
            {
                return row.professionItem;
            }));
        if (professionCount == 0 || professionCount > AllianceTarget)
        {
            plan.error = "profession_metric_coverage_invalid";
            return plan;
        }

        plan.desired.reserve(tier);
        for (RotationMetric const& row : ordered)
            if (row.professionItem)
                plan.desired.push_back(MakeRotationKey(2, row, 0));
        for (RotationMetric const& row : ordered)
        {
            if (plan.desired.size() >= AllianceTarget)
                break;
            if (!row.professionItem)
                plan.desired.push_back(MakeRotationKey(2, row, 0));
        }
        if (plan.desired.size() != AllianceTarget || ordered.size() < SecondaryTarget)
        {
            plan.error = "profession_market_capacity_insufficient";
            return plan;
        }
        for (std::size_t index = 0; index < SecondaryTarget; ++index)
        {
            plan.desired.push_back(MakeRotationKey(6, ordered[index], 0));
            plan.desired.push_back(MakeRotationKey(7, ordered[index], 0));
        }

        std::set<BusinessKey> unique(plan.desired.begin(), plan.desired.end());
        for (BusinessKey const& key : plan.desired)
        {
            if (key.stack == 0 || key.startBid == 0 || key.startBid >= key.buyout)
            {
                plan.error = "rotation_business_key_invalid";
                return plan;
            }
            plan.allianceListings += key.market == 2;
            plan.hordeListings += key.market == 6;
            plan.neutralListings += key.market == 7;
        }
        if (plan.desired.size() != tier || unique.size() != tier ||
            plan.allianceListings != AllianceTarget ||
            plan.hordeListings != SecondaryTarget ||
            plan.neutralListings != SecondaryTarget)
        {
            plan.error = "profession_rotation_distribution_mismatch";
            return plan;
        }
        plan.selectedItems = AllianceTarget;
        plan.ok = true;
        return plan;
    }

    ordered.resize(selected);

    plan.desired.reserve(tier);
    for (std::size_t index = 0; index < ordered.size(); ++index)
    {
        plan.desired.push_back(MakeRotationKey(2, ordered[index], 0));
        plan.desired.push_back(MakeRotationKey(2, ordered[index], 1));
        if (index < secondary)
        {
            plan.desired.push_back(MakeRotationKey(6, ordered[index], 0));
            plan.desired.push_back(MakeRotationKey(7, ordered[index], 0));
        }
    }

    std::set<BusinessKey> unique(plan.desired.begin(), plan.desired.end());
    for (BusinessKey const& key : plan.desired)
    {
        if (key.stack == 0 || key.startBid == 0 || key.startBid >= key.buyout)
        {
            plan.error = "rotation_business_key_invalid";
            return plan;
        }
        plan.allianceListings += key.market == 2;
        plan.hordeListings += key.market == 6;
        plan.neutralListings += key.market == 7;
    }
    if (plan.desired.size() != tier || unique.size() != tier ||
        plan.allianceListings != selected * 2 ||
        plan.hordeListings != secondary || plan.neutralListings != secondary)
    {
        plan.error = "rotation_distribution_mismatch";
        return plan;
    }

    plan.selectedItems = selected;
    plan.ok = true;
    return plan;
}

inline ReconcilePlan BuildRotationReconcilePlan(
    std::span<BusinessKey const> desired,
    std::span<ObservedAuction const> observed)
{
    ReconcilePlan plan;
    std::set<BusinessKey> desiredKeys(desired.begin(), desired.end());
    if (desired.empty() || desiredKeys.size() != desired.size())
    {
        plan.error = "rotation_desired_keys_invalid";
        return plan;
    }
    plan.desiredCount = desired.size();

    std::set<BusinessKey> observedDesired;
    for (ObservedAuction const& auction : observed)
    {
        if (!desiredKeys.contains(auction.key))
        {
            if (!auction.hasBid && auction.auctionId != 0)
                plan.retirementCandidates.push_back(auction.auctionId);
            continue;
        }
        if (!observedDesired.insert(auction.key).second)
        {
            plan.error = "duplicate_synthetic_business_key";
            return plan;
        }
    }

    plan.existingCount = observedDesired.size();
    for (BusinessKey const& key : desired)
        if (!observedDesired.contains(key))
            plan.creates.push_back(key);
    std::sort(plan.retirementCandidates.begin(), plan.retirementCandidates.end());
    plan.ok = true;
    return plan;
}

inline std::size_t ApplyLiveListingCeiling(
    ReconcilePlan& plan,
    std::size_t observedSellerListings,
    std::size_t maxLiveListings)
{
    if (!plan.ok || maxLiveListings < plan.desiredCount)
    {
        plan.ok = false;
        plan.error = "live_listing_ceiling_invalid";
        plan.creates.clear();
        plan.retires.clear();
        return 0;
    }

    std::size_t const headroom = observedSellerListings >= maxLiveListings ? 0 :
        maxLiveListings - observedSellerListings;
    if (plan.retires.size() > std::numeric_limits<std::size_t>::max() - headroom)
    {
        plan.ok = false;
        plan.error = "live_listing_ceiling_overflow";
        plan.creates.clear();
        plan.retires.clear();
        return 0;
    }
    std::size_t const available = headroom + plan.retires.size();
    if (plan.creates.size() <= available)
        return 0;

    std::size_t const deferred = plan.creates.size() - available;
    plan.creates.resize(available);
    return deferred;
}

inline std::size_t ApplyDesiredLiveListingTarget(
    ReconcilePlan& plan,
    std::size_t observedSellerListings)
{
    if (!plan.ok)
        return 0;

    plan.retires.clear();
    std::size_t const originalCreates = plan.creates.size();
    std::size_t const deficit = observedSellerListings >= plan.desiredCount ? 0 :
        plan.desiredCount - observedSellerListings;
    std::size_t const surplus = observedSellerListings > plan.desiredCount ?
        observedSellerListings - plan.desiredCount : 0;
    std::size_t const baseRetires = std::min(
        surplus, plan.retirementCandidates.size());
    std::size_t const swapCapacity =
        plan.retirementCandidates.size() - baseRetires;
    if (deficit > std::numeric_limits<std::size_t>::max() - swapCapacity)
    {
        plan.ok = false;
        plan.error = "desired_listing_target_overflow";
        plan.creates.clear();
        return originalCreates;
    }
    std::size_t const allowedCreates = std::min(
        originalCreates, deficit + swapCapacity);
    std::size_t const swapCreates = allowedCreates > deficit ?
        allowedCreates - deficit : 0;
    std::size_t const retireCount = baseRetires + swapCreates;

    plan.creates.resize(allowedCreates);
    plan.retires.assign(plan.retirementCandidates.begin(),
        plan.retirementCandidates.begin() + retireCount);
    return originalCreates - allowedCreates;
}
}

#endif