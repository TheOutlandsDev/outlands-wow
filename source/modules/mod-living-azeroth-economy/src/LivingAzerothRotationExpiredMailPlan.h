#ifndef LIVING_AZEROTH_ROTATION_EXPIRED_MAIL_PLAN_H
#define LIVING_AZEROTH_ROTATION_EXPIRED_MAIL_PLAN_H

#include "LivingAzerothExpiredMailPlan.h"

namespace LivingAzeroth::Economy
{
inline ExpiredMailPlan BuildRotationExpiredMailPlan(
    std::span<BusinessKey const> allowed,
    std::span<ExpiredMailObservation const> observed,
    std::uint32_t sellerGuid,
    std::uint32_t maxPerRun)
{
    ExpiredMailPlan plan;
    if (sellerGuid == 0)
    {
        plan.error = "seller_guid_required";
        return plan;
    }
    if (maxPerRun == 0 || maxPerRun > 11000)
    {
        plan.error = "max_per_run_out_of_bounds";
        return plan;
    }
    if (observed.size() > maxPerRun)
    {
        plan.error = "expired_mail_limit_exceeded";
        return plan;
    }

    std::set<std::tuple<std::uint8_t, std::uint32_t, std::uint32_t>> allowedKeys;
    for (BusinessKey const& key : allowed)
        allowedKeys.emplace(key.market, key.itemEntry, key.stack);
    if (allowedKeys.empty())
    {
        plan.error = "expired_mail_allowed_keys_empty";
        return plan;
    }

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
        if (!allowedKeys.contains({ row.market, row.itemEntry, row.stack }))
        {
            plan.error = "expired_mail_not_in_rotation_catalog";
            return plan;
        }
        if (!seenMailIds.insert(row.mailId).second ||
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
