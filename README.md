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
the party already covers. Bot roles come from playerbots' own classifier
(`AiFactory::GetPlayerRoles`), so specs are read the same way the bot AI reads them.

If no bot of a needed role is available, the slot is filled with a damage bot rather than
left empty: a party one short of its ideal shape beats one short of its size.

## Bots it will not take

- already in someone's group
- opposite faction
- in combat, a battleground, or an arena
- inside a dungeon or raid (pulling them out would strand their party)
- outside `LfgAutofill.LevelRange` levels of the queueing player

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
