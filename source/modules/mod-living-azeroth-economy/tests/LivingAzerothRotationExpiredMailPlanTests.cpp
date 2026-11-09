#include "../src/LivingAzerothRotationExpiredMailPlan.h"

#include <cassert>
#include <iostream>
#include <vector>

using namespace LivingAzeroth::Economy;

namespace
{
BusinessKey AllowedKey(std::uint32_t entry)
{
    return { 2, entry, 20, 100 };
}

ExpiredMailObservation ValidRow(std::uint32_t index)
{
    std::uint32_t const entry = 10000 + index;
    return {
        20000 + index, 30000 + index, 2, 2, 900001, true, 0, 0, entry, 20, 900001,
        1, 0, 0, entry, 0, 3, 40000 + index, 20
    };
}
}

int main()
{
    std::vector<BusinessKey> allowed;
    std::vector<ExpiredMailObservation> rows;
    allowed.reserve(11000);
    rows.reserve(11000);
    for (std::uint32_t index = 0; index < 11000; ++index)
    {
        allowed.push_back(AllowedKey(10000 + index));
        rows.push_back(ValidRow(index));
    }

    auto full = BuildRotationExpiredMailPlan(allowed, rows, 900001, 11000);
    assert(full.ok && full.cleanup.size() == 11000);

    auto capRejected = BuildRotationExpiredMailPlan(allowed, rows, 900001, 11001);
    assert(!capRejected.ok && capRejected.error == "max_per_run_out_of_bounds");

    rows.push_back(ValidRow(11000));
    auto tooMany = BuildRotationExpiredMailPlan(allowed, rows, 900001, 11000);
    assert(!tooMany.ok && tooMany.error == "expired_mail_limit_exceeded");

    rows.resize(1);
    rows[0].itemEntry = 999999;
    rows[0].subjectItemEntry = 999999;
    auto unapproved = BuildRotationExpiredMailPlan(allowed, rows, 900001, 11000);
    assert(!unapproved.ok && unapproved.error == "expired_mail_not_in_rotation_catalog");

    std::cout << "LivingAzerothRotationExpiredMailPlanTests PASS\n";
    return 0;
}
