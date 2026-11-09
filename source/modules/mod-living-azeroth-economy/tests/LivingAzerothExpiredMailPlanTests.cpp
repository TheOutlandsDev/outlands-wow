#include "../src/LivingAzerothExpiredMailPlan.h"

#include <cassert>
#include <iostream>
#include <vector>

using namespace LivingAzeroth::Economy;

namespace
{
ExpiredMailObservation ValidRow()
{
    return {
        70, 26478, 2, 6, 900001, true, 0, 0, 2770, 20, 900001,
        1, 0, 0, 2770, 0, 3, 31, 20
    };
}
}

int main()
{
    {
        std::vector<ExpiredMailObservation> rows = { ValidRow() };
        auto plan = BuildExpiredMailPlan(StaticCatalog, rows, 900001, 900);
        assert(plan.ok && plan.cleanup.size() == 1);
    }
    {
        std::vector<ExpiredMailObservation> rows;
        auto plan = BuildExpiredMailPlan(StaticCatalog, rows, 900001, 90);
        assert(plan.ok && plan.cleanup.empty());
    }
    {
        std::vector<ExpiredMailObservation> rows = { ValidRow() };
        auto plan = BuildExpiredMailPlan(StaticCatalog, rows, 900001, 90);
        assert(plan.ok && plan.cleanup.size() == 1);
        assert(plan.cleanup[0].mailId == 70 && plan.cleanup[0].itemGuid == 26478);
    }
    {
        auto row = ValidRow();
        row.subjectResponse = 2;
        std::vector<ExpiredMailObservation> rows = { row };
        auto plan = BuildExpiredMailPlan(StaticCatalog, rows, 900001, 90);
        assert(!plan.ok && plan.error == "expired_mail_subject_invalid");
    }
    {
        auto row = ValidRow();
        row.itemEntry = 6948;
        row.subjectItemEntry = 6948;
        std::vector<ExpiredMailObservation> rows = { row };
        auto plan = BuildExpiredMailPlan(StaticCatalog, rows, 900001, 90);
        assert(!plan.ok && plan.error == "expired_mail_not_in_locked_catalog");
    }
    {
        auto row = ValidRow();
        row.auctionReferences = 1;
        std::vector<ExpiredMailObservation> rows = { row };
        auto plan = BuildExpiredMailPlan(StaticCatalog, rows, 900001, 90);
        assert(!plan.ok && plan.error == "expired_mail_shape_invalid");
    }
    {
        auto row = ValidRow();
        std::vector<ExpiredMailObservation> rows = { row, row };
        auto plan = BuildExpiredMailPlan(StaticCatalog, rows, 900001, 90);
        assert(!plan.ok && plan.error == "expired_mail_duplicate");
    }
    {
        std::vector<ExpiredMailObservation> rows(91, ValidRow());
        auto plan = BuildExpiredMailPlan(StaticCatalog, rows, 900001, 90);
        assert(!plan.ok && plan.error == "expired_mail_limit_exceeded");
    }
    {
        std::vector<ExpiredMailObservation> rows;
        auto plan = BuildExpiredMailPlan(StaticCatalog, rows, 900001, 901);
        assert(!plan.ok && plan.error == "max_per_run_out_of_bounds");
    }
    {
        std::vector<ExpiredMailObservation> rows = { ValidRow() };
        auto plan = BuildExpiredMailPlan(StaticCatalog, rows, 0, 90);
        assert(!plan.ok && plan.error == "seller_guid_required");
    }

    std::cout << "LivingAzerothExpiredMailPlanTests PASS\n";
    return 0;
}
