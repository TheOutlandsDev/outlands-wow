#ifndef LIVING_AZEROTH_WRITER_COMPATIBILITY_PLAN_H
#define LIVING_AZEROTH_WRITER_COMPATIBILITY_PLAN_H

#include <cstdint>

namespace LivingAzeroth::Economy::WriterCompatibility
{
struct StaticSellerObservation
{
    bool Enabled = false;
    bool ProcurementMayBuyOwnedSystem = false;
    std::uint32_t SystemSellerCount = 0;
    bool LockedSystemSellerPresent = false;
    std::uint32_t Account = 0;
    std::uint32_t Guid = 0;
    std::uint32_t DurationHours = 0;
    std::uint32_t ListingTier = 0;
    std::uint32_t MaxLiveListings = 0;
    bool CatalogMatches = false;
    bool ReclaimExpiredMail = false;
    std::uint32_t ReclaimMaxPerRun = 0;
    bool RelistOnExpire = false;
};

inline bool IsSupportedTier(std::uint32_t tier)
{
    return tier == 3000 || tier == 6000 || tier == 9000 || tier == 11000;
}

inline bool Compatible(StaticSellerObservation const& observed)
{
    if (!observed.Enabled)
        return true;
    bool const reclaimPolicyValid = !observed.RelistOnExpire ||
        observed.ReclaimExpiredMail;
    bool const reclaimLimitValid = !observed.ReclaimExpiredMail ||
        observed.ReclaimMaxPerRun == observed.ListingTier;
    return !observed.ProcurementMayBuyOwnedSystem &&
        observed.SystemSellerCount == 1 && observed.LockedSystemSellerPresent &&
        observed.Account > 0 && observed.Guid == 900001 &&
        observed.DurationHours == 48 && IsSupportedTier(observed.ListingTier) &&
        observed.MaxLiveListings >= observed.ListingTier &&
        observed.MaxLiveListings <= 12000 && observed.CatalogMatches &&
        reclaimPolicyValid && reclaimLimitValid;
}
}

#endif
