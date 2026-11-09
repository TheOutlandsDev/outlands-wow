#include "LivingAzerothDestinationReceiptPlan.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace Receipt = LivingAzeroth::Economy::DestinationReceipt;

namespace
{
void Expect(Receipt::State actual, Receipt::State expected, std::string_view name)
{
    if (actual == expected)
        return;

    std::cerr << "FAIL " << name << std::endl;
    std::exit(1);
}
}

int main()
{
    Receipt::Observation observed;
    observed.QuerySucceeded = true;
    observed.ExpectedActionType = "auction_buyout";
    observed.ExpectedPayloadSha256 = "payload-a";
    Expect(Receipt::Classify(observed), Receipt::State::Missing,
        "missing receipt permits first attempt");

    observed = {};
    observed.QuerySucceeded = true;
    observed.Exists = true;
    observed.IdentityMatches = true;
    observed.StoredActionType = "auction_buyout";
    observed.StoredPayloadSha256 = "payload-a";
    observed.ExpectedActionType = "auction_buyout";
    observed.ExpectedPayloadSha256 = "payload-a";
    Expect(Receipt::Classify(observed), Receipt::State::Applied,
        "matching receipt suppresses duplicate");

    observed = {};
    observed.QuerySucceeded = true;
    observed.Exists = true;
    observed.IdentityMatches = true;
    observed.StoredActionType = "auction_buyout";
    observed.StoredPayloadSha256 = "payload-b";
    observed.ExpectedActionType = "auction_buyout";
    observed.ExpectedPayloadSha256 = "payload-a";
    Expect(Receipt::Classify(observed), Receipt::State::Conflict,
        "different payload conflicts");

    observed = {};
    observed.QuerySucceeded = true;
    observed.Exists = true;
    observed.IdentityMatches = false;
    observed.StoredActionType = "auction_buyout";
    observed.StoredPayloadSha256 = "payload-a";
    observed.ExpectedActionType = "auction_buyout";
    observed.ExpectedPayloadSha256 = "payload-a";
    Expect(Receipt::Classify(observed), Receipt::State::Conflict,
        "different identity conflicts");

    observed = {};
    observed.QuerySucceeded = false;
    observed.ExpectedActionType = "auction_buyout";
    observed.ExpectedPayloadSha256 = "payload-a";
    Expect(Receipt::Classify(observed), Receipt::State::Invalid,
        "read failure remains unknown");

    std::cout << "PASS cases=5" << std::endl;
    return 0;
}
