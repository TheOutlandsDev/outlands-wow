#include "LivingAzerothCommerceSettlementPlan.h"

#include <iostream>

using namespace LivingAzeroth::Economy::ProfileCommerce;

namespace
{
int failures = 0;

void Check(bool condition, char const* name)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}
}

int main()
{
    std::uint64_t const cut = CalculateAuctionCutForBuyout(4500, 5, 1.0f);
    Check(cut == 225, "five percent cut");

    SettlementObservation settled;
    settled.Action = SettlementActionKind::AuctionBuyout;
    settled.AgeSeconds = 5;
    settled.TotalPrice = 4500;
    settled.Deposit = 100;
    settled.Cut = cut;
    settled.PurchaseAuctionReferences = 0;
    settled.WonMailCount = 1;
    settled.SellerMailCount = 1;
    settled.SellerMailMoney = 4375;
    auto decision = PlanSettlement(settled);
    Check(decision.Outcome == SettlementOutcome::Verified,
        "exact purchase settlement");

    auto wrongMoney = settled;
    wrongMoney.SellerMailMoney = 4374;
    decision = PlanSettlement(wrongMoney);
    Check(decision.Outcome == SettlementOutcome::Pending,
        "wrong seller proceeds remain pending during grace");
    wrongMoney.AgeSeconds = 30;
    decision = PlanSettlement(wrongMoney);
    Check(decision.Outcome == SettlementOutcome::Rejected,
        "wrong seller proceeds reject at timeout");

    auto duplicateBuyerMail = settled;
    duplicateBuyerMail.WonMailCount = 2;
    decision = PlanSettlement(duplicateBuyerMail);
    Check(decision.Outcome == SettlementOutcome::Rejected,
        "duplicate buyer mail rejected");

    auto duplicateSellerMail = settled;
    duplicateSellerMail.SellerMailCount = 2;
    decision = PlanSettlement(duplicateSellerMail);
    Check(decision.Outcome == SettlementOutcome::Rejected,
        "duplicate seller mail rejected");

    auto auctionStillPresent = settled;
    auctionStillPresent.PurchaseAuctionReferences = 1;
    decision = PlanSettlement(auctionStillPresent);
    Check(decision.Outcome == SettlementOutcome::Pending,
        "auction still present remains pending");

    std::cout << "{\"result\":\"" << (failures == 0 ? "PASS" : "FAIL")
              << "\",\"failures\":" << failures
              << ",\"suite\":\"procurement-settlement\"}\n";
    return failures == 0 ? 0 : 1;
}
