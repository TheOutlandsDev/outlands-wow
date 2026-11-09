#include "../src/LivingAzerothEconomyPlan.h"

#include <cstdlib>
#include <iostream>
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
    std::vector<BusinessKey> desired = BuildDesiredMatrix(StaticCatalog);
    Require(desired.size() == 900, "desired matrix must contain 900 rows");
    Require(std::set<BusinessKey>(desired.begin(), desired.end()).size() == 900,
        "desired matrix must contain 900 unique business keys");

    ReconcilePlan first = BuildReconcilePlan(StaticCatalog, {});
    Require(first.ok, "empty fixture must plan successfully");
    Require(first.creates.size() == 900, "first reconciliation must emit 900 creates");
    Require(first.existingCount == 0, "first reconciliation must observe zero existing keys");

    std::vector<ObservedAuction> complete;
    for (BusinessKey const& key : desired)
        complete.push_back({ key, false });

    ReconcilePlan second = BuildReconcilePlan(StaticCatalog, complete);
    Require(second.ok, "complete fixture must plan successfully");
    Require(second.creates.empty(), "second reconciliation must emit zero mutations");
    Require(second.existingCount == 900, "second reconciliation must observe 900 keys");

    auto duplicateCatalog = StaticCatalog;
    duplicateCatalog[1].itemEntry = duplicateCatalog[0].itemEntry;
    ReconcilePlan duplicateCatalogPlan = BuildReconcilePlan(duplicateCatalog, {});
    Require(!duplicateCatalogPlan.ok, "duplicate catalog must fail closed");
    Require(duplicateCatalogPlan.creates.empty(), "duplicate catalog must emit no creates");

    auto malformedCatalog = StaticCatalog;
    malformedCatalog[0].startBid = malformedCatalog[0].buyout;
    ReconcilePlan malformedCatalogPlan = BuildReconcilePlan(malformedCatalog, {});
    Require(!malformedCatalogPlan.ok, "malformed catalog must fail closed");
    Require(malformedCatalogPlan.creates.empty(), "malformed catalog must emit no creates");

    std::vector<ObservedAuction> duplicateObserved = complete;
    duplicateObserved.push_back(complete.front());
    ReconcilePlan duplicateObservedPlan = BuildReconcilePlan(StaticCatalog, duplicateObserved);
    Require(!duplicateObservedPlan.ok, "duplicate observed key must fail closed");
    Require(duplicateObservedPlan.creates.empty(), "duplicate observed key must emit no creates");

    std::vector<ObservedAuction> unexpectedObserved = complete;
    unexpectedObserved[0].key.itemEntry = 999999;
    ReconcilePlan unexpectedObservedPlan = BuildReconcilePlan(StaticCatalog, unexpectedObserved);
    Require(!unexpectedObservedPlan.ok, "unexpected observed key must fail closed");
    Require(unexpectedObservedPlan.creates.empty(), "unexpected observed key must emit no creates");

    std::vector<ObservedAuction> bidObserved = complete;
    bidObserved[0].hasBid = true;
    ReconcilePlan bidObservedPlan = BuildReconcilePlan(StaticCatalog, bidObserved);
    Require(!bidObservedPlan.ok, "synthetic bid must fail closed");
    Require(bidObservedPlan.creates.empty(), "synthetic bid must emit no creates");

    std::cout
        << "{\"result\":\"PASS\",\"catalog_rows\":" << StaticCatalog.size()
        << ",\"desired_keys\":" << desired.size()
        << ",\"first_creates\":" << first.creates.size()
        << ",\"second_creates\":" << second.creates.size()
        << ",\"catalog_sha256\":\"" << StaticCatalogSha256 << "\"}\n";
    return 0;
}
