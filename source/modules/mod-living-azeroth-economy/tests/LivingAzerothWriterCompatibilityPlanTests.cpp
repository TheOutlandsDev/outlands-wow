#include "LivingAzerothWriterCompatibilityPlan.h"

#include <cstdlib>
#include <iostream>

namespace Compatibility = LivingAzeroth::Economy::WriterCompatibility;

namespace
{
void Require(bool condition, char const* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

Compatibility::StaticSellerObservation ProductionShape()
{
    Compatibility::StaticSellerObservation observed;
    observed.Enabled = true;
    observed.SystemSellerCount = 1;
    observed.LockedSystemSellerPresent = true;
    observed.Account = 58;
    observed.Guid = 900001;
    observed.DurationHours = 48;
    observed.ListingTier = 11000;
    observed.MaxLiveListings = 12000;
    observed.CatalogMatches = true;
    observed.ReclaimExpiredMail = true;
    observed.ReclaimMaxPerRun = 11000;
    observed.RelistOnExpire = true;
    return observed;
}
}

int main()
{
    auto observed = ProductionShape();
    Require(Compatibility::Compatible(observed),
        "locked production static seller and procurement are compatible");

    observed.ProcurementMayBuyOwnedSystem = true;
    Require(!Compatibility::Compatible(observed),
        "procurement may not buy from the static system seller");
    observed.ProcurementMayBuyOwnedSystem = false;

    observed.ReclaimExpiredMail = false;
    Require(!Compatibility::Compatible(observed),
        "relisting without reclaim must be rejected");
    observed.RelistOnExpire = false;
    Require(Compatibility::Compatible(observed),
        "disabled reclaim and relist remain compatible");

    observed = ProductionShape();
    observed.ReclaimMaxPerRun = 10999;
    Require(!Compatibility::Compatible(observed),
        "reclaim limit must equal listing tier");
    observed = ProductionShape();
    observed.Guid = 554;
    Require(!Compatibility::Compatible(observed),
        "only the locked system seller is compatible");

    std::cout << "{\"result\":\"PASS\",\"production_shape\":true,"
                 "\"relist_without_reclaim\":false}\n";
}
