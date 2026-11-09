#include "LivingAzerothCommerceRotationPlan.h"

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace LivingAzeroth::Economy::CommerceRotation;

static void Require(bool condition, char const* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main()
{
    std::vector<Candidate> candidates = {
        { 8, Faction::Horde, true, false },
        { 4, Faction::Alliance, true, false },
        { 3, Faction::Alliance, false, false },
        { 7, Faction::Horde, true, false },
        { 2, Faction::Other, true, false }
    };
    auto decisions = Plan(candidates, 1);
    Require(decisions.size() == 2, "one candidate per faction expected");
    Require(decisions[0].guid == 4 && decisions[0].faction == Faction::Alliance,
        "lowest available Alliance guid expected");
    Require(decisions[1].guid == 7 && decisions[1].faction == Faction::Horde,
        "lowest available Horde guid expected");

    candidates.push_back({ 1, Faction::Alliance, true, true });
    decisions = Plan(candidates, 1);
    Require(decisions.size() == 1 && decisions[0].guid == 7,
        "present Alliance broker must suppress Alliance teleport");

    Require(Plan(candidates, 0).empty(), "zero cap must fail closed");
    std::cout << "{\"result\":\"PASS\",\"max_per_faction\":1}\n";
}
