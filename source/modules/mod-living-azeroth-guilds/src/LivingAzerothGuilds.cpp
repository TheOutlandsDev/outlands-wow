/*
 * Living Azeroth: native faction guild ownership and persistent rankings.
 *
 * The module is deliberately fail-closed. Its default configuration is both
 * disabled and observe-only. Native guild calls are made only after the
 * character and guild factions have independently matched.
 */

#include "LivingAzerothGuildPolicy.h"

#include "CharacterCache.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>

namespace
{
using LivingAzeroth::Guilds::CanEnroll;
using LivingAzeroth::Guilds::ComputeRankPoints;
using LivingAzeroth::Guilds::EarnedNativeRank;
using LivingAzeroth::Guilds::FactionFromRace;
using LivingAzeroth::Guilds::FactionName;
using LivingAzeroth::Guilds::NativeFaction;
using LivingAzeroth::Guilds::RankName;
using LivingAzeroth::Guilds::RankPolicy;
using LivingAzeroth::Guilds::SubjectKind;

constexpr std::uint32_t GuildPolicySchemaVersion = 1;
constexpr std::uint8_t NoObservedRank = std::numeric_limits<std::uint8_t>::max();

struct GuildPolicyConfig
{
    bool Enabled = false;
    bool ObserveOnly = true;
    bool ApplyEnrollment = false;
    bool ApplyRankChanges = false;
    bool MoveOwnedAiFromOtherGuilds = false;
    bool AutoEnrollUnguildedHumans = false;
    bool AllowCreateMissingGuilds = false;
    bool RequireTwoSideGuildsOff = true;
    std::string AllianceName = "Living Azeroth Alliance";
    std::string AllianceLeaderName;
    std::uint32_t AllianceLeaderGuid = 0;
    std::uint32_t AllianceLegacyGuildId = 0;
    std::string AllianceLegacyName = "Living Azeroth";
    std::string HordeName = "Living Azeroth Horde";
    std::string HordeLeaderName;
    std::uint32_t HordeLeaderGuid = 0;
    std::string HistoryPath = "/run/living-azeroth/logs/guild-ranking/history.jsonl";
    std::uint32_t AuditIntervalMs = 60 * IN_MILLISECONDS;
    std::uint32_t MaxBatchSize = 25;
    std::uint32_t PolicyVersion = 1;
    std::unordered_set<std::uint32_t> ExcludedCharacterGuids;
};

struct CharacterProjection
{
    std::uint32_t Guid = 0;
    std::uint32_t Account = 0;
    std::uint8_t Race = 0;
    std::uint8_t Level = 0;
    std::uint32_t HonorableKills = 0;
    std::uint32_t RewardedQuests = 0;
    std::uint32_t ObservedGuildId = 0;
    std::uint8_t ObservedRank = NoObservedRank;
    NativeFaction Faction = NativeFaction::Invalid;
    SubjectKind Subject = SubjectKind::Human;
    std::uint32_t ExpectedGuildId = 0;
    std::uint32_t RankPoints = 0;
    std::uint8_t ProjectedRank = 9;
    std::string EnrollmentState = "pending";
    std::string DecisionReason = "pending_target_guild";
};

std::string JsonEscape(std::string const& input)
{
    std::ostringstream escaped;
    for (char character : input)
    {
        switch (character)
        {
            case '\\': escaped << "\\\\"; break;
            case '"': escaped << "\\\""; break;
            case '\n': escaped << "\\n"; break;
            case '\r': escaped << "\\r"; break;
            case '\t': escaped << "\\t"; break;
            default: escaped << character; break;
        }
    }
    return escaped.str();
}

char const* SubjectName(SubjectKind subject)
{
    switch (subject)
    {
        case SubjectKind::Ai: return "ai";
        case SubjectKind::System: return "system";
        case SubjectKind::Excluded: return "excluded";
        case SubjectKind::Quarantined: return "quarantined";
        case SubjectKind::Human: return "human";
    }
    return "quarantined";
}

std::unordered_set<std::uint32_t> ParseGuidSet(std::string const& value)
{
    std::unordered_set<std::uint32_t> result;
    std::stringstream input(value);
    std::string token;
    while (std::getline(input, token, ','))
    {
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char character)
        {
            return std::isspace(character) != 0;
        }), token.end());
        if (token.empty())
            continue;
        try
        {
            std::size_t consumed = 0;
            unsigned long parsed = std::stoul(token, &consumed, 10);
            if (consumed == token.size() && parsed <= std::numeric_limits<std::uint32_t>::max())
                result.insert(static_cast<std::uint32_t>(parsed));
        }
        catch (...)
        {
            LOG_WARN("module", "LivingAzerothGuild: ignored invalid excluded GUID token '{}'.", token);
        }
    }
    return result;
}

class LivingAzerothGuildController
{
public:
    static LivingAzerothGuildController& Instance()
    {
        static LivingAzerothGuildController instance;
        return instance;
    }

    void LoadConfig()
    {
        _config.Enabled = sConfigMgr->GetOption<bool>("LivingAzerothGuild.Enable", false);
        _config.ObserveOnly = sConfigMgr->GetOption<bool>("LivingAzerothGuild.ObserveOnly", true);
        _config.ApplyEnrollment = sConfigMgr->GetOption<bool>("LivingAzerothGuild.ApplyEnrollment", false);
        _config.ApplyRankChanges = sConfigMgr->GetOption<bool>("LivingAzerothGuild.ApplyRankChanges", false);
        _config.MoveOwnedAiFromOtherGuilds =
            sConfigMgr->GetOption<bool>("LivingAzerothGuild.MoveOwnedAiFromOtherGuilds", false);
        _config.AutoEnrollUnguildedHumans =
            sConfigMgr->GetOption<bool>("LivingAzerothGuild.AutoEnrollUnguildedHumans", false);
        _config.AllowCreateMissingGuilds =
            sConfigMgr->GetOption<bool>("LivingAzerothGuild.AllowCreateMissingGuilds", false);
        _config.RequireTwoSideGuildsOff =
            sConfigMgr->GetOption<bool>("LivingAzerothGuild.RequireTwoSideGuildsOff", true);
        _config.AllianceName = sConfigMgr->GetOption<std::string>(
            "LivingAzerothGuild.Alliance.Name", "Living Azeroth Alliance");
        _config.AllianceLeaderName = sConfigMgr->GetOption<std::string>(
            "LivingAzerothGuild.Alliance.LeaderName", "");
        _config.AllianceLeaderGuid = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzerothGuild.Alliance.LeaderGuid", 0);
        _config.AllianceLegacyGuildId = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzerothGuild.Alliance.LegacyGuildId", 0);
        _config.AllianceLegacyName = sConfigMgr->GetOption<std::string>(
            "LivingAzerothGuild.Alliance.LegacyName", "Living Azeroth");
        _config.HordeName = sConfigMgr->GetOption<std::string>(
            "LivingAzerothGuild.Horde.Name", "Living Azeroth Horde");
        _config.HordeLeaderName = sConfigMgr->GetOption<std::string>(
            "LivingAzerothGuild.Horde.LeaderName", "");
        _config.HordeLeaderGuid = sConfigMgr->GetOption<std::uint32_t>(
            "LivingAzerothGuild.Horde.LeaderGuid", 0);
        _config.HistoryPath = sConfigMgr->GetOption<std::string>(
            "LivingAzerothGuild.HistoryPath", "/run/living-azeroth/logs/guild-ranking/history.jsonl");
        _config.AuditIntervalMs = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>("LivingAzerothGuild.AuditIntervalSeconds", 60),
            std::uint32_t(5), std::uint32_t(3600)) * IN_MILLISECONDS;
        _config.MaxBatchSize = std::clamp(
            sConfigMgr->GetOption<std::uint32_t>("LivingAzerothGuild.MaxBatchSize", 25),
            std::uint32_t(1), std::uint32_t(100));
        _config.PolicyVersion = std::max(
            sConfigMgr->GetOption<std::uint32_t>("LivingAzerothGuild.PolicyVersion", 1),
            std::uint32_t(1));
        _config.ExcludedCharacterGuids = ParseGuidSet(sConfigMgr->GetOption<std::string>(
            "LivingAzerothGuild.ExcludedCharacterGuids", "900001"));
    }

    void OnStartup()
    {
        _elapsedMs = 0;
        _cursorGuid = 0;
        _projectedThisCycle = 0;
        _schemaReady = false;
        _mutationsReady = false;

        if (!_config.Enabled)
        {
            LOG_INFO("module", "LivingAzerothGuild: disabled (safe default).");
            return;
        }

        QueryResult schema = CharacterDatabase.Query(
            "SELECT schema_version FROM la_guild_schema WHERE singleton_id = 1");
        _schemaReady = schema && (*schema)[0].Get<std::uint32_t>() >= GuildPolicySchemaVersion;
        bool const crossFactionGuilds = sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GUILD);
        bool const leaderConfigReady = !_config.AllianceLeaderName.empty() &&
            _config.AllianceLeaderGuid != 0 && !_config.HordeLeaderName.empty() &&
            _config.HordeLeaderGuid != 0 && _config.AllianceLeaderGuid != _config.HordeLeaderGuid;
        _mutationsReady = _schemaReady && leaderConfigReady && !_config.ObserveOnly &&
            (!_config.RequireTwoSideGuildsOff || !crossFactionGuilds);

        if (!_schemaReady)
            LOG_ERROR("module", "LivingAzerothGuild: schema unavailable; controller is fail-closed.");
        if (_config.RequireTwoSideGuildsOff && crossFactionGuilds)
            LOG_ERROR("module", "LivingAzerothGuild: AllowTwoSide.Interaction.Guild must be 0; native mutations are blocked.");
        if (!leaderConfigReady)
            LOG_ERROR("module", "LivingAzerothGuild: both distinct leader names and GUIDs are required; native mutations are blocked.");

        std::ostringstream extra;
        extra << ",\"schema_ready\":" << (_schemaReady ? "true" : "false")
              << ",\"observe_only\":" << (_config.ObserveOnly ? "true" : "false")
              << ",\"mutations_ready\":" << (_mutationsReady ? "true" : "false")
              << ",\"leader_config_ready\":" << (leaderConfigReady ? "true" : "false")
              << ",\"cross_faction_guilds\":" << (crossFactionGuilds ? "true" : "false")
              << ",\"policy_version\":" << _config.PolicyVersion;
        WriteHistory("startup", extra.str());
    }

    void OnShutdown()
    {
        if (_config.Enabled)
            WriteHistory("shutdown", ",\"projected_in_cycle\":" + std::to_string(_projectedThisCycle));
    }

    void OnUpdate(std::uint32_t diff)
    {
        if (!_config.Enabled || !_schemaReady)
            return;
        _elapsedMs += diff;
        if (_elapsedMs < _config.AuditIntervalMs)
            return;
        _elapsedMs = 0;
        ProjectBatch();
    }

    void OnPlayerLogin(Player* player)
    {
        if (!_config.Enabled || !_schemaReady || !player)
            return;

        CharacterProjection projection;
        if (!LoadProjection(player->GetGUID().GetCounter(), projection))
            return;
        ResolveExpectedGuild(projection);
        EvaluateProjection(projection);
        PersistProjection(projection);

        if (_mutationsReady)
        {
            ResolveOrCreateGuildOnLogin(player, projection);
            ResolveExpectedGuild(projection);
            EvaluateProjection(projection);
            ApplyNativePolicy(player, projection);
            if (LoadProjection(player->GetGUID().GetCounter(), projection))
            {
                ResolveExpectedGuild(projection);
                EvaluateProjection(projection);
                PersistProjection(projection);
            }
        }
    }

private:
    bool LoadProjection(std::uint32_t guid, CharacterProjection& projection) const
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT c.guid,c.account,c.race,c.level,c.totalKills,"
            "(SELECT COUNT(*) FROM character_queststatus_rewarded q WHERE q.guid=c.guid),"
            "IFNULL(gm.guildid,0),IFNULL(gm.`rank`,255) "
            "FROM characters c LEFT JOIN guild_member gm ON gm.guid=c.guid WHERE c.guid={}", guid);
        if (!result)
            return false;
        PopulateProjection(result->Fetch(), projection);
        return true;
    }

    void PopulateProjection(Field* fields, CharacterProjection& projection) const
    {
        projection.Guid = fields[0].Get<std::uint32_t>();
        projection.Account = fields[1].Get<std::uint32_t>();
        projection.Race = fields[2].Get<std::uint8_t>();
        projection.Level = fields[3].Get<std::uint8_t>();
        projection.HonorableKills = fields[4].Get<std::uint32_t>();
        projection.RewardedQuests = fields[5].Get<std::uint32_t>();
        projection.ObservedGuildId = fields[6].Get<std::uint32_t>();
        projection.ObservedRank = fields[7].Get<std::uint8_t>();
        projection.Faction = FactionFromRace(projection.Race);
        projection.Subject = Classify(projection.Guid, projection.Account);
        projection.RankPoints = ComputeRankPoints(
            projection.Level, projection.RewardedQuests, projection.HonorableKills);
        projection.ProjectedRank = EarnedNativeRank(projection.RankPoints);
    }

    SubjectKind Classify(std::uint32_t guid, std::uint32_t account) const
    {
        if (_config.ExcludedCharacterGuids.contains(guid))
            return SubjectKind::Excluded;
        if (CharacterDatabase.Query(
                "SELECT 1 FROM la_guild_exclusion WHERE character_guid={} AND active=1", guid))
            return SubjectKind::Excluded;
        if (PlayerbotsDatabase.Query(
                "SELECT 1 FROM playerbots_account_type WHERE account_id={} AND account_type IN (1,2)",
                account))
            return SubjectKind::Ai;
        return SubjectKind::Human;
    }

    Guild* GetExpectedGuild(NativeFaction faction) const
    {
        if (faction == NativeFaction::Alliance)
            return sGuildMgr->GetGuildByName(_config.AllianceName);
        if (faction == NativeFaction::Horde)
            return sGuildMgr->GetGuildByName(_config.HordeName);
        return nullptr;
    }

    void ResolveExpectedGuild(CharacterProjection& projection) const
    {
        Guild* guild = GetExpectedGuild(projection.Faction);
        projection.ExpectedGuildId = guild ? guild->GetId() : 0;
        if (guild && guild->GetLeaderGUID().GetCounter() == projection.Guid &&
            projection.Subject != SubjectKind::Excluded)
            projection.Subject = SubjectKind::System;
    }

    bool GuildFactionMatches(Guild const* guild, NativeFaction faction) const
    {
        if (!guild || faction == NativeFaction::Invalid)
            return false;
        CharacterCacheEntry const* leader = sCharacterCache->GetCharacterCacheByGuid(guild->GetLeaderGUID());
        return leader && FactionFromRace(leader->Race) == faction;
    }

    std::string const& ConfiguredLeaderName(NativeFaction faction) const
    {
        return faction == NativeFaction::Alliance
            ? _config.AllianceLeaderName : _config.HordeLeaderName;
    }

    std::uint32_t ConfiguredLeaderGuid(NativeFaction faction) const
    {
        return faction == NativeFaction::Alliance
            ? _config.AllianceLeaderGuid : _config.HordeLeaderGuid;
    }

    bool PlayerMatchesConfiguredLeader(Player const* player, NativeFaction faction) const
    {
        return player && faction != NativeFaction::Invalid &&
            player->GetGUID().GetCounter() == ConfiguredLeaderGuid(faction) &&
            player->GetName() == ConfiguredLeaderName(faction) &&
            FactionFromRace(player->getRace()) == faction;
    }

    bool GuildLeaderMatchesConfiguredIdentity(Guild const* guild, NativeFaction faction) const
    {
        if (!guild || faction == NativeFaction::Invalid ||
            guild->GetLeaderGUID().GetCounter() != ConfiguredLeaderGuid(faction))
            return false;
        CharacterCacheEntry const* leader = sCharacterCache->GetCharacterCacheByGuid(guild->GetLeaderGUID());
        return leader && leader->Name == ConfiguredLeaderName(faction) &&
            FactionFromRace(leader->Race) == faction;
    }

    void EvaluateProjection(CharacterProjection& projection) const
    {
        if (projection.Faction == NativeFaction::Invalid)
        {
            projection.Subject = SubjectKind::Quarantined;
            projection.EnrollmentState = "blocked";
            projection.DecisionReason = "invalid_character_race";
            return;
        }
        if (projection.Subject == SubjectKind::Excluded)
        {
            projection.EnrollmentState = "excluded";
            projection.DecisionReason = "explicit_exclusion";
            return;
        }
        Guild* expected = GetExpectedGuild(projection.Faction);
        if (!expected)
        {
            projection.EnrollmentState = "pending";
            projection.DecisionReason = "pending_target_guild";
            return;
        }
        if (!GuildFactionMatches(expected, projection.Faction))
        {
            projection.EnrollmentState = "blocked";
            projection.DecisionReason = "target_leader_wrong_faction";
            return;
        }
        if (!GuildLeaderMatchesConfiguredIdentity(expected, projection.Faction))
        {
            projection.EnrollmentState = "blocked";
            projection.DecisionReason = "target_leader_identity_mismatch";
            return;
        }
        if (projection.ObservedGuildId == expected->GetId())
        {
            projection.EnrollmentState = "ready";
            projection.DecisionReason = projection.ObservedRank == projection.ProjectedRank ||
                expected->GetLeaderGUID().GetCounter() == projection.Guid
                ? "ready" : "rank_change_proposed";
            return;
        }
        if (projection.ObservedGuildId != 0 && projection.Subject == SubjectKind::Human)
        {
            projection.EnrollmentState = "protected";
            projection.DecisionReason = "human_external_guild_protected";
            return;
        }
        if (projection.ObservedGuildId != 0 && projection.Subject == SubjectKind::Ai &&
            !_config.MoveOwnedAiFromOtherGuilds)
        {
            projection.EnrollmentState = "protected";
            projection.DecisionReason = "ai_move_switch_off";
            return;
        }
        projection.EnrollmentState = "pending";
        projection.DecisionReason = projection.ObservedGuildId == 0
            ? "enrollment_proposed" : "ai_move_proposed";
    }

    void PersistProjection(CharacterProjection const& projection) const
    {
        CharacterDatabase.Execute(
            "INSERT INTO la_guild_character "
            "(character_guid,subject_kind,native_team,expected_guild_id,observed_guild_id,"
            "observed_native_rank,projected_native_rank,level_snapshot,rewarded_quests_snapshot,"
            "honorable_kills_snapshot,rank_points,enrollment_state,policy_version) VALUES "
            "({},'{}',{},{},{},{},{},{},{},{},{},'{}',{}) "
            "ON DUPLICATE KEY UPDATE "
            "revision=revision+IF(subject_kind<>VALUES(subject_kind) OR native_team<>VALUES(native_team) OR "
            "NOT(expected_guild_id<=>VALUES(expected_guild_id)) OR NOT(observed_guild_id<=>VALUES(observed_guild_id)) OR "
            "NOT(observed_native_rank<=>VALUES(observed_native_rank)) OR projected_native_rank<>VALUES(projected_native_rank) OR "
            "level_snapshot<>VALUES(level_snapshot) OR rewarded_quests_snapshot<>VALUES(rewarded_quests_snapshot) OR "
            "honorable_kills_snapshot<>VALUES(honorable_kills_snapshot) OR rank_points<>VALUES(rank_points) OR "
            "enrollment_state<>VALUES(enrollment_state) OR policy_version<>VALUES(policy_version),1,0),"
            "subject_kind=VALUES(subject_kind),native_team=VALUES(native_team),expected_guild_id=VALUES(expected_guild_id),"
            "observed_guild_id=VALUES(observed_guild_id),observed_native_rank=VALUES(observed_native_rank),"
            "projected_native_rank=VALUES(projected_native_rank),level_snapshot=VALUES(level_snapshot),"
            "rewarded_quests_snapshot=VALUES(rewarded_quests_snapshot),honorable_kills_snapshot=VALUES(honorable_kills_snapshot),"
            "rank_points=VALUES(rank_points),enrollment_state=VALUES(enrollment_state),policy_version=VALUES(policy_version)",
            projection.Guid, SubjectName(projection.Subject), static_cast<std::uint8_t>(projection.Faction),
            projection.ExpectedGuildId ? std::to_string(projection.ExpectedGuildId) : "NULL",
            projection.ObservedGuildId ? std::to_string(projection.ObservedGuildId) : "NULL",
            projection.ObservedRank == NoObservedRank ? "NULL" : std::to_string(projection.ObservedRank),
            projection.ProjectedRank, projection.Level, projection.RewardedQuests,
            projection.HonorableKills, projection.RankPoints, projection.EnrollmentState,
            _config.PolicyVersion);

        if (projection.DecisionReason != "ready")
        {
            CharacterDatabase.Execute(
                "INSERT IGNORE INTO la_guild_decision "
                "(decision_key,character_guid,decision_kind,observed_guild_id,proposed_guild_id,"
                "observed_rank,proposed_rank,status,reason,policy_version) VALUES "
                "(SHA2(CONCAT_WS(':',{},{},{},{},{},'{}'),256),{},'enrollment',{},{},{},{},'observed','{}',{})",
                _config.PolicyVersion, projection.Guid, projection.ObservedGuildId,
                projection.ExpectedGuildId, projection.ProjectedRank, projection.DecisionReason,
                projection.Guid,
                projection.ObservedGuildId ? std::to_string(projection.ObservedGuildId) : "NULL",
                projection.ExpectedGuildId ? std::to_string(projection.ExpectedGuildId) : "NULL",
                projection.ObservedRank == NoObservedRank ? "NULL" : std::to_string(projection.ObservedRank),
                projection.ProjectedRank, projection.DecisionReason, _config.PolicyVersion);
        }
    }

    void ProjectBatch()
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT c.guid,c.account,c.race,c.level,c.totalKills,"
            "(SELECT COUNT(*) FROM character_queststatus_rewarded q WHERE q.guid=c.guid),"
            "IFNULL(gm.guildid,0),IFNULL(gm.`rank`,255) "
            "FROM characters c LEFT JOIN guild_member gm ON gm.guid=c.guid "
            "WHERE c.guid>{} ORDER BY c.guid LIMIT {}", _cursorGuid, _config.MaxBatchSize);

        if (!result)
        {
            std::ostringstream extra;
            extra << ",\"projected\":" << _projectedThisCycle;
            WriteHistory("projection_cycle_complete", extra.str());
            _cursorGuid = 0;
            _projectedThisCycle = 0;
            return;
        }

        do
        {
            CharacterProjection projection;
            PopulateProjection(result->Fetch(), projection);
            ResolveExpectedGuild(projection);
            EvaluateProjection(projection);
            PersistProjection(projection);
            if (_mutationsReady)
                ApplyNativePolicy(nullptr, projection);
            _cursorGuid = projection.Guid;
            ++_projectedThisCycle;
        }
        while (result->NextRow());
    }

    std::uint8_t NativeRankCount(std::uint32_t guildId) const
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM guild_rank WHERE guildid={}", guildId);
        return result ? (*result)[0].Get<std::uint8_t>() : 0;
    }

    void ConfigureTenRanks(Guild* guild, Player* leader, NativeFaction faction) const
    {
        if (!guild || !leader || !leader->GetSession() || guild->GetLeaderGUID() != leader->GetGUID())
            return;
        std::uint8_t rankCount = NativeRankCount(guild->GetId());
        while (rankCount < RankPolicy.size())
        {
            guild->HandleAddNewRank(leader->GetSession(), RankName(faction, rankCount));
            std::uint8_t nextCount = NativeRankCount(guild->GetId());
            if (nextCount <= rankCount)
                break;
            rankCount = nextCount;
        }
        if (rankCount != RankPolicy.size())
            return;

        std::array<GuildBankRightsAndSlots, GUILD_BANK_MAX_TABS> noBankRights{};
        for (auto const& rank : RankPolicy)
        {
            guild->HandleSetRankInfo(leader->GetSession(), rank.NativeRank,
                RankName(faction, rank.NativeRank), rank.Rights, 0, noBankRights);
        }
    }

    void ResolveOrCreateGuildOnLogin(Player* player, CharacterProjection const& projection)
    {
        if (!player || projection.Faction == NativeFaction::Invalid ||
            projection.Subject == SubjectKind::Excluded || projection.Subject == SubjectKind::Quarantined)
            return;

        Guild* guild = GetExpectedGuild(projection.Faction);
        bool const configuredLeader = PlayerMatchesConfiguredLeader(player, projection.Faction);
        if (!guild && projection.Faction == NativeFaction::Alliance &&
            _config.AllianceLegacyGuildId != 0)
        {
            Guild* legacy = sGuildMgr->GetGuildById(_config.AllianceLegacyGuildId);
            if (configuredLeader && legacy && legacy->GetName() == _config.AllianceLegacyName &&
                legacy->GetLeaderGUID() == player->GetGUID() &&
                FactionFromRace(player->getRace()) == NativeFaction::Alliance &&
                !sGuildMgr->GetGuildByName(_config.AllianceName) && legacy->SetName(_config.AllianceName))
            {
                guild = legacy;
                WriteHistory("legacy_guild_renamed", ",\"guild_id\":" + std::to_string(guild->GetId()));
            }
        }

        if (!guild && _config.AllowCreateMissingGuilds && configuredLeader && player->GetGuildId() == 0)
        {
            if (projection.Subject == SubjectKind::Human || projection.Subject == SubjectKind::Ai)
            {
                std::string const& name = projection.Faction == NativeFaction::Alliance
                    ? _config.AllianceName : _config.HordeName;
                Guild* created = new Guild();
                if (created->Create(player, name))
                {
                    sGuildMgr->AddGuild(created);
                    guild = created;
                    WriteHistory("guild_created", ",\"guild_id\":" + std::to_string(guild->GetId()) +
                        ",\"faction\":\"" + std::string(FactionName(projection.Faction)) + "\"");
                }
                else
                    delete created;
            }
        }

        if (guild && GuildFactionMatches(guild, projection.Faction) &&
            GuildLeaderMatchesConfiguredIdentity(guild, projection.Faction))
        {
            ConfigureTenRanks(guild, player, projection.Faction);
            bool const ranksReady = NativeRankCount(guild->GetId()) == RankPolicy.size();
            CharacterDatabase.Execute(
                "UPDATE la_faction_guild SET guild_id={},leader_guid={},state='{}',policy_version={},revision=revision+1 "
                "WHERE team={} AND (NOT(guild_id<=>{}) OR NOT(leader_guid<=>{}) OR state<>'{}' OR policy_version<>{})",
                guild->GetId(), guild->GetLeaderGUID().GetCounter(), ranksReady ? "ready" : "declared",
                _config.PolicyVersion, static_cast<std::uint8_t>(projection.Faction), guild->GetId(),
                guild->GetLeaderGUID().GetCounter(), ranksReady ? "ready" : "declared",
                _config.PolicyVersion);
        }
    }

    void ApplyNativePolicy(Player* onlinePlayer, CharacterProjection const& projection)
    {
        if (!_mutationsReady || projection.Faction == NativeFaction::Invalid ||
            projection.Subject == SubjectKind::Excluded || projection.Subject == SubjectKind::Quarantined)
            return;
        Guild* expected = GetExpectedGuild(projection.Faction);
        if (!expected || !GuildFactionMatches(expected, projection.Faction) ||
            !GuildLeaderMatchesConfiguredIdentity(expected, projection.Faction))
            return;

        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(projection.Guid);
        bool const inExternalGuild = projection.ObservedGuildId != 0 &&
            projection.ObservedGuildId != expected->GetId();
        if (!CanEnroll(projection.Subject, projection.Faction, projection.Faction,
                inExternalGuild, _config.MoveOwnedAiFromOtherGuilds,
                _config.AutoEnrollUnguildedHumans))
            return;

        std::uint32_t observedGuildId = projection.ObservedGuildId;
        if (_config.ApplyEnrollment && inExternalGuild && projection.Subject == SubjectKind::Ai &&
            _config.MoveOwnedAiFromOtherGuilds)
        {
            if (Guild* previous = sGuildMgr->GetGuildById(observedGuildId))
                previous->DeleteMember(guid, false, true, false);
            observedGuildId = sCharacterCache->GetCharacterGuildIdByGuid(guid);
        }

        if (_config.ApplyEnrollment && observedGuildId == 0)
        {
            std::uint8_t rank = NativeRankCount(expected->GetId()) == RankPolicy.size()
                ? projection.ProjectedRank : GUILD_RANK_NONE;
            if (!expected->AddMember(guid, rank))
                return;
            observedGuildId = expected->GetId();
        }

        if (_config.ApplyRankChanges && observedGuildId == expected->GetId() &&
            NativeRankCount(expected->GetId()) == RankPolicy.size() &&
            expected->GetLeaderGUID() != guid)
        {
            expected->ChangeMemberRank(guid, projection.ProjectedRank);
        }

        if (onlinePlayer && expected->GetLeaderGUID() == onlinePlayer->GetGUID())
            ConfigureTenRanks(expected, onlinePlayer, projection.Faction);
    }

    bool WriteHistory(std::string const& record, std::string const& extra) const
    {
        if (_config.HistoryPath.empty())
            return false;
        std::filesystem::path path(_config.HistoryPath);
        std::error_code error;
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return false;
        std::ofstream output(path, std::ios::app);
        if (!output)
            return false;
        output << "{\"timestamp_unix\":" << GameTime::GetGameTime().count()
               << ",\"record\":\"" << JsonEscape(record) << "\"" << extra << "}\n";
        output.flush();
        return bool(output);
    }

    GuildPolicyConfig _config;
    std::uint32_t _elapsedMs = 0;
    std::uint32_t _cursorGuid = 0;
    std::uint32_t _projectedThisCycle = 0;
    bool _schemaReady = false;
    bool _mutationsReady = false;
};

class LivingAzerothGuildWorldScript : public WorldScript
{
public:
    LivingAzerothGuildWorldScript()
        : WorldScript("LivingAzerothGuildWorldScript",
            {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
             WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN})
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            LivingAzerothGuildController::Instance().LoadConfig();
    }

    void OnBeforeWorldInitialized() override
    {
        LivingAzerothGuildController::Instance().LoadConfig();
    }

    void OnStartup() override
    {
        LivingAzerothGuildController::Instance().OnStartup();
    }

    void OnUpdate(std::uint32_t diff) override
    {
        LivingAzerothGuildController::Instance().OnUpdate(diff);
    }

    void OnShutdown() override
    {
        LivingAzerothGuildController::Instance().OnShutdown();
    }
};

class LivingAzerothGuildPlayerScript : public PlayerScript
{
public:
    LivingAzerothGuildPlayerScript()
        : PlayerScript("LivingAzerothGuildPlayerScript", {PLAYERHOOK_ON_LOGIN})
    {
    }

    void OnPlayerLogin(Player* player) override
    {
        LivingAzerothGuildController::Instance().OnPlayerLogin(player);
    }
};
}

void AddLivingAzerothGuildScripts()
{
    new LivingAzerothGuildWorldScript();
    new LivingAzerothGuildPlayerScript();
}
