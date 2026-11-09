/*
 * Living Azeroth: bounded ambient population prototype.
 *
 * This component assigns an exact, independent random-Playerbot roster to
 * configured starter-zone anchors. It does not add bots to human groups,
 * enable starter-zone PvP, control quests, or replace Playerbots behavior.
 */

#include "Config.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldScript.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr uint32 POPULATION_ANCHOR_COUNT = 8;
constexpr uint32 POPULATION_CAPITAL_ANCHOR_COUNT = 4;
constexpr uint32 POPULATION_MAX_PER_FACTION = 32;

enum class PopulationState
{
    Disabled,
    WaitingForRoster,
    Assigning,
    Active,
    Failed,
    Finished
};

struct PopulationLocation
{
    std::string Name;
    uint32 Map = 0;
    uint32 Zone = 0;
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float O = 0.0f;
};

struct PopulationConfig
{
    bool Enabled = false;
    bool AutoStart = false;
    bool RestoreOnShutdown = true;
    uint32 TargetPerFaction = 10;
    uint32 CapitalSlotsPerFaction = 0;
    uint32 MinLevel = 5;
    uint32 MaxLevel = 12;
    uint32 RosterTimeoutMs = 300000;
    uint32 AssignmentIntervalMs = 500;
    uint32 VerifyIntervalMs = 30000;
    uint32 AssignmentHoldSeconds = 7200;
    std::string HistoryPath;
    std::array<PopulationLocation, POPULATION_ANCHOR_COUNT> AllianceAnchors;
    std::array<PopulationLocation, POPULATION_ANCHOR_COUNT> HordeAnchors;
};

struct PopulationAssignment
{
    ObjectGuid Guid;
    TeamId Team = TEAM_NEUTRAL;
    std::string Name;
    PopulationLocation Origin;
    bool OriginalPvP = false;
    uint8 OriginalLevel = 0;
    uint8 TargetLevel = 0;
    bool ProfileInitialized = false;
    uint32 OverlevelEquipmentRemoved = 0;
    uint32 PostInitInvalidEquipment = 0;
    uint32 AnchorIndex = 0;
    bool Assigned = false;
    bool Restored = false;
};

char const* StateName(PopulationState state)
{
    switch (state)
    {
        case PopulationState::Disabled:
            return "DISABLED";
        case PopulationState::WaitingForRoster:
            return "WAITING_FOR_ROSTER";
        case PopulationState::Assigning:
            return "ASSIGNING";
        case PopulationState::Active:
            return "ACTIVE";
        case PopulationState::Failed:
            return "FAILED";
        case PopulationState::Finished:
            return "FINISHED";
    }

    return "UNKNOWN";
}

char const* TeamName(TeamId team)
{
    if (team == TEAM_ALLIANCE)
        return "Alliance";
    if (team == TEAM_HORDE)
        return "Horde";
    return "Neutral";
}

std::string JsonEscape(std::string const& input)
{
    std::ostringstream escaped;
    for (char character : input)
    {
        switch (character)
        {
            case '\\':
                escaped << "\\\\";
                break;
            case '"':
                escaped << "\\\"";
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
                escaped << character;
                break;
        }
    }

    return escaped.str();
}

class LivingAzerothPopulationController
{
public:
    static LivingAzerothPopulationController& Instance()
    {
        static LivingAzerothPopulationController instance;
        return instance;
    }

    void LoadConfig(bool reload)
    {
        bool wasEnabled = _config.Enabled;

        _config.Enabled = sConfigMgr->GetOption<bool>("Population.Enable", false);
        _config.AutoStart = sConfigMgr->GetOption<bool>("Population.AutoStart", false);
        _config.RestoreOnShutdown = sConfigMgr->GetOption<bool>("Population.RestoreOnShutdown", true);
        _config.TargetPerFaction = std::clamp(
            sConfigMgr->GetOption<uint32>("Population.TargetPerFaction", 10), uint32(1),
            POPULATION_MAX_PER_FACTION);
        _config.CapitalSlotsPerFaction = std::clamp(
            sConfigMgr->GetOption<uint32>("Population.CapitalSlotsPerFaction", 0), uint32(0),
            _config.TargetPerFaction);
        _config.MinLevel =
            std::clamp(sConfigMgr->GetOption<uint32>("Population.MinLevel", 5), uint32(1), uint32(80));
        _config.MaxLevel =
            std::clamp(sConfigMgr->GetOption<uint32>("Population.MaxLevel", 12), uint32(1), uint32(80));
        _config.RosterTimeoutMs =
            std::clamp(sConfigMgr->GetOption<uint32>("Population.RosterTimeoutSeconds", 300), uint32(30),
                       uint32(1800)) *
            IN_MILLISECONDS;
        _config.AssignmentIntervalMs = std::clamp(
            sConfigMgr->GetOption<uint32>("Population.AssignmentIntervalMs", 500), uint32(100), uint32(5000));
        _config.VerifyIntervalMs =
            std::clamp(sConfigMgr->GetOption<uint32>("Population.VerifyIntervalSeconds", 30), uint32(5),
                       uint32(300)) *
            IN_MILLISECONDS;
        _config.AssignmentHoldSeconds = std::clamp(
            sConfigMgr->GetOption<uint32>("Population.AssignmentHoldSeconds", 7200), uint32(300), uint32(86400));
        _config.HistoryPath = sConfigMgr->GetOption<std::string>(
            "Population.HistoryPath", "/run/living-azeroth/logs/population/ambient-history.jsonl");

        _config.AllianceAnchors = {
            LoadLocation("StormwindAuctionNorth", "Population.Alliance.Anchor1", 0, 1519,
                         -8821.53f, 659.886f, 97.4645f, 0.401426f),
            LoadLocation("StormwindAuctionSouth", "Population.Alliance.Anchor2", 0, 1519,
                         -8814.55f, 660.315f, 96.6818f, 1.72788f),
            LoadLocation("IronforgeAuctionEast", "Population.Alliance.Anchor3", 0, 1537,
                         -4948.01f, -901.528f, 505.172f, 3.80482f),
            LoadLocation("IronforgeAuctionWest", "Population.Alliance.Anchor4", 0, 1537,
                         -4967.86f, -917.619f, 505.169f, 0.785398f),
            LoadLocation("NorthshireRoad", "Population.Alliance.Anchor5", 0, 12,
                         -8914.7f, -133.6f, 80.6f, 1.95f),
            LoadLocation("Goldshire", "Population.Alliance.Anchor6", 0, 12,
                         -9465.5f, 74.0f, 56.8f, 4.59f),
            LoadLocation("Eastvale", "Population.Alliance.Anchor7", 0, 12,
                         -9469.8f, -1289.2f, 41.2f, 3.25f),
            LoadLocation("ElwynnEastRoad", "Population.Alliance.Anchor8", 0, 12,
                         -9610.2f, -1032.1f, 41.3f, 3.14f)
        };
        _config.HordeAnchors = {
            LoadLocation("OrgrimmarAuctionSouth", "Population.Horde.Anchor1", 1, 1637,
                         1667.62f, -4463.76f, 20.3911f, 1.71042f),
            LoadLocation("OrgrimmarAuctionNorth", "Population.Horde.Anchor2", 1, 1637,
                         1695.92f, -4455.55f, 20.3911f, 2.00713f),
            LoadLocation("UndercityAuctionEast", "Population.Horde.Anchor3", 0, 1497,
                         1542.45f, 255.202f, -56.7948f, 1.01229f),
            LoadLocation("UndercityAuctionWest", "Population.Horde.Anchor4", 0, 1497,
                         1542.4f, 225.162f, -56.7902f, 5.23599f),
            LoadLocation("ValleyOfTrials", "Population.Horde.Anchor5", 1, 14,
                         -629.1f, -4228.1f, 38.2f, 5.01f),
            LoadLocation("RazorHill", "Population.Horde.Anchor6", 1, 14,
                         340.4f, -4686.3f, 16.5f, 4.19f),
            LoadLocation("SenjinVillage", "Population.Horde.Anchor7", 1, 14,
                         -825.6f, -4920.8f, 19.7f, 0.26f),
            LoadLocation("DurotarEastRoad", "Population.Horde.Anchor8", 1, 14,
                         1102.1f, -4945.4f, 15.7f, 2.36f)
        };

        if (_config.MinLevel > _config.MaxLevel)
        {
            LOG_ERROR("living_azeroth.population",
                      "Population.MinLevel must be less than or equal to Population.MaxLevel; disabling.");
            _config.Enabled = false;
        }

        if (_config.Enabled && sConfigMgr->GetOption<bool>("War.Enable", false))
        {
            LOG_ERROR("living_azeroth.population",
                      "Population and War cannot be enabled together in this prototype; disabling population.");
            _config.Enabled = false;
        }

        LOG_INFO("living_azeroth.population",
                 "Ambient population configuration loaded: enabled={}, autoStart={}, targetPerFaction={}, "
                 "capitalSlotsPerFaction={}, levelRange={}-{}, restoreOnShutdown={}.",
                 _config.Enabled, _config.AutoStart, _config.TargetPerFaction,
                 _config.CapitalSlotsPerFaction, _config.MinLevel, _config.MaxLevel,
                 _config.RestoreOnShutdown);

        if (reload && wasEnabled && !_config.Enabled && IsRunning())
        {
            RestoreAll("configuration_disabled");
            SetState(PopulationState::Finished);
        }
        else if (reload && _config.Enabled && _config.AutoStart && !IsRunning())
            Start();
    }

    void OnStartup()
    {
        if (_config.Enabled && _config.AutoStart)
            Start();
    }

    void OnUpdate(uint32 diff)
    {
        if (!IsRunning())
            return;

        _stateElapsedMs += diff;
        _actionElapsedMs += diff;

        switch (_state)
        {
            case PopulationState::WaitingForRoster:
                if (_actionElapsedMs >= IN_MILLISECONDS)
                {
                    _actionElapsedMs = 0;
                    TryRegisterRoster();
                }
                if (_state == PopulationState::WaitingForRoster && _stateElapsedMs >= _config.RosterTimeoutMs)
                    Fail("roster_timeout");
                break;
            case PopulationState::Assigning:
                if (_actionElapsedMs >= _config.AssignmentIntervalMs)
                {
                    _actionElapsedMs = 0;
                    AssignNext();
                }
                break;
            case PopulationState::Active:
                if (_actionElapsedMs >= _config.VerifyIntervalMs)
                {
                    _actionElapsedMs = 0;
                    VerifyAssignments();
                }
                break;
            case PopulationState::Disabled:
            case PopulationState::Failed:
            case PopulationState::Finished:
                break;
        }
    }

    void OnShutdown()
    {
        if (!IsRunning())
            return;

        if (_config.RestoreOnShutdown)
            RestoreAll("world_shutdown");

        WriteHistory("population_end", ",\"reason\":\"world_shutdown\"");
        SetState(PopulationState::Finished);
    }

private:
    LivingAzerothPopulationController() = default;

    PopulationLocation LoadLocation(std::string name, std::string const& prefix, uint32 map, uint32 zone, float x,
                                    float y, float z, float orientation)
    {
        PopulationLocation location;
        location.Name = std::move(name);
        location.Map = sConfigMgr->GetOption<uint32>(prefix + ".Map", map);
        location.Zone = sConfigMgr->GetOption<uint32>(prefix + ".Zone", zone);
        location.X = sConfigMgr->GetOption<float>(prefix + ".X", x);
        location.Y = sConfigMgr->GetOption<float>(prefix + ".Y", y);
        location.Z = sConfigMgr->GetOption<float>(prefix + ".Z", z);
        location.O = sConfigMgr->GetOption<float>(prefix + ".O", orientation);
        return location;
    }

    bool IsRunning() const
    {
        return _state == PopulationState::WaitingForRoster || _state == PopulationState::Assigning ||
               _state == PopulationState::Active;
    }

    uint32 CountOverlevelEquipment(Player* player, uint8 maximumLevel) const
    {
        uint32 count = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            ItemTemplate const* itemTemplate = item->GetTemplate();
            if (itemTemplate && itemTemplate->RequiredLevel > maximumLevel)
                ++count;
        }

        return count;
    }

    uint32 RemoveOverlevelEquipment(Player* player, uint8 maximumLevel) const
    {
        uint32 removed = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            ItemTemplate const* itemTemplate = item->GetTemplate();
            if (!itemTemplate || itemTemplate->RequiredLevel <= maximumLevel)
                continue;

            player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
            ++removed;
        }

        return removed;
    }

    void Start()
    {
        if (IsRunning())
        {
            LOG_WARN("living_azeroth.population", "Start ignored because the population gate is already running.");
            return;
        }

        _assignments.clear();
        _assignmentIndex = 0;
        _teleportAttempts = 0;
        _teleportFailures = 0;
        _stateElapsedMs = 0;
        _actionElapsedMs = 0;
        _runId = std::to_string(GameTime::GetGameTime().count());

        std::ostringstream extra;
        extra << ",\"target_per_faction\":" << _config.TargetPerFaction
              << ",\"min_level\":" << _config.MinLevel
              << ",\"max_level\":" << _config.MaxLevel
              << ",\"party_bots\":false,\"starter_pvp\":false";
        WriteHistory("population_start", extra.str());
        SetState(PopulationState::WaitingForRoster);
    }

    void SetState(PopulationState state)
    {
        PopulationState previous = _state;
        _state = state;
        _stateElapsedMs = 0;
        _actionElapsedMs = 0;

        LOG_INFO("living_azeroth.population", "Population run {} state {} -> {}.", _runId, StateName(previous),
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
                !sRandomPlayerbotMgr.IsRandomBot(player) || player->GetGroup() || player->IsInCombat() ||
                player->getClass() == CLASS_DEATH_KNIGHT)
            {
                continue;
            }

            if (player->GetTeamId() == TEAM_ALLIANCE)
                alliance.push_back(player);
            else if (player->GetTeamId() == TEAM_HORDE)
                horde.push_back(player);
        }

        SortPlayers(alliance);
        SortPlayers(horde);

        if (alliance.size() < _config.TargetPerFaction || horde.size() < _config.TargetPerFaction)
        {
            LOG_INFO("living_azeroth.population",
                     "Waiting for independent ambient roster: Alliance={}, Horde={}, required={} each.",
                     alliance.size(), horde.size(), _config.TargetPerFaction);
            return;
        }

        RegisterFaction(alliance, TEAM_ALLIANCE);
        RegisterFaction(horde, TEAM_HORDE);

        uint32 expected = _config.TargetPerFaction * 2;
        if (_assignments.size() != expected)
        {
            Fail("roster_invariant_failure");
            return;
        }

        std::ostringstream extra;
        extra << ",\"alliance\":" << _config.TargetPerFaction
              << ",\"horde\":" << _config.TargetPerFaction
              << ",\"total\":" << expected;
        WriteHistory("roster_registered", extra.str());
        SetState(PopulationState::Assigning);
    }

    void SortPlayers(std::vector<Player*>& players)
    {
        std::sort(players.begin(), players.end(), [](Player const* left, Player const* right)
        {
            return left->GetGUID().GetCounter() < right->GetGUID().GetCounter();
        });
    }

    void RegisterFaction(std::vector<Player*> const& candidates, TeamId team)
    {
        for (uint32 slot = 0; slot < _config.TargetPerFaction; ++slot)
        {
            Player* player = candidates[slot];
            PopulationAssignment assignment;
            assignment.Guid = player->GetGUID();
            assignment.Team = team;
            assignment.Name = player->GetName();
            assignment.Origin.Name = "Original";
            assignment.Origin.Map = player->GetMapId();
            assignment.Origin.Zone = player->GetZoneId();
            assignment.Origin.X = player->GetPositionX();
            assignment.Origin.Y = player->GetPositionY();
            assignment.Origin.Z = player->GetPositionZ();
            assignment.Origin.O = player->GetOrientation();
            assignment.OriginalPvP = player->IsPvP();
            assignment.OriginalLevel = player->GetLevel();
            assignment.ProfileInitialized =
                sRandomPlayerbotMgr.GetPersistentEventValue(assignment.Guid.GetCounter(), "living_azeroth_profile") != 0;
            assignment.TargetLevel = assignment.ProfileInitialized
                                         ? assignment.OriginalLevel
                                         : uint8(_config.MinLevel + (slot % (_config.MaxLevel - _config.MinLevel + 1)));
            assignment.AnchorIndex = slot < _config.CapitalSlotsPerFaction
                ? slot % POPULATION_CAPITAL_ANCHOR_COUNT
                : POPULATION_CAPITAL_ANCHOR_COUNT +
                    ((slot - _config.CapitalSlotsPerFaction) %
                     (POPULATION_ANCHOR_COUNT - POPULATION_CAPITAL_ANCHOR_COUNT));
            _assignments.push_back(std::move(assignment));
        }
    }

    void AssignNext()
    {
        if (_assignmentIndex >= _assignments.size())
        {
            VerifyAssignments();
            SetState(PopulationState::Active);
            return;
        }

        PopulationAssignment& assignment = _assignments[_assignmentIndex];
        Player* player = ObjectAccessor::FindConnectedPlayer(assignment.Guid);
        if (!player || !player->IsInWorld() || player->IsBeingTeleported() || player->GetGroup())
        {
            Fail("registered_bot_unavailable_during_assignment");
            return;
        }

        player->CombatStop(true);
        player->SetPvP(false);
        if (player->isDead())
        {
            player->ResurrectPlayer(1.0f, false);
            player->SpawnCorpseBones();
        }

        if (!assignment.ProfileInitialized)
        {
            assignment.OverlevelEquipmentRemoved =
                RemoveOverlevelEquipment(player, assignment.TargetLevel);
            PlayerbotFactory factory(player, assignment.TargetLevel);
            factory.Randomize(false);
            factory.InitEquipment(true, false);
            player->SaveToDB(false, false);
            assignment.PostInitInvalidEquipment =
                CountOverlevelEquipment(player, assignment.TargetLevel);
            if (assignment.PostInitInvalidEquipment != 0)
            {
                Fail("profile_initialization_overlevel_equipment");
                return;
            }
        }
        sRandomPlayerbotMgr.SetPersistentEventValue(
            assignment.Guid.GetCounter(), "living_azeroth_profile", 1, 315360000);

        PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(player);
        if (botAI)
            botAI->ResetStrategies(false);

        sRandomPlayerbotMgr.ScheduleTeleport(assignment.Guid.GetCounter(), _config.AssignmentHoldSeconds);

        std::array<PopulationLocation, POPULATION_ANCHOR_COUNT> const& anchors =
            assignment.Team == TEAM_ALLIANCE ? _config.AllianceAnchors : _config.HordeAnchors;
        PopulationLocation const& location = anchors[assignment.AnchorIndex];

        uint32 factionSlot = uint32(_assignmentIndex);
        if (assignment.Team == TEAM_HORDE)
            factionSlot -= _config.TargetPerFaction;
        float rowOffset = float(factionSlot / POPULATION_ANCHOR_COUNT) * 4.0f;
        float sideOffset = (factionSlot % 2 == 0 ? -3.0f : 3.0f);

        ++_teleportAttempts;
        bool teleported = player->TeleportTo(location.Map, location.X + rowOffset, location.Y + sideOffset,
                                             location.Z, location.O);
        if (!teleported)
        {
            ++_teleportFailures;
            Fail("assignment_teleport_failed");
            return;
        }

        if (player->IsBeingTeleported())
        {
            if (!botAI)
            {
                ++_teleportFailures;
                Fail("assignment_teleport_ack_ai_missing");
                return;
            }
            botAI->HandleTeleportAck();
        }
        if (player->IsBeingTeleported() || !player->IsInWorld())
        {
            ++_teleportFailures;
            Fail("assignment_teleport_ack_failed");
            return;
        }

        assignment.Assigned = true;
        std::ostringstream extra;
        extra << ",\"guid\":" << assignment.Guid.GetCounter()
              << ",\"name\":\"" << JsonEscape(assignment.Name) << "\""
              << ",\"faction\":\"" << TeamName(assignment.Team) << "\""
              << ",\"anchor\":\"" << JsonEscape(location.Name) << "\""
              << ",\"zone\":" << location.Zone
              << ",\"original_level\":" << uint32(assignment.OriginalLevel)
              << ",\"target_level\":" << uint32(assignment.TargetLevel)
              << ",\"profile_preexisting\":"
              << (assignment.ProfileInitialized ? "true" : "false")
              << ",\"overlevel_equipment_removed\":" << assignment.OverlevelEquipmentRemoved
              << ",\"post_init_invalid_equipment\":" << assignment.PostInitInvalidEquipment
              << ",\"grouped\":false,\"pvp\":false"
              << ",\"teleport_complete\":true";
        WriteHistory("bot_assigned", extra.str());

        ++_assignmentIndex;
    }

    void VerifyAssignments()
    {
        uint32 allianceOnline = 0;
        uint32 hordeOnline = 0;
        uint32 allianceInZone = 0;
        uint32 hordeInZone = 0;
        uint32 allianceCapitalOnline = 0;
        uint32 allianceCapitalInZone = 0;
        uint32 hordeCapitalOnline = 0;
        uint32 hordeCapitalInZone = 0;
        uint32 grouped = 0;
        // Earned levels are persistent profile progress, not drift. Only a level
        // below the assignment baseline is a regression.
        uint32 levelBelowTarget = 0;
        uint32 levelProgressed = 0;

        for (PopulationAssignment const& assignment : _assignments)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(assignment.Guid);
            if (!player || !player->IsInWorld())
                continue;

            if (player->GetGroup())
                ++grouped;
            if (player->GetLevel() < assignment.TargetLevel)
                ++levelBelowTarget;
            else if (player->GetLevel() > assignment.TargetLevel)
                ++levelProgressed;

            std::array<PopulationLocation, POPULATION_ANCHOR_COUNT> const& anchors =
                assignment.Team == TEAM_ALLIANCE ? _config.AllianceAnchors : _config.HordeAnchors;
            uint32 targetZone = anchors[assignment.AnchorIndex].Zone;

            if (assignment.Team == TEAM_ALLIANCE)
            {
                ++allianceOnline;
                if (player->GetZoneId() == targetZone)
                    ++allianceInZone;
                if (assignment.AnchorIndex < POPULATION_CAPITAL_ANCHOR_COUNT)
                {
                    ++allianceCapitalOnline;
                    if (player->GetZoneId() == targetZone)
                        ++allianceCapitalInZone;
                }
            }
            else if (assignment.Team == TEAM_HORDE)
            {
                ++hordeOnline;
                if (player->GetZoneId() == targetZone)
                    ++hordeInZone;
                if (assignment.AnchorIndex < POPULATION_CAPITAL_ANCHOR_COUNT)
                {
                    ++hordeCapitalOnline;
                    if (player->GetZoneId() == targetZone)
                        ++hordeCapitalInZone;
                }
            }
        }

        std::ostringstream extra;
        extra << ",\"alliance_online\":" << allianceOnline
              << ",\"alliance_in_zone\":" << allianceInZone
              << ",\"alliance_capital_online\":" << allianceCapitalOnline
              << ",\"alliance_capital_in_zone\":" << allianceCapitalInZone
              << ",\"horde_online\":" << hordeOnline
              << ",\"horde_in_zone\":" << hordeInZone
              << ",\"horde_capital_online\":" << hordeCapitalOnline
              << ",\"horde_capital_in_zone\":" << hordeCapitalInZone
              << ",\"grouped\":" << grouped
              << ",\"level_mismatch\":" << levelBelowTarget
              << ",\"level_below_target\":" << levelBelowTarget
              << ",\"level_progressed\":" << levelProgressed
              << ",\"teleport_attempts\":" << _teleportAttempts
              << ",\"teleport_failures\":" << _teleportFailures;
        WriteHistory("population_snapshot", extra.str());

        LOG_INFO("living_azeroth.population",
                 "Ambient snapshot: Alliance online/in-zone={}/{}, Horde online/in-zone={}/{}, grouped={}, "
                 "levelBelowTarget={}, levelProgressed={}.",
                 allianceOnline, allianceInZone, hordeOnline, hordeInZone, grouped, levelBelowTarget,
                 levelProgressed);
    }

    void RestoreAll(std::string const& reason)
    {
        uint32 restored = 0;
        for (PopulationAssignment& assignment : _assignments)
        {
            if (!assignment.Assigned || assignment.Restored)
                continue;

            Player* player = ObjectAccessor::FindConnectedPlayer(assignment.Guid);
            bool positionRestored = false;
            if (player && player->IsInWorld())
            {
                player->CombatStop(true);
                player->SetPvP(false);
                if (PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(player))
                    botAI->ResetStrategies(false);

                sRandomPlayerbotMgr.ScheduleTeleport(assignment.Guid.GetCounter(), 60);
                positionRestored = player->TeleportTo(assignment.Origin.Map, assignment.Origin.X, assignment.Origin.Y,
                                                      assignment.Origin.Z, assignment.Origin.O);
                player->SetPvP(assignment.OriginalPvP);
            }

            assignment.Restored = positionRestored;
            if (positionRestored)
                ++restored;

            std::ostringstream extra;
            extra << ",\"guid\":" << assignment.Guid.GetCounter()
                  << ",\"name\":\"" << JsonEscape(assignment.Name) << "\""
                  << ",\"position_restored\":" << (positionRestored ? "true" : "false")
                  << ",\"reason\":\"" << JsonEscape(reason) << "\"";
            WriteHistory("bot_released", extra.str());
        }

        std::ostringstream extra;
        extra << ",\"restored\":" << restored
              << ",\"registered\":" << _assignments.size()
              << ",\"levels_restored\":false"
              << ",\"reason\":\"" << JsonEscape(reason) << "\"";
        WriteHistory("population_restored", extra.str());
    }

    void Fail(std::string const& reason)
    {
        LOG_ERROR("living_azeroth.population", "Ambient population run {} failed: {}.", _runId, reason);
        std::ostringstream extra;
        extra << ",\"reason\":\"" << JsonEscape(reason) << "\"";
        WriteHistory("population_failure", extra.str());
        if (_config.RestoreOnShutdown)
            RestoreAll(reason);
        SetState(PopulationState::Failed);
    }

    bool WriteHistory(std::string const& record, std::string const& extra)
    {
        std::filesystem::path path(_config.HistoryPath);
        std::error_code error;
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            LOG_ERROR("living_azeroth.population", "Cannot create population history directory: {}.",
                      error.message());
            return false;
        }

        std::ofstream output(path, std::ios::app);
        if (!output)
        {
            LOG_ERROR("living_azeroth.population", "Cannot open population history path {}.", _config.HistoryPath);
            return false;
        }

        output << "{\"timestamp_unix\":" << GameTime::GetGameTime().count()
               << ",\"run_id\":\"" << JsonEscape(_runId) << "\""
               << ",\"record\":\"" << JsonEscape(record) << "\""
               << ",\"state\":\"" << StateName(_state) << "\""
               << extra << "}\n";
        output.flush();
        return bool(output);
    }

    PopulationConfig _config;
    PopulationState _state = PopulationState::Disabled;
    std::vector<PopulationAssignment> _assignments;
    std::string _runId;
    std::size_t _assignmentIndex = 0;
    uint32 _teleportAttempts = 0;
    uint32 _teleportFailures = 0;
    uint32 _stateElapsedMs = 0;
    uint32 _actionElapsedMs = 0;
};

class LivingAzerothPopulationWorldScript : public WorldScript
{
public:
    LivingAzerothPopulationWorldScript()
        : WorldScript("LivingAzerothPopulationWorldScript",
                      {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
                       WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN})
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            LivingAzerothPopulationController::Instance().LoadConfig(true);
    }

    void OnBeforeWorldInitialized() override
    {
        LivingAzerothPopulationController::Instance().LoadConfig(false);
    }

    void OnStartup() override
    {
        LivingAzerothPopulationController::Instance().OnStartup();
    }

    void OnUpdate(uint32 diff) override
    {
        LivingAzerothPopulationController::Instance().OnUpdate(diff);
    }

    void OnShutdown() override
    {
        LivingAzerothPopulationController::Instance().OnShutdown();
    }
};
}

void AddLivingAzerothPopulationScripts()
{
    new LivingAzerothPopulationWorldScript();
}
