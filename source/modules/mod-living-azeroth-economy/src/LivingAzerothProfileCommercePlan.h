#ifndef LIVING_AZEROTH_PROFILE_COMMERCE_PLAN_H
#define LIVING_AZEROTH_PROFILE_COMMERCE_PLAN_H

#include <algorithm>
#include <cstdint>
#include <limits>

namespace LivingAzeroth::Economy::ProfileCommerce
{
enum class OwnerKind : std::uint8_t
{
    OwnedAi,
    OwnedSystem,
    Human,
    Unknown
};

enum class DecisionReason : std::uint8_t
{
    Approved,
    FeatureDisabled,
    ObserveOnly,
    NotOwnedAi,
    NotAtAuctioneer,
    CatalogRejected,
    UnsafeItem,
    ItemStillNeeded,
    MissingReferencePrice,
    ZeroPrice,
    BelowNormalPriceBand,
    AboveNormalPriceBand,
    ProfileListingCap,
    SellerCycleCap,
    SellerDepositUnaffordable,
    BootMutationCap,
    DuplicateAction,
    InvalidAuction,
    SelfTrade,
    ProtectedHumanAuction,
    UnownedSeller,
    PurchaseNotUseful,
    InsufficientFunds,
    ReserveViolation,
    PurchasePriceCeiling,
    DailyPurchaseCap,
    DailySpendCap,
    BuyerCycleCap
};

struct Policy
{
    bool Enabled = false;
    bool ObserveOnly = true;
    bool SellerEnabled = false;
    bool BuyerEnabled = false;
    bool AllowHumanAuctions = false;
    bool RequireCatalogApproval = true;
    std::uint32_t MinimumUnitPricePercent = 80;
    std::uint32_t MaximumUnitPricePercent = 120;
    std::uint32_t BuyerCeilingPercent = 110;
    std::uint32_t MaxListingsPerProfile = 3;
    std::uint32_t MaxSellerActionsPerCycle = 2;
    std::uint32_t MaxBuyerActionsPerCycle = 1;
    std::uint32_t MaxPurchasesPerProfilePerDay = 2;
    std::uint64_t MaxSpendPerProfilePerDay = 20000;
    std::uint64_t MinimumMoneyReserve = 1000;
    std::uint32_t MaxMutationsPerBoot = 0;
};

struct SellerObservation
{
    bool OwnedAi = false;
    bool AtAuctioneer = false;
    bool CatalogApproved = false;
    bool Tradable = false;
    bool Bound = false;
    bool QuestItem = false;
    bool Conjured = false;
    bool HasDuration = false;
    bool NeededByProfile = true;
    bool DuplicateAction = false;
    std::uint32_t ExistingListings = 0;
    std::uint32_t SellerActionsThisCycle = 0;
    std::uint32_t MutationsThisBoot = 0;
    std::uint64_t ReferenceUnitPrice = 0;
    std::uint64_t ProposedUnitPrice = 0;
    std::uint64_t Money = 0;
    std::uint64_t Deposit = 0;
};

struct BuyerObservation
{
    bool OwnedAi = false;
    bool AtAuctioneer = false;
    bool CatalogApproved = false;
    bool UsefulToProfile = false;
    bool DuplicateAction = false;
    OwnerKind SellerKind = OwnerKind::Unknown;
    std::uint32_t BuyerGuid = 0;
    std::uint32_t SellerGuid = 0;
    std::uint32_t BuyerAccount = 0;
    std::uint32_t SellerAccount = 0;
    std::uint32_t ItemCount = 0;
    std::uint32_t PurchasesToday = 0;
    std::uint32_t BuyerActionsThisCycle = 0;
    std::uint32_t MutationsThisBoot = 0;
    std::uint64_t ReferenceUnitPrice = 0;
    std::uint64_t Buyout = 0;
    std::uint64_t Money = 0;
    std::uint64_t SpentToday = 0;
};

struct Decision
{
    bool WouldAct = false;
    bool MayMutate = false;
    DecisionReason Reason = DecisionReason::FeatureDisabled;
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

inline Decision PlanSeller(Policy const& policy, SellerObservation const& observed)
{
    if (!policy.Enabled || !policy.SellerEnabled)
        return { false, false, DecisionReason::FeatureDisabled };
    if (!observed.OwnedAi)
        return { false, false, DecisionReason::NotOwnedAi };
    if (!observed.AtAuctioneer)
        return { false, false, DecisionReason::NotAtAuctioneer };
    if (policy.RequireCatalogApproval && !observed.CatalogApproved)
        return { false, false, DecisionReason::CatalogRejected };
    if (!observed.Tradable || observed.Bound || observed.QuestItem ||
        observed.Conjured || observed.HasDuration)
        return { false, false, DecisionReason::UnsafeItem };
    if (observed.NeededByProfile)
        return { false, false, DecisionReason::ItemStillNeeded };
    if (observed.ReferenceUnitPrice == 0)
        return { false, false, DecisionReason::MissingReferencePrice };
    if (observed.ProposedUnitPrice == 0)
        return { false, false, DecisionReason::ZeroPrice };
    if (observed.ProposedUnitPrice <
        PercentOf(observed.ReferenceUnitPrice, policy.MinimumUnitPricePercent))
        return { false, false, DecisionReason::BelowNormalPriceBand };
    if (observed.ProposedUnitPrice >
        PercentOf(observed.ReferenceUnitPrice, policy.MaximumUnitPricePercent))
        return { false, false, DecisionReason::AboveNormalPriceBand };
    if (observed.ExistingListings >= policy.MaxListingsPerProfile)
        return { false, false, DecisionReason::ProfileListingCap };
    if (observed.SellerActionsThisCycle >= policy.MaxSellerActionsPerCycle)
        return { false, false, DecisionReason::SellerCycleCap };
    if (observed.Deposit == 0 || observed.Money < observed.Deposit ||
        observed.Money - observed.Deposit < policy.MinimumMoneyReserve)
        return { false, false, DecisionReason::SellerDepositUnaffordable };
    if (!policy.ObserveOnly && (policy.MaxMutationsPerBoot == 0 ||
        observed.MutationsThisBoot >= policy.MaxMutationsPerBoot))
        return { false, false, DecisionReason::BootMutationCap };
    if (observed.DuplicateAction)
        return { false, false, DecisionReason::DuplicateAction };
    if (policy.ObserveOnly)
        return { true, false, DecisionReason::ObserveOnly };
    return { true, true, DecisionReason::Approved };
}

inline Decision PlanBuyer(Policy const& policy, BuyerObservation const& observed)
{
    if (!policy.Enabled || !policy.BuyerEnabled)
        return { false, false, DecisionReason::FeatureDisabled };
    if (!observed.OwnedAi)
        return { false, false, DecisionReason::NotOwnedAi };
    if (!observed.AtAuctioneer)
        return { false, false, DecisionReason::NotAtAuctioneer };
    if (policy.RequireCatalogApproval && !observed.CatalogApproved)
        return { false, false, DecisionReason::CatalogRejected };
    if (observed.ItemCount == 0 || observed.Buyout == 0 ||
        observed.BuyerGuid == 0 || observed.SellerGuid == 0)
        return { false, false, DecisionReason::InvalidAuction };
    if (observed.BuyerGuid == observed.SellerGuid ||
        (observed.BuyerAccount != 0 && observed.BuyerAccount == observed.SellerAccount))
        return { false, false, DecisionReason::SelfTrade };
    if (observed.SellerKind == OwnerKind::Human && !policy.AllowHumanAuctions)
        return { false, false, DecisionReason::ProtectedHumanAuction };
    if (observed.SellerKind != OwnerKind::OwnedAi &&
        observed.SellerKind != OwnerKind::OwnedSystem &&
        !(observed.SellerKind == OwnerKind::Human && policy.AllowHumanAuctions))
        return { false, false, DecisionReason::UnownedSeller };
    if (!observed.UsefulToProfile)
        return { false, false, DecisionReason::PurchaseNotUseful };
    if (observed.ReferenceUnitPrice == 0)
        return { false, false, DecisionReason::MissingReferencePrice };
    std::uint64_t const referenceLot = SaturatingMultiply(
        observed.ReferenceUnitPrice, observed.ItemCount);
    if (observed.Buyout > PercentOf(referenceLot, policy.BuyerCeilingPercent))
        return { false, false, DecisionReason::PurchasePriceCeiling };
    if (observed.Money < observed.Buyout)
        return { false, false, DecisionReason::InsufficientFunds };
    if (observed.Money - observed.Buyout < policy.MinimumMoneyReserve)
        return { false, false, DecisionReason::ReserveViolation };
    if (observed.PurchasesToday >= policy.MaxPurchasesPerProfilePerDay)
        return { false, false, DecisionReason::DailyPurchaseCap };
    if (observed.SpentToday > policy.MaxSpendPerProfilePerDay ||
        observed.Buyout > policy.MaxSpendPerProfilePerDay - observed.SpentToday)
        return { false, false, DecisionReason::DailySpendCap };
    if (observed.BuyerActionsThisCycle >= policy.MaxBuyerActionsPerCycle)
        return { false, false, DecisionReason::BuyerCycleCap };
    if (!policy.ObserveOnly && (policy.MaxMutationsPerBoot == 0 ||
        observed.MutationsThisBoot >= policy.MaxMutationsPerBoot))
        return { false, false, DecisionReason::BootMutationCap };
    if (observed.DuplicateAction)
        return { false, false, DecisionReason::DuplicateAction };
    if (policy.ObserveOnly)
        return { true, false, DecisionReason::ObserveOnly };
    return { true, true, DecisionReason::Approved };
}
}

#endif
