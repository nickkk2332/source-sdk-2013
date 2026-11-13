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

	// Creates a new item straight into a free backpack slot. Returns INVALID_ITEM_ID on failure.
	// nQuality -1 means the item's usual quality.
	itemid_t	GiveItem( const CEconItemDefinition *pItemDef, int nQuality );

	// Creates an item. pAttributes (attribute name -> value text) may be NULL.
	// nUnacknowledgedReason 0 places it in a free backpack slot; otherwise
	// (an UNACK_ITEM_* value) it arrives as a new item the game shows you.
	itemid_t	CreateItem( const CEconItemDefinition *pItemDef, int nQuality, KeyValues *pAttributes,
							eEconItemOrigin eOrigin, int nUnacknowledgedReason );

	// Crates. The backpack UI calls RequestUnlockCrate(); the crate is opened
	// on the next Think(), the way a reply from an item server would arrive.
	// unKeyID is INVALID_ITEM_ID for crates that don't need a key.
	void		RequestUnlockCrate( itemid_t unKeyID, itemid_t unCrateID );
	bool		UnlockCrate( itemid_t unKeyID, itemid_t unCrateID );
	bool		OpenCrateWithAnyKey( itemid_t unCrateID );

	// Attributes are stored by name, as text (e.g. "attach particle effect" "13").
	bool		SetItemAttribute( itemid_t unItemID, const CEconItemAttributeDefinition *pAttrDef, const char *pszValue );
	bool		RemoveItemAttribute( itemid_t unItemID, const CEconItemAttributeDefinition *pAttrDef );
	bool		AddToItemAttribute( itemid_t unItemID, const CEconItemAttributeDefinition *pAttrDef, int nAmount );

	// Name tag / description tag text. NULL or "" removes it.
	bool		SetItemCustomText( itemid_t unItemID, bool bDescription, const char *pszText );
	bool		SetItemQuality( itemid_t unItemID, int nQuality );
	bool		SetItemStyle( itemid_t unItemID, int nStyle );

	// The item as the game sees it (definition and attributes combined). NULL if it doesn't exist.
	const CEconItem				*GetLiveItem( itemid_t unItemID ) const { return FindLiveItem( unItemID ); }
	const CEconItemDefinition	*GetItemDef( itemid_t unItemID ) const;

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
	bool		SetItemAttributeInternal( itemid_t unItemID, const CEconItemAttributeDefinition *pAttrDef, const char *pszValue, bool bNotifyServer );
	void		ApplyPendingMoves();
	void		ApplyPendingUnlocks();
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

	struct PendingUnlock_t
	{
		itemid_t	m_unKeyID;
		itemid_t	m_unCrateID;
	};
	CUtlVector< PendingUnlock_t > m_vecPendingUnlocks;

	// How many items sit in each backpack slot (index 0 = not placed yet).
	// A count rather than a flag because two items share a slot mid-swap.
	int			m_nSlotUseCount[ MAX_NUM_BACKPACK_SLOTS + 1 ];
};

CTFOfflineInventory &TFOfflineInventory();

// Helpers shared with the other offline inventory console commands.
void		OfflineInventory_GetItemDefDisplayName( const CEconItemDefinition *pItemDef, char *pszOut, int nOutSize );
void		OfflineInventory_FindItemDefs( const char *pszQuery, CUtlVector< const CEconItemDefinition * > &vecOut );
itemid_t	OfflineInventory_ParseExistingItemID( const char *pszArg );	// prints why on failure

#endif // TF_OFFLINE_INVENTORY_H
