#include "LivingAzerothGuildProcurementExecutionPlan.h"

#include <iostream>

namespace Procurement = LivingAzeroth::Economy::GuildProcurement;

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

Procurement::ExecutionPolicy GoodPolicy()
{
    Procurement::ExecutionPolicy policy;
    policy.Enabled = true;
    policy.MutationMode = true;
    policy.ConfirmationValid = true;
    policy.SchemaReady = true;
    policy.CatalogHashValid = true;
    policy.MaxPurchasesPerFactionPerDay = 0;
    policy.MaxSpendPerFactionPerDay = 600000;
    policy.MaxPurchasesPerSellerPerDay = 0;
    policy.MaxPurchasesPerFactionPerCycle = 3;
    policy.MaxMutationsPerBoot = 120;
    return policy;
}

Procurement::ExecutionObservation GoodObservation()
{
    Procurement::ExecutionObservation observed;
    observed.ProposalApproved = true;
    observed.MarketFaction = Procurement::Faction::Alliance;
    observed.BuyerConfigured = true;
    observed.BuyerOwnedAi = true;
    observed.BuyerOnline = true;
    observed.BuyerAtAuctioneer = true;
    observed.BuyerAccount = 100;
    observed.SellerAccount = 200;
    observed.SellerGuid = 300;
    observed.SellerKind = Procurement::OwnerKind::Human;
    observed.ItemCount = 1;
    observed.Buyout = 4500;
    observed.BuyerMoney = 10000;
    return observed;
}

void ExpectReason(Procurement::ExecutionPolicy policy,
    Procurement::ExecutionObservation observed, Procurement::ExecutionReason reason,
    char const* name)
{
    auto decision = Procurement::PlanExecution(policy, observed);
    Check(!decision.MayMutate && decision.Why == reason, name);
}
}

int main()
{
    auto policy = GoodPolicy();
    auto observed = GoodObservation();
    auto approved = Procurement::PlanExecution(policy, observed);
    Check(approved.WouldDispatch && approved.MayMutate &&
        approved.Why == Procurement::ExecutionReason::Approved, "approved mutation");

    auto changedPolicy = policy;
    changedPolicy.Enabled = false;
    ExpectReason(changedPolicy, observed, Procurement::ExecutionReason::FeatureDisabled,
        "feature disabled");
    changedPolicy = policy;
    changedPolicy.MutationMode = false;
    ExpectReason(changedPolicy, observed, Procurement::ExecutionReason::MutationModeDisabled,
        "mutation disabled");
    changedPolicy = policy;
    changedPolicy.ConfirmationValid = false;
    ExpectReason(changedPolicy, observed, Procurement::ExecutionReason::ConfirmationMissing,
        "confirmation missing");
    changedPolicy = policy;
    changedPolicy.SchemaReady = false;
    ExpectReason(changedPolicy, observed, Procurement::ExecutionReason::SchemaUnavailable,
        "schema unavailable");
    changedPolicy = policy;
    changedPolicy.CatalogHashValid = false;
    ExpectReason(changedPolicy, observed, Procurement::ExecutionReason::CatalogHashMismatch,
        "catalog mismatch");
    changedPolicy = policy;
    changedPolicy.MaxMutationsPerBoot = 0;
    ExpectReason(changedPolicy, observed, Procurement::ExecutionReason::InvalidLimits,
        "zero boot cap invalid");
    changedPolicy = policy;
    changedPolicy.MaxMutationsPerBoot = 121;
    ExpectReason(changedPolicy, observed, Procurement::ExecutionReason::InvalidLimits,
        "boot cap above 120 invalid");
    changedPolicy = policy;
    changedPolicy.MaxPurchasesPerFactionPerDay = 121;
    ExpectReason(changedPolicy, observed, Procurement::ExecutionReason::InvalidLimits,
        "daily faction cap above 120 invalid" );
    changedPolicy = policy;
    changedPolicy.MaxSpendPerFactionPerDay = 600001;
    ExpectReason(changedPolicy, observed, Procurement::ExecutionReason::InvalidLimits,
        "daily faction spend above 60 gold invalid" );
    changedPolicy = policy;
    changedPolicy.MaxPurchasesPerFactionPerCycle = 4;
    ExpectReason(changedPolicy, observed, Procurement::ExecutionReason::InvalidLimits,
        "cycle cap above three invalid");
    changedPolicy = policy;
    changedPolicy.MaxOwnedSystemPurchasesPerFactionPerDay = 31;
    ExpectReason(changedPolicy, observed, Procurement::ExecutionReason::InvalidLimits,
        "system daily cap above 30 invalid");

    auto changed = observed;
    changed.ProposalApproved = false;
    ExpectReason(policy, changed, Procurement::ExecutionReason::ProposalRejected,
        "proposal rejected");
    changed = observed;
    changed.SellerKind = Procurement::OwnerKind::OwnedSystem;
    changed.SellerSystemAllowlisted = true;
    ExpectReason(policy, changed, Procurement::ExecutionReason::OwnedSystemSellerDisabled,
        "system execution disabled by default");
    changedPolicy = policy;
    changedPolicy.AllowOwnedSystemSellerAuctions = true;
    ExpectReason(changedPolicy, changed,
        Procurement::ExecutionReason::OwnedSystemConfirmationMissing,
        "system execution confirmation required");
    changedPolicy.OwnedSystemConfirmationValid = true;
    changed.SellerSystemAllowlisted = false;
    ExpectReason(changedPolicy, changed,
        Procurement::ExecutionReason::OwnedSystemSellerNotAllowlisted,
        "system execution exact allowlist required");
    changed.SellerSystemAllowlisted = true;
    Check(Procurement::PlanExecution(changedPolicy, changed).MayMutate,
        "allowlisted confirmed system execution accepted");
    changed.OwnedSystemPurchasesToday = 20;
    ExpectReason(changedPolicy, changed,
        Procurement::ExecutionReason::OwnedSystemDailyPurchaseCap,
        "system execution daily cap");
    changed = observed;
    changed.MarketFaction = Procurement::Faction::Neutral;
    ExpectReason(policy, changed, Procurement::ExecutionReason::UnsupportedFaction,
        "neutral rejected");
    changed = observed;
    changed.BuyerConfigured = false;
    ExpectReason(policy, changed, Procurement::ExecutionReason::BuyerNotConfigured,
        "buyer not configured");
    changed = observed;
    changed.BuyerOwnedAi = false;
    ExpectReason(policy, changed, Procurement::ExecutionReason::BuyerNotOwnedAi,
        "buyer not owned AI");
    changed = observed;
    changed.BuyerOnline = false;
    ExpectReason(policy, changed, Procurement::ExecutionReason::BuyerOffline,
        "buyer offline");
    changed = observed;
    changed.BuyerAtAuctioneer = false;
    ExpectReason(policy, changed, Procurement::ExecutionReason::BuyerNotAtAuctioneer,
        "buyer not at auctioneer");
    changed = observed;
    changed.SellerAccount = changed.BuyerAccount;
    ExpectReason(policy, changed, Procurement::ExecutionReason::SelfTrade,
        "self trade");
    changed = observed;
    changed.HasBid = true;
    ExpectReason(policy, changed, Procurement::ExecutionReason::BidPresent,
        "bid present");
    changed = observed;
    changed.AuctionClaimed = true;
    ExpectReason(policy, changed, Procurement::ExecutionReason::AuctionAlreadyClaimed,
        "restart claim persists");
    changed = observed;
    changed.MutationsThisBoot = 120;
    ExpectReason(policy, changed, Procurement::ExecutionReason::BootMutationCap,
        "boot cap");
    changed = observed;
    changed.PurchasesThisFactionCycle = 3;
    ExpectReason(policy, changed, Procurement::ExecutionReason::FactionCycleCap,
        "configured cycle cap");
    changed = observed;
    changedPolicy = policy;
    changedPolicy.MaxPurchasesPerFactionPerDay = 60;
    changed.FactionPurchasesToday = 60;
    ExpectReason(changedPolicy, changed, Procurement::ExecutionReason::FactionDailyPurchaseCap,
        "configured faction count cap");
    changed = observed;
    changed.FactionSpendToday = 598000;
    ExpectReason(policy, changed, Procurement::ExecutionReason::FactionDailySpendCap,
        "faction spend cap");
    changed = observed;
    changedPolicy = policy;
    changedPolicy.MaxPurchasesPerSellerPerDay = 2;
    changed.SellerPurchasesToday = 2;
    ExpectReason(changedPolicy, changed, Procurement::ExecutionReason::SellerDailyPurchaseCap,
        "configured seller cap");
    changedPolicy.MaxPurchasesPerSellerPerDay = 0;
    Check(Procurement::PlanExecution(changedPolicy, changed).MayMutate,
        "zero seller cap disables hard cutoff");
    changed = observed;
    changed.BuyerMoney = 4499;
    ExpectReason(policy, changed, Procurement::ExecutionReason::InsufficientFunds,
        "insufficient funds");
    changed = observed;
    changed.BuyerMoney = 5000;
    ExpectReason(policy, changed, Procurement::ExecutionReason::ReserveViolation,
        "reserve protected");
    changed = observed;
    changed.BuyerMoney = 5500;
    Check(Procurement::PlanExecution(policy, changed).MayMutate,
        "reserve exact boundary");

    std::cout << "{\"result\":\"" << (failures == 0 ? "PASS" : "FAIL")
              << "\",\"failures\":" << failures
              << ",\"mode\":\"bounded-mutation\"}\n";
    return failures == 0 ? 0 : 1;
}
