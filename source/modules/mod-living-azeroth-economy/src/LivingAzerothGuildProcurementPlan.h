#ifndef LIVING_AZEROTH_GUILD_PROCUREMENT_PLAN_H
#define LIVING_AZEROTH_GUILD_PROCUREMENT_PLAN_H

#include <cstdint>
#include <limits>
#include <span>

namespace LivingAzeroth::Economy::GuildProcurement
{
enum class Faction : std::uint8_t { Alliance, Horde, Neutral, Other };
enum class OwnerKind : std::uint8_t { Human, OwnedAi, OwnedSystem, Unknown };
enum class Reason : std::uint8_t
{
    ObserveOnlyApproved,
    FeatureDisabled,
    MutationModeRejected,
    ConfirmationMissing,
    UnsupportedFaction,
    InvalidAuction,
    SellerNotHuman,
    SellerOwnedAi,
    SellerOwnedSystem,
    OwnedSystemSellerDisabled,
    OwnedSystemSellerNotAllowlisted,
    OwnedSystemDailyPurchaseCap,
    SellerUnknown,
    SelfTrade,
    CatalogRejected,
    UnsafeItem,
    BidPresent,
    DuplicateAuction,
    MissingReferencePrice,
    PurchasePriceCeiling,
    FactionDailyPurchaseCap,
    FactionDailySpendCap,
    SellerDailyPurchaseCap,
    FactionCycleCap
};

struct Policy
{
    bool Enabled = false;
    bool ObserveOnly = true;
    bool ConfirmationValid = false;
    bool RequireCatalogApproval = true;
    bool AllowOwnedSystemSellerAuctions = false;
    std::uint32_t CeilingPercent = 110;
    // Zero disables the hard count cap. Spend and timed-cycle limits remain.
    std::uint32_t MaxPurchasesPerFactionPerDay = 0;
    std::uint64_t MaxSpendPerFactionPerDay = 50000;
    // Zero disables the hard seller cap; selection still applies soft diversity.
    std::uint32_t MaxPurchasesPerSellerPerDay = 0;
    std::uint32_t MaxOwnedSystemPurchasesPerFactionPerDay = 20;
    std::uint32_t MaxProposalsPerFactionPerCycle = 1;
};

struct Observation
{
    Faction MarketFaction = Faction::Other;
    OwnerKind SellerKind = OwnerKind::Unknown;
    bool CatalogApproved = false;
    bool SafeItem = false;
    bool HasBid = false;
    bool DuplicateAuction = false;
    bool SellerSystemAllowlisted = false;
    std::uint32_t BuyerAccount = 0;
    std::uint32_t SellerAccount = 0;
    std::uint32_t SellerGuid = 0;
    std::uint32_t ItemCount = 0;
    std::uint32_t FactionPurchasesToday = 0;
    std::uint32_t SellerPurchasesToday = 0;
    std::uint32_t OwnedSystemPurchasesToday = 0;
    std::uint32_t FactionProposalsThisCycle = 0;
    std::uint64_t ReferenceUnitPrice = 0;
    std::uint64_t Buyout = 0;
    std::uint64_t FactionSpendToday = 0;
};

struct Decision
{
    bool WouldPropose = false;
    bool MayMutate = false;
    Reason Why = Reason::FeatureDisabled;
    std::uint64_t ReferenceLotPrice = 0;
    std::uint64_t CeilingPrice = 0;
};

struct Candidate
{
    std::uint32_t AuctionId = 0;
    Observation Observed;
};

struct Selection
{
    bool Found = false;
    std::uint32_t AuctionId = 0;
    Decision Planned;
};

constexpr std::uint64_t SaturatingMultiply(std::uint64_t left, std::uint64_t right)
{
    if (left == 0 || right == 0)
        return 0;
    if (left > std::numeric_limits<std::uint64_t>::max() / right)
        return std::numeric_limits<std::uint64_t>::max();
    return left * right;
}

constexpr std::uint64_t PercentOf(std::uint64_t value, std::uint32_t percent)
{
    return SaturatingMultiply(value, percent) / 100;
}

inline Decision Plan(Policy const& policy, Observation const& observed)
{
    if (!policy.Enabled)
        return { false, false, Reason::FeatureDisabled };
    if (!policy.ObserveOnly)
        return { false, false, Reason::MutationModeRejected };
    if (!policy.ConfirmationValid)
        return { false, false, Reason::ConfirmationMissing };
    if (observed.MarketFaction != Faction::Alliance && observed.MarketFaction != Faction::Horde)
        return { false, false, Reason::UnsupportedFaction };
    if (observed.SellerGuid == 0 || observed.ItemCount == 0 || observed.Buyout == 0)
        return { false, false, Reason::InvalidAuction };
    if (observed.SellerKind == OwnerKind::OwnedAi)
        return { false, false, Reason::SellerOwnedAi };
    if (observed.SellerKind == OwnerKind::OwnedSystem)
    {
        if (!policy.AllowOwnedSystemSellerAuctions)
            return { false, false, Reason::OwnedSystemSellerDisabled };
        if (!observed.SellerSystemAllowlisted)
            return { false, false, Reason::OwnedSystemSellerNotAllowlisted };
        if (observed.OwnedSystemPurchasesToday >=
            policy.MaxOwnedSystemPurchasesPerFactionPerDay)
            return { false, false, Reason::OwnedSystemDailyPurchaseCap };
    }
    if (observed.SellerKind == OwnerKind::Unknown)
        return { false, false, Reason::SellerUnknown };
    if (observed.SellerKind != OwnerKind::Human &&
        observed.SellerKind != OwnerKind::OwnedSystem)
        return { false, false, Reason::SellerNotHuman };
    if (observed.BuyerAccount != 0 && observed.BuyerAccount == observed.SellerAccount)
        return { false, false, Reason::SelfTrade };
    if (policy.RequireCatalogApproval && !observed.CatalogApproved)
        return { false, false, Reason::CatalogRejected };
    if (!observed.SafeItem)
        return { false, false, Reason::UnsafeItem };
    if (observed.HasBid)
        return { false, false, Reason::BidPresent };
    if (observed.DuplicateAuction)
        return { false, false, Reason::DuplicateAuction };
    if (observed.ReferenceUnitPrice == 0)
        return { false, false, Reason::MissingReferencePrice };

    std::uint64_t const referenceLot = SaturatingMultiply(
        observed.ReferenceUnitPrice, observed.ItemCount);
    std::uint64_t const ceiling = PercentOf(referenceLot, policy.CeilingPercent);
    if (observed.Buyout > ceiling)
        return { false, false, Reason::PurchasePriceCeiling, referenceLot, ceiling };
    if (policy.MaxPurchasesPerFactionPerDay != 0 &&
        observed.FactionPurchasesToday >= policy.MaxPurchasesPerFactionPerDay)
        return { false, false, Reason::FactionDailyPurchaseCap, referenceLot, ceiling };
    if (observed.FactionSpendToday > policy.MaxSpendPerFactionPerDay ||
        observed.Buyout > policy.MaxSpendPerFactionPerDay - observed.FactionSpendToday)
        return { false, false, Reason::FactionDailySpendCap, referenceLot, ceiling };
    if (policy.MaxPurchasesPerSellerPerDay != 0 &&
        observed.SellerKind == OwnerKind::Human &&
        observed.SellerPurchasesToday >= policy.MaxPurchasesPerSellerPerDay)
        return { false, false, Reason::SellerDailyPurchaseCap, referenceLot, ceiling };
    if (observed.FactionProposalsThisCycle >= policy.MaxProposalsPerFactionPerCycle)
        return { false, false, Reason::FactionCycleCap, referenceLot, ceiling };

    return { true, false, Reason::ObserveOnlyApproved, referenceLot, ceiling };
}

inline Selection Select(Policy const& policy, std::span<Candidate const> candidates)
{
    Selection selected;
    std::uint64_t selectedScore = 0;
    std::uint64_t selectedBuyout = 0;
    for (Candidate const& candidate : candidates)
    {
        Decision const decision = Plan(policy, candidate.Observed);
        if (!decision.WouldPropose)
            continue;
        std::uint32_t const diversityCount = candidate.Observed.SellerPurchasesToday > 20 ?
            20 : candidate.Observed.SellerPurchasesToday;
        std::uint64_t const score = SaturatingMultiply(candidate.Observed.Buyout,
            100 + static_cast<std::uint64_t>(diversityCount) * 5);
        if (!selected.Found || score < selectedScore ||
            (score == selectedScore && candidate.Observed.Buyout < selectedBuyout) ||
            (score == selectedScore && candidate.Observed.Buyout == selectedBuyout &&
                candidate.AuctionId < selected.AuctionId))
        {
            selected = { true, candidate.AuctionId, decision };
            selectedScore = score;
            selectedBuyout = candidate.Observed.Buyout;
        }
    }
    return selected;
}
}

#endif
