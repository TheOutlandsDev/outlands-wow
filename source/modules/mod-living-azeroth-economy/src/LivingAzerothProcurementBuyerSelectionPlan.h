#ifndef LIVING_AZEROTH_PROCUREMENT_BUYER_SELECTION_PLAN_H
#define LIVING_AZEROTH_PROCUREMENT_BUYER_SELECTION_PLAN_H

#include "LivingAzerothGuildProcurementPlan.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>

namespace LivingAzeroth::Economy::GuildProcurement
{
struct BuyerSelectionCandidate
{
    std::uint32_t Guid = 0;
    std::uint32_t Account = 0;
    Faction CharacterFaction = Faction::Other;
    std::uint64_t Money = 0;
    bool OwnedAi = false;
    bool Loaded = false;
    bool SessionReady = false;
    bool InWorld = false;
    bool Teleporting = false;
    bool RandomBot = false;
};

enum class BuyerSelectionReason
{
    SelectedPreferred,
    SelectedFailover,
    NoEligibleBuyer
};

struct BuyerSelection
{
    bool Found = false;
    bool Failover = false;
    std::uint32_t Guid = 0;
    std::uint32_t Account = 0;
    BuyerSelectionReason Reason = BuyerSelectionReason::NoEligibleBuyer;
};

inline BuyerSelection SelectBuyer(
    std::uint32_t preferredGuid,
    Faction faction,
    std::uint32_t sellerAccount,
    std::uint64_t buyout,
    std::uint64_t minimumReserve,
    std::span<BuyerSelectionCandidate const> candidates)
{
    BuyerSelection selected;
    if (buyout > std::numeric_limits<std::uint64_t>::max() - minimumReserve)
        return selected;
    std::uint64_t const requiredMoney = buyout + minimumReserve;

    auto eligible = [&](BuyerSelectionCandidate const& candidate)
    {
        return candidate.Guid != 0 && candidate.Account != 0 &&
            candidate.CharacterFaction == faction && candidate.OwnedAi &&
            candidate.Loaded && candidate.SessionReady && candidate.InWorld &&
            !candidate.Teleporting && candidate.RandomBot &&
            candidate.Account != sellerAccount && candidate.Money >= requiredMoney;
    };

    auto preferred = std::find_if(candidates.begin(), candidates.end(),
        [&](BuyerSelectionCandidate const& candidate)
        {
            return candidate.Guid == preferredGuid && eligible(candidate);
        });
    if (preferred != candidates.end())
    {
        selected.Found = true;
        selected.Guid = preferred->Guid;
        selected.Account = preferred->Account;
        selected.Reason = BuyerSelectionReason::SelectedPreferred;
        return selected;
    }

    BuyerSelectionCandidate const* failover = nullptr;
    for (BuyerSelectionCandidate const& candidate : candidates)
    {
        if (!eligible(candidate) || (failover && failover->Guid <= candidate.Guid))
            continue;
        failover = &candidate;
    }
    if (!failover)
        return selected;

    selected.Found = true;
    selected.Failover = true;
    selected.Guid = failover->Guid;
    selected.Account = failover->Account;
    selected.Reason = BuyerSelectionReason::SelectedFailover;
    return selected;
}
}

#endif
