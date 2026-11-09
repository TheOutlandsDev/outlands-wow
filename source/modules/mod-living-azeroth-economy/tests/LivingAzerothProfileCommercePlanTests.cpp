#include "LivingAzerothProfileCommercePlan.h"

#include <cassert>
#include <cstdint>

using namespace LivingAzeroth::Economy::ProfileCommerce;

int main()
{
    Policy policy;
    policy.Enabled = true;
    policy.SellerEnabled = true;
    policy.BuyerEnabled = true;
    policy.MaxMutationsPerBoot = 2;

    SellerObservation seller;
    seller.OwnedAi = true;
    seller.AtAuctioneer = true;
    seller.CatalogApproved = true;
    seller.Tradable = true;
    seller.NeededByProfile = false;
    seller.ReferenceUnitPrice = 1000;
    seller.ProposedUnitPrice = 1000;
    seller.Money = 10000;
    seller.Deposit = 100;

    Decision decision = PlanSeller(policy, seller);
    assert(decision.WouldAct);
    assert(!decision.MayMutate);
    assert(decision.Reason == DecisionReason::ObserveOnly);

    policy.ObserveOnly = false;
    decision = PlanSeller(policy, seller);
    assert(decision.WouldAct && decision.MayMutate);

    seller.ProposedUnitPrice = 799;
    assert(PlanSeller(policy, seller).Reason == DecisionReason::BelowNormalPriceBand);
    seller.ProposedUnitPrice = 1201;
    assert(PlanSeller(policy, seller).Reason == DecisionReason::AboveNormalPriceBand);
    seller.ProposedUnitPrice = 1000;
    seller.Bound = true;
    assert(PlanSeller(policy, seller).Reason == DecisionReason::UnsafeItem);
    seller.Bound = false;
    seller.CatalogApproved = false;
    assert(PlanSeller(policy, seller).Reason == DecisionReason::CatalogRejected);
    seller.CatalogApproved = true;
    seller.Money = seller.Deposit;
    assert(PlanSeller(policy, seller).Reason == DecisionReason::SellerDepositUnaffordable);
    seller.Money = 10000;
    seller.MutationsThisBoot = policy.MaxMutationsPerBoot;
    assert(PlanSeller(policy, seller).Reason == DecisionReason::BootMutationCap);
    seller.MutationsThisBoot = 0;
    seller.DuplicateAction = true;
    assert(PlanSeller(policy, seller).Reason == DecisionReason::DuplicateAction);
    seller.DuplicateAction = false;

    BuyerObservation buyer;
    buyer.OwnedAi = true;
    buyer.AtAuctioneer = true;
    buyer.CatalogApproved = true;
    buyer.UsefulToProfile = true;
    buyer.SellerKind = OwnerKind::OwnedSystem;
    buyer.BuyerGuid = 100;
    buyer.SellerGuid = 900001;
    buyer.BuyerAccount = 20;
    buyer.SellerAccount = 64;
    buyer.ItemCount = 2;
    buyer.ReferenceUnitPrice = 1000;
    buyer.Buyout = 2000;
    buyer.Money = 10000;

    decision = PlanBuyer(policy, buyer);
    assert(decision.WouldAct && decision.MayMutate);

    buyer.SellerKind = OwnerKind::Human;
    assert(PlanBuyer(policy, buyer).Reason == DecisionReason::ProtectedHumanAuction);
    buyer.SellerKind = OwnerKind::OwnedAi;
    buyer.SellerAccount = buyer.BuyerAccount;
    assert(PlanBuyer(policy, buyer).Reason == DecisionReason::SelfTrade);
    buyer.SellerAccount = 21;
    buyer.Buyout = 2201;
    assert(PlanBuyer(policy, buyer).Reason == DecisionReason::PurchasePriceCeiling);
    buyer.Buyout = 2000;
    buyer.Money = 2500;
    assert(PlanBuyer(policy, buyer).Reason == DecisionReason::ReserveViolation);
    buyer.Money = 10000;
    buyer.PurchasesToday = policy.MaxPurchasesPerProfilePerDay;
    assert(PlanBuyer(policy, buyer).Reason == DecisionReason::DailyPurchaseCap);
    buyer.PurchasesToday = 0;
    buyer.SpentToday = policy.MaxSpendPerProfilePerDay - 1000;
    assert(PlanBuyer(policy, buyer).Reason == DecisionReason::DailySpendCap);
    buyer.SpentToday = 0;
    buyer.CatalogApproved = false;
    assert(PlanBuyer(policy, buyer).Reason == DecisionReason::CatalogRejected);
    buyer.CatalogApproved = true;
    buyer.MutationsThisBoot = policy.MaxMutationsPerBoot;
    assert(PlanBuyer(policy, buyer).Reason == DecisionReason::BootMutationCap);
    buyer.MutationsThisBoot = 0;
    buyer.DuplicateAction = true;
    assert(PlanBuyer(policy, buyer).Reason == DecisionReason::DuplicateAction);

    policy.MaxMutationsPerBoot = 0;
    buyer.DuplicateAction = false;
    assert(PlanBuyer(policy, buyer).Reason == DecisionReason::BootMutationCap);
    policy.ObserveOnly = true;
    decision = PlanBuyer(policy, buyer);
    assert(decision.WouldAct && !decision.MayMutate &&
        decision.Reason == DecisionReason::ObserveOnly);

    assert(SaturatingMultiply(std::numeric_limits<std::uint64_t>::max(), 2) ==
        std::numeric_limits<std::uint64_t>::max());
    return 0;
}
