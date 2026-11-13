//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Offline crafting: what Valve's item servers did when you pressed
//			"Craft" in the crafting window.
//
//			Recipes come from the game's own item schema (the same list the
//			crafting window shows). The items are checked against the recipe,
//			used up, and the recipe's results are added to the backpack as
//			new "crafted" items. Recipes with a random result (Fabricate
//			Headgear, Fabricate Class Weapon, ...) pick from the items the
//			schema marks as craftable, the way the item servers did.
//
//=============================================================================

#ifndef TF_OFFLINE_CRAFTING_H
#define TF_OFFLINE_CRAFTING_H
#ifdef _WIN32
#pragma once
#endif

// Carries out a craft request (k_EMsgGCCraft: a MsgGCCraft_t followed by the
// item IDs) and answers the crafting window the way the item servers would.
void OfflineCrafting_ProcessCraftRequest( const void *pBody, uint32 cubBody );

#endif // TF_OFFLINE_CRAFTING_H
