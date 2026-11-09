#include "../src/LivingAzerothRotationMarketPlan.h"

#include <cstdlib>
#include <array>
#include <iostream>
#include <map>
#include <set>
#include <string_view>
#include <vector>

namespace
{
using namespace LivingAzeroth::Economy;

[[noreturn]] void Fail(std::string_view message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message)
{
    if (!condition)
        Fail(message);
}
}

int main()
{
    std::vector<RotationMetric> metrics;
    for (std::uint32_t entry = 1; entry <= 3000; ++entry)
        metrics.push_back({ entry, entry % 20 + 1, 100 + entry, 120 + entry, 300 + entry });

    RotationPlan tierA = BuildRotationMarketPlan(metrics, 3000, 20669);
    Require(tierA.ok, "tier A must pass");
    Require(tierA.selectedItems == 1200, "tier A selected count");
    Require(tierA.allianceListings == 2400, "tier A alliance count");
    Require(tierA.hordeListings == 300, "tier A horde count");
    Require(tierA.neutralListings == 300, "tier A neutral count");
    Require(std::set<BusinessKey>(tierA.desired.begin(), tierA.desired.end()).size() == 3000,
        "tier A keys unique");

    RotationPlan tierARepeat = BuildRotationMarketPlan(metrics, 3000, 20669);
    Require(tierARepeat.desired == tierA.desired, "same epoch deterministic");
    RotationPlan tierANext = BuildRotationMarketPlan(metrics, 3000, 20670);
    Require(tierANext.ok && tierANext.desired != tierA.desired, "next epoch rotates");

    RotationPlan tierB = BuildRotationMarketPlan(metrics, 6000, 20669);
    Require(tierB.ok, "tier B must pass");
    Require(tierB.selectedItems == 2400, "tier B selected count");
    Require(tierB.allianceListings == 4800, "tier B alliance count");
    Require(tierB.hordeListings == 600, "tier B horde count");
    Require(tierB.neutralListings == 600, "tier B neutral count");
    Require(std::set<BusinessKey>(tierB.desired.begin(), tierB.desired.end()).size() == 6000,
        "tier B keys unique");

    std::vector<RotationMetric> professionMetrics;
    for (std::uint32_t entry = 1; entry <= 11599; ++entry)
    {
        professionMetrics.push_back({
            entry, entry % 20 + 1, 100 + entry, 120 + entry, 300 + entry,
            entry <= 4332,
            static_cast<std::uint8_t>(entry <= 284 ? 2 : entry <= 1109 ? 1 : 0)
        });
    }
    RotationPlan tierC = BuildRotationMarketPlan(professionMetrics, 9000, 20669);
    Require(tierC.ok, "tier C must pass");
    Require(tierC.selectedItems == 7200, "tier C selected count");
    Require(tierC.allianceListings == 7200, "tier C alliance count");
    Require(tierC.hordeListings == 900, "tier C horde count");
    Require(tierC.neutralListings == 900, "tier C neutral count");
    Require(std::set<BusinessKey>(tierC.desired.begin(), tierC.desired.end()).size() == 9000,
        "tier C keys unique");
    std::set<std::uint32_t> listedProfessionItems;
    for (BusinessKey const& key : tierC.desired)
        if (key.market == 2 && key.itemEntry <= 4332)
            listedProfessionItems.insert(key.itemEntry);
    Require(listedProfessionItems.size() == 4332,
        "tier C guarantees every profession item in Alliance");

    RotationPlan tierCRepeat = BuildRotationMarketPlan(professionMetrics, 9000, 20669);
    Require(tierCRepeat.desired == tierC.desired, "tier C same epoch deterministic");
    RotationPlan tierCNext = BuildRotationMarketPlan(professionMetrics, 9000, 20670);
    Require(tierCNext.ok && tierCNext.desired != tierC.desired,
        "tier C non-guaranteed items rotate");

    RotationPlan tierD = BuildRotationMarketPlan(professionMetrics, 11000, 20669);
    Require(tierD.ok, "tier D must pass");
    Require(tierD.selectedItems == 4537, "tier D selected item count");
    Require(tierD.allianceListings == 9000, "tier D alliance count");
    Require(tierD.hordeListings == 1000, "tier D horde count");
    Require(tierD.neutralListings == 1000, "tier D neutral count");
    Require(std::set<BusinessKey>(tierD.desired.begin(), tierD.desired.end()).size() == 11000,
        "tier D keys unique");

    std::map<std::uint32_t, std::size_t> alliancePerItem;
    std::map<std::uint32_t, std::set<std::uint32_t>> allianceStacks;
    for (BusinessKey const& key : tierD.desired)
    {
        if (key.market != 2)
            continue;
        ++alliancePerItem[key.itemEntry];
        allianceStacks[key.itemEntry].insert(key.stack);
    }
    for (std::uint32_t entry = 1; entry <= 284; ++entry)
        Require(alliancePerItem[entry] == 8, "tier D low-tier item has eight listings");
    for (std::uint32_t entry = 285; entry <= 1109; ++entry)
        Require(alliancePerItem[entry] == 4, "tier D commodity has four listings");
    for (std::uint32_t entry = 1110; entry <= 4332; ++entry)
        Require(alliancePerItem[entry] == 1, "tier D base profession item has one listing");
    Require(allianceStacks[1].size() > 1, "tier D low-tier stacks vary");
    Require(allianceStacks[285].size() > 1, "tier D commodity stacks vary");

    std::array<std::size_t, RotationExpirationBucketCount> expirationBuckets{};
    for (std::size_t index = 0; index < tierD.desired.size(); ++index)
    {
        std::uint32_t const duration = RotationExpirationSeconds(index);
        Require(duration >= 24U * 60U * 60U, "tier D expiration minimum");
        Require(duration <= (47U * 60U + 45U) * 60U, "tier D expiration maximum");
        ++expirationBuckets[(duration - RotationExpirationBaseSeconds) /
            RotationExpirationBucketSeconds];
    }
    Require(*std::min_element(expirationBuckets.begin(), expirationBuckets.end()) == 114,
        "tier D expiration minimum bucket");
    Require(*std::max_element(expirationBuckets.begin(), expirationBuckets.end()) == 115,
        "tier D expiration maximum bucket");

    ReconcilePlan tierDMigration = BuildRotationReconcilePlan(tierD.desired, {});
    Require(tierDMigration.ok && tierDMigration.creates.size() == 11000,
        "tier D initial reconcile");
    std::size_t const tierDDeferred =
        ApplyLiveListingCeiling(tierDMigration, 10916, 12000);
    Require(tierDMigration.ok && tierDMigration.creates.size() == 1084 &&
        tierDDeferred == 9916, "tier D migration obeys hard live ceiling");

    ReconcilePlan overTarget = BuildRotationReconcilePlan(tierD.desired, {});
    std::size_t const overTargetDeferred =
        ApplyDesiredLiveListingTarget(overTarget, 11911);
    Require(overTarget.ok && overTarget.creates.empty() &&
        overTargetDeferred == 11000,
        "tier D over target suppresses every create");

    ReconcilePlan belowTarget = BuildRotationReconcilePlan(tierD.desired, {});
    std::size_t const belowTargetDeferred =
        ApplyDesiredLiveListingTarget(belowTarget, 10988);
    Require(belowTarget.ok && belowTarget.creates.size() == 12 &&
        belowTargetDeferred == 10988,
        "tier D below target creates exact deficit");
    std::size_t const belowTargetCeilingDeferred =
        ApplyLiveListingCeiling(belowTarget, 10988, 12000);
    Require(belowTarget.creates.size() == 12 && belowTargetCeilingDeferred == 0,
        "target and safety ceiling compose without overfill");

    std::array<BusinessKey, 3> swapDesired = {{
        { 2, 100, 1, 9, 10 },
        { 2, 101, 1, 9, 10 },
        { 2, 102, 1, 9, 10 }
    }};
    BusinessKey const obsoleteA { 2, 200, 1, 9, 10 };
    BusinessKey const obsoleteB { 2, 201, 1, 9, 10 };
    BusinessKey const obsoleteC { 2, 202, 1, 9, 10 };
    std::array<ObservedAuction, 3> swapObserved = {{
        { swapDesired[0], false, 1 },
        { obsoleteA, false, 11 },
        { obsoleteB, false, 10 }
    }};
    ReconcilePlan swap = BuildRotationReconcilePlan(swapDesired, swapObserved);
    std::size_t const swapDeferred = ApplyDesiredLiveListingTarget(swap, 3);
    Require(swap.ok && swap.creates.size() == 2 && swap.retires.size() == 2 &&
        swap.retires[0] == 10 && swap.retires[1] == 11 && swapDeferred == 0,
        "exact target swaps obsolete listings for missing desired listings");
    Require(ApplyLiveListingCeiling(swap, 3, 3) == 0 && swap.creates.size() == 2,
        "one-for-one swaps remain under the live ceiling");

    std::array<ObservedAuction, 3> claimedObserved = {{
        { swapDesired[0], false, 1 },
        { obsoleteA, true, 11 },
        { obsoleteB, false, 10 }
    }};
    ReconcilePlan claimed = BuildRotationReconcilePlan(swapDesired, claimedObserved);
    std::size_t const claimedDeferred = ApplyDesiredLiveListingTarget(claimed, 3);
    Require(claimed.creates.size() == 1 && claimed.retires.size() == 1 &&
        claimed.retires[0] == 10 && claimedDeferred == 1,
        "claimed obsolete listing is never retired or replaced");

    std::array<ObservedAuction, 4> surplusObserved = {{
        { swapDesired[0], false, 1 },
        { obsoleteA, false, 11 },
        { obsoleteB, false, 10 },
        { obsoleteC, false, 12 }
    }};
    ReconcilePlan surplusSwap = BuildRotationReconcilePlan(swapDesired, surplusObserved);
    Require(ApplyDesiredLiveListingTarget(surplusSwap, 4) == 0 &&
        surplusSwap.creates.size() == 2 && surplusSwap.retires.size() == 3,
        "surplus retirement plus swaps converge to the exact target");

    ReconcilePlan migration = BuildRotationReconcilePlan(tierC.desired, {});
    Require(migration.ok && migration.creates.size() == 9000,
        "tier C initial reconcile");
    std::size_t const deferred = ApplyLiveListingCeiling(migration, 10916, 12000);
    Require(migration.ok && migration.creates.size() == 1084 && deferred == 7916,
        "tier C migration obeys hard live ceiling");

    ReconcilePlan first = BuildRotationReconcilePlan(tierA.desired, {});
    Require(first.ok && first.creates.size() == 3000, "tier A first reconcile");
    std::vector<ObservedAuction> complete;
    for (BusinessKey const& key : tierA.desired)
        complete.push_back({ key, false });
    ReconcilePlan second = BuildRotationReconcilePlan(tierA.desired, complete);
    Require(second.ok && second.creates.empty() && second.existingCount == 3000,
        "tier A idempotent reconcile");

    std::cout << "{\"result\":\"PASS\",\"tier_a\":3000,\"tier_b\":6000,"
        "\"tier_c\":9000,\"tier_d\":11000,\"profession_items\":4332,"
        "\"alliance_d\":9000,\"expiration_buckets\":96}\n";
}
