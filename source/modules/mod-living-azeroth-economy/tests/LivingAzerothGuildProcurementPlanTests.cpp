#include "../src/LivingAzerothGuildProcurementPlan.h"

#include <iostream>
#include <string_view>
#include <vector>

using namespace LivingAzeroth::Economy::GuildProcurement;

namespace
{
int failures = 0;

void Check(bool condition, std::string_view name)
{
    if (!condition)
    {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

Policy EnabledPolicy()
{
    Policy policy;
    policy.Enabled = true;
    policy.ObserveOnly = true;
    policy.ConfirmationValid = true;
    return policy;
}

Observation FairHumanBag()
{
    Observation observed;
    observed.MarketFaction = Faction::Alliance;
    observed.SellerKind = OwnerKind::Human;
    observed.CatalogApproved = true;
    observed.SafeItem = true;
    observed.SellerAccount = 65;
    observed.SellerGuid = 606;
    observed.ItemCount = 1;
    observed.ReferenceUnitPrice = 5184;
    observed.Buyout = 4500;
    return observed;
}

Observation FairOwnedSystemBag()
{
    Observation observed = FairHumanBag();
    observed.SellerKind = OwnerKind::OwnedSystem;
    observed.SellerSystemAllowlisted = true;
    observed.SellerAccount = 42;
    observed.SellerGuid = 900001;
    return observed;
}
}

int main()
{
    Policy policy = EnabledPolicy();
    Observation observed = FairHumanBag();
    Decision decision = Plan(policy, observed);
    Check(decision.WouldPropose && !decision.MayMutate &&
        decision.Why == Reason::ObserveOnlyApproved, "fair human bag observe-only proposal");
    Check(decision.ReferenceLotPrice == 5184 && decision.CeilingPrice == 5702,
        "reference and 110 percent ceiling");

    policy.ObserveOnly = false;
    Check(Plan(policy, observed).Why == Reason::MutationModeRejected, "mutation mode fails closed");
    policy = EnabledPolicy();
    policy.ConfirmationValid = false;
    Check(Plan(policy, observed).Why == Reason::ConfirmationMissing, "confirmation required");
    policy = EnabledPolicy();

    observed = FairOwnedSystemBag();
    Check(Plan(policy, observed).Why == Reason::OwnedSystemSellerDisabled,
        "system seller disabled by default");
    policy.AllowOwnedSystemSellerAuctions = true;
    observed.SellerSystemAllowlisted = false;
    Check(Plan(policy, observed).Why == Reason::OwnedSystemSellerNotAllowlisted,
        "system seller exact allowlist required");
    observed.SellerSystemAllowlisted = true;
    Check(Plan(policy, observed).WouldPropose, "allowlisted system seller accepted");
    observed.OwnedSystemPurchasesToday = 20;
    Check(Plan(policy, observed).Why == Reason::OwnedSystemDailyPurchaseCap,
        "system seller daily cap independent");
    observed = FairOwnedSystemBag();
    observed.SellerPurchasesToday = 99;
    Check(Plan(policy, observed).WouldPropose,
        "human per-seller cap does not consume system cap");
    policy = EnabledPolicy();
    observed = FairHumanBag();
    observed.SellerKind = OwnerKind::OwnedAi;
    Check(Plan(policy, observed).Why == Reason::SellerOwnedAi, "AI seller rejected");
    observed.SellerKind = OwnerKind::Unknown;
    Check(Plan(policy, observed).Why == Reason::SellerUnknown, "unknown seller rejected");
    observed = FairHumanBag();
    observed.BuyerAccount = 65;
    Check(Plan(policy, observed).Why == Reason::SelfTrade, "same account rejected");
    observed = FairHumanBag();
    observed.CatalogApproved = false;
    Check(Plan(policy, observed).Why == Reason::CatalogRejected, "catalog required");
    observed = FairHumanBag();
    observed.SafeItem = false;
    Check(Plan(policy, observed).Why == Reason::UnsafeItem, "unsafe item rejected");
    observed = FairHumanBag();
    observed.HasBid = true;
    Check(Plan(policy, observed).Why == Reason::BidPresent, "bid listing rejected");
    observed = FairHumanBag();
    observed.DuplicateAuction = true;
    Check(Plan(policy, observed).Why == Reason::DuplicateAuction, "duplicate rejected");
    observed = FairHumanBag();
    observed.Buyout = 5703;
    Check(Plan(policy, observed).Why == Reason::PurchasePriceCeiling, "ceiling boundary rejected");
    observed.Buyout = 5702;
    Check(Plan(policy, observed).WouldPropose, "ceiling boundary accepted");
    observed = FairHumanBag();
    policy.MaxPurchasesPerFactionPerDay = 5;
    observed.FactionPurchasesToday = 5;
    Check(Plan(policy, observed).Why == Reason::FactionDailyPurchaseCap, "configured faction count cap");
    policy.AllowOwnedSystemSellerAuctions = true;
    observed = FairOwnedSystemBag();
    observed.FactionPurchasesToday = 5;
    Check(Plan(policy, observed).Why == Reason::FactionDailyPurchaseCap,
        "system purchases consume combined faction cap");
    policy = EnabledPolicy();
    observed = FairHumanBag();
    observed.FactionSpendToday = 47000;
    Check(Plan(policy, observed).Why == Reason::FactionDailySpendCap, "faction spend cap");
    observed = FairHumanBag();
    policy.MaxPurchasesPerSellerPerDay = 2;
    observed.SellerPurchasesToday = 2;
    Check(Plan(policy, observed).Why == Reason::SellerDailyPurchaseCap, "configured seller cap");
    policy.MaxPurchasesPerSellerPerDay = 0;
    Check(Plan(policy, observed).WouldPropose, "zero seller cap disables hard cutoff");
    observed = FairHumanBag();
    policy.MaxProposalsPerFactionPerCycle = 3;
    observed.FactionProposalsThisCycle = 3;
    Check(Plan(policy, observed).Why == Reason::FactionCycleCap, "configured cycle cap");

    std::vector<Candidate> candidates = {
        { 22, FairHumanBag() }, { 20, FairHumanBag() }, { 30, FairHumanBag() }
    };
    candidates[0].Observed.Buyout = 4000;
    candidates[1].Observed.Buyout = 4000;
    candidates[2].Observed.Buyout = 3999;
    Selection selected = Select(policy, candidates);
    Check(selected.Found && selected.AuctionId == 30, "lowest buyout selected");
    candidates[2].Observed.Buyout = 4000;
    selected = Select(policy, candidates);
    Check(selected.Found && selected.AuctionId == 20, "auction id tie break");

    candidates[0].Observed.Buyout = 4000;
    candidates[0].Observed.SellerPurchasesToday = 4;
    candidates[1].Observed.Buyout = 4300;
    candidates[1].Observed.SellerPurchasesToday = 0;
    candidates[2].Observed.Buyout = 5000;
    candidates[2].Observed.SellerPurchasesToday = 0;
    selected = Select(policy, candidates);
    Check(selected.Found && selected.AuctionId == 20,
        "soft seller diversity prefers a modestly higher fresh seller");

    std::cout << "{\"result\":\"" << (failures ? "FAIL" : "PASS")
              << "\",\"failures\":" << failures << ",\"mode\":\"observe-only\"}\n";
    return failures == 0 ? 0 : 1;
}
