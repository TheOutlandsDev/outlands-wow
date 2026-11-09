#include "LivingAzerothProcurementBuyerSelectionPlan.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace Procurement = LivingAzeroth::Economy::GuildProcurement;

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

Procurement::BuyerSelectionCandidate Candidate(
    std::uint32_t guid, std::uint32_t account,
    Procurement::Faction faction, std::uint64_t money = 100000)
{
    return { guid, account, faction, money, true, true, true, true, false, true };
}
}

int main()
{
    using Procurement::Faction;
    std::vector candidates = {
        Candidate(35, 7, Faction::Alliance),
        Candidate(21, 8, Faction::Alliance),
        Candidate(14, 9, Faction::Horde)
    };

    auto preferred = Procurement::SelectBuyer(35, Faction::Alliance, 99,
        4500, 1000, candidates);
    Require(preferred.Found && !preferred.Failover && preferred.Guid == 35,
        "loaded preferred buyer must win");

    candidates[0].InWorld = false;
    auto failover = Procurement::SelectBuyer(35, Faction::Alliance, 99,
        4500, 1000, candidates);
    Require(failover.Found && failover.Failover && failover.Guid == 21,
        "offline preferred must use deterministic same-faction failover");

    candidates.push_back(Candidate(11, 10, Faction::Alliance));
    auto deterministic = Procurement::SelectBuyer(35, Faction::Alliance, 99,
        4500, 1000, candidates);
    Require(deterministic.Found && deterministic.Guid == 11,
        "lowest eligible failover GUID must be deterministic");

    candidates[3].OwnedAi = false;
    candidates[1].Account = 99;
    auto protectedSeller = Procurement::SelectBuyer(35, Faction::Alliance, 99,
        4500, 1000, candidates);
    Require(!protectedSeller.Found,
        "human candidate and seller account must be excluded");

    candidates[1].Account = 8;
    candidates[1].Money = 5499;
    auto reserve = Procurement::SelectBuyer(35, Faction::Alliance, 99,
        4500, 1000, candidates);
    Require(!reserve.Found, "buyer below buyout plus reserve must be excluded");

    candidates[2].CharacterFaction = Faction::Horde;
    auto wrongFaction = Procurement::SelectBuyer(35, Faction::Alliance, 99,
        4500, 1000, std::span(candidates).subspan(2, 1));
    Require(!wrongFaction.Found, "wrong-faction buyer must be excluded");

    std::cout << "{\"result\":\"PASS\",\"preferred\":35,"
                 "\"deterministic_failover\":11,\"no_eligible\":true}\n";
}
