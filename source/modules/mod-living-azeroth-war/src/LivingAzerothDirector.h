#ifndef LIVING_AZEROTH_DIRECTOR_H
#define LIVING_AZEROTH_DIRECTOR_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace LivingAzeroth::Director
{
constexpr std::uint32_t AI3_POPULATION_HARD_CAP = 50;
constexpr std::uint64_t SECONDS_PER_HOUR = 3600;
constexpr std::uint8_t FACTION_BALANCED = 255;

enum class Provenance
{
    Online,
    LastKnown,
    Baseline
};

inline char const* ToString(Provenance provenance)
{
    switch (provenance)
    {
        case Provenance::Online:
            return "online";
        case Provenance::LastKnown:
            return "last_known";
        case Provenance::Baseline:
            return "baseline";
    }

    return "baseline";
}

struct ProfessionSummary
{
    std::uint16_t SkillId = 0;
    std::uint16_t Value = 0;
    std::uint8_t Step = 0;
};

struct ProfileInput
{
    std::uint64_t CharacterGuid = 0;
    std::uint8_t Faction = FACTION_BALANCED;
    std::uint8_t ClassId = 0;
    std::uint8_t Level = 1;
    std::uint32_t MapId = 0;
    std::uint32_t ZoneId = 0;
    bool Grouped = false;
    bool InInstance = false;
    std::uint32_t AverageItemLevelTimes100 = 0;
    std::uint8_t EquippedItemCount = 0;
    std::uint8_t ActiveQuestCount = 0;
    std::uint32_t RewardedQuestCount = 0;
    ProfessionSummary Profession1;
    ProfessionSummary Profession2;
};

struct SessionCandidate
{
    bool SessionPresent = false;
    bool PlayerPresent = false;
    bool InWorld = false;
    bool IsBot = false;
    bool IsGameMaster = false;
    std::uint32_t AccountId = 0;
    ProfileInput Profile;
};

struct ReferenceProfile
{
    ProfileInput Profile;
    std::uint64_t ObservedAtUnix = 0;
    std::string SourceSnapshotId;
    std::string ConfigSha256;
};

struct DirectorConfig
{
    bool Enabled = false;
    bool ObserveOnly = true;
    std::uint32_t DecisionIntervalSeconds = 30;

    std::uint64_t PrimaryCharacterGuid = 0;
    bool AggregateAllHumans = true;
    bool ExcludeGameMasters = true;
    std::set<std::uint32_t> ExcludedAccountIds;
    std::set<std::uint64_t> ExcludedCharacterGuids;

    std::string OfflineMode = "LastKnownThenBaseline";
    std::uint32_t LastKnownMaxAgeHours = 168;
    std::uint8_t BaselineLevel = 10;
    std::string BaselineFactionMode = "Balanced";
    std::uint32_t BaselineMap = 0;
    std::uint32_t BaselineZone = 12;

    std::uint32_t PopulationOfflineTarget = 50;
    std::uint32_t PopulationPerHuman = 10;
    std::uint32_t PopulationMaximum = 50;
    std::uint32_t PopulationLoginBatch = 2;
    std::uint32_t PopulationLogoutBatch = 1;
    std::uint32_t PopulationChangeCooldownSeconds = 60;
    std::uint8_t PopulationLevelBelow = 3;
    std::uint8_t PopulationLevelAbove = 3;
    bool PopulationPreserveCombatGroupQueue = true;
    std::uint32_t PopulationVisibleReleaseRadius = 150;

    std::uint8_t ActivityBackgroundPercent = 20;
    bool PartyEnabled = false;
    bool PartyHumanInitiatedOnly = true;
    std::string GuildHumanEnrollment = "OptIn";

    bool EconomyEnabled = false;
    bool EconomyProfileAdaptive = false;
    std::uint32_t EconomyUpdateIntervalMinutes = 30;
    std::uint32_t EconomyMaximumSyntheticListings = 75;

    // AI3 fail-closed actuator observations. These are inputs to validation,
    // never outputs that the director can change.
    bool LegacyPopulationEnabled = false;
    bool LegacyPersistentProfilesEnabled = false;
    bool LegacySharedGuildEnabled = false;
    bool WarEnabled = false;
    bool PlayerbotsEnabled = false;
    bool RandomBotAutologinEnabled = false;
    bool AhBotSellerEnabled = false;
    bool AhBotBuyerEnabled = false;
};

struct PopulationCohort
{
    std::uint8_t Faction = FACTION_BALANCED;
    std::uint8_t LevelMinimum = 1;
    std::uint8_t LevelMaximum = 1;
    std::uint32_t MapId = 0;
    std::uint32_t ZoneId = 0;
    std::uint32_t MemberCount = 0;
    std::uint32_t RequestedTarget = 0;
    std::uint32_t BoundedTarget = 0;
};

struct PolicyPlan
{
    Provenance Source = Provenance::Baseline;
    std::uint32_t EligibleOnlineHumanCount = 0;
    std::vector<ProfileInput> Inputs;
    std::vector<PopulationCohort> Cohorts;
    std::uint32_t PopulationRequestedTarget = 0;
    std::uint32_t PopulationBoundedTarget = 0;
    std::uint32_t BackgroundRequestedTarget = 0;
};

struct DirectorView
{
    std::string SnapshotId;
    std::string ConfigSha256;
    PolicyPlan Plan;
};

struct FixtureReport
{
    std::uint32_t Passed = 0;
    std::uint32_t Total = 0;
    std::vector<std::string> Failures;
};

inline std::vector<std::string> ValidateConfig(DirectorConfig const& config)
{
    std::vector<std::string> errors;

    if (config.DecisionIntervalSeconds < 5 || config.DecisionIntervalSeconds > 3600)
        errors.emplace_back("Director.DecisionIntervalSeconds must be between 5 and 3600.");
    if (config.OfflineMode != "LastKnownThenBaseline")
        errors.emplace_back("AI3 supports only Director.Offline.Mode=LastKnownThenBaseline.");
    if (config.LastKnownMaxAgeHours == 0 || config.LastKnownMaxAgeHours > 168)
        errors.emplace_back("Director.Offline.LastKnownMaxAgeHours must be between 1 and 168.");
    if (config.BaselineLevel == 0 || config.BaselineLevel > 80)
        errors.emplace_back("Director.Offline.BaselineLevel must be between 1 and 80.");
    if (config.BaselineFactionMode != "Balanced")
        errors.emplace_back("AI3 requires Director.Offline.BaselineFactionMode=Balanced.");
    if (config.PopulationMaximum == 0 || config.PopulationMaximum > AI3_POPULATION_HARD_CAP)
        errors.emplace_back("Director.Population.Maximum must be between 1 and 50.");
    if (config.PopulationOfflineTarget == 0 ||
        config.PopulationOfflineTarget > config.PopulationMaximum)
    {
        errors.emplace_back(
            "Director.Population.OfflineTarget must be between 1 and the configured maximum.");
    }
    if (config.PopulationPerHuman == 0 || config.PopulationPerHuman > config.PopulationMaximum)
        errors.emplace_back("Director.Population.PerHuman must be between 1 and the configured maximum.");
    if (config.PopulationLoginBatch == 0 || config.PopulationLoginBatch > 2)
        errors.emplace_back("AI3 permits a login batch of one or two only.");
    if (config.PopulationLogoutBatch != 1)
        errors.emplace_back("AI3 requires Director.Population.LogoutBatch=1.");
    if (config.PopulationChangeCooldownSeconds < 60)
        errors.emplace_back("Director.Population.ChangeCooldownSeconds must be at least 60.");
    if (config.PopulationLevelBelow > 10 || config.PopulationLevelAbove > 10)
        errors.emplace_back("Director population level offsets must not exceed ten.");
    if (config.ActivityBackgroundPercent != 20)
        errors.emplace_back("AI3 requires Director.Activity.BackgroundPercent=20.");
    if (!config.PartyHumanInitiatedOnly)
        errors.emplace_back("Director.Party.HumanInitiatedOnly must remain enabled.");
    if (config.GuildHumanEnrollment != "OptIn")
        errors.emplace_back("Director.Guild.HumanEnrollment must remain OptIn.");
    if (config.EconomyUpdateIntervalMinutes < 30)
        errors.emplace_back("Director.Economy.UpdateIntervalMinutes must be at least 30.");
    if (config.EconomyMaximumSyntheticListings > 75)
        errors.emplace_back("Director.Economy.MaximumSyntheticListings must not exceed 75.");
    if (!config.AggregateAllHumans)
        errors.emplace_back("AI3 requires Director.Profile.AggregateAllHumans=1.");
    if (!config.ExcludeGameMasters)
        errors.emplace_back("AI3 requires Director.Profile.ExcludeGameMasters=1.");
    if (config.PrimaryCharacterGuid != 0 &&
        config.ExcludedCharacterGuids.count(config.PrimaryCharacterGuid) != 0)
    {
        errors.emplace_back("The primary character GUID is also explicitly excluded.");
    }

    if (config.Enabled)
    {
        if (!config.ObserveOnly)
            errors.emplace_back("AI3 refuses Director.ObserveOnly=0.");
        if (config.PartyEnabled)
            errors.emplace_back("The party adapter is not available in AI3.");
        if (config.EconomyEnabled || config.EconomyProfileAdaptive)
            errors.emplace_back("The economy adapter is not available in AI3.");
        if (config.LegacyPopulationEnabled)
            errors.emplace_back("Population.Enable must remain disabled during AI3.");
        if (config.LegacyPersistentProfilesEnabled)
            errors.emplace_back("Population.PersistentProfiles must remain disabled during AI3.");
        if (config.LegacySharedGuildEnabled)
            errors.emplace_back("Population.SharedGuild.Enable must remain disabled during AI3.");
        if (config.WarEnabled)
            errors.emplace_back("War.Enable must remain disabled during AI3.");
        if (config.PlayerbotsEnabled)
            errors.emplace_back("AiPlayerbot.Enabled must remain disabled during AI3.");
        if (config.RandomBotAutologinEnabled)
            errors.emplace_back("AiPlayerbot.RandomBotAutologin must remain disabled during AI3.");
        if (config.AhBotSellerEnabled || config.AhBotBuyerEnabled)
            errors.emplace_back("AHBot seller and buyer must remain disabled during AI3.");
    }

    return errors;
}

inline bool IsEligibleHuman(SessionCandidate const& candidate, DirectorConfig const& config)
{
    if (!candidate.SessionPresent || !candidate.PlayerPresent || !candidate.InWorld || candidate.IsBot)
        return false;
    if (config.ExcludeGameMasters && candidate.IsGameMaster)
        return false;
    if (config.ExcludedAccountIds.count(candidate.AccountId) != 0)
        return false;
    if (config.ExcludedCharacterGuids.count(candidate.Profile.CharacterGuid) != 0)
        return false;
    return candidate.Profile.CharacterGuid != 0;
}

inline std::vector<ProfileInput> FilterEligibleHumans(
    std::vector<SessionCandidate> const& candidates,
    DirectorConfig const& config)
{
    std::vector<ProfileInput> profiles;
    for (SessionCandidate const& candidate : candidates)
    {
        if (IsEligibleHuman(candidate, config))
            profiles.push_back(candidate.Profile);
    }

    std::sort(profiles.begin(), profiles.end(), [](ProfileInput const& left, ProfileInput const& right)
    {
        return left.CharacterGuid < right.CharacterGuid;
    });
    return profiles;
}

inline bool IsReferenceFresh(
    ReferenceProfile const& reference,
    DirectorConfig const& config,
    std::uint64_t nowUnix)
{
    if (config.PrimaryCharacterGuid == 0 ||
        reference.Profile.CharacterGuid != config.PrimaryCharacterGuid ||
        reference.ObservedAtUnix > nowUnix)
    {
        return false;
    }

    std::uint64_t maximumAge =
        static_cast<std::uint64_t>(config.LastKnownMaxAgeHours) * SECONDS_PER_HOUR;
    return nowUnix - reference.ObservedAtUnix <= maximumAge;
}

inline void AllocateCohortTargets(
    std::vector<PopulationCohort>& cohorts,
    std::uint32_t boundedTotal,
    std::uint32_t requestedTotal)
{
    if (cohorts.empty() || requestedTotal == 0)
        return;

    struct Remainder
    {
        std::size_t Index = 0;
        std::uint64_t Value = 0;
    };

    std::uint32_t allocated = 0;
    std::vector<Remainder> remainders;
    for (std::size_t index = 0; index < cohorts.size(); ++index)
    {
        std::uint64_t scaled =
            static_cast<std::uint64_t>(boundedTotal) * cohorts[index].RequestedTarget;
        cohorts[index].BoundedTarget =
            static_cast<std::uint32_t>(scaled / requestedTotal);
        allocated += cohorts[index].BoundedTarget;
        remainders.push_back({index, scaled % requestedTotal});
    }

    std::sort(remainders.begin(), remainders.end(), [](Remainder const& left, Remainder const& right)
    {
        if (left.Value != right.Value)
            return left.Value > right.Value;
        return left.Index < right.Index;
    });

    for (std::size_t index = 0; allocated < boundedTotal && index < remainders.size(); ++index)
    {
        ++cohorts[remainders[index].Index].BoundedTarget;
        ++allocated;
    }
}

inline std::vector<PopulationCohort> BuildCohorts(
    std::vector<ProfileInput> const& inputs,
    DirectorConfig const& config,
    std::uint32_t requestedTotal,
    std::uint32_t boundedTotal,
    Provenance source)
{
    std::vector<PopulationCohort> cohorts;

    if (source == Provenance::Baseline)
    {
        PopulationCohort cohort;
        cohort.Faction = FACTION_BALANCED;
        cohort.LevelMinimum =
            static_cast<std::uint8_t>(std::max<int>(1, config.BaselineLevel - config.PopulationLevelBelow));
        cohort.LevelMaximum =
            static_cast<std::uint8_t>(std::min<int>(80, config.BaselineLevel + config.PopulationLevelAbove));
        cohort.MapId = config.BaselineMap;
        cohort.ZoneId = config.BaselineZone;
        cohort.RequestedTarget = requestedTotal;
        cohort.BoundedTarget = boundedTotal;
        cohorts.push_back(cohort);
        return cohorts;
    }

    std::uint32_t perMemberRequested =
        source == Provenance::LastKnown ?
            requestedTotal : config.PopulationPerHuman;

    for (ProfileInput const& input : inputs)
    {
        std::uint8_t minimum =
            static_cast<std::uint8_t>(std::max<int>(1, input.Level - config.PopulationLevelBelow));
        std::uint8_t maximum =
            static_cast<std::uint8_t>(std::min<int>(80, input.Level + config.PopulationLevelAbove));

        auto existing = std::find_if(cohorts.begin(), cohorts.end(),
            [&](PopulationCohort const& cohort)
        {
            return cohort.Faction == input.Faction &&
                   cohort.LevelMinimum == minimum &&
                   cohort.LevelMaximum == maximum &&
                   cohort.MapId == input.MapId &&
                   cohort.ZoneId == input.ZoneId;
        });

        if (existing == cohorts.end())
        {
            PopulationCohort cohort;
            cohort.Faction = input.Faction;
            cohort.LevelMinimum = minimum;
            cohort.LevelMaximum = maximum;
            cohort.MapId = input.MapId;
            cohort.ZoneId = input.ZoneId;
            cohort.MemberCount = 1;
            cohort.RequestedTarget = perMemberRequested;
            cohorts.push_back(cohort);
        }
        else
        {
            ++existing->MemberCount;
            existing->RequestedTarget += perMemberRequested;
        }
    }

    AllocateCohortTargets(cohorts, boundedTotal, requestedTotal);
    return cohorts;
}

inline PolicyPlan BuildPolicyPlan(
    DirectorConfig const& config,
    std::vector<ProfileInput> const& eligibleHumans,
    std::optional<ReferenceProfile> const& reference,
    std::uint64_t nowUnix)
{
    PolicyPlan plan;
    plan.EligibleOnlineHumanCount = static_cast<std::uint32_t>(eligibleHumans.size());

    if (!eligibleHumans.empty())
    {
        plan.Source = Provenance::Online;
        plan.Inputs = eligibleHumans;
        std::uint64_t requested =
            static_cast<std::uint64_t>(eligibleHumans.size()) *
            config.PopulationPerHuman;
        plan.PopulationRequestedTarget = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                requested, std::numeric_limits<std::uint32_t>::max()));
    }
    else if (reference && IsReferenceFresh(*reference, config, nowUnix))
    {
        plan.Source = Provenance::LastKnown;
        plan.Inputs.push_back(reference->Profile);
        plan.PopulationRequestedTarget = config.PopulationOfflineTarget;
    }
    else
    {
        plan.Source = Provenance::Baseline;
        plan.PopulationRequestedTarget = config.PopulationOfflineTarget;
    }

    plan.PopulationBoundedTarget =
        std::min(plan.PopulationRequestedTarget, config.PopulationMaximum);
    plan.BackgroundRequestedTarget = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(plan.PopulationBoundedTarget) *
         config.ActivityBackgroundPercent + 99) / 100);
    plan.Cohorts = BuildCohorts(
        plan.Inputs,
        config,
        plan.PopulationRequestedTarget,
        plan.PopulationBoundedTarget,
        plan.Source);
    return plan;
}

inline std::string CanonicalConfig(DirectorConfig const& config)
{
    std::ostringstream output;
    output << "enabled=" << config.Enabled
           << "|observe_only=" << config.ObserveOnly
           << "|decision_seconds=" << config.DecisionIntervalSeconds
           << "|primary_guid=" << config.PrimaryCharacterGuid
           << "|aggregate_all=" << config.AggregateAllHumans
           << "|exclude_gm=" << config.ExcludeGameMasters
           << "|offline_mode=" << config.OfflineMode
           << "|last_known_hours=" << config.LastKnownMaxAgeHours
           << "|baseline_level=" << static_cast<std::uint32_t>(config.BaselineLevel)
           << "|baseline_faction=" << config.BaselineFactionMode
           << "|baseline_map=" << config.BaselineMap
           << "|baseline_zone=" << config.BaselineZone
           << "|offline_target=" << config.PopulationOfflineTarget
           << "|per_human=" << config.PopulationPerHuman
           << "|maximum=" << config.PopulationMaximum
           << "|login_batch=" << config.PopulationLoginBatch
           << "|logout_batch=" << config.PopulationLogoutBatch
           << "|cooldown=" << config.PopulationChangeCooldownSeconds
           << "|level_below=" << static_cast<std::uint32_t>(config.PopulationLevelBelow)
           << "|level_above=" << static_cast<std::uint32_t>(config.PopulationLevelAbove)
           << "|preserve_busy=" << config.PopulationPreserveCombatGroupQueue
           << "|visible_release_radius=" << config.PopulationVisibleReleaseRadius
           << "|background_percent=" << static_cast<std::uint32_t>(config.ActivityBackgroundPercent)
           << "|party_enabled=" << config.PartyEnabled
           << "|party_human_only=" << config.PartyHumanInitiatedOnly
           << "|guild_enrollment=" << config.GuildHumanEnrollment
           << "|economy_enabled=" << config.EconomyEnabled
           << "|economy_adaptive=" << config.EconomyProfileAdaptive
           << "|economy_interval=" << config.EconomyUpdateIntervalMinutes
           << "|economy_max_listings=" << config.EconomyMaximumSyntheticListings
           << "|legacy_population=" << config.LegacyPopulationEnabled
           << "|legacy_profiles=" << config.LegacyPersistentProfilesEnabled
           << "|legacy_shared_guild=" << config.LegacySharedGuildEnabled
           << "|war_enabled=" << config.WarEnabled
           << "|playerbots_enabled=" << config.PlayerbotsEnabled
           << "|random_bot_autologin=" << config.RandomBotAutologinEnabled
           << "|ahbot_seller=" << config.AhBotSellerEnabled
           << "|ahbot_buyer=" << config.AhBotBuyerEnabled;

    output << "|excluded_accounts=";
    for (std::uint32_t accountId : config.ExcludedAccountIds)
        output << accountId << ',';
    output << "|excluded_guids=";
    for (std::uint64_t guid : config.ExcludedCharacterGuids)
        output << guid << ',';
    return output.str();
}

inline std::string CanonicalPolicyInput(PolicyPlan const& plan)
{
    std::ostringstream output;
    output << "source=" << ToString(plan.Source)
           << "|online_humans=" << plan.EligibleOnlineHumanCount
           << "|requested=" << plan.PopulationRequestedTarget
           << "|bounded=" << plan.PopulationBoundedTarget
           << "|background=" << plan.BackgroundRequestedTarget;

    for (ProfileInput const& input : plan.Inputs)
    {
        output << "|member=" << input.CharacterGuid
               << ',' << static_cast<std::uint32_t>(input.Faction)
               << ',' << static_cast<std::uint32_t>(input.ClassId)
               << ',' << static_cast<std::uint32_t>(input.Level)
               << ',' << input.MapId
               << ',' << input.ZoneId
               << ',' << input.Grouped
               << ',' << input.InInstance
               << ',' << input.AverageItemLevelTimes100
               << ',' << static_cast<std::uint32_t>(input.EquippedItemCount)
               << ',' << static_cast<std::uint32_t>(input.ActiveQuestCount)
               << ',' << input.RewardedQuestCount
               << ',' << input.Profession1.SkillId
               << ',' << input.Profession1.Value
               << ',' << static_cast<std::uint32_t>(input.Profession1.Step)
               << ',' << input.Profession2.SkillId
               << ',' << input.Profession2.Value
               << ',' << static_cast<std::uint32_t>(input.Profession2.Step);
    }
    return output.str();
}

inline SessionCandidate MakeFixtureHuman(std::uint64_t guid, std::uint8_t level = 10)
{
    SessionCandidate candidate;
    candidate.SessionPresent = true;
    candidate.PlayerPresent = true;
    candidate.InWorld = true;
    candidate.AccountId = static_cast<std::uint32_t>(guid + 100);
    candidate.Profile.CharacterGuid = guid;
    candidate.Profile.Level = level;
    candidate.Profile.MapId = 0;
    candidate.Profile.ZoneId = 12;
    return candidate;
}

inline std::pair<std::string, std::string> CanonicalOrderingFixtureMaterials()
{
    DirectorConfig config;
    config.Enabled = true;
    config.PrimaryCharacterGuid = 42;

    std::vector<SessionCandidate> orderA = {
        MakeFixtureHuman(30), MakeFixtureHuman(10), MakeFixtureHuman(20)
    };
    std::vector<SessionCandidate> orderB = {
        MakeFixtureHuman(20), MakeFixtureHuman(30), MakeFixtureHuman(10)
    };
    PolicyPlan planA = BuildPolicyPlan(
        config, FilterEligibleHumans(orderA, config), std::nullopt, 1);
    PolicyPlan planB = BuildPolicyPlan(
        config, FilterEligibleHumans(orderB, config), std::nullopt, 1);
    return {CanonicalPolicyInput(planA), CanonicalPolicyInput(planB)};
}

inline FixtureReport RunPolicyFixtures()
{
    FixtureReport report;
    auto record = [&](std::string name, bool passed)
    {
        ++report.Total;
        if (passed)
            ++report.Passed;
        else
            report.Failures.push_back(std::move(name));
    };

    DirectorConfig config;
    config.Enabled = true;
    config.PrimaryCharacterGuid = 42;

    {
        std::vector<SessionCandidate> candidates;
        candidates.push_back(MakeFixtureHuman(42));

        SessionCandidate randomBot = MakeFixtureHuman(1001);
        randomBot.IsBot = true;
        candidates.push_back(randomBot);

        SessionCandidate accountBot = MakeFixtureHuman(1002);
        accountBot.IsBot = true;
        candidates.push_back(accountBot);

        SessionCandidate gm = MakeFixtureHuman(1003);
        gm.IsGameMaster = true;
        candidates.push_back(gm);

        SessionCandidate excludedAccount = MakeFixtureHuman(1004);
        config.ExcludedAccountIds.insert(excludedAccount.AccountId);
        candidates.push_back(excludedAccount);

        SessionCandidate excludedGuid = MakeFixtureHuman(1005);
        config.ExcludedCharacterGuids.insert(excludedGuid.Profile.CharacterGuid);
        candidates.push_back(excludedGuid);

        SessionCandidate missingPlayer = MakeFixtureHuman(1006);
        missingPlayer.PlayerPresent = false;
        candidates.push_back(missingPlayer);

        SessionCandidate outOfWorld = MakeFixtureHuman(1007);
        outOfWorld.InWorld = false;
        candidates.push_back(outOfWorld);

        auto accepted = FilterEligibleHumans(candidates, config);
        record("mixed_session_filter", accepted.size() == 1 && accepted[0].CharacterGuid == 42);
    }

    {
        DirectorConfig orderedConfig = config;
        orderedConfig.ExcludedAccountIds.clear();
        orderedConfig.ExcludedCharacterGuids.clear();
        std::vector<SessionCandidate> candidates = {
            MakeFixtureHuman(30), MakeFixtureHuman(10), MakeFixtureHuman(20)
        };
        auto accepted = FilterEligibleHumans(candidates, orderedConfig);
        record("stable_guid_order",
               accepted.size() == 3 &&
               accepted[0].CharacterGuid == 10 &&
               accepted[1].CharacterGuid == 20 &&
               accepted[2].CharacterGuid == 30);
    }

    {
        auto materials = CanonicalOrderingFixtureMaterials();
        record("canonical_input_hash_stability",
               !materials.first.empty() && materials.first == materials.second);
    }

    constexpr std::uint64_t now = 2'000'000;
    {
        PolicyPlan plan = BuildPolicyPlan(config, {}, std::nullopt, now);
        record("baseline_without_reference",
               plan.Source == Provenance::Baseline &&
               plan.EligibleOnlineHumanCount == 0 &&
               plan.Inputs.empty() &&
               plan.PopulationBoundedTarget == 50);
    }

    ReferenceProfile reference;
    reference.Profile = MakeFixtureHuman(42, 25).Profile;
    reference.ObservedAtUnix = now - 168 * SECONDS_PER_HOUR;
    {
        PolicyPlan plan = BuildPolicyPlan(config, {}, reference, now);
        record("last_known_exactly_168_hours",
               plan.Source == Provenance::LastKnown &&
               plan.EligibleOnlineHumanCount == 0 &&
               plan.Inputs.size() == 1 &&
               plan.Cohorts.size() == 1 &&
               plan.Cohorts[0].RequestedTarget == 50 &&
               plan.Cohorts[0].BoundedTarget == 50);
    }

    {
        ReferenceProfile expired = reference;
        expired.ObservedAtUnix -= 1;
        PolicyPlan plan = BuildPolicyPlan(config, {}, expired, now);
        record("last_known_168_hours_plus_one_second",
               plan.Source == Provenance::Baseline && plan.Inputs.empty());
    }

    {
        std::vector<ProfileInput> online = {MakeFixtureHuman(99, 40).Profile};
        PolicyPlan plan = BuildPolicyPlan(config, online, reference, now);
        record("online_overrides_reference",
               plan.Source == Provenance::Online &&
               plan.Inputs.size() == 1 &&
               plan.Inputs[0].CharacterGuid == 99);
    }

    {
        std::vector<ProfileInput> online;
        for (std::uint64_t guid = 1; guid <= 6; ++guid)
            online.push_back(MakeFixtureHuman(guid, 20).Profile);
        PolicyPlan plan = BuildPolicyPlan(config, online, std::nullopt, now);
        std::uint32_t cohortTotal = 0;
        for (PopulationCohort const& cohort : plan.Cohorts)
            cohortTotal += cohort.BoundedTarget;
        record("population_hard_cap",
               plan.PopulationRequestedTarget == 60 &&
               plan.PopulationBoundedTarget == 50 &&
               cohortTotal == 50);
    }

    {
        std::vector<ProfileInput> online = {
            MakeFixtureHuman(1, 1).Profile,
            MakeFixtureHuman(2, 80).Profile
        };
        PolicyPlan plan = BuildPolicyPlan(config, online, std::nullopt, now);
        bool sawLow = false;
        bool sawHigh = false;
        for (PopulationCohort const& cohort : plan.Cohorts)
        {
            sawLow = sawLow || (cohort.LevelMinimum == 1 && cohort.LevelMaximum == 4);
            sawHigh = sawHigh || (cohort.LevelMinimum == 77 && cohort.LevelMaximum == 80);
        }
        record("level_band_clamping", sawLow && sawHigh);
    }

    {
        DirectorConfig unsafe = config;
        unsafe.ObserveOnly = false;
        unsafe.PopulationMaximum = 51;
        unsafe.AhBotSellerEnabled = true;
        record("unsafe_config_fails_closed", ValidateConfig(unsafe).size() >= 3);
    }

    return report;
}
}

std::optional<LivingAzeroth::Director::DirectorView> GetLivingAzerothDirectorView();
void AddLivingAzerothDirectorScripts();

#endif




