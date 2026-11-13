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
| `inv_give_all [all \| weapons \| cosmetics \| taunts \| tools]` | Add one of every item you don't already have. `tools` gives paint cans, name tags, strange parts, Strangifiers, Killstreak Kits and the like. |
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

## Tools

Tools work from the backpack like in real TF2: pick the tool, then the item.
Get a set with `inv_give_all tools`, or one at a time with `inv_give`.

- **Paint cans**: paint an item (team paints too).
- **Name Tag / Description Tag**: rename an item or give it a description. You
  can name stock weapons too; that gives you your own copy to name.
- **Strange parts**: add a counter to a Strange item (up to three).
- **Strange filters**: make a counter only count on one map, e.g.
  `inv_give "Strange Filter: Badwater"`.
- **Strange Count Transfer Tool**: move counts from one item to another of the same kind.
- **Strangifiers**: make the item they're for Strange.
- **Killstreak Kits**: add a killstreak to the weapon they're for. Specialized and
  Professional kits from `inv_give` pick their sheen (and eye effect) when used.
- **Festivizer**: festivize a weapon.
- **Taunt Unusualifier**: give a taunt an unusual effect.

The backpack's **Restore** options work too: remove paint, names, descriptions,
strange parts, killstreaks or festivizing, and reset strange counters. So does
picking an item's **style**.

## Strange counters

Strange items count kills (and whatever their strange parts track) while you
play, and the counts are saved with your items. Kills on bots count, which real
TF2 doesn't do; turn that off with `inv_strange_counts_bots 0`.

## Mann Co. Store

Open the store from the main menu's store button (or `open_store` in the
console). Everything is for sale: every weapon, cosmetic, taunt and bundle,
plus keys, cases and the tools above. Checkout pays from a fake wallet that
starts at $100, and your purchases show up as new items.

| Command | What it does |
| --- | --- |
| `inv_wallet` | Show your balance. |
| `inv_wallet_add <dollars>` | Add money, e.g. `inv_wallet_add 50`. |
| `inv_wallet_set <dollars>` | Set your balance. |
| `inv_store_sale <percent>` | Put everything on sale (0 to 90% off). |
| `inv_store_reload` | Reload prices after editing the price file. |

Valve's real prices only ever lived on their servers, so the store uses flat
defaults: weapons $0.99, hats $7.99, other cosmetics $4.99, taunts $5.99,
bundles $19.99, keys $2.49, paint $1.99, cases $0.99. Change any of them in
`game/mod_tf/cfg/offline_store_prices.txt` (create it), then run
`inv_store_reload`:

```
"offline_store_prices"
{
	"Team Captain"				"14.99"
	"Mann Co. Supply Crate Key"	"0"		// 0 = not for sale
}
```

Items can be named by their English name, internal name or number. The
Featured tab picks 12 cosmetics and taunts each day.

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

## Crafting

Open Items, then Crafting, pick a recipe, add the items and press Craft, like in
real TF2. The items are used up and what you made shows up as new items. Every
recipe the game ships with works, including:

- **Metal**: smelt two weapons for the same class into Scrap, 3 Scrap into
  Reclaimed, 3 Reclaimed into Refined, and back down again.
- **Tokens**: smelt three weapons for the same class (or same slot) into a class
  (or slot) token.
- **Fabricate Class Weapon**: Scrap + class token + slot token gives a random
  weapon for that class and slot.
- **Fabricate Headgear**: 3 Refined gives a random hat. 4 Refined + a class token
  gives a random hat for that class.
- **Weapon blueprints** (Pain Train, Homewrecker, Gunboats, ...), the Saxton Hale
  mask, the Headtaker and the Strange Count Transfer Tool.

The **Custom** recipe at the bottom of the Special list tries your items against
every recipe, like it used to.

When a recipe gives something random, it picks from the items the game marks as
craftable, leaving out stock weapons, war paints, festives and other limited
editions, promo all-class melees (Saxxy, Bat Outta Hell, ...), and Halloween-only
cosmetics, which only show up during Halloween. Valve's exact lists only ever
lived on their servers, so this is a close match, not an exact one.

Items you can't use in crafting, as in real TF2: anything marked "Not Usable in
Crafting", Community, Valve and Self-Made items, and weapons bought from the
store (store-bought cosmetics are fine). Items from `inv_give`, crates and
crafting can all be crafted.

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

- Trading and gifts still expect Valve's servers, so they do nothing.
- The store doesn't sell map stamps, and "try it on" previews don't work.
- Bonus items that need extra setup (the Taunt Unusualifier, gifts) don't drop
  from cases yet.
- "Your Strange item reached a new rank" messages don't appear; the rank in the
  item's name does change.
- Backpack "sort" does nothing yet; dragging items around does work.
