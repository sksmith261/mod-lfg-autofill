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
#include "World.h"

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

    // Whether opposite-faction bots may be recruited.
    //
    // Gated on the core's own setting as well as ours, and deliberately so: GroupHandler
    // only rejects cross-faction *invites*, so Group::AddMember would happily assemble a
    // mixed party on a server that is not configured for one. Deferring to the core means
    // this module can never produce a group state the rest of the server disagrees with.
    bool CrossFaction()
    {
        return sConfigMgr->GetOption<bool>("LfgAutofill.CrossFaction", true) &&
               sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP);
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

    // A player's role from their talent spec, via playerbots' own classifier.
    FillRole RoleOfPlayer(Player* player)
    {
        uint8 roles = AiFactory::GetPlayerRoles(player);
        if (roles & BOT_ROLE_TANK)
            return FillRole::Tank;
        if (roles & BOT_ROLE_HEALER)
            return FillRole::Healer;
        return FillRole::Damage;
    }

    // What a random bot will answer when LFG asks it to confirm its role.
    //
    // This mirrors LfgJoinAction::GetRoles in mod-playerbots rather than reusing
    // AiFactory::GetPlayerRoles above, because the two disagree and it is this one that
    // decides whether the group passes LFGMgr::CheckGroupRoles. GetPlayerRoles has no
    // Death Knight case at all, so a blood DK reads as damage here while answering tank
    // there; and it calls every feral druid a tank, while the bot answers damage without
    // Thick Hide. Recruiting against the wrong classifier assembles a party that looks
    // correct, then fails the role check, and the queue dies with nothing shown to the
    // player. If that function changes upstream, this has to follow it.
    FillRole BotLfgRole(Player* bot)
    {
        uint8 const spec = AiFactory::GetPlayerSpecTab(bot);

        switch (bot->getClass())
        {
            case CLASS_DRUID:
                if (spec == 2)
                    return FillRole::Healer;
                if (spec == 1 && bot->HasAura(16931)) // thick hide
                    return FillRole::Tank;
                return FillRole::Damage;
            case CLASS_PALADIN:
                if (spec == 1)
                    return FillRole::Tank;
                if (spec == 0)
                    return FillRole::Healer;
                return FillRole::Damage;
            case CLASS_PRIEST:
                return spec != 2 ? FillRole::Healer : FillRole::Damage;
            case CLASS_SHAMAN:
                return spec == 2 ? FillRole::Healer : FillRole::Damage;
            case CLASS_WARRIOR:
                return spec == 2 ? FillRole::Tank : FillRole::Damage;
            case CLASS_DEATH_KNIGHT:
                return spec == 0 ? FillRole::Tank : FillRole::Damage;
            default:
                return FillRole::Damage;
        }
    }

    // Random bots answer the role check themselves and must be read the way they will
    // answer it; humans tick their own boxes, so their spec is the best guess available.
    FillRole RoleOfMember(Player* member)
    {
        if (GET_PLAYERBOT_AI(member) && sRandomPlayerbotMgr.IsRandomBot(member))
            return BotLfgRole(member);
        return RoleOfPlayer(member);
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

    // The dungeons the player asked for, with a random pick expanded into what it can roll,
    // which is the form LFGMgr::JoinLfg compares locks against.
    lfg::LfgDungeonSet ExpandDungeons(lfg::LfgDungeonSet const& dungeons)
    {
        if (dungeons.size() == 1)
        {
            uint32 const id = *dungeons.begin();
            if (sLFGMgr->GetDungeonType(id) == lfg::LFG_TYPE_RANDOM)
                return sLFGMgr->GetDungeonsByRandom(id);
        }
        return dungeons;
    }

    // Which of `open` this bot would still leave open.
    //
    // LFGMgr::GetCompatibleDungeons drops from the group's list every dungeon locked for any
    // one member, and a group whose list empties is refused with LFG_JOIN_PARTY_NOT_MEET_REQS
    // — the queue simply never goes out. So a bot has to be judged on what it leaves behind,
    // not only on its level and role.
    //
    // This bites hardest on Death Knights. InitializeLockedDungeons locks a DK out of every
    // dungeon in the game until it has been rewarded quest 13188 or 13189, and random bots do
    // not quest, so in practice every DK bot is locked out of everything. Recruiting one is
    // enough to empty the list and kill the queue for the whole party.
    lfg::LfgDungeonSet DungeonsLeftOpenBy(Player* bot, lfg::LfgDungeonSet const& open)
    {
        lfg::LfgLockMap const& locks = sLFGMgr->GetLockedDungeons(bot->GetGUID());
        if (locks.empty())
            return open;

        lfg::LfgDungeonSet remaining;
        for (uint32 dungeonId : open)
        {
            bool locked = false;
            for (auto const& lock : locks)
            {
                // Lock keys carry the difficulty in their high bits; ids compare on the low 24.
                if ((lock.first & 0x00FFFFFF) == (dungeonId & 0x00FFFFFF))
                {
                    locked = true;
                    break;
                }
            }

            if (!locked)
                remaining.insert(dungeonId);
        }

        return remaining;
    }

    // An online random bot that is free to be recruited for this player's run.
    // `bots` is passed in rather than fetched here: GetAllBots() returns the map by value,
    // and this runs once per missing role.
    // `open` is the set of dungeons still reachable by everyone recruited so far; a bot that
    // would empty it is skipped, and the set is narrowed to whatever the chosen bot leaves.
    // An empty `open` means the caller is not queueing (.lfgfill now), so locks do not apply.
    Player* PickBot(Player* player, FillRole need, PlayerBotMap const& bots,
                    std::unordered_set<ObjectGuid> const& taken, lfg::LfgDungeonSet& open)
    {
        uint32 const levelsAbove = LevelsAbove();
        uint8 const playerLevel = player->GetLevel();
        bool const crossFaction = CrossFaction();

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

            if (!crossFaction && bot->GetTeamId() != player->GetTeamId())
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

            if (BotLfgRole(bot) != need)
                continue;

            if (!open.empty())
            {
                lfg::LfgDungeonSet remaining = DungeonsLeftOpenBy(bot, open);
                if (remaining.empty())
                    continue;

                open.swap(remaining);
            }

            return bot;
        }

        return nullptr;
    }

    // Returns the number of bots actually recruited. `dungeons` is what the player is about
    // to queue for, or empty when the fill is not headed for a queue at all.
    uint32 FillGroup(Player* player, uint8 lfgRoles, uint8 target, lfg::LfgDungeonSet const& dungeons)
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

        // Running tally of what the party covers, counted the way LFGMgr will count it.
        uint8 tanks = 0;
        uint8 healers = 0;
        uint8 damage = 0;

        auto note = [&](FillRole role)
        {
            switch (role)
            {
                case FillRole::Tank:   ++tanks;   break;
                case FillRole::Healer: ++healers; break;
                default:               ++damage;  break;
            }
        };

        // LFGMgr::CheckGroupRoles rejects a group the moment any of these is exceeded, and
        // a rejected role check kills the queue without telling the player why. So a bot
        // that would push the party past one of them is worse than an empty slot: a valid
        // four-man queues, an invalid five-man does not.
        auto fits = [&](FillRole role)
        {
            switch (role)
            {
                case FillRole::Tank:   return tanks < lfg::LFG_TANKS_NEEDED;
                case FillRole::Healer: return healers < lfg::LFG_HEALERS_NEEDED;
                default:               return damage < lfg::LFG_DPS_NEEDED;
            }
        };

        note(RoleFromLfgMask(player, lfgRoles));

        if (group)
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == player)
                    continue;
                note(RoleOfMember(member));
            }
        }

        std::vector<FillRole> needed = MissingRoles(tanks > 0, healers > 0, current, target);
        if (needed.empty())
            return 0;

        PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
        std::unordered_set<ObjectGuid> taken;
        std::vector<std::pair<Player*, FillRole>> recruited;

        // Narrowed as bots are picked, so each candidate is judged against what the ones
        // already recruited still leave reachable.
        lfg::LfgDungeonSet open = ExpandDungeons(dungeons);

        for (FillRole role : needed)
        {
            Player* bot = fits(role) ? PickBot(player, role, bots, taken, open) : nullptr;
            FillRole filled = role;

            // No bot of that role available. Rather than leave the slot empty, take a
            // damage bot — a party one short of its ideal shape still beats one short of
            // its size. It is recorded as damage, which is what it is: labelling it with
            // the role we wanted puts a claim on the group that the bot itself will
            // contradict the moment LFG asks it, and the whole queue fails on that.
            if (!bot && role != FillRole::Damage && fits(FillRole::Damage))
            {
                bot = PickBot(player, FillRole::Damage, bots, taken, open);
                filled = FillRole::Damage;
            }

            if (!bot)
                continue;

            taken.insert(bot->GetGUID());
            note(filled);
            recruited.emplace_back(bot, filled);
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

        // Say so when the party came up short. The queue still goes out and is still
        // valid — the shape is just thinner than asked for, and a player who is told that
        // can decide to wait for a better fill instead of wondering mid-dungeon.
        if (Announce() && recruited.size() < needed.size())
        {
            if (!tanks && !healers)
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff4CFF00[LFG Autofill]|r No tank or healer was available; queueing without one.");
            else if (!tanks)
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff4CFF00[LFG Autofill]|r No tank was available; queueing without one.");
            else if (!healers)
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff4CFF00[LFG Autofill]|r No healer was available; queueing without one.");
            else
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff4CFF00[LFG Autofill]|r Only filled to {}.", current + added);
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

                FillGroup(player, join.roles, DesiredSize(player), join.dungeons);

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

                // No dungeon set: ".lfgfill now" is not headed for a queue, so a bot that
                // would be locked out of a dungeon is still perfectly good company.
                uint32 const added = FillGroup(player, 0, target, lfg::LfgDungeonSet{});
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
