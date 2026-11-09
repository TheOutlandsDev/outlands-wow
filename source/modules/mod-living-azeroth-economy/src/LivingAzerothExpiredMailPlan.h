/*
 * Living Azeroth deterministic expired-auction mail cleanup planner.
 */

#ifndef LIVING_AZEROTH_EXPIRED_MAIL_PLAN_H
#define LIVING_AZEROTH_EXPIRED_MAIL_PLAN_H

#include "LivingAzerothEconomyPlan.h"

#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace LivingAzeroth::Economy
{
struct ExpiredMailObservation
{
    std::uint32_t mailId = 0;
    std::uint32_t itemGuid = 0;
    std::uint8_t messageType = 0;
    std::uint8_t market = 0;
    std::uint32_t receiverGuid = 0;
    bool hasItems = false;
    std::uint32_t money = 0;
    std::uint32_t cod = 0;
    std::uint32_t itemEntry = 0;
    std::uint32_t stack = 0;
    std::uint32_t ownerGuid = 0;
    std::uint32_t mailItemCount = 0;
    std::uint32_t auctionReferences = 0;
    std::uint32_t inventoryReferences = 0;
    std::uint32_t subjectItemEntry = 0;
    std::uint32_t subjectZero = 0;
    std::uint32_t subjectResponse = 0;
    std::uint32_t subjectAuctionId = 0;
    std::uint32_t subjectStack = 0;
};

struct ExpiredMailCleanup
{
    std::uint32_t mailId = 0;
    std::uint32_t itemGuid = 0;
};

struct ExpiredMailPlan
{
    bool ok = false;
    std::string error;
    std::vector<ExpiredMailCleanup> cleanup;
};

inline ExpiredMailPlan BuildExpiredMailPlan(
    std::span<CatalogRow const> catalog,
    std::span<ExpiredMailObservation const> observed,
    std::uint32_t sellerGuid,
    std::uint32_t maxPerRun)
{
    ExpiredMailPlan plan;
    plan.error = ValidateCatalog(catalog);
    if (!plan.error.empty())
        return plan;

    if (sellerGuid == 0)
    {
        plan.error = "seller_guid_required";
        return plan;
    }

    if (maxPerRun == 0 || maxPerRun > 900)
    {
        plan.error = "max_per_run_out_of_bounds";
        return plan;
    }

    if (observed.size() > maxPerRun)
    {
        plan.error = "expired_mail_limit_exceeded";
        return plan;
    }

    std::set<std::tuple<std::uint8_t, std::uint32_t, std::uint32_t>> desired;
    for (std::uint8_t market : StaticMarkets)
        for (CatalogRow const& row : catalog)
            desired.emplace(market, row.itemEntry, row.stack);

    std::set<std::tuple<std::uint8_t, std::uint32_t, std::uint32_t>> seenKeys;
    std::set<std::uint32_t> seenMailIds;
    std::set<std::uint32_t> seenItemGuids;
    for (ExpiredMailObservation const& row : observed)
    {
        if (row.mailId == 0 || row.itemGuid == 0 || row.subjectAuctionId == 0)
        {
            plan.error = "expired_mail_identity_invalid";
            return plan;
        }

        if (row.messageType != 2 || row.receiverGuid != sellerGuid || !row.hasItems ||
            row.money != 0 || row.cod != 0 || row.ownerGuid != sellerGuid ||
            row.mailItemCount != 1 || row.auctionReferences != 0 ||
            row.inventoryReferences != 0)
        {
            plan.error = "expired_mail_shape_invalid";
            return plan;
        }

        if (row.subjectItemEntry != row.itemEntry || row.subjectZero != 0 ||
            row.subjectResponse != 3 || row.subjectStack != row.stack)
        {
            plan.error = "expired_mail_subject_invalid";
            return plan;
        }

        auto const key = std::make_tuple(row.market, row.itemEntry, row.stack);
        if (!desired.contains(key))
        {
            plan.error = "expired_mail_not_in_locked_catalog";
            return plan;
        }

        if (!seenKeys.insert(key).second || !seenMailIds.insert(row.mailId).second ||
            !seenItemGuids.insert(row.itemGuid).second)
        {
            plan.error = "expired_mail_duplicate";
            return plan;
        }

        plan.cleanup.push_back({ row.mailId, row.itemGuid });
    }

    plan.ok = true;
    return plan;
}
}

#endif
