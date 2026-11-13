//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Offline inventory: a backpack that lives in a save file on this
//			machine instead of on Valve's item servers.
//
//			The save file (cfg/offline_inventory.txt in the mod folder) is the
//			source of truth. On startup it is turned into the game's normal
//			in-memory backpack, so the regular backpack and loadout screens work
//			on it. Every change made here is applied to that live backpack and
//			written back to the file.
//
//			Which items are equipped is still stored by the existing local
//			loadout file (cfg/local_loadout.txt).
//
//=============================================================================

#ifndef TF_OFFLINE_INVENTORY_H
#define TF_OFFLINE_INVENTORY_H
#ifdef _WIN32
#pragma once
#endif

#include "econ_item_constants.h"
#include "steam/steamclientpublic.h"
#include "utlvector.h"

class KeyValues;
class CEconItem;
class CEconItemDefinition;
class CEconItemAttributeDefinition;

namespace GCSDK
{
	class CGCClientSharedObjectCache;
}

class CTFOfflineInventory
{
public:
	CTFOfflineInventory();
	~CTFOfflineInventory();

	// Called every frame. Loads the save file once Steam, the item schema and
	// the local backpack are ready, and writes pending changes to disk.
	void		Think();
	void		Shutdown();

	bool		IsLoaded() const { return m_bLoaded; }

	// Creates a new item in the backpack. Returns INVALID_ITEM_ID on failure.
	itemid_t	GiveItem( const CEconItemDefinition *pItemDef, int nQuality );

	// Attributes are stored by name, as text (e.g. "attach particle effect" "13").
	bool		SetItemAttribute( itemid_t unItemID, const CEconItemAttributeDefinition *pAttrDef, const char *pszValue );
	bool		RemoveItemAttribute( itemid_t unItemID, const CEconItemAttributeDefinition *pAttrDef );

	bool		DeleteItem( itemid_t unItemID );
	int			DeleteAllItems();

	// Called when the backpack UI moves an item (or acknowledges a new one).
	bool		SetItemPosition( itemid_t unItemID, uint32 unBackendPosition );

	// Wrap many changes in these to rebuild the live backpack once at the end
	// instead of once per item (e.g. giving thousands of items at once).
	void		BeginBulkChange();
	void		EndBulkChange();

	int			GetItemCount() const;
	int			GetFreeSlotCount() const;
	itemid_t	GetLastGivenItemID() const { return m_unLastGivenItemID; }

	// The "items" block of the save file, for listing. May be NULL before loading.
	KeyValues	*GetItemsKV() const;
	KeyValues	*FindItemKV( itemid_t unItemID ) const;

	// Adds every equipped item, base64-encoded, under an "items" key for the server.
	void		WriteEquippedItemsForServer( KeyValues *pKV ) const;

private:
	bool		Load();
	void		Save();
	bool		Resubscribe();
	void		ApplyPendingMoves();
	bool		ApplyItemPosition( itemid_t unItemID, uint32 unBackendPosition );

	bool		BuildEconItem( KeyValues *pItemKV, CEconItem *pItem ) const;
	bool		CreateLiveItem( KeyValues *pItemKV );
	bool		UpdateLiveItem( KeyValues *pItemKV );
	CEconItem	*FindLiveItem( itemid_t unItemID ) const;
	GCSDK::CGCClientSharedObjectCache *GetLiveSOCache() const;
	int			FindFreeBackpackSlot() const;
	void		NotifyEquipmentChanged();

	CSteamID	m_steamID;
	KeyValues	*m_pData;
	bool		m_bLoaded;
	bool		m_bDirty;
	bool		m_bSaveDisabled;		// set if the save file existed but couldn't be read
	int			m_nBulkChangeDepth;
	bool		m_bBulkChangePending;
	itemid_t	m_unNextItemID;
	itemid_t	m_unLastGivenItemID;
	uint64		m_ulCacheVersion;

	struct PendingMove_t
	{
		itemid_t	m_unItemID;
		uint32		m_unBackendPosition;
	};
	CUtlVector< PendingMove_t > m_vecPendingMoves;

	// How many items sit in each backpack slot (index 0 = not placed yet).
	// A count rather than a flag because two items share a slot mid-swap.
	int			m_nSlotUseCount[ MAX_NUM_BACKPACK_SLOTS + 1 ];
};

CTFOfflineInventory &TFOfflineInventory();

#endif // TF_OFFLINE_INVENTORY_H
