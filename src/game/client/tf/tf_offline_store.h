//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Offline Mann Co. Store: a price list built from the item schema,
//			and purchases paid from a fake wallet in the offline inventory.
//
//			The store UI is Valve's; it normally gets its price list from the
//			item servers and checks out through the Steam overlay. Offline, the
//			price list is built here and checkout charges the wallet instead.
//
//=============================================================================

#ifndef TF_OFFLINE_STORE_H
#define TF_OFFLINE_STORE_H
#ifdef _WIN32
#pragma once
#endif

class KeyValues;
class CEconItemDefinition;

// Builds a price list in the format the store expects. The caller owns it.
KeyValues	*OfflineStore_CreatePriceSheet();

// Adds the items for a purchase to the backpack as new "purchased" items
// (a bundle gives its contents). Returns how many items were created.
int			OfflineStore_GiveItem( const CEconItemDefinition *pItemDef, int nCount );

// "$12.34"
void		OfflineStore_FormatMoney( int nCents, char *pszOut, int nOutSize );

#endif // TF_OFFLINE_STORE_H
