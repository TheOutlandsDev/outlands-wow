/*
 * Living Azeroth durable commerce settlement planner.
 */

#ifndef LIVING_AZEROTH_COMMERCE_SETTLEMENT_PLAN_H
#define LIVING_AZEROTH_COMMERCE_SETTLEMENT_PLAN_H

#include <cstdint>
#include <limits>

namespace LivingAzeroth::Economy::ProfileCommerce
{
enum class SettlementActionKind
{
    AuctionList,
    AuctionBuyout
};

enum class SettlementOutcome
{
    Pending,
    Verified,
    Rejected
};

struct SettlementObservation
{
    SettlementActionKind Action = SettlementActionKind::AuctionList;
    std::uint64_t AgeSeconds = 0;
    std::uint64_t TotalPrice = 0;
    std::uint64_t Deposit = 0;
    std::uint64_t Cut = 0;
    std::uint32_t ExactListingCount = 0;
    std::uint32_t ItemAuctionReferences = 0;
    std::uint32_t ListingAuctionId = 0;
    std::uint64_t ListingDeposit = 0;
    std::uint32_t PurchaseAuctionReferences = 0;
    std::uint32_t WonMailCount = 0;
    std::uint32_t SellerMailCount = 0;
    std::uint64_t SellerMailMoney = 0;
};

struct SettlementDecision
{
    SettlementOutcome Outcome = SettlementOutcome::Pending;
    char const* Reason = "grace_period";
};

inline std::uint64_t CalculateAuctionCutForBuyout(
    std::uint32_t buyout,
    std::uint32_t cutPercent,
    float auctionCutRate)
{
    if (buyout == 0 || cutPercent == 0 || auctionCutRate <= 0.0f)
        return 0;

    std::uint32_t const baseCut = static_cast<std::uint32_t>(
        static_cast<float>(buyout) * static_cast<float>(cutPercent) / 100.0f);
    float const scaledCut = static_cast<float>(baseCut) * auctionCutRate;
    if (scaledCut <= 0.0f)
        return 0;
    if (scaledCut >= static_cast<float>(std::numeric_limits<std::int32_t>::max()))
        return static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    return static_cast<std::uint64_t>(static_cast<std::int32_t>(scaledCut));
}

inline SettlementDecision PlanSettlement(
    SettlementObservation const& observed,
    std::uint64_t graceSeconds = 5,
    std::uint64_t rejectionSeconds = 30)
{
    if (graceSeconds == 0 || rejectionSeconds <= graceSeconds ||
        observed.TotalPrice == 0)
    {
        return { SettlementOutcome::Rejected, "invalid_settlement_policy" };
    }

    if (observed.AgeSeconds < graceSeconds)
        return { SettlementOutcome::Pending, "grace_period" };

    if (observed.Action == SettlementActionKind::AuctionList)
    {
        if (observed.ExactListingCount == 1 &&
            observed.ItemAuctionReferences == 1 &&
            observed.ListingAuctionId != 0 &&
            observed.ListingDeposit == observed.Deposit)
        {
            return { SettlementOutcome::Verified, "listing_persisted" };
        }

        if (observed.ExactListingCount > 1 || observed.ItemAuctionReferences > 1)
            return { SettlementOutcome::Rejected, "listing_duplicate" };
    }
    else
    {
        bool expectedMoneyValid = observed.TotalPrice <=
            std::numeric_limits<std::uint64_t>::max() - observed.Deposit;
        std::uint64_t expectedMoney = 0;
        if (expectedMoneyValid)
        {
            expectedMoney = observed.TotalPrice + observed.Deposit;
            expectedMoneyValid = expectedMoney >= observed.Cut;
            if (expectedMoneyValid)
                expectedMoney -= observed.Cut;
        }

        if (observed.PurchaseAuctionReferences == 0 &&
            observed.WonMailCount == 1 &&
            observed.SellerMailCount == 1 &&
            expectedMoneyValid && observed.SellerMailMoney == expectedMoney)
        {
            return { SettlementOutcome::Verified, "purchase_mail_settled" };
        }

        if (observed.WonMailCount > 1 || observed.SellerMailCount > 1)
            return { SettlementOutcome::Rejected, "purchase_mail_duplicate" };
    }

    if (observed.AgeSeconds >= rejectionSeconds)
        return { SettlementOutcome::Rejected, "settlement_timeout" };

    return { SettlementOutcome::Pending, "awaiting_persistence" };
}
}

#endif
