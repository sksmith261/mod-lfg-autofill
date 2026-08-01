/*
 * mod-lfg-autofill
 *
 * Fills the gaps in a party with playerbots before a Dungeon Finder queue goes out:
 * the player says how many bodies they want, and the module works out which roles are
 * missing and recruits online bots to cover them.
 *
 * The 3.3.5a Dungeon Finder pane is client-side FrameXML and cannot be extended from the
 * server, so the party size arrives over a chat command (.lfgfill) instead of a UI control.
 * The role half needs no new input at all — the pane already sends the player's role
 * checkboxes with the join packet, and that is what OnPlayerCanJoinLfg hands us.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Configuration/Config.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "LFG.h"
#include "LFGMgr.h"

#include "AiFactory.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr uint8 kMinPartySize = 2;
    constexpr uint8 kMaxPartySize = 5;

    enum class FillRole : uint8
    {
        Tank,
        Healer,
        Damage
    };

    // Per-player target party size. Deliberately in-memory only: this is a "for the next
    // run" preference, not a character setting, and a value that quietly outlives a session
    // would surprise people more than it helps. Cleared on logout.
    std::unordered_map<ObjectGuid, uint8> s_desiredSize;

    // A queue join that we cancelled so we could fill the party first, waiting to be
    // re-issued on the next world tick. See OnPlayerCanJoinLfg for why it cannot be done
    // inline.
    struct PendingJoin
    {
        ObjectGuid            playerGuid;
        uint8                 roles;
        lfg::LfgDungeonSet    dungeons;
        std::string           comment;
    };

    std::vector<PendingJoin> s_pending;

    // Guards the re-issued JoinLfg call against being cancelled by our own hook again.
    std::unordered_set<ObjectGuid> s_reissuing;

    bool ModuleEnabled()
    {
        return sConfigMgr->GetOption<bool>("LfgAutofill.Enable", true);
    }

    uint8 DefaultPartySize()
    {
        uint32 v = sConfigMgr->GetOption<uint32>("LfgAutofill.DefaultPartySize", 0);
        if (v == 0)
            return 0;
        return static_cast<uint8>(std::clamp<uint32>(v, kMinPartySize, kMaxPartySize));
    }

    // Bots are only ever taken at or above the queueing player's level. A bot below it is
    // dead weight the player has to carry; one a few levels above pulls its own weight in
    // a dungeon whose level range the player already qualifies for.
    uint32 LevelsAbove()
    {
        return sConfigMgr->GetOption<uint32>("LfgAutofill.LevelsAbove", 3);
    }

    bool Announce()
    {
        return sConfigMgr->GetOption<bool>("LfgAutofill.Announce", true);
    }

    // The size this player wants, or 0 for "don't fill".
    uint8 DesiredSize(Player* player)
    {
        auto it = s_desiredSize.find(player->GetGUID());
        if (it != s_desiredSize.end())
            return it->second;
        return DefaultPartySize();
    }

    char const* RoleName(FillRole role)
    {
        switch (role)
        {
            case FillRole::Tank:   return "tank";
            case FillRole::Healer: return "healer";
            default:               return "damage";
        }
    }

    // A bot's role comes from its talent spec, via playerbots' own classifier, so a
    // protection warrior counts as a tank here exactly as it does to the bot AI.
    FillRole RoleOfPlayer(Player* player)
    {
        uint8 roles = AiFactory::GetPlayerRoles(player);
        if (roles & BOT_ROLE_TANK)
            return FillRole::Tank;
        if (roles & BOT_ROLE_HEALER)
            return FillRole::Healer;
        return FillRole::Damage;
    }

    // The queueing player's own role comes from the pane's checkboxes when they ticked
    // one, and falls back to their spec when they did not (or ticked several).
    FillRole RoleFromLfgMask(Player* player, uint8 lfgRoles)
    {
        bool tank   = (lfgRoles & lfg::PLAYER_ROLE_TANK)   != 0;
        bool healer = (lfgRoles & lfg::PLAYER_ROLE_HEALER) != 0;
        bool damage = (lfgRoles & lfg::PLAYER_ROLE_DAMAGE) != 0;

        // Exactly one ticked is an unambiguous statement of intent; anything else is not,
        // so fall back to what the character is actually specced for.
        if (tank && !healer && !damage)
            return FillRole::Tank;
        if (healer && !tank && !damage)
            return FillRole::Healer;
        if (damage && !tank && !healer)
            return FillRole::Damage;

        return RoleOfPlayer(player);
    }

    uint8 LfgMaskForRole(FillRole role)
    {
        switch (role)
        {
            case FillRole::Tank:   return lfg::PLAYER_ROLE_TANK;
            case FillRole::Healer: return lfg::PLAYER_ROLE_HEALER;
            default:               return lfg::PLAYER_ROLE_DAMAGE;
        }
    }

    // Which roles a party of `size` still needs, given what it already has.
    // Standard 5-man shape: one tank, one healer, the rest damage.
    std::vector<FillRole> MissingRoles(bool haveTank, bool haveHealer, uint32 current, uint8 target)
    {
        std::vector<FillRole> needed;
        if (current >= target)
            return needed;

        uint32 slots = target - current;

        if (!haveTank && slots > 0)
        {
            needed.push_back(FillRole::Tank);
            --slots;
        }
        if (!haveHealer && slots > 0)
        {
            needed.push_back(FillRole::Healer);
            --slots;
        }
        while (slots > 0)
        {
            needed.push_back(FillRole::Damage);
            --slots;
        }
        return needed;
    }

    // An online random bot that is free to be recruited for this player's run.
    // `bots` is passed in rather than fetched here: GetAllBots() returns the map by value,
    // and this runs once per missing role.
    Player* PickBot(Player* player, FillRole need, PlayerBotMap const& bots,
                    std::unordered_set<ObjectGuid> const& taken)
    {
        uint32 const levelsAbove = LevelsAbove();
        uint8 const playerLevel = player->GetLevel();

        for (auto const& itr : bots)
        {
            Player* bot = itr.second;
            if (!bot || !bot->IsInWorld() || bot == player)
                continue;

            if (taken.count(bot->GetGUID()))
                continue;

            // Never poach a bot that is already someone's party member.
            if (bot->GetGroup())
                continue;

            if (bot->GetTeamId() != player->GetTeamId())
                continue;

            if (bot->IsInCombat() || bot->InBattleground() || bot->InArena())
                continue;

            // Pulling a bot out of a dungeon it is already running would strand its party.
            if (Map* map = bot->GetMap())
                if (map->IsDungeon() || map->IsRaid())
                    continue;

            if (bot->GetLevel() < playerLevel || bot->GetLevel() > playerLevel + levelsAbove)
                continue;

            if (!GET_PLAYERBOT_AI(bot))
                continue;

            if (RoleOfPlayer(bot) != need)
                continue;

            return bot;
        }

        return nullptr;
    }

    // Returns the number of bots actually recruited.
    uint32 FillGroup(Player* player, uint8 lfgRoles, uint8 target)
    {
        if (!target)
            return 0;

        Group* group = player->GetGroup();

        // Only ever reshape a party the player owns.
        if (group && group->GetLeaderGUID() != player->GetGUID())
            return 0;

        uint32 current = group ? group->GetMembersCount() : 1;
        if (current >= target)
            return 0;

        bool haveTank = false;
        bool haveHealer = false;

        auto note = [&](FillRole role)
        {
            if (role == FillRole::Tank)
                haveTank = true;
            else if (role == FillRole::Healer)
                haveHealer = true;
        };

        note(RoleFromLfgMask(player, lfgRoles));

        if (group)
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == player)
                    continue;
                note(RoleOfPlayer(member));
            }
        }

        std::vector<FillRole> needed = MissingRoles(haveTank, haveHealer, current, target);
        if (needed.empty())
            return 0;

        PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
        std::unordered_set<ObjectGuid> taken;
        std::vector<std::pair<Player*, FillRole>> recruited;

        for (FillRole role : needed)
        {
            Player* bot = PickBot(player, role, bots, taken);

            // No bot of that role in range. Rather than leave the slot empty, take any
            // damage bot — a party that is one short of its ideal shape still beats a
            // party that is one short of its size.
            if (!bot && role != FillRole::Damage)
                bot = PickBot(player, FillRole::Damage, bots, taken);

            if (!bot)
                continue;

            taken.insert(bot->GetGUID());
            recruited.emplace_back(bot, role);
        }

        if (recruited.empty())
        {
            if (Announce())
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff4CFF00[LFG Autofill]|r No suitable bots are available right now.");
            return 0;
        }

        if (!group)
        {
            group = new Group();
            if (!group->Create(player))
            {
                delete group;
                LOG_ERROR("module.lfgautofill", "Failed to create a group for {}", player->GetName());
                return 0;
            }
            sGroupMgr->AddGroup(group);
        }

        uint32 added = 0;
        for (auto const& [bot, role] : recruited)
        {
            if (!group->AddMember(bot, LfgMaskForRole(role)))
                continue;

            // Hand the bot to this player straight away. RandomPlayerbotMgr would do this
            // itself on a later sweep, but waiting means the bots stand around for a few
            // seconds after being grouped, which reads as broken.
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                botAI->SetMaster(player);
                botAI->ResetStrategies();
            }

            ++added;

            if (Announce())
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff4CFF00[LFG Autofill]|r {} joined as {}.", bot->GetName(), RoleName(role));
        }

        return added;
    }

    class lfg_autofill_playerscript : public PlayerScript
    {
    public:
        lfg_autofill_playerscript() : PlayerScript("lfg_autofill_playerscript",
            { PLAYERHOOK_CAN_JOIN_LFG, PLAYERHOOK_ON_LOGOUT }) {}

        bool OnPlayerCanJoinLfg(Player* player, uint8 roles, lfg::LfgDungeonSet& dungeons,
                                std::string const& comment) override
        {
            if (!player || !ModuleEnabled())
                return true;

            // Our own re-issued join — let it straight through.
            if (s_reissuing.count(player->GetGUID()))
                return true;

            uint8 const target = DesiredSize(player);
            if (!target)
                return true;

            Group* group = player->GetGroup();
            if (group && group->GetLeaderGUID() != player->GetGUID())
                return true;

            uint32 const current = group ? group->GetMembersCount() : 1;
            if (current >= target)
                return true;

            // The party cannot be filled from inside this hook. LFGMgr::JoinLfg captures
            // the group pointer and queue GUID *before* calling us (LFGMgr.cpp:604), so a
            // group created or grown here would be invisible to the rest of the join: a
            // solo player would queue as solo while actually sitting in a party of five.
            //
            // So cancel this attempt, fill on the next world tick, and re-issue the join
            // once the party is real. To the player it looks like one queue press.
            s_pending.push_back({ player->GetGUID(), roles, dungeons, comment });
            return false;
        }

        void OnPlayerLogout(Player* player) override
        {
            s_desiredSize.erase(player->GetGUID());
            s_reissuing.erase(player->GetGUID());
        }
    };

    class lfg_autofill_worldscript : public WorldScript
    {
    public:
        lfg_autofill_worldscript() : WorldScript("lfg_autofill_worldscript", { WORLDHOOK_ON_UPDATE }) {}

        void OnUpdate(uint32 /*diff*/) override
        {
            if (s_pending.empty())
                return;

            std::vector<PendingJoin> pending;
            pending.swap(s_pending);

            for (PendingJoin& join : pending)
            {
                Player* player = ObjectAccessor::FindPlayer(join.playerGuid);
                if (!player || !player->IsInWorld())
                    continue;

                FillGroup(player, join.roles, DesiredSize(player));

                s_reissuing.insert(join.playerGuid);
                sLFGMgr->JoinLfg(player, join.roles, join.dungeons, join.comment);
                s_reissuing.erase(join.playerGuid);
            }
        }
    };

    class lfg_autofill_commandscript : public CommandScript
    {
    public:
        lfg_autofill_commandscript() : CommandScript("lfg_autofill_commandscript") {}

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable commandTable =
            {
                { "lfgfill", HandleLfgFillCommand, SEC_PLAYER, Console::No }
            };
            return commandTable;
        }

        static bool HandleLfgFillCommand(ChatHandler* handler, Optional<std::string_view> arg)
        {
            Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!player)
                return false;

            if (!ModuleEnabled())
            {
                handler->PSendSysMessage("LFG autofill is disabled on this server.");
                return true;
            }

            if (!arg || arg->empty())
            {
                uint8 const size = DesiredSize(player);
                if (size)
                    handler->PSendSysMessage("LFG autofill: your party will be filled to {} before queueing.", size);
                else
                    handler->PSendSysMessage("LFG autofill: off. Use .lfgfill <{}-{}> to set a party size.",
                                             kMinPartySize, kMaxPartySize);
                return true;
            }

            std::string value{ *arg };
            std::transform(value.begin(), value.end(), value.begin(), ::tolower);

            if (value == "off" || value == "0")
            {
                s_desiredSize[player->GetGUID()] = 0;
                handler->PSendSysMessage("LFG autofill: off.");
                return true;
            }

            if (value == "now")
            {
                // A bare "now" from someone who never set a size means "give me a full
                // party" — falling through to 0 and doing nothing would just look broken.
                uint8 target = DesiredSize(player);
                if (!target)
                    target = kMaxPartySize;

                uint32 const added = FillGroup(player, 0, target);
                if (added)
                    handler->PSendSysMessage("LFG autofill: recruited {} bot(s).", added);
                else
                    handler->PSendSysMessage("LFG autofill: nothing to fill.");
                return true;
            }

            uint32 size = 0;
            try
            {
                size = static_cast<uint32>(std::stoul(value));
            }
            catch (...)
            {
                handler->PSendSysMessage("Usage: .lfgfill <{}-{}> | off | now", kMinPartySize, kMaxPartySize);
                return true;
            }

            if (size < kMinPartySize || size > kMaxPartySize)
            {
                handler->PSendSysMessage("Party size must be between {} and {}.", kMinPartySize, kMaxPartySize);
                return true;
            }

            s_desiredSize[player->GetGUID()] = static_cast<uint8>(size);
            handler->PSendSysMessage(
                "LFG autofill: your party will be filled to {} with bots covering the missing roles.", size);
            return true;
        }
    };
}

void AddLfgAutofillScripts()
{
    new lfg_autofill_playerscript();
    new lfg_autofill_worldscript();
    new lfg_autofill_commandscript();
}
