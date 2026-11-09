#include "../src/LivingAzerothCommerceSettlementPlan.h"

#include <cassert>
#include <iostream>

using namespace LivingAzeroth::Economy::ProfileCommerce;

int main()
{
    {
        SettlementObservation row;
        row.Action = SettlementActionKind::AuctionList;
        row.AgeSeconds = 5;
        row.TotalPrice = 1000;
        row.Deposit = 50;
        row.ExactListingCount = 1;
        row.ItemAuctionReferences = 1;
        row.ListingAuctionId = 40;
        row.ListingDeposit = 50;
        auto decision = PlanSettlement(row);
        assert(decision.Outcome == SettlementOutcome::Verified);
    }
    {
        SettlementObservation row;
        row.Action = SettlementActionKind::AuctionBuyout;
        row.AgeSeconds = 5;
        row.TotalPrice = 1000;
        row.Deposit = 50;
        row.Cut = 50;
        row.PurchaseAuctionReferences = 0;
        row.WonMailCount = 1;
        row.SellerMailCount = 1;
        row.SellerMailMoney = 1000;
        auto decision = PlanSettlement(row);
        assert(decision.Outcome == SettlementOutcome::Verified);
    }
    {
        SettlementObservation row;
        row.Action = SettlementActionKind::AuctionBuyout;
        row.AgeSeconds = 10;
        row.TotalPrice = 1000;
        auto decision = PlanSettlement(row);
        assert(decision.Outcome == SettlementOutcome::Pending);
    }
    {
        SettlementObservation row;
        row.Action = SettlementActionKind::AuctionList;
        row.AgeSeconds = 5;
        row.TotalPrice = 1000;
        row.ExactListingCount = 2;
        row.ItemAuctionReferences = 2;
        auto decision = PlanSettlement(row);
        assert(decision.Outcome == SettlementOutcome::Rejected);
    }
    {
        SettlementObservation row;
        row.Action = SettlementActionKind::AuctionBuyout;
        row.AgeSeconds = 30;
        row.TotalPrice = 1000;
        auto decision = PlanSettlement(row);
        assert(decision.Outcome == SettlementOutcome::Rejected);
    }

    {
        std::uint64_t const cut = CalculateAuctionCutForBuyout(500, 5, 1.0f);
        assert(cut == 25);
        assert(CalculateAuctionCutForBuyout(500, 5, 2.0f) == 50);
        assert(CalculateAuctionCutForBuyout(500, 5, 0.0f) == 0);

        SettlementObservation row;
        row.Action = SettlementActionKind::AuctionBuyout;
        row.AgeSeconds = 5;
        row.TotalPrice = 500;
        row.Deposit = 156;
        row.Cut = cut;
        row.PurchaseAuctionReferences = 0;
        row.WonMailCount = 1;
        row.SellerMailCount = 1;
        row.SellerMailMoney = 631;
        auto decision = PlanSettlement(row);
        assert(decision.Outcome == SettlementOutcome::Verified);
    }

    std::cout << "LivingAzerothCommerceSettlementPlanTests PASS\n";
    return 0;
}
