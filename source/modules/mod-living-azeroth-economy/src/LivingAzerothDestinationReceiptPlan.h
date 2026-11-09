#ifndef LIVING_AZEROTH_DESTINATION_RECEIPT_PLAN_H
#define LIVING_AZEROTH_DESTINATION_RECEIPT_PLAN_H

#include <string_view>

namespace LivingAzeroth::Economy::DestinationReceipt
{
enum class State
{
    Missing,
    Applied,
    Conflict,
    Invalid
};

struct Observation
{
    bool QuerySucceeded = false;
    bool Exists = false;
    bool IdentityMatches = false;
    std::string_view StoredActionType;
    std::string_view StoredPayloadSha256;
    std::string_view ExpectedActionType;
    std::string_view ExpectedPayloadSha256;
};

constexpr State Classify(Observation const& observed)
{
    if (!observed.QuerySucceeded)
        return State::Invalid;
    if (!observed.Exists)
        return State::Missing;
    if (observed.IdentityMatches &&
        observed.StoredActionType == observed.ExpectedActionType &&
        observed.StoredPayloadSha256 == observed.ExpectedPayloadSha256)
        return State::Applied;
    return State::Conflict;
}
}

#endif
