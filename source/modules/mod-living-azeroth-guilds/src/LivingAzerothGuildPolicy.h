#ifndef LIVING_AZEROTH_GUILD_POLICY_H
#define LIVING_AZEROTH_GUILD_POLICY_H

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

namespace LivingAzeroth::Guilds
{
enum class NativeFaction : std::uint8_t
{
    Alliance = 0,
    Horde = 1,
    Invalid = 255
};

enum class SubjectKind : std::uint8_t
{
    Human,
    Ai,
    System,
    Excluded,
    Quarantined
};

struct RankPolicyEntry
{
    std::uint8_t NativeRank;
    std::uint32_t MinimumPoints;
    std::uint32_t Rights;
    std::string_view AllianceName;
    std::string_view HordeName;
};

constexpr std::array<RankPolicyEntry, 10> RankPolicy = {{
    {0, std::numeric_limits<std::uint32_t>::max(), 0x001DF1FFu, "High Commander", "High Warlord"},
    {1, 25000u, 0x0000E04Fu, "War Council", "War Council"},
    {2, 18000u, 0x0000E04Fu, "General", "General"},
    {3, 13000u, 0x0000E04Fu, "Colonel", "Colonel"},
    {4, 9500u, 0x0000E04Fu, "Major", "Major"},
    {5, 6500u, 0x00002043u, "Captain", "Captain"},
    {6, 4000u, 0x00002043u, "Lieutenant", "Lieutenant"},
    {7, 2000u, 0x00002043u, "Sergeant", "Sergeant"},
    {8, 1000u, 0x00000043u, "Veteran", "Veteran"},
    {9, 0u, 0x00000043u, "Recruit", "Recruit"}
}};

constexpr NativeFaction FactionFromRace(std::uint8_t race)
{
    switch (race)
    {
        case 1:  // Human
        case 3:  // Dwarf
        case 4:  // Night elf
        case 7:  // Gnome
        case 11: // Draenei
            return NativeFaction::Alliance;
        case 2:  // Orc
        case 5:  // Undead
        case 6:  // Tauren
        case 8:  // Troll
        case 10: // Blood elf
            return NativeFaction::Horde;
        default:
            return NativeFaction::Invalid;
    }
}

constexpr std::uint32_t SaturatingAdd(std::uint64_t left, std::uint64_t right)
{
    std::uint64_t const total = left + right;
    return total > std::numeric_limits<std::uint32_t>::max()
        ? std::numeric_limits<std::uint32_t>::max()
        : static_cast<std::uint32_t>(total);
}

constexpr std::uint32_t ComputeRankPoints(
    std::uint8_t level,
    std::uint32_t rewardedQuests,
    std::uint32_t honorableKills)
{
    std::uint64_t const levelPoints = static_cast<std::uint64_t>(level) * 100u;
    std::uint64_t const questPoints = static_cast<std::uint64_t>(rewardedQuests) * 20u;
    std::uint64_t const honorPoints = static_cast<std::uint64_t>(honorableKills) * 50u;
    return SaturatingAdd(SaturatingAdd(levelPoints, questPoints), honorPoints);
}

constexpr std::uint8_t EarnedNativeRank(std::uint32_t points)
{
    for (std::size_t index = 1; index < RankPolicy.size(); ++index)
        if (points >= RankPolicy[index].MinimumPoints)
            return RankPolicy[index].NativeRank;
    return 9;
}

constexpr bool CanEnroll(
    SubjectKind subject,
    NativeFaction characterFaction,
    NativeFaction guildFaction,
    bool hasExternalGuild,
    bool moveOwnedAi,
    bool autoEnrollHuman)
{
    if (characterFaction == NativeFaction::Invalid || characterFaction != guildFaction)
        return false;
    if (subject == SubjectKind::Excluded || subject == SubjectKind::Quarantined)
        return false;
    if (subject == SubjectKind::System)
        return true;
    if (subject == SubjectKind::Ai)
        return !hasExternalGuild || moveOwnedAi;
    if (subject == SubjectKind::Human)
        return !hasExternalGuild && autoEnrollHuman;
    return false;
}

constexpr std::string_view FactionName(NativeFaction faction)
{
    return faction == NativeFaction::Alliance ? "alliance" :
        faction == NativeFaction::Horde ? "horde" : "invalid";
}

constexpr std::string_view RankName(NativeFaction faction, std::uint8_t rank)
{
    if (rank >= RankPolicy.size())
        return "invalid";
    return faction == NativeFaction::Horde
        ? RankPolicy[rank].HordeName
        : RankPolicy[rank].AllianceName;
}
}

#endif
