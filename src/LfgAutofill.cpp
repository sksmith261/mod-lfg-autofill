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
 *
 * When the recruitable band (the player's level up to LevelsAbove over it) cannot cover
 * every slot, bots are borrowed from other level brackets, re-levelled into the band with
 * PlayerbotFactory, and put back on the level they came from when they leave the party.
 * This mirrors mod-lfr-autofill's borrowing — same loan table pattern, same
 * record-outlives-the-queue lifecycle — with one twist that module does not have: fills
 * here happen in the middle of a cancelled queue join, and a re-level is spread over
 * several world ticks, so the join is parked in s_waiting and re-issued only once the
 * last borrowed bot has landed (or a timeout gives up on the stragglers).
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Group.h"
#include "GroupMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "GroupScript.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "LFG.h"
#include "LFGMgr.h"
#include "World.h"

#include "AiFactory.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
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

    // A cancelled join whose borrowed bots are still being re-levelled in, waiting to be
    // re-issued when the last of them lands — or when the timeout gives up on stragglers.
    struct WaitingJoin
    {
        PendingJoin        join;
        lfg::LfgDungeonSet open;      // dungeons the party assembled so far leaves reachable; empty = no lock filtering
        uint32             waitedMs = 0;
    };

    std::unordered_map<ObjectGuid, WaitingJoin> s_waiting;

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

    // ---------------------------------------------------------------------------------------
    // Borrowing (mirrors mod-lfr-autofill)
    //
    // Strictly a fallback: it only runs for slots the recruitable band could not fill, so on
    // a population shaped for where the players are it never fires at all.
    // ---------------------------------------------------------------------------------------

    bool BorrowEnabled()
    {
        return sConfigMgr->GetOption<bool>("LfgAutofill.BorrowFromSurplus", true);
    }

    // A party is five, so four is not a limit so much as a statement of intent; it exists
    // so no single fill can ever re-roll more than a party's worth of the population.
    uint32 MaxBorrowPerFill()
    {
        return sConfigMgr->GetOption<uint32>("LfgAutofill.MaxBorrowPerFill", 4);
    }

    // PlayerbotFactory::Randomize tears down and rebuilds skills, spells, talents, gear,
    // bags and consumables across roughly eighteen phases. Re-levels are spread one per
    // interval so they cannot hitch the world thread.
    uint32 BorrowIntervalMs()
    {
        return sConfigMgr->GetOption<uint32>("LfgAutofill.BorrowIntervalMs", 250);
    }

    // How long a cancelled queue join waits for its borrowed bots before going out with
    // whoever has landed. Unlike a raid fill, a queue press is a moment — the player is
    // watching the eye, and a join that silently never happens reads as a broken pane.
    uint32 BorrowTimeoutMs()
    {
        return sConfigMgr->GetOption<uint32>("LfgAutofill.BorrowTimeoutMs", 10000);
    }

    // Bot GUID -> the level it was on before being borrowed.
    //
    // Mirrored into the characters DB (lfg_autofill_borrowed), because this is the one
    // piece of module state that must survive a restart: an in-memory-only record would
    // leave borrowed bots permanently stuck at the party's level with nothing knowing what
    // to put them back to.
    std::unordered_map<ObjectGuid, uint8> s_borrowed;

    struct BotJob
    {
        ObjectGuid bot;
        ObjectGuid leader;   // empty on a return job
        uint8      level = 0;
        bool       promote = false;
    };

    std::vector<BotJob> s_jobs;
    uint32 s_jobTimer = 0;

    void RecordBorrow(ObjectGuid bot, uint8 originalLevel)
    {
        s_borrowed[bot] = originalLevel;
        CharacterDatabase.Execute(
            "REPLACE INTO lfg_autofill_borrowed (guid, original_level) VALUES ({}, {})",
            bot.GetCounter(), uint32(originalLevel));
    }

    void ForgetBorrow(ObjectGuid bot)
    {
        s_borrowed.erase(bot);
        CharacterDatabase.Execute("DELETE FROM lfg_autofill_borrowed WHERE guid = {}", bot.GetCounter());
    }

    // Queues a borrowed bot to be put back on the level it came from. Idempotent via the
    // job queue, NOT by dropping the record: the loan stays on the books until the return
    // re-level has actually happened, so a bot that is offline when its return job runs is
    // retried rather than stranded at the party's level. (mod-lfr-autofill shipped the
    // other way first and paid for it; see fac9a1b in its history.)
    void ReleaseBot(ObjectGuid botGuid)
    {
        auto it = s_borrowed.find(botGuid);
        if (it == s_borrowed.end())
            return;

        for (BotJob const& queued : s_jobs)
            if (queued.bot == botGuid && !queued.promote)
                return;

        BotJob job;
        job.bot = botGuid;
        job.level = it->second;
        job.promote = false;
        s_jobs.push_back(job);
    }

    // Promote jobs still queued or retrying for this leader — borrowed bots that have not
    // landed in the party yet.
    uint32 OutstandingBorrows(ObjectGuid leader)
    {
        uint32 count = 0;
        for (BotJob const& job : s_jobs)
            if (job.promote && job.leader == leader)
                ++count;
        return count;
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

    // Shared eligibility, applied to natural recruits and donors alike. Level is deliberately
    // not checked here — that is the one rule the two disagree on.
    bool BotIsAvailable(Player* player, Player* bot, std::unordered_set<ObjectGuid> const& taken,
                        bool crossFaction)
    {
        if (!bot || !bot->IsInWorld() || bot == player)
            return false;

        if (taken.count(bot->GetGUID()))
            return false;

        // Never poach a bot that is already someone's party member.
        if (bot->GetGroup())
            return false;

        if (!crossFaction && bot->GetTeamId() != player->GetTeamId())
            return false;

        if (bot->IsInCombat() || bot->InBattleground() || bot->InArena())
            return false;

        // Pulling a bot out of a dungeon it is already running would strand its party.
        if (Map* map = bot->GetMap())
            if (map->IsDungeon() || map->IsRaid())
                return false;

        if (!GET_PLAYERBOT_AI(bot))
            return false;

        // A bot already on loan is not free, even though it looks it. Covers the window
        // between a party releasing a bot and the return job restoring its level.
        if (s_borrowed.count(bot->GetGUID()))
            return false;

        return true;
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

            if (!BotIsAvailable(player, bot, taken, crossFaction))
                continue;

            if (bot->GetLevel() < playerLevel || bot->GetLevel() > playerLevel + levelsAbove)
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

    // ---------------------------------------------------------------------------------------
    // Donor selection (mirrors mod-lfr-autofill)
    // ---------------------------------------------------------------------------------------

    struct Bracket
    {
        uint8  lower = 0;
        uint8  upper = 0;
        uint32 pct   = 0;
    };

    // Read from mod-player-bot-level-brackets' own configuration rather than duplicating the
    // bracket table here, so the two cannot drift apart and quietly disagree about what a
    // surplus is. Boundaries are identical for both factions in that module — only the
    // percentages differ — so the Alliance table is used for both.
    //
    // If that module is not installed the keys are absent, every bracket comes back empty, and
    // donor ordering falls back to raw population. Borrowing still works; it just cannot
    // reason about targets.
    std::vector<Bracket> ReadBrackets()
    {
        std::vector<Bracket> brackets;

        for (uint32 i = 1; i <= 16; ++i)
        {
            std::string const idx = std::to_string(i);
            uint32 const lower = sConfigMgr->GetOption<uint32>("BotLevelBrackets.Alliance.Range" + idx + ".Lower", 0);
            uint32 const upper = sConfigMgr->GetOption<uint32>("BotLevelBrackets.Alliance.Range" + idx + ".Upper", 0);
            if (!lower || !upper || upper < lower)
                break;

            Bracket b;
            b.lower = static_cast<uint8>(lower);
            b.upper = static_cast<uint8>(upper);
            b.pct   = sConfigMgr->GetOption<uint32>("BotLevelBrackets.Alliance.Range" + idx + ".Pct", 0);
            brackets.push_back(b);
        }

        return brackets;
    }

    int BracketOf(uint8 level, std::vector<Bracket> const& brackets)
    {
        for (size_t i = 0; i < brackets.size(); ++i)
            if (level >= brackets[i].lower && level <= brackets[i].upper)
                return static_cast<int>(i);
        return -1;
    }

    // Brackets in the order they should be raided for donors, best first.
    //
    // The bracket that *contains* the player's level always comes first, and it is not merely
    // the least-bad option — it is free. Taking a level 67 bot down to a level 60 player
    // leaves it inside 60-69, so the census sees no change at all: nothing moves between
    // brackets, and there is nothing for mod-player-bot-level-brackets to correct afterwards.
    //
    // Only once that bracket is exhausted does anything actually move, and then the order is
    // by surplus against the configured target, so the bots taken are the ones that module
    // was already trying to shed. Brackets at or below their target are still usable as a
    // last resort, but they sort last.
    std::vector<int> DonorBracketOrder(PlayerBotMap const& bots, std::vector<Bracket> const& brackets,
                                       uint8 playerLevel)
    {
        std::vector<int> order;
        if (brackets.empty())
            return order;

        std::vector<int32> actual(brackets.size(), 0);
        int32 total = 0;

        for (auto const& itr : bots)
        {
            Player* bot = itr.second;
            if (!bot || !bot->IsInWorld())
                continue;

            int const idx = BracketOf(bot->GetLevel(), brackets);
            if (idx < 0)
                continue;

            ++actual[idx];
            ++total;
        }

        int const homeBracket = BracketOf(playerLevel, brackets);

        std::vector<std::pair<int32, int>> ranked; // surplus, index
        for (size_t i = 0; i < brackets.size(); ++i)
        {
            if (static_cast<int>(i) == homeBracket)
                continue;

            int32 const target = static_cast<int32>((uint64(total) * brackets[i].pct) / 100);
            ranked.emplace_back(actual[i] - target, static_cast<int>(i));
        }

        std::sort(ranked.begin(), ranked.end(),
                  [](auto const& a, auto const& b) { return a.first > b.first; });

        if (homeBracket >= 0)
            order.push_back(homeBracket);
        for (auto const& [surplus, idx] : ranked)
            order.push_back(idx);

        return order;
    }

    // A bot to re-level to the queueing player's own level, taken from the best donor
    // bracket that has one.
    //
    // Role is matched on the bot as it stands now, before any re-roll. That is a strong hint
    // rather than a promise: PlayerbotFactory re-picks talents from scratch, so a bot
    // borrowed as a healer can land as damage. The party's real shape is therefore
    // re-checked as each bot lands, not assumed from this pick.
    Player* PickDonor(Player* player, FillRole need, PlayerBotMap const& bots,
                      std::unordered_set<ObjectGuid> const& taken,
                      std::vector<Bracket> const& brackets, std::vector<int> const& order)
    {
        uint32 const levelsAbove = LevelsAbove();
        uint8 const playerLevel = player->GetLevel();
        bool const crossFaction = CrossFaction();

        auto usable = [&](Player* bot) -> bool
        {
            if (!BotIsAvailable(player, bot, taken, crossFaction))
                return false;

            // Inside the recruitable band already: the natural pass had its chance at this
            // one, and re-rolling a bot that needs no re-roll would be pure waste.
            if (bot->GetLevel() >= playerLevel && bot->GetLevel() <= playerLevel + levelsAbove)
                return false;

            if (BotLfgRole(bot) != need)
                return false;

            // Death Knights cannot exist below 55, so re-levelling one under that would
            // produce a character the core will not accept.
            if (bot->getClass() == CLASS_DEATH_KNIGHT && playerLevel < 55)
                return false;

            // No dungeon-lock check here, deliberately: locks are level-dependent and this
            // bot is about to change level, so they are recomputed and judged when it lands.
            return true;
        };

        for (int bracketIdx : order)
        {
            for (auto const& itr : bots)
            {
                Player* bot = itr.second;
                if (!bot || !bot->IsInWorld())
                    continue;

                if (BracketOf(bot->GetLevel(), brackets) != bracketIdx)
                    continue;

                if (usable(bot))
                    return bot;
            }
        }

        // No bracket table available at all — fall back to any usable bot.
        if (order.empty())
        {
            for (auto const& itr : bots)
                if (usable(itr.second))
                    return itr.second;
        }

        return nullptr;
    }

    struct FillOutcome
    {
        uint32             added = 0;      // recruited from the band, already in the party
        uint32             borrowed = 0;   // queued for re-levelling, not yet in the party
        lfg::LfgDungeonSet open;           // what the party assembled so far leaves reachable
    };

    // `dungeons` is what the player is about to queue for, or empty when the fill is not
    // headed for a queue at all.
    FillOutcome FillGroup(Player* player, uint8 lfgRoles, uint8 target, lfg::LfgDungeonSet const& dungeons)
    {
        FillOutcome outcome;

        if (!target)
            return outcome;

        Group* group = player->GetGroup();

        // Only ever reshape a party the player owns.
        if (group && group->GetLeaderGUID() != player->GetGUID())
            return outcome;

        // Borrows still in flight from a previous fill count as bodies: pressing queue
        // twice while they re-level must not conscript a second set for the same slots.
        uint32 current = (group ? group->GetMembersCount() : 1) + OutstandingBorrows(player->GetGUID());
        if (current >= target)
            return outcome;

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
            return outcome;

        PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
        std::unordered_set<ObjectGuid> taken;
        std::vector<std::pair<Player*, FillRole>> recruited;
        std::vector<FillRole> unfilled;

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
            {
                unfilled.push_back(role);
                continue;
            }

            taken.insert(bot->GetGUID());
            note(filled);
            recruited.emplace_back(bot, filled);
        }

        // Borrow for whatever the band could not crew. Strictly a fallback — with a healthy
        // pool at the player's level `unfilled` is empty and none of this runs.
        if (!unfilled.empty() && BorrowEnabled())
        {
            std::vector<Bracket> const brackets = ReadBrackets();
            std::vector<int> const order = DonorBracketOrder(bots, brackets, player->GetLevel());

            uint32 const budget = MaxBorrowPerFill();

            for (FillRole role : unfilled)
            {
                if (outcome.borrowed >= budget)
                    break;

                Player* bot = PickDonor(player, role, bots, taken, brackets, order);
                if (!bot && role != FillRole::Damage)
                    bot = PickDonor(player, FillRole::Damage, bots, taken, brackets, order);

                if (!bot)
                    continue;

                taken.insert(bot->GetGUID());

                // Recorded before the re-level, not after: if the server dies between the
                // two, the row is what lets the startup sweep put the bot back.
                //
                // Unless the bot is already on the books — then it is being re-borrowed
                // while the return from a previous loan is still pending, and its current
                // level is the old party's, not a home to record. Keep the original record
                // and cancel the stale return so it cannot re-level the bot out from under
                // this party.
                if (s_borrowed.count(bot->GetGUID()))
                {
                    ObjectGuid const guid = bot->GetGUID();
                    s_jobs.erase(std::remove_if(s_jobs.begin(), s_jobs.end(),
                        [&guid](BotJob const& queued) { return queued.bot == guid && !queued.promote; }),
                        s_jobs.end());
                }
                else
                    RecordBorrow(bot->GetGUID(), bot->GetLevel());

                BotJob job;
                job.bot = bot->GetGUID();
                job.leader = player->GetGUID();
                job.level = player->GetLevel();
                job.promote = true;
                s_jobs.push_back(job);

                ++outcome.borrowed;
            }
        }

        if (recruited.empty() && !outcome.borrowed)
        {
            if (Announce())
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff4CFF00[LFG Autofill]|r No suitable bots are available right now.");
            return outcome;
        }

        if (!group)
        {
            group = new Group();
            if (!group->Create(player))
            {
                delete group;
                LOG_ERROR("module.lfgautofill", "Failed to create a group for {}", player->GetName());
                return outcome;
            }
            sGroupMgr->AddGroup(group);
        }

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

            ++outcome.added;

            if (Announce())
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff4CFF00[LFG Autofill]|r {} joined as {}.", bot->GetName(), RoleName(role));
        }

        // Say so when the party came up short. The queue still goes out and is still
        // valid — the shape is just thinner than asked for, and a player who is told that
        // can decide to wait for a better fill instead of wondering mid-dungeon. Skipped
        // while borrows are in flight: the shape is not final until they land, and each
        // landing announces itself.
        if (Announce() && !outcome.borrowed && recruited.size() < needed.size())
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
                    "|cff4CFF00[LFG Autofill]|r Only filled to {}.", current + outcome.added);
        }

        outcome.open = std::move(open);
        return outcome;
    }

    // Re-issue a queue join we cancelled earlier. The s_reissuing guard lets it pass our own
    // OnPlayerCanJoinLfg hook untouched. Non-const because JoinLfg takes the dungeon set by
    // mutable reference.
    void Reissue(PendingJoin& join)
    {
        Player* player = ObjectAccessor::FindPlayer(join.playerGuid);
        if (!player || !player->IsInWorld())
            return;

        s_reissuing.insert(join.playerGuid);
        sLFGMgr->JoinLfg(player, join.roles, join.dungeons, join.comment);
        s_reissuing.erase(join.playerGuid);
    }

    // Runs one queued re-level. Returns false if the job should be retried rather than
    // dropped.
    bool RunJob(BotJob const& job)
    {
        Player* bot = ObjectAccessor::FindPlayer(job.bot);

        // Gone offline. An abandoned promote becomes a release: usually the bot is still at
        // its original level and the queued return is a harmless refresh, but on a re-borrow
        // it is sitting at a previous party's level and the return is what un-strands it. A
        // return job keeps cycling so the bot is put back the moment it logs in; if the
        // server dies first, the loan row is still on disk for the startup sweep. The
        // exception is a guid that no longer resolves to a character at all — that bot can
        // never log in again, so the job and its record are scrubbed rather than cycled
        // forever.
        if (!bot || !bot->IsInWorld())
        {
            if (job.promote)
            {
                ReleaseBot(job.bot);
                return true;
            }

            std::string name;
            if (!sCharacterCache->GetCharacterNameByGuid(job.bot, name))
            {
                ForgetBorrow(job.bot);
                return true;
            }

            return false;
        }

        // Re-rolling a bot mid-fight would strip the gear out from under it. Retry later.
        if (bot->IsInCombat())
            return false;

        PlayerbotFactory factory(bot, job.level);
        factory.Randomize(false);

        // GiveLevel does not refresh LFG's per-player lock map, and the re-issued JoinLfg
        // bypasses the client handler that normally rebuilds it before a join — so rebuild
        // it here, or the queue is judged against the locks of a level that no longer
        // exists.
        sLFGMgr->InitializeLockedDungeons(bot);

        if (!job.promote)
        {
            // The loan is closed only now that the bot's level is actually back where it
            // started — never earlier, or a crash in the window loses the record.
            ForgetBorrow(job.bot);

            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                botAI->SetMaster(nullptr);
                botAI->ResetStrategies();
            }
            return true;
        }

        Player* leader = ObjectAccessor::FindPlayer(job.leader);
        Group* group = leader ? leader->GetGroup() : nullptr;

        // The party went away while this bot was waiting its turn — it was re-levelled for
        // nothing, so hand it straight back rather than leaving it stranded.
        if (!leader || !group || group->GetLeaderGUID() != leader->GetGUID() || group->IsFull())
        {
            ReleaseBot(job.bot);
            return true;
        }

        auto waiting = s_waiting.find(job.leader);

        // Judged on what it leaves reachable, exactly like a natural recruit — but against
        // its post-re-level locks, which are the ones the queue will actually see.
        if (waiting != s_waiting.end() && !waiting->second.open.empty())
        {
            lfg::LfgDungeonSet remaining = DungeonsLeftOpenBy(bot, waiting->second.open);
            if (remaining.empty())
            {
                ReleaseBot(job.bot);
                return true;
            }
            waiting->second.open.swap(remaining);
        }

        // The re-roll re-picks talents, so the bot may have landed as a different role than
        // it was borrowed for. Count the party the way LFGMgr will; a bot that busts a role
        // cap is worse than an empty slot, because it kills the whole queue.
        FillRole const landedRole = BotLfgRole(bot);
        {
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

            note(RoleFromLfgMask(leader, waiting != s_waiting.end() ? waiting->second.join.roles : 0));
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == leader)
                    continue;
                note(RoleOfMember(member));
            }
            note(landedRole);

            if (tanks > lfg::LFG_TANKS_NEEDED || healers > lfg::LFG_HEALERS_NEEDED ||
                damage > lfg::LFG_DPS_NEEDED)
            {
                ReleaseBot(job.bot);
                return true;
            }
        }

        if (!group->AddMember(bot, LfgMaskForRole(landedRole)))
        {
            ReleaseBot(job.bot);
            return true;
        }

        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        {
            botAI->SetMaster(leader);
            botAI->ResetStrategies();
        }

        if (Announce())
            ChatHandler(leader->GetSession()).PSendSysMessage(
                "|cff4CFF00[LFG Autofill]|r {} joined as {} (re-levelled to {}).",
                bot->GetName(), RoleName(landedRole), job.level);

        return true;
    }

    // One queued re-level per interval, so a run of them cannot hitch the world thread.
    void PumpJobs(uint32 diff)
    {
        if (s_jobs.empty())
            return;

        s_jobTimer += diff;
        if (s_jobTimer < BorrowIntervalMs())
            return;

        s_jobTimer = 0;

        BotJob const job = s_jobs.front();
        s_jobs.erase(s_jobs.begin());

        // A job that cannot run yet goes to the back, so one bot stuck in combat cannot
        // block the whole queue behind it.
        if (!RunJob(job))
            s_jobs.push_back(job);
    }

    // Re-issue every parked join whose borrows have all landed, and give up on joins that
    // have waited past the timeout — those go out with whoever arrived, and the stragglers
    // are cancelled and sent home.
    void SweepWaiting(uint32 diff)
    {
        if (s_waiting.empty())
            return;

        uint32 const timeout = BorrowTimeoutMs();

        for (auto it = s_waiting.begin(); it != s_waiting.end();)
        {
            it->second.waitedMs += diff;
            ObjectGuid const guid = it->first;

            if (!OutstandingBorrows(guid))
            {
                PendingJoin join = std::move(it->second.join);
                it = s_waiting.erase(it);
                Reissue(join);
                continue;
            }

            if (it->second.waitedMs >= timeout)
            {
                // Usually a donor that logged out or refuses to leave combat. Collect the
                // stragglers before touching the queue: ReleaseBot appends to s_jobs, so it
                // cannot run mid-iteration.
                std::vector<ObjectGuid> stragglers;
                for (BotJob const& job : s_jobs)
                    if (job.promote && job.leader == guid)
                        stragglers.push_back(job.bot);

                s_jobs.erase(std::remove_if(s_jobs.begin(), s_jobs.end(),
                    [&guid](BotJob const& job) { return job.promote && job.leader == guid; }),
                    s_jobs.end());

                for (ObjectGuid straggler : stragglers)
                    ReleaseBot(straggler);

                if (Player* player = ObjectAccessor::FindPlayer(guid))
                    if (Announce())
                        ChatHandler(player->GetSession()).PSendSysMessage(
                            "|cff4CFF00[LFG Autofill]|r Gave up waiting on {} bot(s); queueing without them.",
                            uint32(stragglers.size()));

                PendingJoin join = std::move(it->second.join);
                it = s_waiting.erase(it);
                Reissue(join);
                continue;
            }

            ++it;
        }
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

            // Already waiting on borrowed bots from an earlier press. Keep the newest
            // packet — the player may have changed dungeons or roles — and keep waiting;
            // this press re-issues like the first would have.
            auto waiting = s_waiting.find(player->GetGUID());
            if (waiting != s_waiting.end())
            {
                waiting->second.join.roles = roles;
                waiting->second.join.dungeons = dungeons;
                waiting->second.join.comment = comment;
                return false;
            }

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

            // Any parked join dies with the session. Its outstanding promote jobs discover
            // the leader is gone when they run, and release their bots there.
            s_waiting.erase(player->GetGUID());
        }
    };

    class lfg_autofill_worldscript : public WorldScript
    {
    public:
        lfg_autofill_worldscript() : WorldScript("lfg_autofill_worldscript",
            { WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_STARTUP }) {}

        // Anything still on the books at startup was left mid-loan by a shutdown or a
        // crash. Those bots are sitting at some party's level with nothing else aware of
        // it, so they are read back and queued for return before any can be recruited
        // again.
        void OnStartup() override
        {
            QueryResult result = CharacterDatabase.Query(
                "SELECT guid, original_level FROM lfg_autofill_borrowed");
            if (!result)
                return;

            uint32 count = 0;
            do
            {
                Field* fields = result->Fetch();
                ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());

                s_borrowed[guid] = fields[1].Get<uint8>();
                ReleaseBot(guid);
                ++count;
            } while (result->NextRow());

            LOG_INFO("module.lfgautofill", "Recovered {} borrowed bot(s) left over from the last run.", count);
        }

        void OnUpdate(uint32 diff) override
        {
            // Cancelled joins from last tick: fill, then either re-issue at once or park
            // the join until its borrowed bots land.
            if (!s_pending.empty())
            {
                std::vector<PendingJoin> pending;
                pending.swap(s_pending);

                for (PendingJoin& join : pending)
                {
                    Player* player = ObjectAccessor::FindPlayer(join.playerGuid);
                    if (!player || !player->IsInWorld())
                        continue;

                    FillOutcome outcome = FillGroup(player, join.roles, DesiredSize(player), join.dungeons);

                    if (outcome.borrowed || OutstandingBorrows(join.playerGuid))
                    {
                        if (Announce() && outcome.borrowed)
                            ChatHandler(player->GetSession()).PSendSysMessage(
                                "|cff4CFF00[LFG Autofill]|r Re-levelling {} bot(s) to {}; queueing when they arrive.",
                                outcome.borrowed, player->GetLevel());

                        WaitingJoin waiting;
                        waiting.join = std::move(join);
                        waiting.open = std::move(outcome.open);
                        s_waiting[waiting.join.playerGuid] = std::move(waiting);
                        continue;
                    }

                    Reissue(join);
                }
            }

            PumpJobs(diff);
            SweepWaiting(diff);
        }
    };

    class lfg_autofill_groupscript : public GroupScript
    {
    public:
        lfg_autofill_groupscript() : GroupScript("lfg_autofill_groupscript") {}

        // Covers every way a borrowed bot can leave the party: kicked, the leader logging
        // out, the group breaking up after the run.
        void OnRemoveMember(Group* /*group*/, ObjectGuid guid, RemoveMethod /*method*/,
                            ObjectGuid /*kicker*/, char const* /*reason*/) override
        {
            ReleaseBot(guid);
        }

        void OnDisband(Group* group) override
        {
            if (!group)
                return;

            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    ReleaseBot(member->GetGUID());
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
                FillOutcome const outcome = FillGroup(player, 0, target, lfg::LfgDungeonSet{});
                if (outcome.borrowed)
                    handler->PSendSysMessage(
                        "LFG autofill: recruited {} bot(s); re-levelling {} more to {} — they will join shortly.",
                        outcome.added, outcome.borrowed, player->GetLevel());
                else if (outcome.added)
                    handler->PSendSysMessage("LFG autofill: recruited {} bot(s).", outcome.added);
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
    new lfg_autofill_groupscript();
    new lfg_autofill_commandscript();
}
