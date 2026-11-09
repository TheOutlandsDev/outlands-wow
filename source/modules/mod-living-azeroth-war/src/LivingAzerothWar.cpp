/*
 * Living Azeroth: Battle for Hillsbrad prototype.
 *
 * This module intentionally controls only a registered, bounded Playerbot
 * roster. It does not implement territory ownership, rewards, reinforcement
 * waves, or autonomous command authority.
 */

#include "Config.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldScript.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr uint32 WAR_FACTION_SIZE = 10;
constexpr uint32 WAR_TOTAL_SIZE = WAR_FACTION_SIZE * 2;

enum class WarState
{
    Disabled,
    WaitingForRoster,
    Preparing,
    Staging,
    Countdown,
    Active,
    Complete,
    Aborting,
    Cleanup,
    Finished
};

struct WarLocation
{
    std::string Name;
    uint32 Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float O = 0.0f;
};

struct OriginalLocation
{
    uint32 Map = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float O = 0.0f;
};

struct WarParticipant
{
    ObjectGuid Guid;
    TeamId Team = TEAM_NEUTRAL;
    std::string Name;
    std::string Role;
    OriginalLocation Origin;
    uint8 OriginalLevel = 0;
    bool OriginalPvP = false;
    bool Commander = false;
    bool Prepared = false;
    bool RoleMatch = false;
    bool Restored = false;
};

struct WarConfig
{
    bool Enabled = false;
    bool AutoStart = false;
    uint32 NormalizeLevel = 30;
    uint32 RosterTimeoutMs = 300000;
    uint32 PreparationIntervalMs = 500;
    uint32 StagingMs = 20000;
    uint32 CountdownMs = 10000;
    uint32 ObjectiveMs = 45000;
    uint32 ConclusionMs = 5000;
    uint32 AbortDisengageMs = 3000;
    uint32 CleanupIntervalMs = 250;
    uint32 PerformanceLatencyMs = 200;
    uint32 PerformanceSustainedMs = 30000;
    uint32 ExtremeSpikeMs = 500;
    float CaptureRadius = 60.0f;
    float MaximumTeleportFailurePercent = 15.0f;
    std::string HistoryPath;
    WarLocation Southshore;
    WarLocation TarrenMill;
    WarLocation RoadCheckpoint;
    WarLocation SupplyPoint;
};

char const* StateName(WarState state)
{
    switch (state)
    {
        case WarState::Disabled:
            return "DISABLED";
        case WarState::WaitingForRoster:
            return "WAITING_FOR_ROSTER";
        case WarState::Preparing:
            return "PREPARING";
        case WarState::Staging:
            return "STAGING";
        case WarState::Countdown:
            return "COUNTDOWN";
        case WarState::Active:
            return "ACTIVE";
        case WarState::Complete:
            return "COMPLETE";
        case WarState::Aborting:
            return "ABORTING";
        case WarState::Cleanup:
            return "CLEANUP";
        case WarState::Finished:
            return "FINISHED";
    }

    return "UNKNOWN";
}

char const* TeamName(TeamId team)
{
    if (team == TEAM_ALLIANCE)
    {
        return "Alliance";
    }

    if (team == TEAM_HORDE)
    {
        return "Horde";
    }

    return "Neutral";
}

std::string JsonEscape(std::string const& value)
{
    std::ostringstream escaped;
    for (unsigned char character : value)
    {
        switch (character)
        {
            case '\\':
                escaped << "\\\\";
                break;
            case '"':
                escaped << "\\\"";
                break;
            case '\b':
                escaped << "\\b";
                break;
            case '\f':
                escaped << "\\f";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default:
                if (character >= 0x20)
                {
                    escaped << character;
                }
                break;
        }
    }

    return escaped.str();
}

class LivingAzerothWarController
{
public:
    static LivingAzerothWarController& Instance()
    {
        static LivingAzerothWarController instance;
        return instance;
    }

    void LoadConfig(bool reload)
    {
        bool wasEnabled = _config.Enabled;

        _config.Enabled = sConfigMgr->GetOption<bool>("War.Enable", false);
        _config.AutoStart = sConfigMgr->GetOption<bool>("War.AutoStart", false);
        _config.NormalizeLevel = std::clamp(
            sConfigMgr->GetOption<uint32>("War.NormalizeLevel", 30), uint32(20), uint32(35));
        _config.RosterTimeoutMs =
            std::clamp(sConfigMgr->GetOption<uint32>("War.RosterTimeoutSeconds", 300), uint32(30), uint32(1800)) *
            IN_MILLISECONDS;
        _config.PreparationIntervalMs = std::clamp(
            sConfigMgr->GetOption<uint32>("War.PreparationIntervalMs", 500), uint32(100), uint32(5000));
        _config.StagingMs =
            std::clamp(sConfigMgr->GetOption<uint32>("War.StagingSeconds", 20), uint32(5), uint32(300)) *
            IN_MILLISECONDS;
        _config.CountdownMs =
            std::clamp(sConfigMgr->GetOption<uint32>("War.CountdownSeconds", 10), uint32(5), uint32(120)) *
            IN_MILLISECONDS;
        _config.ObjectiveMs =
            std::clamp(sConfigMgr->GetOption<uint32>("War.ObjectiveSeconds", 45), uint32(10), uint32(900)) *
            IN_MILLISECONDS;
        _config.ConclusionMs =
            std::clamp(sConfigMgr->GetOption<uint32>("War.ConclusionSeconds", 5), uint32(1), uint32(120)) *
            IN_MILLISECONDS;
        _config.AbortDisengageMs =
            std::clamp(sConfigMgr->GetOption<uint32>("War.AbortDisengageSeconds", 3), uint32(1), uint32(60)) *
            IN_MILLISECONDS;
        _config.CleanupIntervalMs = std::clamp(
            sConfigMgr->GetOption<uint32>("War.CleanupReleaseIntervalMs", 250), uint32(100), uint32(5000));
        _config.CaptureRadius =
            std::clamp(sConfigMgr->GetOption<float>("War.CaptureRadius", 60.0f), 10.0f, 150.0f);
        _config.PerformanceLatencyMs = std::clamp(
            sConfigMgr->GetOption<uint32>("War.PerformanceAbortLatencyMs", 200), uint32(50), uint32(1000));
        _config.PerformanceSustainedMs = std::clamp(
            sConfigMgr->GetOption<uint32>("War.PerformanceAbortSustainedSeconds", 30), uint32(5), uint32(300)) *
            IN_MILLISECONDS;
        _config.ExtremeSpikeMs = std::clamp(
            sConfigMgr->GetOption<uint32>("War.ExtremeUpdateSpikeMs", 500), uint32(100), uint32(5000));
        _config.MaximumTeleportFailurePercent = std::clamp(
            sConfigMgr->GetOption<float>("War.MaximumTeleportFailurePercent", 15.0f), 0.0f, 100.0f);
        _config.HistoryPath =
            sConfigMgr->GetOption<std::string>("War.HistoryPath",
                                               "/run/living-azeroth/logs/war/hillsbrad-history.jsonl");

        _config.Southshore = LoadLocation(
            "Southshore", "War.Southshore", 0, -853.221f, -533.529f, 9.98556f, 0.242866f);
        _config.TarrenMill = LoadLocation(
            "TarrenMill", "War.TarrenMill", 0, -34.1467f, -923.366f, 54.5576f, 0.15019f);
        _config.RoadCheckpoint = LoadLocation(
            "RoadCheckpoint", "War.RoadCheckpoint", 0, -436.657f, -581.254f, 53.5944f, 1.25917f);
        _config.SupplyPoint = LoadLocation(
            "SupplyPoint", "War.SupplyPoint", 0, -510.344f, 9.53799f, 50.345f, 1.33728f);

        uint32 configuredFactionSize =
            sConfigMgr->GetOption<uint32>("War.MaximumActivePerFaction", WAR_FACTION_SIZE);
        bool reinforcements = sConfigMgr->GetOption<bool>("War.ReinforcementsEnable", false);
        bool rewards = sConfigMgr->GetOption<bool>("War.RewardsEnable", false);

        if (configuredFactionSize != WAR_FACTION_SIZE)
        {
            LOG_ERROR("living_azeroth.war",
                      "War.MaximumActivePerFaction must be exactly {} for this prototype; disabling event.",
                      WAR_FACTION_SIZE);
            _config.Enabled = false;
        }

        if (reinforcements || rewards)
        {
            LOG_ERROR("living_azeroth.war",
                      "Reinforcements and rewards are unsupported in this prototype; disabling event.");
            _config.Enabled = false;
        }

        LOG_INFO("living_azeroth.war",
                 "Battle for Hillsbrad configuration loaded: enabled={}, autoStart={}, factionSize={}, level={}.",
                 _config.Enabled, _config.AutoStart, WAR_FACTION_SIZE, _config.NormalizeLevel);

        if (reload && wasEnabled && !_config.Enabled && IsRunning())
        {
            Abort("configuration_disabled_during_event");
        }
        else if (reload && _config.Enabled && _config.AutoStart && _state == WarState::Disabled)
        {
            Start();
        }
    }

    void OnStartup()
    {
        if (_config.Enabled && _config.AutoStart)
        {
            Start();
        }
    }

    void OnUpdate(uint32 diff)
    {
        if (_state == WarState::Disabled || _state == WarState::Finished)
        {
            return;
        }

        MonitorPerformance(diff);
        if (_state == WarState::Aborting)
        {
            _stateElapsedMs += diff;
            if (_stateElapsedMs >= _config.AbortDisengageMs)
            {
                BeginCleanup();
            }
            return;
        }

        _stateElapsedMs += diff;
        _actionElapsedMs += diff;

        switch (_state)
        {
            case WarState::WaitingForRoster:
                if (_actionElapsedMs >= IN_MILLISECONDS)
                {
                    _actionElapsedMs = 0;
                    TryRegisterRoster();
                }
                if (_state == WarState::WaitingForRoster && _stateElapsedMs >= _config.RosterTimeoutMs)
                {
                    Abort("roster_timeout");
                }
                break;
            case WarState::Preparing:
                if (_actionElapsedMs >= _config.PreparationIntervalMs)
                {
                    _actionElapsedMs = 0;
                    PrepareNextParticipant();
                }
                break;
            case WarState::Staging:
                if (_stateElapsedMs >= _config.StagingMs)
                {
                    SetState(WarState::Countdown);
                }
                break;
            case WarState::Countdown:
                if (_stateElapsedMs >= _config.CountdownMs)
                {
                    StartObjective(0);
                }
                break;
            case WarState::Active:
                if (_stateElapsedMs >= _config.ObjectiveMs)
                {
                    FinishCurrentObjective();
                }
                break;
            case WarState::Complete:
                if (_stateElapsedMs >= _config.ConclusionMs)
                {
                    BeginCleanup();
                }
                break;
            case WarState::Cleanup:
                if (_actionElapsedMs >= _config.CleanupIntervalMs)
                {
                    _actionElapsedMs = 0;
                    CleanupNextParticipant();
                }
                break;
            case WarState::Disabled:
            case WarState::Aborting:
            case WarState::Finished:
                break;
        }
    }

    void OnPvPKill(Player* killer, Player* killed)
    {
        if (_state != WarState::Active || !killer || !killed)
        {
            return;
        }

        WarParticipant* killerParticipant = FindParticipant(killer->GetGUID());
        WarParticipant* killedParticipant = FindParticipant(killed->GetGUID());
        if (!killerParticipant || !killedParticipant || killerParticipant->Team == killedParticipant->Team)
        {
            return;
        }

        if (killerParticipant->Team == TEAM_ALLIANCE)
        {
            ++_allianceKillScore;
        }
        else if (killerParticipant->Team == TEAM_HORDE)
        {
            ++_hordeKillScore;
        }

        std::ostringstream extra;
        extra << ",\"objective\":\"" << JsonEscape(CurrentObjective().Name) << "\""
              << ",\"killer_guid\":" << killerParticipant->Guid.GetCounter()
              << ",\"killer\":\"" << JsonEscape(killerParticipant->Name) << "\""
              << ",\"killed_guid\":" << killedParticipant->Guid.GetCounter()
              << ",\"killed\":\"" << JsonEscape(killedParticipant->Name) << "\""
              << ",\"scoring_faction\":\"" << TeamName(killerParticipant->Team) << "\"";
        WriteHistory("pvp_kill", extra.str());
    }

    void OnShutdown()
    {
        if (!IsRunning())
        {
            return;
        }

        if (_state != WarState::Complete && !_aborted)
        {
            _aborted = true;
            _abortReason = "world_shutdown";
            _winner = "none";
            WriteHistory("technical_abort", ",\"reason\":\"world_shutdown\"");
        }

        CleanupAllParticipants();
        FinalizeEvent();
    }

private:
    using PlayerPredicate = std::function<bool(Player*)>;

    LivingAzerothWarController() = default;

    WarLocation LoadLocation(std::string name, std::string const& prefix, uint32 map, float x, float y, float z,
                             float orientation)
    {
        WarLocation location;
        location.Name = std::move(name);
        location.Map = sConfigMgr->GetOption<uint32>(prefix + ".Map", map);
        location.X = sConfigMgr->GetOption<float>(prefix + ".X", x);
        location.Y = sConfigMgr->GetOption<float>(prefix + ".Y", y);
        location.Z = sConfigMgr->GetOption<float>(prefix + ".Z", z);
        location.O = sConfigMgr->GetOption<float>(prefix + ".O", orientation);
        return location;
    }

    bool IsRunning() const
    {
        return _state != WarState::Disabled && _state != WarState::Finished;
    }

    void Start()
    {
        if (IsRunning())
        {
            LOG_WARN("living_azeroth.war", "Start ignored because an event is already running.");
            return;
        }

        ResetEvent();
        _eventId = std::to_string(GameTime::GetGameTime().count());
        LOG_INFO("living_azeroth.war", "Starting Battle for Hillsbrad event {}.", _eventId);
        WriteHistory("event_start",
                     ",\"required_alliance\":10,\"required_horde\":10,\"reinforcements\":false,"
                     "\"rewards\":false");
        SetState(WarState::WaitingForRoster);
    }

    void ResetEvent()
    {
        _participants.clear();
        _prepareIndex = 0;
        _cleanupIndex = 0;
        _objectiveIndex = 0;
        _allianceKillScore = 0;
        _hordeKillScore = 0;
        _allianceObjectiveScore = 0;
        _hordeObjectiveScore = 0;
        _teleportAttempts = 0;
        _teleportFailures = 0;
        _normalizedCount = 0;
        _restoredCount = 0;
        _sustainedLatencyMs = 0;
        _stateElapsedMs = 0;
        _actionElapsedMs = 0;
        _aborted = false;
        _abortReason.clear();
        _winner = "none";
        _historyHealthy = true;
    }

    void SetState(WarState state)
    {
        WarState previous = _state;
        _state = state;
        _stateElapsedMs = 0;
        _actionElapsedMs = 0;

        LOG_INFO("living_azeroth.war", "Event {} state {} -> {}.", _eventId, StateName(previous),
                 StateName(state));

        std::ostringstream extra;
        extra << ",\"from\":\"" << StateName(previous) << "\""
              << ",\"to\":\"" << StateName(state) << "\"";
        WriteHistory("state_transition", extra.str());
    }

    void TryRegisterRoster()
    {
        std::vector<Player*> alliance;
        std::vector<Player*> horde;

        for (auto const& botEntry : sRandomPlayerbotMgr.GetAllBots())
        {
            Player* player = botEntry.second;
            if (!player || !player->IsInWorld() || player->IsBeingTeleported() ||
                !sRandomPlayerbotMgr.IsRandomBot(player))
            {
                continue;
            }

            if (player->GetTeamId() == TEAM_ALLIANCE)
            {
                alliance.push_back(player);
            }
            else if (player->GetTeamId() == TEAM_HORDE)
            {
                horde.push_back(player);
            }
        }

        SortPlayers(alliance);
        SortPlayers(horde);

        if (alliance.size() < WAR_FACTION_SIZE || horde.size() < WAR_FACTION_SIZE)
        {
            LOG_INFO("living_azeroth.war", "Waiting for roster: Alliance={}, Horde={}, required={} each.",
                     alliance.size(), horde.size(), WAR_FACTION_SIZE);
            return;
        }

        SelectFactionRoster(alliance, TEAM_ALLIANCE);
        SelectFactionRoster(horde, TEAM_HORDE);

        if (_participants.size() != WAR_TOTAL_SIZE)
        {
            Abort("roster_invariant_failure");
            return;
        }

        LOG_INFO("living_azeroth.war", "Registered exact 10v10 roster for event {}.", _eventId);
        WriteHistory("roster_registered", ",\"alliance\":10,\"horde\":10,\"total\":20");
        SetState(WarState::Preparing);
    }

    void SortPlayers(std::vector<Player*>& players)
    {
        std::sort(players.begin(), players.end(), [](Player const* left, Player const* right)
        {
            return left->GetGUID().GetCounter() < right->GetGUID().GetCounter();
        });
    }

    void SelectFactionRoster(std::vector<Player*> const& candidates, TeamId team)
    {
        std::vector<Player*> remaining = candidates;
        std::array<std::pair<std::string, PlayerPredicate>, WAR_FACTION_SIZE> slots =
        {{
            {"commander", [](Player* player) { return PlayerbotAI::IsTank(player, true); }},
            {"frontline", [](Player* player) { return PlayerbotAI::IsTank(player, true); }},
            {"frontline", [](Player* player) { return PlayerbotAI::IsMelee(player, true); }},
            {"healer", [](Player* player) { return PlayerbotAI::IsHeal(player, true); }},
            {"healer", [](Player* player) { return PlayerbotAI::IsHeal(player, true); }},
            {"ranged", [](Player* player) { return PlayerbotAI::IsRanged(player, true); }},
            {"ranged", [](Player* player) { return PlayerbotAI::IsRanged(player, true); }},
            {"melee", [](Player* player) { return PlayerbotAI::IsMelee(player, true); }},
            {"melee", [](Player* player) { return PlayerbotAI::IsMelee(player, true); }},
            {"reserve", [](Player*) { return true; }}
        }};

        for (auto const& [role, predicate] : slots)
        {
            auto selected = std::find_if(remaining.begin(), remaining.end(), predicate);
            if (selected == remaining.end())
            {
                selected = remaining.begin();
            }

            Player* player = *selected;
            WarParticipant participant;
            participant.Guid = player->GetGUID();
            participant.Team = team;
            participant.Name = player->GetName();
            participant.Role = role;
            participant.Origin.Map = player->GetMapId();
            participant.Origin.X = player->GetPositionX();
            participant.Origin.Y = player->GetPositionY();
            participant.Origin.Z = player->GetPositionZ();
            participant.Origin.O = player->GetOrientation();
            participant.OriginalLevel = player->GetLevel();
            participant.OriginalPvP = player->IsPvP();
            participant.Commander = role == "commander";
            _participants.push_back(std::move(participant));
            remaining.erase(selected);
        }
    }

    void PrepareNextParticipant()
    {
        if (_prepareIndex >= _participants.size())
        {
            FinishPreparation();
            return;
        }

        WarParticipant& participant = _participants[_prepareIndex];
        Player* player = ObjectAccessor::FindConnectedPlayer(participant.Guid);
        if (!player || !player->IsInWorld() || player->IsBeingTeleported())
        {
            Abort("registered_participant_missing_during_preparation");
            return;
        }

        player->CombatStop(true);
        player->SetControlled(true, UNIT_STATE_ROOT);
        player->SetPvP(false);

        PlayerbotFactory factory(player, _config.NormalizeLevel);
        factory.Randomize(false);
        ++_normalizedCount;

        if (PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(player))
        {
            botAI->ChangeStrategy("-grind", BOT_STATE_NON_COMBAT);
            botAI->ChangeStrategy("-rpg", BOT_STATE_NON_COMBAT);
            botAI->ChangeStrategy("-travel", BOT_STATE_NON_COMBAT);
            botAI->ChangeStrategy("-quest", BOT_STATE_NON_COMBAT);
            botAI->ChangeStrategy("+pvp", BOT_STATE_NON_COMBAT);
        }

        sRandomPlayerbotMgr.ScheduleTeleport(participant.Guid.GetCounter(), 3600);
        participant.Prepared = true;
        participant.RoleMatch = RoleMatches(player, participant.Role);

        WarLocation const& staging =
            participant.Team == TEAM_ALLIANCE ? _config.Southshore : _config.TarrenMill;
        TeleportParticipant(participant, staging, FactionSlot(participant), true);

        std::ostringstream extra;
        extra << ",\"guid\":" << participant.Guid.GetCounter()
              << ",\"name\":\"" << JsonEscape(participant.Name) << "\""
              << ",\"faction\":\"" << TeamName(participant.Team) << "\""
              << ",\"role\":\"" << JsonEscape(participant.Role) << "\""
              << ",\"role_match\":" << (participant.RoleMatch ? "true" : "false")
              << ",\"original_level\":" << uint32(participant.OriginalLevel)
              << ",\"normalized_level\":" << uint32(player->GetLevel());
        WriteHistory("participant_prepared", extra.str());

        ++_prepareIndex;
    }

    bool RoleMatches(Player* player, std::string const& role) const
    {
        if (role == "commander" || role == "frontline")
        {
            return PlayerbotAI::IsTank(player, true) || PlayerbotAI::IsMelee(player, true);
        }
        if (role == "healer")
        {
            return PlayerbotAI::IsHeal(player, true);
        }
        if (role == "ranged")
        {
            return PlayerbotAI::IsRanged(player, true);
        }
        if (role == "melee")
        {
            return PlayerbotAI::IsMelee(player, true);
        }
        return true;
    }

    uint32 FactionSlot(WarParticipant const& participant) const
    {
        uint32 slot = 0;
        for (WarParticipant const& current : _participants)
        {
            if (current.Team != participant.Team)
            {
                continue;
            }
            if (current.Guid == participant.Guid)
            {
                return slot;
            }
            ++slot;
        }

        return slot;
    }

    void FinishPreparation()
    {
        if (_normalizedCount != WAR_TOTAL_SIZE ||
            std::any_of(_participants.begin(), _participants.end(),
                        [](WarParticipant const& participant) { return !participant.Prepared; }))
        {
            Abort("preparation_invariant_failure");
            return;
        }

        if (TeleportFailurePercent() > _config.MaximumTeleportFailurePercent)
        {
            Abort("teleport_failure_threshold_during_staging");
            return;
        }

        WriteHistory("preparation_complete", ",\"normalized\":20,\"level\":30");
        SetState(WarState::Staging);
    }

    void StartObjective(uint32 objectiveIndex)
    {
        if (objectiveIndex >= Objectives().size())
        {
            Abort("objective_index_invariant_failure");
            return;
        }

        _objectiveIndex = objectiveIndex;
        WarLocation const& objective = CurrentObjective();
        LOG_INFO("living_azeroth.war", "Starting objective {} of 5: {}.", objectiveIndex + 1, objective.Name);

        for (WarParticipant& participant : _participants)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(participant.Guid);
            if (!player || !player->IsInWorld())
            {
                Abort("participant_missing_before_objective");
                return;
            }

            player->CombatStop(true);
            player->SetPvP(false);
            player->SetControlled(true, UNIT_STATE_ROOT);
            if (player->isDead())
            {
                player->ResurrectPlayer(1.0f, false);
                player->SpawnCorpseBones();
            }

            TeleportParticipant(participant, objective, FactionSlot(participant), true);
        }

        if (TeleportFailurePercent() > _config.MaximumTeleportFailurePercent)
        {
            Abort("teleport_failure_threshold_before_objective");
            return;
        }

        for (WarParticipant& participant : _participants)
        {
            if (Player* player = ObjectAccessor::FindConnectedPlayer(participant.Guid))
            {
                player->SetPvP(true);
                player->SetControlled(false, UNIT_STATE_ROOT);
            }
        }

        std::ostringstream extra;
        extra << ",\"index\":" << objectiveIndex + 1
              << ",\"objective\":\"" << JsonEscape(objective.Name) << "\""
              << ",\"duration_seconds\":" << _config.ObjectiveMs / IN_MILLISECONDS;
        WriteHistory("objective_start", extra.str());
        SetState(WarState::Active);
    }

    std::array<WarLocation, 5> Objectives() const
    {
        WarLocation commander = _config.RoadCheckpoint;
        commander.Name = "CommanderSurvival";

        WarLocation southshore = _config.Southshore;
        southshore.Name = "SouthshoreDefenseCapture";

        WarLocation tarrenMill = _config.TarrenMill;
        tarrenMill.Name = "TarrenMillDefenseCapture";

        WarLocation checkpoint = _config.RoadCheckpoint;
        checkpoint.Name = "RoadCheckpoint";

        WarLocation supply = _config.SupplyPoint;
        supply.Name = "SupplyObjective";

        return {southshore, tarrenMill, checkpoint, commander, supply};
    }

    WarLocation CurrentObjective() const
    {
        return Objectives()[_objectiveIndex];
    }

    bool TeleportParticipant(WarParticipant& participant, WarLocation const& location, uint32 slot, bool separated)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(participant.Guid);
        if (!player || !player->IsInWorld())
        {
            ++_teleportAttempts;
            ++_teleportFailures;
            return false;
        }

        float factionDirection = participant.Team == TEAM_ALLIANCE ? -1.0f : 1.0f;
        float separation = separated ? 10.0f : 0.0f;
        float rowOffset = float(slot / 5) * 2.5f;
        float columnOffset = (float(slot % 5) - 2.0f) * 3.0f;
        float x = location.X + factionDirection * (separation + rowOffset);
        float y = location.Y + columnOffset;
        float orientation = participant.Team == TEAM_ALLIANCE ? 0.0f : 3.14159265f;

        ++_teleportAttempts;
        if (!player->TeleportTo(location.Map, x, y, location.Z, orientation))
        {
            ++_teleportFailures;
            LOG_ERROR("living_azeroth.war", "Teleport failed for {} ({}) to {}.", participant.Name,
                      participant.Guid.GetCounter(), location.Name);
            return false;
        }

        return true;
    }

    float TeleportFailurePercent() const
    {
        if (_teleportAttempts == 0)
        {
            return 0.0f;
        }

        return 100.0f * float(_teleportFailures) / float(_teleportAttempts);
    }

    void FinishCurrentObjective()
    {
        WarLocation const objective = CurrentObjective();
        uint32 allianceAlive = 0;
        uint32 hordeAlive = 0;
        TeamId winner = TEAM_NEUTRAL;

        if (_objectiveIndex == 3)
        {
            bool allianceCommanderAlive = CommanderAlive(TEAM_ALLIANCE);
            bool hordeCommanderAlive = CommanderAlive(TEAM_HORDE);
            if (allianceCommanderAlive)
            {
                ++_allianceObjectiveScore;
                allianceAlive = 1;
            }
            if (hordeCommanderAlive)
            {
                ++_hordeObjectiveScore;
                hordeAlive = 1;
            }
            if (allianceCommanderAlive != hordeCommanderAlive)
            {
                winner = allianceCommanderAlive ? TEAM_ALLIANCE : TEAM_HORDE;
            }
        }
        else
        {
            CountLivingAtObjective(objective, allianceAlive, hordeAlive);
            if (allianceAlive > hordeAlive)
            {
                ++_allianceObjectiveScore;
                winner = TEAM_ALLIANCE;
            }
            else if (hordeAlive > allianceAlive)
            {
                ++_hordeObjectiveScore;
                winner = TEAM_HORDE;
            }
        }

        std::ostringstream extra;
        extra << ",\"index\":" << _objectiveIndex + 1
              << ",\"objective\":\"" << JsonEscape(objective.Name) << "\""
              << ",\"alliance_living_in_radius\":" << allianceAlive
              << ",\"horde_living_in_radius\":" << hordeAlive
              << ",\"winner\":\"" << TeamName(winner) << "\"";
        WriteHistory("objective_end", extra.str());

        if (_objectiveIndex + 1 < Objectives().size())
        {
            StartObjective(_objectiveIndex + 1);
            return;
        }

        CompleteEvent();
    }

    void CountLivingAtObjective(WarLocation const& objective, uint32& alliance, uint32& horde) const
    {
        for (WarParticipant const& participant : _participants)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(participant.Guid);
            if (!player || !player->IsInWorld() || !player->IsAlive() || player->GetMapId() != objective.Map ||
                player->GetDistance2d(objective.X, objective.Y) > _config.CaptureRadius)
            {
                continue;
            }

            if (participant.Team == TEAM_ALLIANCE)
            {
                ++alliance;
            }
            else if (participant.Team == TEAM_HORDE)
            {
                ++horde;
            }
        }
    }

    bool CommanderAlive(TeamId team) const
    {
        auto commander = std::find_if(_participants.begin(), _participants.end(),
                                      [team](WarParticipant const& participant)
        {
            return participant.Team == team && participant.Commander;
        });
        if (commander == _participants.end())
        {
            return false;
        }

        Player* player = ObjectAccessor::FindConnectedPlayer(commander->Guid);
        return player && player->IsInWorld() && player->IsAlive();
    }

    void CompleteEvent()
    {
        uint32 allianceTotal = _allianceKillScore + _allianceObjectiveScore;
        uint32 hordeTotal = _hordeKillScore + _hordeObjectiveScore;
        if (allianceTotal > hordeTotal)
        {
            _winner = "Alliance";
        }
        else if (hordeTotal > allianceTotal)
        {
            _winner = "Horde";
        }
        else
        {
            _winner = "Draw";
        }

        std::ostringstream extra;
        extra << ",\"alliance_kills\":" << _allianceKillScore
              << ",\"horde_kills\":" << _hordeKillScore
              << ",\"alliance_objectives\":" << _allianceObjectiveScore
              << ",\"horde_objectives\":" << _hordeObjectiveScore
              << ",\"winner\":\"" << _winner << "\"";
        WriteHistory("event_result", extra.str());
        SetState(WarState::Complete);
    }

    void Abort(std::string reason)
    {
        if (_state == WarState::Aborting || _state == WarState::Cleanup || _state == WarState::Finished)
        {
            return;
        }

        _aborted = true;
        _abortReason = std::move(reason);
        _winner = "none";

        LOG_ERROR("living_azeroth.war", "Event {} technical abort: {}.", _eventId, _abortReason);
        std::ostringstream extra;
        extra << ",\"reason\":\"" << JsonEscape(_abortReason) << "\""
              << ",\"winner_declared\":false";
        WriteHistory("technical_abort", extra.str());

        DisengageParticipants();
        SetState(WarState::Aborting);
    }

    void DisengageParticipants()
    {
        for (WarParticipant& participant : _participants)
        {
            if (Player* player = ObjectAccessor::FindConnectedPlayer(participant.Guid))
            {
                player->CombatStop(true);
                player->SetPvP(false);
                player->SetControlled(true, UNIT_STATE_ROOT);
            }
        }
    }

    void BeginCleanup()
    {
        DisengageParticipants();
        _cleanupIndex = 0;
        SetState(WarState::Cleanup);
    }

    void CleanupNextParticipant()
    {
        if (_cleanupIndex >= _participants.size())
        {
            FinalizeEvent();
            return;
        }

        CleanupParticipant(_participants[_cleanupIndex]);
        ++_cleanupIndex;
    }

    void CleanupAllParticipants()
    {
        for (WarParticipant& participant : _participants)
        {
            if (!participant.Restored)
            {
                CleanupParticipant(participant);
            }
        }
    }

    void CleanupParticipant(WarParticipant& participant)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(participant.Guid);
        bool restored = false;

        if (player && player->IsInWorld())
        {
            player->CombatStop(true);
            player->SetPvP(false);
            player->SetControlled(false, UNIT_STATE_ROOT);
            if (player->isDead())
            {
                player->ResurrectPlayer(1.0f, false);
                player->SpawnCorpseBones();
            }

            if (PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(player))
            {
                botAI->ResetStrategies(false);
            }

            sRandomPlayerbotMgr.ScheduleTeleport(participant.Guid.GetCounter(), 60);
            restored = player->TeleportTo(participant.Origin.Map, participant.Origin.X, participant.Origin.Y,
                                          participant.Origin.Z, participant.Origin.O);
            player->SetPvP(participant.OriginalPvP);
        }

        participant.Restored = restored;
        if (restored)
        {
            ++_restoredCount;
        }

        std::ostringstream extra;
        extra << ",\"guid\":" << participant.Guid.GetCounter()
              << ",\"name\":\"" << JsonEscape(participant.Name) << "\""
              << ",\"restored\":" << (restored ? "true" : "false");
        WriteHistory("participant_released", extra.str());
    }

    void FinalizeEvent()
    {
        if (_state == WarState::Finished)
        {
            return;
        }

        std::ostringstream extra;
        extra << ",\"status\":\"" << (_aborted ? "aborted" : "complete") << "\""
              << ",\"winner\":\"" << (_aborted ? "none" : _winner) << "\""
              << ",\"abort_reason\":\"" << JsonEscape(_abortReason) << "\""
              << ",\"registered\":" << _participants.size()
              << ",\"normalized\":" << _normalizedCount
              << ",\"restored\":" << _restoredCount
              << ",\"alliance_kills\":" << _allianceKillScore
              << ",\"horde_kills\":" << _hordeKillScore
              << ",\"alliance_objectives\":" << _allianceObjectiveScore
              << ",\"horde_objectives\":" << _hordeObjectiveScore
              << ",\"teleport_attempts\":" << _teleportAttempts
              << ",\"teleport_failures\":" << _teleportFailures
              << ",\"history_healthy\":" << (_historyHealthy ? "true" : "false");
        WriteHistory("event_end", extra.str());
        SetState(WarState::Finished);

        LOG_INFO("living_azeroth.war",
                 "Event {} finalized: status={}, winner={}, restored={}/{}, teleportFailures={}/{}.",
                 _eventId, _aborted ? "aborted" : "complete", _aborted ? "none" : _winner, _restoredCount,
                 _participants.size(), _teleportFailures, _teleportAttempts);
    }

    void MonitorPerformance(uint32 diff)
    {
        if (_state != WarState::Countdown && _state != WarState::Active)
        {
            _sustainedLatencyMs = 0;
            return;
        }

        if (diff > _config.ExtremeSpikeMs)
        {
            std::ostringstream reason;
            reason << "extreme_world_update_spike_" << diff << "ms";
            Abort(reason.str());
            return;
        }

        if (diff > _config.PerformanceLatencyMs)
        {
            _sustainedLatencyMs += diff;
        }
        else
        {
            _sustainedLatencyMs = 0;
        }

        if (_sustainedLatencyMs >= _config.PerformanceSustainedMs)
        {
            Abort("sustained_world_update_latency");
        }
    }

    WarParticipant* FindParticipant(ObjectGuid guid)
    {
        auto participant = std::find_if(_participants.begin(), _participants.end(),
                                        [guid](WarParticipant const& current)
        {
            return current.Guid == guid;
        });
        return participant == _participants.end() ? nullptr : &*participant;
    }

    bool WriteHistory(std::string const& record, std::string const& extra)
    {
        if (_config.HistoryPath.empty())
        {
            _historyHealthy = false;
            return false;
        }

        std::error_code error;
        std::filesystem::path path(_config.HistoryPath);
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path(), error);
        }

        if (error)
        {
            _historyHealthy = false;
            LOG_ERROR("living_azeroth.war", "Cannot create history directory: {}.", error.message());
            return false;
        }

        std::ofstream output(path, std::ios::app);
        if (!output)
        {
            _historyHealthy = false;
            LOG_ERROR("living_azeroth.war", "Cannot open history path {}.", _config.HistoryPath);
            return false;
        }

        output << "{\"timestamp_unix\":" << GameTime::GetGameTime().count()
               << ",\"event_id\":\"" << JsonEscape(_eventId) << "\""
               << ",\"record\":\"" << JsonEscape(record) << "\""
               << ",\"state\":\"" << StateName(_state) << "\""
               << extra << "}\n";
        output.flush();

        if (!output)
        {
            _historyHealthy = false;
            LOG_ERROR("living_azeroth.war", "History write failed for {}.", _config.HistoryPath);
            return false;
        }

        return true;
    }

    WarConfig _config;
    WarState _state = WarState::Disabled;
    std::vector<WarParticipant> _participants;
    std::string _eventId;
    std::string _abortReason;
    std::string _winner = "none";
    std::size_t _prepareIndex = 0;
    std::size_t _cleanupIndex = 0;
    uint32 _objectiveIndex = 0;
    uint32 _allianceKillScore = 0;
    uint32 _hordeKillScore = 0;
    uint32 _allianceObjectiveScore = 0;
    uint32 _hordeObjectiveScore = 0;
    uint32 _teleportAttempts = 0;
    uint32 _teleportFailures = 0;
    uint32 _normalizedCount = 0;
    uint32 _restoredCount = 0;
    uint32 _sustainedLatencyMs = 0;
    uint32 _stateElapsedMs = 0;
    uint32 _actionElapsedMs = 0;
    bool _aborted = false;
    bool _historyHealthy = true;
};

class LivingAzerothWarWorldScript : public WorldScript
{
public:
    LivingAzerothWarWorldScript()
        : WorldScript("LivingAzerothWarWorldScript",
                      {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
                       WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN})
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
        {
            LivingAzerothWarController::Instance().LoadConfig(true);
        }
    }

    void OnBeforeWorldInitialized() override
    {
        LivingAzerothWarController::Instance().LoadConfig(false);
    }

    void OnStartup() override
    {
        LivingAzerothWarController::Instance().OnStartup();
    }

    void OnUpdate(uint32 diff) override
    {
        LivingAzerothWarController::Instance().OnUpdate(diff);
    }

    void OnShutdown() override
    {
        LivingAzerothWarController::Instance().OnShutdown();
    }
};

class LivingAzerothWarPlayerScript : public PlayerScript
{
public:
    LivingAzerothWarPlayerScript()
        : PlayerScript("LivingAzerothWarPlayerScript", {PLAYERHOOK_ON_PVP_KILL})
    {
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        LivingAzerothWarController::Instance().OnPvPKill(killer, killed);
    }
};
}

void AddLivingAzerothWarScripts()
{
    new LivingAzerothWarWorldScript();
    new LivingAzerothWarPlayerScript();
}
