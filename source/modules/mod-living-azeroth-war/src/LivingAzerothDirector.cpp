#include "LivingAzerothDirector.h"

#include "AsyncCallbackProcessor.h"
#include "Config.h"
#include "CryptoHash.h"
#include "CryptoRandom.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>

namespace LivingAzeroth::Director
{
namespace
{
constexpr char POLICY_VERSION[] = "ai3-observer-v1";
constexpr std::uint32_t EXPECTED_SCHEMA_TABLES = 4;
constexpr std::uint32_t EXPECTED_FIXTURES = 10;

enum class ControllerState
{
    Disabled,
    Configured,
    LoadingSchema,
    LoadingReference,
    Ready,
    Failed,
    ShuttingDown
};

struct PendingSnapshot
{
    DirectorView View;
    std::uint64_t CapturedAtUnix = 0;
    std::uint64_t CapturedAtUnixMs = 0;
    std::string InputSha256;
    std::optional<ReferenceProfile> ReferenceUpdate;
};

struct DecisionRow
{
    std::string Subsystem;
    std::string Action;
    std::uint32_t RequestedTarget = 0;
    std::uint32_t BoundedTarget = 0;
    std::string Reason;
    std::string RateLimitJson;
    std::string BudgetJson;
    std::string DetailJson;
};

std::uint64_t NowUnixMs()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::string HexDigest(Acore::Crypto::SHA256::Digest const& digest)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::uint8_t value : digest)
        output << std::setw(2) << static_cast<std::uint32_t>(value);
    return output.str();
}

std::string Sha256(std::string_view value)
{
    return HexDigest(Acore::Crypto::SHA256::GetDigestOf(value));
}

template <typename Container>
std::string HexBytes(Container const& bytes)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::uint8_t value : bytes)
        output << std::setw(2) << static_cast<std::uint32_t>(value);
    return output.str();
}

bool IsLowerHex64(std::string const& value)
{
    if (value.size() != 64)
        return false;

    return std::all_of(value.begin(), value.end(), [](char character)
    {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

bool IsWotlkClass(std::uint8_t classId)
{
    switch (classId)
    {
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 11:
            return true;
        default:
            return false;
    }
}

bool IsValidPersistedProfile(ProfileInput const& profile)
{
    return profile.CharacterGuid != 0 &&
           profile.Faction <= 1 &&
           IsWotlkClass(profile.ClassId) &&
           profile.Level >= 1 &&
           profile.Level <= 80 &&
           profile.EquippedItemCount <= EQUIPMENT_SLOT_END &&
           profile.ActiveQuestCount <= MAX_QUEST_LOG_SIZE &&
           profile.Profession1.Step <= 6 &&
           profile.Profession2.Step <= 6;
}

std::string Trim(std::string value)
{
    auto isSpace = [](unsigned char character) { return std::isspace(character) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
        [&](char character) { return !isSpace(static_cast<unsigned char>(character)); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
        [&](char character) { return !isSpace(static_cast<unsigned char>(character)); }).base(),
        value.end());
    return value;
}

template <typename T>
bool ParseUnsignedList(std::string const& source, std::set<T>& destination, std::string& error)
{
    destination.clear();
    std::string cleaned = Trim(source);
    if (cleaned.empty())
        return true;
    if (cleaned.front() == ',' || cleaned.back() == ',')
    {
        error = "contains an empty token";
        return false;
    }

    std::istringstream input(cleaned);
    std::string token;
    while (std::getline(input, token, ','))
    {
        token = Trim(token);
        if (token.empty() ||
            !std::all_of(token.begin(), token.end(),
                [](char character) { return character >= '0' && character <= '9'; }))
        {
            error = "contains an empty or non-decimal token";
            return false;
        }

        try
        {
            std::size_t consumed = 0;
            unsigned long long parsed = std::stoull(token, &consumed, 10);
            if (consumed != token.size() ||
                parsed > static_cast<unsigned long long>(std::numeric_limits<T>::max()))
            {
                error = "contains a value outside the supported unsigned range";
                return false;
            }

            destination.insert(static_cast<T>(parsed));
        }
        catch (...)
        {
            error = "contains a value outside the supported unsigned range";
            return false;
        }
    }

    return true;
}

std::string CohortsJson(PolicyPlan const& plan)
{
    std::ostringstream output;
    output << "{\"cohorts\":[";
    for (std::size_t index = 0; index < plan.Cohorts.size(); ++index)
    {
        PopulationCohort const& cohort = plan.Cohorts[index];
        if (index != 0)
            output << ',';
        output << "{\"faction\":" << static_cast<std::uint32_t>(cohort.Faction)
               << ",\"level_min\":" << static_cast<std::uint32_t>(cohort.LevelMinimum)
               << ",\"level_max\":" << static_cast<std::uint32_t>(cohort.LevelMaximum)
               << ",\"map\":" << cohort.MapId
               << ",\"zone\":" << cohort.ZoneId
               << ",\"members\":" << cohort.MemberCount
               << ",\"requested\":" << cohort.RequestedTarget
               << ",\"bounded\":" << cohort.BoundedTarget
               << '}';
    }
    output << "]}";
    return output.str();
}

std::vector<DecisionRow> BuildDecisionRows(PolicyPlan const& plan, DirectorConfig const& config)
{
    std::vector<DecisionRow> rows;
    rows.push_back({
        "population",
        "propose_population_target",
        plan.PopulationRequestedTarget,
        plan.PopulationBoundedTarget,
        "observe_only_no_population_adapter",
        "{\"login_batch\":" + std::to_string(config.PopulationLoginBatch) +
            ",\"logout_batch\":" + std::to_string(config.PopulationLogoutBatch) +
            ",\"cooldown_seconds\":" +
            std::to_string(config.PopulationChangeCooldownSeconds) + "}",
        "{\"hard_cap\":" + std::to_string(AI3_POPULATION_HARD_CAP) +
            ",\"configured_maximum\":" + std::to_string(config.PopulationMaximum) + "}",
        CohortsJson(plan)
    });
    rows.push_back({
        "activity",
        "propose_background_activity",
        plan.BackgroundRequestedTarget,
        plan.BackgroundRequestedTarget,
        "observe_only_no_activity_adapter",
        "{\"interval_seconds\":" + std::to_string(config.DecisionIntervalSeconds) + "}",
        "{\"background_percent\":" +
            std::to_string(static_cast<std::uint32_t>(config.ActivityBackgroundPercent)) + "}",
        "{\"preserve_combat_group_queue\":" +
            std::to_string(config.PopulationPreserveCombatGroupQueue ? 1 : 0) + "}"
    });
    rows.push_back({
        "economy",
        "economy_disabled",
        0,
        0,
        "ai3_economy_adapter_disabled",
        "{\"minimum_interval_minutes\":30}",
        "{\"maximum_synthetic_listings\":" +
            std::to_string(config.EconomyMaximumSyntheticListings) + "}",
        "{\"seller_enabled\":0,\"buyer_enabled\":0,\"profile_adaptive\":0}"
    });
    rows.push_back({
        "guild",
        "human_enrollment_opt_in",
        0,
        0,
        "ai3_guild_adapter_disabled",
        "{\"automatic_human_enrollment\":0}",
        "{\"forced_membership_changes\":0}",
        "{\"human_enrollment\":\"opt_in\"}"
    });
    rows.push_back({
        "companion",
        "companion_disabled",
        0,
        0,
        "ai3_companion_adapter_disabled",
        "{\"human_initiated_only\":1}",
        "{\"ambient_group_mutations\":0}",
        "{\"party_enabled\":0}"
    });
    return rows;
}

std::string MemberValues(ProfileInput const& profile)
{
    std::ostringstream output;
    output << profile.CharacterGuid
           << ',' << static_cast<std::uint32_t>(profile.Faction)
           << ',' << static_cast<std::uint32_t>(profile.ClassId)
           << ',' << static_cast<std::uint32_t>(profile.Level)
           << ',' << profile.MapId
           << ',' << profile.ZoneId
           << ',' << (profile.Grouped ? 1 : 0)
           << ',' << (profile.InInstance ? 1 : 0)
           << ',' << profile.AverageItemLevelTimes100
           << ',' << static_cast<std::uint32_t>(profile.EquippedItemCount)
           << ',' << static_cast<std::uint32_t>(profile.ActiveQuestCount)
           << ',' << profile.RewardedQuestCount
           << ',' << profile.Profession1.SkillId
           << ',' << profile.Profession1.Value
           << ',' << static_cast<std::uint32_t>(profile.Profession1.Step)
           << ',' << profile.Profession2.SkillId
           << ',' << profile.Profession2.Value
           << ',' << static_cast<std::uint32_t>(profile.Profession2.Step);
    return output.str();
}

ProfileInput CaptureProfile(Player* player)
{
    ProfileInput profile;
    profile.CharacterGuid = player->GetGUID().GetCounter();
    profile.Faction = static_cast<std::uint8_t>(player->GetTeamId());
    profile.ClassId = player->getClass();
    profile.Level = player->GetLevel();
    profile.MapId = player->GetMapId();
    profile.ZoneId = player->GetZoneId();
    profile.Grouped = player->GetGroup() != nullptr;
    profile.InInstance = player->GetInstanceId() != 0;

    float itemLevel = player->GetAverageItemLevel();
    if (std::isfinite(itemLevel) && itemLevel > 0.0f)
    {
        double scaled = std::round(static_cast<double>(itemLevel) * 100.0);
        profile.AverageItemLevelTimes100 = static_cast<std::uint32_t>(
            std::min<double>(scaled, std::numeric_limits<std::uint32_t>::max()));
    }

    for (std::uint8_t slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++profile.EquippedItemCount;
    }

    for (std::uint16_t slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        if (player->GetQuestSlotQuestId(slot) != 0)
            ++profile.ActiveQuestCount;
    }
    profile.RewardedQuestCount = static_cast<std::uint32_t>(
        std::min<std::size_t>(player->GetRewardedQuestCount(),
                              std::numeric_limits<std::uint32_t>::max()));

    std::vector<ProfessionSummary> professions;
    for (auto const& [skillId, status] : player->GetSkillStatusMap())
    {
        if (status.uState == SKILL_DELETED)
            continue;

        SkillLineEntry const* entry = sSkillLineStore.LookupEntry(skillId);
        if (!entry || entry->categoryId != SKILL_CATEGORY_PROFESSION)
            continue;

        ProfessionSummary profession;
        profession.SkillId = static_cast<std::uint16_t>(skillId);
        profession.Value = player->GetPureSkillValue(skillId);
        profession.Step = static_cast<std::uint8_t>(player->GetSkillStep(
            static_cast<std::uint16_t>(skillId)));
        professions.push_back(profession);
    }

    std::sort(professions.begin(), professions.end(),
        [](ProfessionSummary const& left, ProfessionSummary const& right)
    {
        return left.SkillId < right.SkillId;
    });
    if (!professions.empty())
        profile.Profession1 = professions[0];
    if (professions.size() > 1)
        profile.Profession2 = professions[1];
    return profile;
}

class LivingAzerothDirectorController
{
public:
    static LivingAzerothDirectorController& Instance()
    {
        static LivingAzerothDirectorController instance;
        return instance;
    }

    void LoadConfig(bool reload)
    {
        if (reload && _configLoaded)
        {
            if (_state != ControllerState::Disabled)
                Fail("configuration_reload_requires_restart");
            else
            {
                LOG_WARN("living_azeroth.director",
                    "Living Azeroth AI3 director ignored disabled-state configuration reload; "
                    "restart is required.");
            }
            return;
        }

        DirectorConfig candidate;
        candidate.Enabled = sConfigMgr->GetOption<bool>("Director.Enable", false);
        candidate.ObserveOnly = sConfigMgr->GetOption<bool>("Director.ObserveOnly", true);
        candidate.DecisionIntervalSeconds =
            sConfigMgr->GetOption<std::uint32_t>("Director.DecisionIntervalSeconds", 30);
        candidate.PrimaryCharacterGuid =
            sConfigMgr->GetOption<std::uint64_t>("Director.Profile.PrimaryCharacterGuid", 0);
        candidate.AggregateAllHumans =
            sConfigMgr->GetOption<bool>("Director.Profile.AggregateAllHumans", true);
        candidate.ExcludeGameMasters =
            sConfigMgr->GetOption<bool>("Director.Profile.ExcludeGameMasters", true);

        candidate.OfflineMode =
            sConfigMgr->GetOption<std::string>("Director.Offline.Mode", "LastKnownThenBaseline");
        candidate.LastKnownMaxAgeHours =
            sConfigMgr->GetOption<std::uint32_t>("Director.Offline.LastKnownMaxAgeHours", 168);
        candidate.BaselineLevel = static_cast<std::uint8_t>(std::min<std::uint32_t>(
            sConfigMgr->GetOption<std::uint32_t>("Director.Offline.BaselineLevel", 10), 255));
        candidate.BaselineFactionMode =
            sConfigMgr->GetOption<std::string>("Director.Offline.BaselineFactionMode", "Balanced");
        candidate.BaselineMap =
            sConfigMgr->GetOption<std::uint32_t>("Director.Offline.BaselineMap", 0);
        candidate.BaselineZone =
            sConfigMgr->GetOption<std::uint32_t>("Director.Offline.BaselineZone", 12);

        candidate.PopulationOfflineTarget =
            sConfigMgr->GetOption<std::uint32_t>("Director.Population.OfflineTarget", 50);
        candidate.PopulationPerHuman =
            sConfigMgr->GetOption<std::uint32_t>("Director.Population.PerHuman", 10);
        candidate.PopulationMaximum =
            sConfigMgr->GetOption<std::uint32_t>("Director.Population.Maximum", 50);
        candidate.PopulationLoginBatch =
            sConfigMgr->GetOption<std::uint32_t>("Director.Population.LoginBatch", 2);
        candidate.PopulationLogoutBatch =
            sConfigMgr->GetOption<std::uint32_t>("Director.Population.LogoutBatch", 1);
        candidate.PopulationChangeCooldownSeconds =
            sConfigMgr->GetOption<std::uint32_t>(
                "Director.Population.ChangeCooldownSeconds", 60);
        candidate.PopulationLevelBelow = static_cast<std::uint8_t>(std::min<std::uint32_t>(
            sConfigMgr->GetOption<std::uint32_t>("Director.Population.LevelBelow", 3), 255));
        candidate.PopulationLevelAbove = static_cast<std::uint8_t>(std::min<std::uint32_t>(
            sConfigMgr->GetOption<std::uint32_t>("Director.Population.LevelAbove", 3), 255));
        candidate.PopulationPreserveCombatGroupQueue =
            sConfigMgr->GetOption<bool>(
                "Director.Population.PreserveCombatGroupQueue", true);
        candidate.PopulationVisibleReleaseRadius =
            sConfigMgr->GetOption<std::uint32_t>(
                "Director.Population.VisibleReleaseRadius", 150);

        candidate.ActivityBackgroundPercent =
            static_cast<std::uint8_t>(std::min<std::uint32_t>(
                sConfigMgr->GetOption<std::uint32_t>(
                    "Director.Activity.BackgroundPercent", 20), 255));
        candidate.PartyEnabled =
            sConfigMgr->GetOption<bool>("Director.Party.Enable", false);
        candidate.PartyHumanInitiatedOnly =
            sConfigMgr->GetOption<bool>("Director.Party.HumanInitiatedOnly", true);
        candidate.GuildHumanEnrollment =
            sConfigMgr->GetOption<std::string>("Director.Guild.HumanEnrollment", "OptIn");

        candidate.EconomyEnabled =
            sConfigMgr->GetOption<bool>("Director.Economy.Enable", false);
        candidate.EconomyProfileAdaptive =
            sConfigMgr->GetOption<bool>("Director.Economy.ProfileAdaptive", false);
        candidate.EconomyUpdateIntervalMinutes =
            sConfigMgr->GetOption<std::uint32_t>(
                "Director.Economy.UpdateIntervalMinutes", 30);
        candidate.EconomyMaximumSyntheticListings =
            sConfigMgr->GetOption<std::uint32_t>(
                "Director.Economy.MaximumSyntheticListings", 75);

        // These are observations only. Defaults are deliberately unsafe for
        // external module actuators so an omitted explicit disable fails closed.
        candidate.LegacyPopulationEnabled =
            sConfigMgr->GetOption<bool>("Population.Enable", true);
        candidate.LegacyPersistentProfilesEnabled =
            sConfigMgr->GetOption<bool>("Population.PersistentProfiles", true);
        candidate.LegacySharedGuildEnabled =
            sConfigMgr->GetOption<bool>("Population.SharedGuild.Enable", true);
        candidate.WarEnabled = sConfigMgr->GetOption<bool>("War.Enable", true);
        candidate.PlayerbotsEnabled =
            sConfigMgr->GetOption<bool>("AiPlayerbot.Enabled", true);
        candidate.RandomBotAutologinEnabled =
            sConfigMgr->GetOption<bool>("AiPlayerbot.RandomBotAutologin", true);
        candidate.AhBotSellerEnabled =
            sConfigMgr->GetOption<bool>("AuctionHouseBot.EnableSeller", true);
        candidate.AhBotBuyerEnabled =
            sConfigMgr->GetOption<bool>("AuctionHouseBot.EnableBuyer", true);

        std::vector<std::string> errors;
        std::string parseError;
        if (!ParseUnsignedList(
                sConfigMgr->GetOption<std::string>(
                    "Director.Profile.ExcludedAccountIds", ""),
                candidate.ExcludedAccountIds,
                parseError))
        {
            errors.push_back("Director.Profile.ExcludedAccountIds " + parseError + '.');
        }
        if (!ParseUnsignedList(
                sConfigMgr->GetOption<std::string>(
                    "Director.Profile.ExcludedCharacterGuids", ""),
                candidate.ExcludedCharacterGuids,
                parseError))
        {
            errors.push_back("Director.Profile.ExcludedCharacterGuids " + parseError + '.');
        }

        std::vector<std::string> validationErrors = ValidateConfig(candidate);
        errors.insert(errors.end(), validationErrors.begin(), validationErrors.end());

        _config = std::move(candidate);
        _configSha256 = Sha256(CanonicalConfig(_config));
        _configLoaded = true;
        if (!_config.Enabled)
        {
            _state = ControllerState::Disabled;
            LOG_INFO("living_azeroth.director",
                "Living Azeroth AI3 director disabled (observe-only proposal loaded).");
            return;
        }

        if (!errors.empty())
        {
            for (std::string const& error : errors)
                LOG_ERROR("living_azeroth.director",
                    "Living Azeroth AI3 director configuration rejected: {}", error);
            Fail("configuration_validation_failed");
            return;
        }

        _state = ControllerState::Configured;
        LOG_INFO("living_azeroth.director",
            "Living Azeroth AI3 director configured with hash {} and all actuators disabled.",
            _configSha256);
    }

    void OnStartup()
    {
        if (_state == ControllerState::Disabled)
            return;
        if (_state != ControllerState::Configured)
        {
            if (_state != ControllerState::Failed)
                Fail("startup_without_valid_configuration");
            return;
        }

        FixtureReport report = RunPolicyFixtures();
        auto canonicalMaterials = CanonicalOrderingFixtureMaterials();
        bool canonicalHashStable =
            !canonicalMaterials.first.empty() &&
            Sha256(canonicalMaterials.first) == Sha256(canonicalMaterials.second);
        if (!canonicalHashStable)
        {
            Fail("canonical_hash_fixture_failure");
            return;
        }

        if (report.Total != EXPECTED_FIXTURES || report.Passed != report.Total)
        {
            for (std::string const& failure : report.Failures)
                LOG_ERROR("living_azeroth.director",
                    "Living Azeroth AI3 director fixture failed: {}", failure);
            Fail("policy_fixture_failure");
            return;
        }

        LOG_INFO("living_azeroth.director",
            "Living Azeroth AI3 director fixtures passed: {}/{}.",
            report.Passed, report.Total);

        _bootNonce = HexBytes(Acore::Crypto::GetRandomBytes<16>());
        _state = ControllerState::LoadingSchema;
        ++_outstandingQueries;
        _queryCallbacks.AddCallback(
            CharacterDatabase.AsyncQuery(
                "SELECT "
                "COUNT(*) AS table_count, "
                "COALESCE(SUM(CASE WHEN ENGINE='InnoDB' THEN 1 ELSE 0 END),0) "
                "AS innodb_count "
                "FROM information_schema.tables "
                "WHERE table_schema=DATABASE() "
                "AND table_name IN "
                "('la_reference_profile','la_profile_snapshot',"
                "'la_profile_snapshot_member','la_director_decision')")
            .WithCallback([this](QueryResult result)
        {
            --_outstandingQueries;
            if (_state == ControllerState::ShuttingDown)
                return;
            if (!result)
            {
                Fail("schema_validation_query_failed");
                return;
            }

            Field* fields = result->Fetch();
            std::uint32_t tableCount = fields[0].Get<std::uint32_t>();
            std::uint32_t innodbCount = fields[1].Get<std::uint32_t>();
            if (tableCount != EXPECTED_SCHEMA_TABLES ||
                innodbCount != EXPECTED_SCHEMA_TABLES)
            {
                Fail("schema_missing_or_non_innodb");
                return;
            }

            LOG_INFO("living_azeroth.director",
                "Living Azeroth AI3 director schema ready: {}/{} InnoDB tables.",
                innodbCount, EXPECTED_SCHEMA_TABLES);
            ScheduleReferenceLoad();
        }));
    }

    void OnUpdate(std::uint32_t diff)
    {
        _queryCallbacks.ProcessReadyCallbacks();
        _transactionCallbacks.ProcessReadyCallbacks();

        if (_state != ControllerState::Ready || _writeInFlight)
            return;

        _elapsedMs += diff;
        std::uint64_t intervalMs =
            static_cast<std::uint64_t>(_config.DecisionIntervalSeconds) * 1000;
        if (_elapsedMs < intervalMs)
            return;

        _elapsedMs %= intervalMs;
        CaptureAndPersist();
    }

    void OnShutdown()
    {
        if (_state == ControllerState::Disabled)
            return;

        _state = ControllerState::ShuttingDown;
        _queryCallbacks.ProcessReadyCallbacks();
        _transactionCallbacks.ProcessReadyCallbacks();
        LOG_INFO("living_azeroth.director",
            "Living Azeroth AI3 director shutdown: outstanding_queries={}, write_in_flight={}.",
            _outstandingQueries, _writeInFlight ? 1 : 0);
    }

    std::optional<DirectorView> View() const
    {
        return _view;
    }

private:
    void ScheduleReferenceLoad()
    {
        if (_state == ControllerState::ShuttingDown)
            return;

        if (_config.PrimaryCharacterGuid == 0)
        {
            _reference.reset();
            MarkReady();
            return;
        }

        _state = ControllerState::LoadingReference;
        std::ostringstream query;
        query << "SELECT "
              << "(r.character_guid IS NOT NULL) AS row_present,"
              << "r.character_guid,r.faction,r.class_id,r.level,r.map_id,r.zone_id,"
              << "r.grouped,r.in_instance,r.average_item_level_x100,"
              << "r.equipped_item_count,r.active_quest_count,r.rewarded_quest_count,"
              << "r.profession1_skill_id,r.profession1_value,r.profession1_step,"
              << "r.profession2_skill_id,r.profession2_value,r.profession2_step,"
              << "r.observed_at_unix,r.source_snapshot_id,r.config_sha256 "
              << "FROM (SELECT 1 AS singleton) AS sentinel "
              << "LEFT JOIN la_reference_profile AS r ON r.character_guid="
              << _config.PrimaryCharacterGuid;

        ++_outstandingQueries;
        _queryCallbacks.AddCallback(
            CharacterDatabase.AsyncQuery(query.str()).WithCallback(
                [this](QueryResult result)
        {
            --_outstandingQueries;
            if (_state == ControllerState::ShuttingDown)
                return;

            if (!result)
            {
                Fail("reference_profile_query_failed");
                return;
            }

            Field* fields = result->Fetch();
            if (!fields[0].Get<bool>())
            {
                _reference.reset();
                MarkReady();
                return;
            }

            ReferenceProfile reference;
            reference.Profile.CharacterGuid = fields[1].Get<std::uint64_t>();
            reference.Profile.Faction = fields[2].Get<std::uint8_t>();
            reference.Profile.ClassId = fields[3].Get<std::uint8_t>();
            reference.Profile.Level = fields[4].Get<std::uint8_t>();
            reference.Profile.MapId = fields[5].Get<std::uint32_t>();
            reference.Profile.ZoneId = fields[6].Get<std::uint32_t>();
            reference.Profile.Grouped = fields[7].Get<bool>();
            reference.Profile.InInstance = fields[8].Get<bool>();
            reference.Profile.AverageItemLevelTimes100 = fields[9].Get<std::uint32_t>();
            reference.Profile.EquippedItemCount = fields[10].Get<std::uint8_t>();
            reference.Profile.ActiveQuestCount = fields[11].Get<std::uint8_t>();
            reference.Profile.RewardedQuestCount = fields[12].Get<std::uint32_t>();
            reference.Profile.Profession1.SkillId = fields[13].Get<std::uint16_t>();
            reference.Profile.Profession1.Value = fields[14].Get<std::uint16_t>();
            reference.Profile.Profession1.Step = fields[15].Get<std::uint8_t>();
            reference.Profile.Profession2.SkillId = fields[16].Get<std::uint16_t>();
            reference.Profile.Profession2.Value = fields[17].Get<std::uint16_t>();
            reference.Profile.Profession2.Step = fields[18].Get<std::uint8_t>();
            reference.ObservedAtUnix = fields[19].Get<std::uint64_t>();
            reference.SourceSnapshotId = fields[20].Get<std::string>();
            reference.ConfigSha256 = fields[21].Get<std::string>();

            if (reference.Profile.CharacterGuid != _config.PrimaryCharacterGuid ||
                !IsValidPersistedProfile(reference.Profile) ||
                !IsLowerHex64(reference.SourceSnapshotId) ||
                !IsLowerHex64(reference.ConfigSha256))
            {
                Fail("invalid_reference_profile_row");
                return;
            }

            _reference = std::move(reference);
            MarkReady();
        }));
    }

    void MarkReady()
    {
        _state = ControllerState::Ready;
        _elapsedMs =
            static_cast<std::uint64_t>(_config.DecisionIntervalSeconds) * 1000;
        LOG_INFO("living_azeroth.director",
            "Living Azeroth AI3 director ready in observe-only mode; primary_guid={}.",
            _config.PrimaryCharacterGuid);
    }

    std::vector<SessionCandidate> CaptureSessionCandidates() const
    {
        std::vector<SessionCandidate> candidates;
        WorldSessionMgr::SessionMap const& sessions = sWorldSessionMgr->GetAllSessions();
        candidates.reserve(sessions.size());

        for (auto const& [accountKey, session] : sessions)
        {
            (void)accountKey;
            SessionCandidate candidate;
            candidate.SessionPresent = session != nullptr;
            if (!session)
            {
                candidates.push_back(candidate);
                continue;
            }

            candidate.AccountId = session->GetAccountId();
            candidate.IsBot = session->IsBot();
            Player* player = session->GetPlayer();
            candidate.PlayerPresent = player != nullptr;
            if (!player)
            {
                candidates.push_back(candidate);
                continue;
            }

            candidate.InWorld = player->IsInWorld();
            candidate.IsGameMaster =
                session->GetSecurity() != SEC_PLAYER || player->IsGameMaster();
            if (candidate.InWorld)
                candidate.Profile = CaptureProfile(player);
            candidates.push_back(std::move(candidate));
        }
        return candidates;
    }

    std::string MakeSnapshotId(
        std::uint64_t capturedAtUnixMs,
        std::string const& inputSha256)
    {
        std::ostringstream material;
        material << "living-azeroth-ai3|"
                 << _bootNonce << '|'
                 << capturedAtUnixMs << '|'
                 << ++_sequence << '|'
                 << _configSha256 << '|'
                 << inputSha256;
        return Sha256(material.str());
    }

    void CaptureAndPersist()
    {
        std::vector<SessionCandidate> candidates = CaptureSessionCandidates();
        std::vector<ProfileInput> eligible = FilterEligibleHumans(candidates, _config);
        std::uint64_t capturedAtUnixMs = NowUnixMs();
        std::uint64_t capturedAtUnix = capturedAtUnixMs / 1000;
        PolicyPlan plan = BuildPolicyPlan(_config, eligible, _reference, capturedAtUnix);
        for (ProfileInput const& input : plan.Inputs)
        {
            if (!IsValidPersistedProfile(input))
            {
                Fail("invalid_policy_profile_input");
                return;
            }
        }
        std::string inputSha256 = Sha256(CanonicalPolicyInput(plan));

        auto pending = std::make_shared<PendingSnapshot>();
        pending->CapturedAtUnix = capturedAtUnix;
        pending->CapturedAtUnixMs = capturedAtUnixMs;
        pending->InputSha256 = inputSha256;
        pending->View.ConfigSha256 = _configSha256;
        pending->View.Plan = plan;
        pending->View.SnapshotId = MakeSnapshotId(capturedAtUnixMs, inputSha256);

        if (!IsLowerHex64(pending->View.SnapshotId) ||
            !IsLowerHex64(pending->View.ConfigSha256) ||
            !IsLowerHex64(pending->InputSha256))
        {
            Fail("internal_hash_validation_failed");
            return;
        }

        if (plan.Source == Provenance::Online &&
            _config.PrimaryCharacterGuid != 0)
        {
            auto primary = std::find_if(plan.Inputs.begin(), plan.Inputs.end(),
                [this](ProfileInput const& profile)
            {
                return profile.CharacterGuid == _config.PrimaryCharacterGuid;
            });
            if (primary != plan.Inputs.end())
            {
                ReferenceProfile reference;
                reference.Profile = *primary;
                reference.ObservedAtUnix = capturedAtUnix;
                reference.SourceSnapshotId = pending->View.SnapshotId;
                reference.ConfigSha256 = _configSha256;
                pending->ReferenceUpdate = std::move(reference);
            }
        }

        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        AppendSnapshotHeader(transaction, *pending);
        AppendSnapshotMembers(transaction, *pending);
        AppendDecisions(transaction, *pending);
        if (pending->ReferenceUpdate)
            AppendReferenceUpsert(transaction, *pending->ReferenceUpdate);

        _writeInFlight = true;
        TransactionCallback& callback = _transactionCallbacks.AddCallback(
            CharacterDatabase.AsyncCommitTransaction(transaction));
        callback.AfterComplete([this, pending](bool success)
        {
            _writeInFlight = false;
            if (!success)
            {
                if (_state != ControllerState::ShuttingDown)
                    Fail("snapshot_transaction_failed");
                return;
            }

            if (pending->ReferenceUpdate)
                _reference = pending->ReferenceUpdate;
            _view = pending->View;
            LOG_INFO("living_azeroth.director",
                "Living Azeroth AI3 director committed snapshot {} "
                "provenance={} humans={} requested={} bounded={} applied=0.",
                pending->View.SnapshotId,
                ToString(pending->View.Plan.Source),
                pending->View.Plan.EligibleOnlineHumanCount,
                pending->View.Plan.PopulationRequestedTarget,
                pending->View.Plan.PopulationBoundedTarget);
        });
    }

    void AppendSnapshotHeader(
        CharacterDatabaseTransaction const& transaction,
        PendingSnapshot const& pending) const
    {
        std::ostringstream query;
        query << "INSERT INTO la_profile_snapshot "
              << "(snapshot_id,captured_at_unix_ms,provenance,policy_version,"
              << "config_sha256,input_sha256,online_human_count,policy_input_count,"
              << "primary_character_guid,population_requested_target,"
              << "population_bounded_target,observe_only) VALUES ('"
              << pending.View.SnapshotId << "',"
              << pending.CapturedAtUnixMs << ",'"
              << ToString(pending.View.Plan.Source) << "','"
              << POLICY_VERSION << "','"
              << pending.View.ConfigSha256 << "','"
              << pending.InputSha256 << "',"
              << pending.View.Plan.EligibleOnlineHumanCount << ','
              << pending.View.Plan.Inputs.size() << ','
              << _config.PrimaryCharacterGuid << ','
              << pending.View.Plan.PopulationRequestedTarget << ','
              << pending.View.Plan.PopulationBoundedTarget << ",1)";
        transaction->Append(query.str());
    }

    void AppendSnapshotMembers(
        CharacterDatabaseTransaction const& transaction,
        PendingSnapshot const& pending) const
    {
        char const* inputKind =
            pending.View.Plan.Source == Provenance::Online ?
                "online_human" : "reference_profile";
        for (std::size_t index = 0; index < pending.View.Plan.Inputs.size(); ++index)
        {
            std::ostringstream query;
            query << "INSERT INTO la_profile_snapshot_member "
                  << "(snapshot_id,ordinal,input_kind,character_guid,faction,class_id,"
                  << "level,map_id,zone_id,grouped,in_instance,average_item_level_x100,"
                  << "equipped_item_count,active_quest_count,rewarded_quest_count,"
                  << "profession1_skill_id,profession1_value,profession1_step,"
                  << "profession2_skill_id,profession2_value,profession2_step) VALUES ('"
                  << pending.View.SnapshotId << "',"
                  << index << ",'"
                  << inputKind << "',"
                  << MemberValues(pending.View.Plan.Inputs[index]) << ')';
            transaction->Append(query.str());
        }
    }

    void AppendDecisions(
        CharacterDatabaseTransaction const& transaction,
        PendingSnapshot const& pending) const
    {
        std::vector<DecisionRow> decisions =
            BuildDecisionRows(pending.View.Plan, _config);
        std::string correlationId =
            Sha256(pending.View.SnapshotId + "|correlation");

        for (DecisionRow const& decision : decisions)
        {
            std::string decisionId =
                Sha256(pending.View.SnapshotId + "|decision|" + decision.Subsystem);
            std::ostringstream query;
            query << "INSERT INTO la_director_decision "
                  << "(decision_id,snapshot_id,correlation_id,decided_at_unix_ms,"
                  << "subsystem,policy_version,action,requested_target,bounded_target,"
                  << "applied_target,state,reason,rate_limit_json,budget_json,detail_json) "
                  << "VALUES ('"
                  << decisionId << "','"
                  << pending.View.SnapshotId << "','"
                  << correlationId << "',"
                  << pending.CapturedAtUnixMs << ",'"
                  << decision.Subsystem << "','"
                  << POLICY_VERSION << "','"
                  << decision.Action << "',"
                  << decision.RequestedTarget << ','
                  << decision.BoundedTarget
                  << ",0,'observe_only','"
                  << decision.Reason << "','"
                  << decision.RateLimitJson << "','"
                  << decision.BudgetJson << "','"
                  << decision.DetailJson << "')";
            transaction->Append(query.str());
        }
    }

    void AppendReferenceUpsert(
        CharacterDatabaseTransaction const& transaction,
        ReferenceProfile const& reference) const
    {
        std::ostringstream query;
        query << "INSERT INTO la_reference_profile "
              << "(character_guid,faction,class_id,level,map_id,zone_id,grouped,"
              << "in_instance,average_item_level_x100,equipped_item_count,"
              << "active_quest_count,rewarded_quest_count,profession1_skill_id,"
              << "profession1_value,profession1_step,profession2_skill_id,"
              << "profession2_value,profession2_step,observed_at_unix,"
              << "source_snapshot_id,config_sha256) VALUES ("
              << MemberValues(reference.Profile) << ','
              << reference.ObservedAtUnix << ",'"
              << reference.SourceSnapshotId << "','"
              << reference.ConfigSha256 << "') "
              << "ON DUPLICATE KEY UPDATE "
              << "faction=VALUES(faction),class_id=VALUES(class_id),"
              << "level=VALUES(level),map_id=VALUES(map_id),zone_id=VALUES(zone_id),"
              << "grouped=VALUES(grouped),in_instance=VALUES(in_instance),"
              << "average_item_level_x100=VALUES(average_item_level_x100),"
              << "equipped_item_count=VALUES(equipped_item_count),"
              << "active_quest_count=VALUES(active_quest_count),"
              << "rewarded_quest_count=VALUES(rewarded_quest_count),"
              << "profession1_skill_id=VALUES(profession1_skill_id),"
              << "profession1_value=VALUES(profession1_value),"
              << "profession1_step=VALUES(profession1_step),"
              << "profession2_skill_id=VALUES(profession2_skill_id),"
              << "profession2_value=VALUES(profession2_value),"
              << "profession2_step=VALUES(profession2_step),"
              << "observed_at_unix=VALUES(observed_at_unix),"
              << "source_snapshot_id=VALUES(source_snapshot_id),"
              << "config_sha256=VALUES(config_sha256)";
        transaction->Append(query.str());
    }

    void Fail(std::string const& reason)
    {
        _state = ControllerState::Failed;
        LOG_ERROR("living_azeroth.director",
            "Living Azeroth AI3 director failed closed: {}.", reason);
    }

    DirectorConfig _config;
    ControllerState _state = ControllerState::Disabled;
    bool _configLoaded = false;
    bool _writeInFlight = false;
    std::uint32_t _outstandingQueries = 0;
    std::uint64_t _elapsedMs = 0;
    std::uint64_t _sequence = 0;
    std::string _bootNonce;
    std::string _configSha256;
    std::optional<ReferenceProfile> _reference;
    std::optional<DirectorView> _view;
    QueryCallbackProcessor _queryCallbacks;
    AsyncCallbackProcessor<TransactionCallback> _transactionCallbacks;
};

class LivingAzerothDirectorWorldScript : public WorldScript
{
public:
    LivingAzerothDirectorWorldScript()
        : WorldScript("LivingAzerothDirectorWorldScript",
            {WORLDHOOK_ON_AFTER_CONFIG_LOAD,
             WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
             WORLDHOOK_ON_STARTUP,
             WORLDHOOK_ON_UPDATE,
             WORLDHOOK_ON_SHUTDOWN})
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            LivingAzerothDirectorController::Instance().LoadConfig(true);
    }

    void OnBeforeWorldInitialized() override
    {
        LivingAzerothDirectorController::Instance().LoadConfig(false);
    }

    void OnStartup() override
    {
        LivingAzerothDirectorController::Instance().OnStartup();
    }

    void OnUpdate(std::uint32_t diff) override
    {
        LivingAzerothDirectorController::Instance().OnUpdate(diff);
    }

    void OnShutdown() override
    {
        LivingAzerothDirectorController::Instance().OnShutdown();
    }
};
}
}

std::optional<LivingAzeroth::Director::DirectorView> GetLivingAzerothDirectorView()
{
    return LivingAzeroth::Director::LivingAzerothDirectorController::Instance().View();
}

void AddLivingAzerothDirectorScripts()
{
    new LivingAzeroth::Director::LivingAzerothDirectorWorldScript();
}








