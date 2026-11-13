# Offline inventory

This build doesn't talk to Valve's item servers. Your backpack lives in a save
file on your PC, and you fill it with console commands. Everything in the real
game's item list is available: every weapon, hat, taunt, paint and unusual effect.

None of this touches your real TF2 inventory, and it isn't compatible with real
TF2 servers.

## Playing

1. Build and launch the mod as described in the main README. Keep Steam running:
   the mod loads TF2's models and item list through it.
2. Open the console (enable it in Options > Advanced if needed).
3. Give yourself some items (see below), then open your loadout from the main
   menu and equip them like normal.
4. Start a server from the main menu ("Create Server"), or with
   `map cp_badlands` in the console, and add bots with `tf_bot_add`.

Changing your loadout while standing in your team's spawn room respawns you
wearing it, like real TF2 (turn this off with `tf_respawn_on_loadoutchanges 0`).

## Commands

| Command | What it does |
| --- | --- |
| `inv_give <item> [quality] [effect]` | Add an item. `<item>` can be a name or part of one (`"team captain"`) or an item number. |
| `inv_give_all [all \| weapons \| cosmetics \| taunts]` | Add one of every item you don't already have. |
| `inv_search <text>` | Find items by name and show their numbers. |
| `inv_list [text]` | List your items with their item IDs. |
| `inv_effects [text]` | List unusual effects and their numbers. |
| `inv_paint <id \| last> <paint can>` | Paint an item, e.g. `inv_paint last "Team Spirit"`. |
| `inv_setattr <id \| last> <attribute> <value>` | Set any attribute on an item. |
| `inv_removeattr <id \| last> <attribute>` | Remove an attribute you set. |
| `inv_delete <id \| last>` | Delete an item. |
| `inv_wipe confirm` | Delete everything and reset your loadouts. |

`last` always means the item you most recently gave yourself.

Qualities: `normal`, `genuine`, `vintage`, `unusual`, `unique`, `community`,
`valve`, `selfmade`, `strange`, `haunted`, `collectors`, `decorated`.

### Examples

```
inv_give "Team Captain" unusual 13
inv_paint last "Team Spirit"

inv_give "Rocket Launcher" strange
inv_setattr last "killstreak tier" 3

inv_give_all cosmetics
```

Attribute names are the ones in TF2's `scripts/items/items_game.txt`. If a name
is wrong, the command says so and changes nothing.

## Crates

Crates and cases open from the backpack like in real TF2: use the key on the
crate (or open a keyless case), and you get the unboxing reveal.

| Command | What it does |
| --- | --- |
| `inv_crates [text]` | List crate series numbers and what each crate is called. |
| `inv_givecrate <series or name> [how many]` | Add crates, plus a matching key for each. |
| `inv_opencrate <id \| last>` | Open a crate from the console instead, using a key you have. |

```
inv_crates cosmetic
inv_givecrate 151 10
inv_givecrate 1
```

What comes out is decided the way Valve's item servers did it, using the
contents lists that ship with the game:

- **Cases** roll a grade first: Mercenary about 80%, Commando 16%, Assassin 3.2%,
  Elite 0.6%. Then there's a chance the item is Strange, a chance a hat is
  Unusual (with that case's effects), and a chance of bonus items.
- **Old Mann Co. Supply Crates** that say "or an Exceedingly Rare Special Item!"
  can give an Unusual hat with that crate's effect series.
- **Strange crates** give Strange weapons; killstreak and war paint crates add
  their sheens and wear.

The game's copy of the item data has the real odds stripped out, so you can
tune them (each is a number from 0 to 1, saved between sessions):

| Setting | Default | Meaning |
| --- | --- | --- |
| `inv_crate_unusual_chance` | `0.01` | Chance of an Unusual where the crate can give one. |
| `inv_crate_strange_chance` | `0.1` | Chance an item from a case is Strange. |
| `inv_crate_bonus_chance` | `0.05` | Chance of each possible bonus item from a case. |

Set `inv_crate_unusual_chance 1` for a night of nothing but Unusuals.

About 25 old event crates (for example the 2010 Winter Crate and the 2013 Robo
Crate) list contents that only ever existed on Valve's servers. Those don't
appear in `inv_crates` and can't be opened.

## The save file

Your items are saved to `game/mod_tf/cfg/offline_inventory.txt`. It's plain text
and safe to edit while the game is closed. Each item looks like this (the
numbers are just an example):

```
"7"
{
	"def"		"1234"
	"name"		"<the item's internal name>"
	"quality"	"5"
	"level"		"57"
	"origin"	"0"
	"slot"		"7"
	"attributes"
	{
		"attach particle effect"	"13"
	}
}
```

- `def` is the item number (see `inv_search`); `name` is only used if that number
  stops existing in a future game update.
- `slot` is the backpack slot (1 to 4000); `0` means "new, not placed yet"
  (`new_reason` then says how it arrived, e.g. found in a crate).
- You can also add `"custom_name"` and `"custom_desc"` to an item.

If the file can't be read when the game starts, you'll get an empty backpack and
a warning, and the game won't overwrite the file until you fix or remove it.

What's equipped is stored separately, in `game/mod_tf/cfg/local_loadout.txt`.

## Not done yet

- Strange counters don't go up: kills aren't recorded anywhere yet.
- The other in-game tools (paint cans, name tags, strange parts, crafting, the
  store) still expect Valve's servers, so using them from the backpack does
  nothing. Use the commands above instead for now.
- Bonus items that need a tool to be set up first (the Taunt Unusualifier,
  gifts) don't drop from cases yet.
- Backpack "sort" does nothing yet; dragging items around does work.
