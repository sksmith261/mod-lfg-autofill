# mod-lfg-autofill

Fills the gaps in a party with playerbots before a Dungeon Finder queue goes out. The player
says how many bodies they want; the module works out which roles are missing and recruits
online bots to cover them.

## Usage

```
.lfgfill 5      fill my party to 5 before queueing
.lfgfill 3      fill to 3
.lfgfill off    stop filling
.lfgfill now    fill immediately, without queueing
.lfgfill        show the current setting
```

Then queue from the Dungeon Finder pane as normal. The party is assembled first and the
queue goes out as a formed group.

## Why the party size comes from a chat command

The 3.3.5a Dungeon Finder pane is client-side FrameXML shipped inside the client MPQ. No
server-side module can add a control to it — that would need an addon installed by every
player. A chat command needs nothing installed.

The *role* half needs no new input at all. The pane already sends the player's role
checkboxes with `CMSG_LFG_JOIN`, and AzerothCore surfaces them on the
`OnPlayerCanJoinLfg` hook, so the module knows what the player signed up as without asking.

If the player ticked exactly one role box, that is taken as their role. Anything else
(nothing ticked, or several) falls back to their talent spec, since a multi-tick is not a
statement of intent.

## How it decides what is missing

Against the standard 5-man shape — one tank, one healer, the rest damage — counting what
the party already covers.

Bot roles are read with the *same* logic the bot itself will use when LFG asks it to
confirm a role (`LfgJoinAction::GetRoles` in mod-playerbots), not with the more obvious
`AiFactory::GetPlayerRoles`. The two disagree: the latter has no Death Knight case, so a
blood DK reads as damage but answers tank, and it calls every feral druid a tank while the
bot answers damage without Thick Hide. Recruiting against the wrong one assembles a party
that looks right, then fails `LFGMgr::CheckGroupRoles`, and the queue dies silently.

No bot is recruited if it would push the party past 1 tank, 1 healer or 3 damage, for the
same reason — a valid four-man queues, an invalid five-man does not.

If no bot of a needed role is available, the slot is filled with a damage bot rather than
left empty: a party one short of its ideal shape beats one short of its size. The
substitute is recorded as damage, which is what it is, and the player is told the party
went out without a tank or healer.

## Bots it will not take

- already in someone's group
- opposite faction, unless `LfgAutofill.CrossFaction` *and* the core's
  `AllowTwoSide.Interaction.Group` are both on
- in combat, a battleground, or an arena
- inside a dungeon or raid (pulling them out would strand their party)
- below the queueing player's level, or more than `LfgAutofill.LevelsAbove` (default 3)
  above it — the range is one-sided on purpose, so a fill never hands the player someone
  to carry

## Implementation note

The fill cannot happen inside the `OnPlayerCanJoinLfg` hook. `LFGMgr::JoinLfg` captures the
group pointer and queue GUID *before* calling the hook, so a group created or grown there
would be invisible to the rest of the join — a solo player would queue as solo while
actually sitting in a party of five.

Instead the hook cancels the join, the party is filled on the next world tick, and the join
is re-issued against the now-real group. To the player it looks like one queue press.

## Interaction with mod-playerbots

`AiPlayerbot.RandomBotJoinLfg` already puts random bots into the LFG queue, so solo players
eventually get matched anyway. That path is random and unbounded in time and gives no
control over composition; this module is the deterministic version. They coexist, but with
both enabled you may end up matched by whichever gets there first.

## Configuration

See `conf/lfg_autofill.conf.dist`. Autofill is opt-in by default
(`LfgAutofill.DefaultPartySize = 0`) — filling a party for someone who did not ask for it
changes their run.
