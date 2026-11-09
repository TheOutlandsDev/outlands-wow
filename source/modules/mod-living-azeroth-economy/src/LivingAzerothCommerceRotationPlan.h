#ifndef LIVING_AZEROTH_COMMERCE_ROTATION_PLAN_H
#define LIVING_AZEROTH_COMMERCE_ROTATION_PLAN_H

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace LivingAzeroth::Economy::CommerceRotation
{
enum class Faction : std::uint8_t
{
    Alliance,
    Horde,
    Other
};

struct Candidate
{
    std::uint32_t guid = 0;
    Faction faction = Faction::Other;
    bool available = false;
    bool atAuctioneer = false;
};

struct Decision
{
    std::uint32_t guid = 0;
    Faction faction = Faction::Other;
};

inline std::vector<Decision> Plan(
    std::span<Candidate const> candidates,
    std::uint32_t maximumPerFaction)
{
    std::vector<Decision> decisions;
    if (maximumPerFaction == 0)
        return decisions;

    std::vector<Candidate> ordered(candidates.begin(), candidates.end());
    std::sort(ordered.begin(), ordered.end(), [](Candidate const& left, Candidate const& right)
    {
        return left.guid < right.guid;
    });

    std::uint32_t alliancePresent = 0;
    std::uint32_t hordePresent = 0;
    for (Candidate const& candidate : ordered)
    {
        if (!candidate.available || !candidate.atAuctioneer)
            continue;
        if (candidate.faction == Faction::Alliance)
            ++alliancePresent;
        else if (candidate.faction == Faction::Horde)
            ++hordePresent;
    }

    for (Candidate const& candidate : ordered)
    {
        if (!candidate.available || candidate.atAuctioneer || candidate.guid == 0)
            continue;
        if (candidate.faction == Faction::Alliance && alliancePresent < maximumPerFaction)
        {
            decisions.push_back({ candidate.guid, candidate.faction });
            ++alliancePresent;
        }
        else if (candidate.faction == Faction::Horde && hordePresent < maximumPerFaction)
        {
            decisions.push_back({ candidate.guid, candidate.faction });
            ++hordePresent;
        }
    }
    return decisions;
}
}

#endif
