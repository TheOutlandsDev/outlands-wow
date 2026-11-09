/*
 * Living Azeroth: persistent ambient profiles, shared guild, and activity telemetry.
 */

#include "Config.h"
#include "GameTime.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldScript.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct ProfileConfig
{
    bool Enabled = false;
    bool SharedGuildEnabled = false;
    std::string GuildName = "Living Azeroth";
    std::string PlayerName = "Shamus";
    std::string HistoryPath = "/run/living-azeroth/logs/population/profile-history.jsonl";
    uint32 GuildCheckIntervalMs = 5000;
    uint32 TelemetryIntervalMs = 30000;
};

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

class LivingAzerothProfileController
{
public:
    static LivingAzerothProfileController& Instance()
    {
        static LivingAzerothProfileController instance;
        return instance;
    }

    void LoadConfig()
    {
        _config.Enabled = sConfigMgr->GetOption<bool>("Population.PersistentProfiles", false);
        _config.SharedGuildEnabled = sConfigMgr->GetOption<bool>("Population.SharedGuild.Enable", false);
        _config.GuildName =
            sConfigMgr->GetOption<std::string>("Population.SharedGuild.Name", "Living Azeroth");
        _config.PlayerName =
            sConfigMgr->GetOption<std::string>("Population.SharedGuild.PlayerName", "Shamus");
        _config.HistoryPath = sConfigMgr->GetOption<std::string>(
            "Population.ProfileHistoryPath", "/run/living-azeroth/logs/population/profile-history.jsonl");
        _config.GuildCheckIntervalMs = std::clamp(
            sConfigMgr->GetOption<uint32>("Population.SharedGuild.CheckIntervalSeconds", 5), uint32(1),
            uint32(60)) *
            IN_MILLISECONDS;
        _config.TelemetryIntervalMs = std::clamp(
            sConfigMgr->GetOption<uint32>("Population.ActivityTelemetryIntervalSeconds", 30), uint32(5),
            uint32(300)) *
            IN_MILLISECONDS;
    }

    void OnStartup()
    {
        _guildElapsedMs = 0;
        _telemetryElapsedMs = 0;
        _npcKills = 0;
        _questsCompleted = 0;
        _levelUps = 0;
        _guildReadyLogged = false;
        WriteHistory("profile_start", "");
    }

    void OnUpdate(uint32 diff)
    {
        if (!_config.Enabled)
            return;

        _guildElapsedMs += diff;
        _telemetryElapsedMs += diff;

        if (_config.SharedGuildEnabled && _guildElapsedMs >= _config.GuildCheckIntervalMs)
        {
            _guildElapsedMs = 0;
            EnsureSharedGuild();
        }

        if (_telemetryElapsedMs >= _config.TelemetryIntervalMs)
        {
            _telemetryElapsedMs = 0;
            WriteActivitySnapshot();
        }
    }

    void OnShutdown()
    {
        if (_config.Enabled)
            WriteActivitySnapshot();
        WriteHistory("profile_end", "");
    }

    void OnPlayerLogin(Player* player)
    {
        if (!_config.Enabled || !_config.SharedGuildEnabled || !player ||
            player->GetName() != _config.PlayerName)
            return;

        Guild* guild = sGuildMgr->GetGuildByName(_config.GuildName);
        if (!guild)
        {
            WriteHistory("player_guild_pending",
                         ",\"player\":\"" + JsonEscape(player->GetName()) + "\"");
            return;
        }

        if (player->GetGuildId() == 0 && !guild->AddMember(player->GetGUID()))
        {
            WriteHistory("player_guild_failure",
                         ",\"player\":\"" + JsonEscape(player->GetName()) +
                             "\",\"reason\":\"add_member_failed\"");
            return;
        }
        if (player->GetGuildId() != guild->GetId())
        {
            WriteHistory("player_guild_failure",
                         ",\"player\":\"" + JsonEscape(player->GetName()) +
                             "\",\"reason\":\"member_of_other_guild\"");
            return;
        }

        if (guild->GetLeaderGUID() != player->GetGUID())
        {
            if (Player* leader = ObjectAccessor::FindConnectedPlayer(guild->GetLeaderGUID()))
                guild->HandleSetLeader(leader->GetSession(), player->GetName());
        }

        std::ostringstream extra;
        extra << ",\"player\":\"" << JsonEscape(player->GetName()) << "\""
              << ",\"guild_id\":" << guild->GetId()
              << ",\"guild_name\":\"" << JsonEscape(guild->GetName()) << "\""
              << ",\"is_leader\":" << (guild->GetLeaderGUID() == player->GetGUID() ? "true" : "false");
        WriteHistory("player_guild_ready", extra.str());
    }

    void OnCreatureKill(Player* player)
    {
        if (IsAmbientBot(player))
            ++_npcKills;
    }

    void OnQuestComplete(Player* player)
    {
        if (IsAmbientBot(player))
            ++_questsCompleted;
    }

    void OnLevelChanged(Player* player, uint8 oldLevel)
    {
        if (IsAmbientBot(player) && player->GetLevel() > oldLevel)
            ++_levelUps;
    }

private:
    bool IsAmbientBot(Player* player) const
    {
        return _config.Enabled && player && sRandomPlayerbotMgr.IsRandomBot(player);
    }

    std::vector<Player*> GetAmbientBots() const
    {
        std::vector<Player*> bots;
        for (auto iterator = sRandomPlayerbotMgr.GetPlayerBotsBegin();
             iterator != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++iterator)
        {
            Player* player = iterator->second;
            if (IsAmbientBot(player) && player->IsInWorld())
                bots.push_back(player);
        }

        std::sort(bots.begin(), bots.end(), [](Player const* left, Player const* right)
        {
            return left->GetGUID().GetCounter() < right->GetGUID().GetCounter();
        });
        return bots;
    }

    void EnsureSharedGuild()
    {
        std::vector<Player*> bots = GetAmbientBots();
        if (bots.empty())
            return;

        Guild* guild = sGuildMgr->GetGuildByName(_config.GuildName);
        if (!guild)
        {
            Player* leader = nullptr;
            for (Player* bot : bots)
            {
                if (bot->GetGuildId() == 0)
                {
                    leader = bot;
                    break;
                }
            }
            if (!leader)
            {
                WriteHistory("guild_failure", ",\"reason\":\"no_unguilded_leader\"");
                return;
            }

            guild = new Guild();
            if (!guild->Create(leader, _config.GuildName))
            {
                delete guild;
                WriteHistory("guild_failure", ",\"reason\":\"create_failed\"");
                return;
            }
            sGuildMgr->AddGuild(guild);
        }

        uint32 added = 0;
        uint32 ready = 0;
        uint32 otherGuild = 0;
        for (Player* bot : bots)
        {
            if (bot->GetGuildId() == guild->GetId())
            {
                ++ready;
                continue;
            }
            if (bot->GetGuildId() != 0)
            {
                if (Guild* previousGuild = sGuildMgr->GetGuildById(bot->GetGuildId()))
                    previousGuild->DeleteMember(bot->GetGUID(), false, true, false);

                if (bot->GetGuildId() != 0)
                {
                    ++otherGuild;
                    continue;
                }
            }
            if (guild->AddMember(bot->GetGUID()))
            {
                ++added;
                ++ready;
            }
        }

        std::ostringstream extra;
        extra << ",\"guild_id\":" << guild->GetId()
              << ",\"guild_name\":\"" << JsonEscape(guild->GetName()) << "\""
              << ",\"online_bots\":" << bots.size()
              << ",\"members_ready\":" << ready
              << ",\"members_added\":" << added
              << ",\"other_guild\":" << otherGuild
              << ",\"guild_members\":" << guild->GetMemberCount();

        if (otherGuild > 0)
            WriteHistory("guild_failure", extra.str() + ",\"reason\":\"bot_in_other_guild\"");
        else if (!_guildReadyLogged || added > 0)
        {
            WriteHistory("guild_ready", extra.str());
            _guildReadyLogged = true;
        }
    }

    void WriteActivitySnapshot()
    {
        if (!_config.Enabled)
            return;

        std::vector<Player*> bots = GetAmbientBots();
        uint32 inCombat = 0;
        uint32 withActiveQuests = 0;
        uint32 sharedGuildMembers = 0;
        Guild* guild = _config.SharedGuildEnabled ? sGuildMgr->GetGuildByName(_config.GuildName) : nullptr;

        for (Player* bot : bots)
        {
            if (bot->IsInCombat())
                ++inCombat;

            bool hasQuest = false;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                if (bot->GetQuestSlotQuestId(slot) != 0)
                {
                    hasQuest = true;
                    break;
                }
            }
            if (hasQuest)
                ++withActiveQuests;
            if (guild && bot->GetGuildId() == guild->GetId())
                ++sharedGuildMembers;
        }

        std::ostringstream extra;
        extra << ",\"online_bots\":" << bots.size()
              << ",\"bots_in_combat\":" << inCombat
              << ",\"bots_with_active_quests\":" << withActiveQuests
              << ",\"npc_kills\":" << _npcKills
              << ",\"quests_completed\":" << _questsCompleted
              << ",\"level_ups\":" << _levelUps
              << ",\"shared_guild_members\":" << sharedGuildMembers
              << ",\"shared_guild_ready\":"
              << (guild && sharedGuildMembers == bots.size() && !bots.empty() ? "true" : "false");
        WriteHistory("activity_snapshot", extra.str());
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
               << ",\"record\":\"" << JsonEscape(record) << "\""
               << extra << "}\n";
        output.flush();
        return bool(output);
    }

    ProfileConfig _config;
    uint32 _guildElapsedMs = 0;
    uint32 _telemetryElapsedMs = 0;
    uint64 _npcKills = 0;
    uint64 _questsCompleted = 0;
    uint64 _levelUps = 0;
    bool _guildReadyLogged = false;
};

class LivingAzerothProfileWorldScript : public WorldScript
{
public:
    LivingAzerothProfileWorldScript()
        : WorldScript("LivingAzerothProfileWorldScript",
                      {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
                       WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN})
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            LivingAzerothProfileController::Instance().LoadConfig();
    }

    void OnBeforeWorldInitialized() override
    {
        LivingAzerothProfileController::Instance().LoadConfig();
    }

    void OnStartup() override
    {
        LivingAzerothProfileController::Instance().OnStartup();
    }

    void OnUpdate(uint32 diff) override
    {
        LivingAzerothProfileController::Instance().OnUpdate(diff);
    }

    void OnShutdown() override
    {
        LivingAzerothProfileController::Instance().OnShutdown();
    }
};

class LivingAzerothProfilePlayerScript : public PlayerScript
{
public:
    LivingAzerothProfilePlayerScript()
        : PlayerScript("LivingAzerothProfilePlayerScript",
                       {PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_CREATURE_KILL,
                        PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_LEVEL_CHANGED})
    {
    }

    void OnPlayerLogin(Player* player) override
    {
        LivingAzerothProfileController::Instance().OnPlayerLogin(player);
    }

    void OnPlayerCreatureKill(Player* killer, Creature*) override
    {
        LivingAzerothProfileController::Instance().OnCreatureKill(killer);
    }

    void OnPlayerCompleteQuest(Player* player, Quest const*) override
    {
        LivingAzerothProfileController::Instance().OnQuestComplete(player);
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        LivingAzerothProfileController::Instance().OnLevelChanged(player, oldLevel);
    }
};
}

void AddLivingAzerothProfileScripts()
{
    new LivingAzerothProfileWorldScript();
    new LivingAzerothProfilePlayerScript();
}
