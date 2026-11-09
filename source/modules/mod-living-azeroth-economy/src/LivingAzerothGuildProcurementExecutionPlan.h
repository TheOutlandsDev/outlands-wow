#ifndef LIVING_AZEROTH_GUILD_PROCUREMENT_EXECUTION_PLAN_H
#define LIVING_AZEROTH_GUILD_PROCUREMENT_EXECUTION_PLAN_H

#include "LivingAzerothGuildProcurementPlan.h"

#include <cstdint>

namespace LivingAzeroth::Economy::GuildProcurement
{
enum class ExecutionReason : std::uint8_t
{
    Approved,
    FeatureDisabled,
    MutationModeDisabled,
    ConfirmationMissing,
    SchemaUnavailable,
    CatalogHashMismatch,
    InvalidLimits,
    ProposalRejected,
    UnsupportedFaction,
    BuyerNotConfigured,
    BuyerNotOwnedAi,
    BuyerOffline,
    BuyerNotAtAuctioneer,
    SelfTrade,
    InvalidAuction,
    BidPresent,
    AuctionAlreadyClaimed,
    BootMutationCap,
    FactionCycleCap,
    FactionDailyPurchaseCap,
    FactionDailySpendCap,
    SellerDailyPurchaseCap,
    OwnedSystemSellerDisabled,
    OwnedSystemConfirmationMissing,
    OwnedSystemSellerNotAllowlisted,
    OwnedSystemDailyPurchaseCap,
    InsufficientFunds,
    ReserveViolation
};

struct ExecutionPolicy
{
    bool Enabled = false;
    bool MutationMode = false;
    bool ConfirmationValid = false;
    bool SchemaReady = false;
    bool CatalogHashValid = false;
    bool AllowOwnedSystemSellerAuctions = false;
    bool OwnedSystemConfirmationValid = false;
    std::uint32_t MaxPurchasesPerFactionPerDay = 0;
    std::uint64_t MaxSpendPerFactionPerDay = 50000;
    std::uint32_t MaxPurchasesPerSellerPerDay = 0;
    std::uint32_t MaxOwnedSystemPurchasesPerFactionPerDay = 20;
    std::uint32_t MaxPurchasesPerFactionPerCycle = 3;
    std::uint32_t MaxMutationsPerBoot = 2;
    std::uint64_t MinimumBuyerReserve = 1000;
};

struct ExecutionObservation
{
    bool ProposalApproved = false;
    Faction MarketFaction = Faction::Other;
    bool BuyerConfigured = false;
    bool BuyerOwnedAi = false;
    bool BuyerOnline = false;
    bool BuyerAtAuctioneer = false;
    bool HasBid = false;
    bool AuctionClaimed = false;
    OwnerKind SellerKind = OwnerKind::Unknown;
    bool SellerSystemAllowlisted = false;
    std::uint32_t BuyerAccount = 0;
    std::uint32_t SellerAccount = 0;
    std::uint32_t SellerGuid = 0;
    std::uint32_t ItemCount = 0;
    std::uint32_t MutationsThisBoot = 0;
    std::uint32_t PurchasesThisFactionCycle = 0;
    std::uint32_t FactionPurchasesToday = 0;
    std::uint32_t SellerPurchasesToday = 0;
    std::uint32_t OwnedSystemPurchasesToday = 0;
    std::uint64_t FactionSpendToday = 0;
    std::uint64_t Buyout = 0;
    std::uint64_t BuyerMoney = 0;
};

struct ExecutionDecision
{
    bool WouldDispatch = false;
    bool MayMutate = false;
    ExecutionReason Why = ExecutionReason::FeatureDisabled;
};

constexpr bool ValidExecutionLimits(ExecutionPolicy const& policy)
{
    return policy.MaxPurchasesPerFactionPerDay <= 120 &&
        policy.MaxSpendPerFactionPerDay >= 1 &&
        policy.MaxSpendPerFactionPerDay <= 600000 &&
        policy.MaxPurchasesPerSellerPerDay <= 100 &&
        policy.MaxOwnedSystemPurchasesPerFactionPerDay >= 1 &&
        policy.MaxOwnedSystemPurchasesPerFactionPerDay <= 30 &&
        policy.MaxPurchasesPerFactionPerCycle >= 1 &&
        policy.MaxPurchasesPerFactionPerCycle <= 3 &&
        policy.MaxMutationsPerBoot >= 1 &&
        policy.MaxMutationsPerBoot <= 120 &&
        policy.MinimumBuyerReserve <= 100000;
}

inline ExecutionDecision PlanExecution(ExecutionPolicy const& policy,
    ExecutionObservation const& observed)
{
    if (!policy.Enabled)
        return { false, false, ExecutionReason::FeatureDisabled };
    if (!policy.MutationMode)
        return { false, false, ExecutionReason::MutationModeDisabled };
    if (!policy.ConfirmationValid)
        return { false, false, ExecutionReason::ConfirmationMissing };
    if (!policy.SchemaReady)
        return { false, false, ExecutionReason::SchemaUnavailable };
    if (!policy.CatalogHashValid)
        return { false, false, ExecutionReason::CatalogHashMismatch };
    if (!ValidExecutionLimits(policy))
        return { false, false, ExecutionReason::InvalidLimits };
    if (!observed.ProposalApproved)
        return { false, false, ExecutionReason::ProposalRejected };
    if (observed.MarketFaction != Faction::Alliance &&
        observed.MarketFaction != Faction::Horde)
        return { false, false, ExecutionReason::UnsupportedFaction };
    if (!observed.BuyerConfigured)
        return { false, false, ExecutionReason::BuyerNotConfigured };
    if (!observed.BuyerOwnedAi)
        return { false, false, ExecutionReason::BuyerNotOwnedAi };
    if (!observed.BuyerOnline)
        return { false, false, ExecutionReason::BuyerOffline };
    if (!observed.BuyerAtAuctioneer)
        return { false, false, ExecutionReason::BuyerNotAtAuctioneer };
    if (observed.BuyerAccount == 0 || observed.SellerAccount == 0 ||
        observed.SellerGuid == 0 || observed.ItemCount == 0 || observed.Buyout == 0)
        return { false, false, ExecutionReason::InvalidAuction };
    if (observed.BuyerAccount == observed.SellerAccount)
        return { false, false, ExecutionReason::SelfTrade };
    if (observed.SellerKind == OwnerKind::OwnedAi ||
        observed.SellerKind == OwnerKind::Unknown)
        return { false, false, ExecutionReason::ProposalRejected };
    if (observed.SellerKind == OwnerKind::OwnedSystem)
    {
        if (!policy.AllowOwnedSystemSellerAuctions)
            return { false, false, ExecutionReason::OwnedSystemSellerDisabled };
        if (!policy.OwnedSystemConfirmationValid)
            return { false, false, ExecutionReason::OwnedSystemConfirmationMissing };
        if (!observed.SellerSystemAllowlisted)
            return { false, false, ExecutionReason::OwnedSystemSellerNotAllowlisted };
        if (observed.OwnedSystemPurchasesToday >=
            policy.MaxOwnedSystemPurchasesPerFactionPerDay)
            return { false, false, ExecutionReason::OwnedSystemDailyPurchaseCap };
    }
    else if (observed.SellerKind != OwnerKind::Human)
        return { false, false, ExecutionReason::ProposalRejected };
    if (observed.HasBid)
        return { false, false, ExecutionReason::BidPresent };
    if (observed.AuctionClaimed)
        return { false, false, ExecutionReason::AuctionAlreadyClaimed };
    if (observed.MutationsThisBoot >= policy.MaxMutationsPerBoot)
        return { false, false, ExecutionReason::BootMutationCap };
    if (observed.PurchasesThisFactionCycle >= policy.MaxPurchasesPerFactionPerCycle)
        return { false, false, ExecutionReason::FactionCycleCap };
    if (policy.MaxPurchasesPerFactionPerDay != 0 &&
        observed.FactionPurchasesToday >= policy.MaxPurchasesPerFactionPerDay)
        return { false, false, ExecutionReason::FactionDailyPurchaseCap };
    if (observed.FactionSpendToday > policy.MaxSpendPerFactionPerDay ||
        observed.Buyout > policy.MaxSpendPerFactionPerDay - observed.FactionSpendToday)
        return { false, false, ExecutionReason::FactionDailySpendCap };
    if (policy.MaxPurchasesPerSellerPerDay != 0 &&
        observed.SellerKind == OwnerKind::Human &&
        observed.SellerPurchasesToday >= policy.MaxPurchasesPerSellerPerDay)
        return { false, false, ExecutionReason::SellerDailyPurchaseCap };
    if (observed.BuyerMoney < observed.Buyout)
        return { false, false, ExecutionReason::InsufficientFunds };
    if (observed.BuyerMoney - observed.Buyout < policy.MinimumBuyerReserve)
        return { false, false, ExecutionReason::ReserveViolation };

    return { true, true, ExecutionReason::Approved };
}
}

#endif
