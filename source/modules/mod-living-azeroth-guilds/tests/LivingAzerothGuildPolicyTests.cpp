#include "../src/LivingAzerothGuildPolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

using namespace LivingAzeroth::Guilds;

namespace
{
void Require(bool condition, std::string const& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

int main()
{
    for (std::uint8_t race : std::array<std::uint8_t, 5>{1, 3, 4, 7, 11})
        Require(FactionFromRace(race) == NativeFaction::Alliance, "Alliance race mapping");
    for (std::uint8_t race : std::array<std::uint8_t, 5>{2, 5, 6, 8, 10})
        Require(FactionFromRace(race) == NativeFaction::Horde, "Horde race mapping");
    for (std::uint8_t race : std::array<std::uint8_t, 4>{0, 9, 12, 255})
        Require(FactionFromRace(race) == NativeFaction::Invalid, "invalid race rejection");

    Require(ComputeRankPoints(1, 0, 0) == 100, "level points");
    Require(ComputeRankPoints(80, 100, 10) == 10500, "combined points");
    Require(ComputeRankPoints(80, std::numeric_limits<std::uint32_t>::max(),
                              std::numeric_limits<std::uint32_t>::max()) ==
                std::numeric_limits<std::uint32_t>::max(),
            "point saturation");

    Require(EarnedNativeRank(0) == 9, "recruit lower boundary");
    Require(EarnedNativeRank(999) == 9, "recruit upper boundary");
    Require(EarnedNativeRank(1000) == 8, "veteran boundary");
    Require(EarnedNativeRank(1999) == 8, "veteran upper boundary");
    Require(EarnedNativeRank(2000) == 7, "sergeant boundary");
    Require(EarnedNativeRank(24999) == 2, "general upper boundary");
    Require(EarnedNativeRank(25000) == 1, "war council boundary");
    Require(EarnedNativeRank(std::numeric_limits<std::uint32_t>::max()) == 1,
            "rank zero is never earned");

    Require(CanEnroll(SubjectKind::Ai, NativeFaction::Alliance, NativeFaction::Alliance,
                      false, false, false),
            "unguilded AI enrollment");
    Require(!CanEnroll(SubjectKind::Ai, NativeFaction::Alliance, NativeFaction::Horde,
                       false, true, false),
            "opposite faction rejection");
    Require(!CanEnroll(SubjectKind::Human, NativeFaction::Alliance, NativeFaction::Alliance,
                       true, true, true),
            "external human guild protection");
    Require(CanEnroll(SubjectKind::Human, NativeFaction::Alliance, NativeFaction::Alliance,
                      false, false, true),
            "unguilded human opt-in");
    Require(!CanEnroll(SubjectKind::Excluded, NativeFaction::Alliance, NativeFaction::Alliance,
                       false, true, true),
            "system exclusion");

    Require(RankPolicy.size() == 10, "exact rank count");
    Require(RankPolicy[0].Rights == 0x001DF1FFu, "guild master rights");
    for (std::size_t index = 1; index < RankPolicy.size(); ++index)
    {
        Require(RankPolicy[index].NativeRank == index, "contiguous native rank IDs");
        Require(RankPolicy[index].Rights != 0x001DF1FFu, "ordinary rank cannot have GM rights");
    }

    std::cout << "PASS: faction mapping, score saturation, ten ranks, and enrollment guards\n";
    return 0;
}
