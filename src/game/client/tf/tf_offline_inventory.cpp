//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Offline inventory. See tf_offline_inventory.h.
//
//			Save file layout (cfg/offline_inventory.txt):
//
//			"offline_inventory"
//			{
//				"next_item_id"	"4"
//				"items"
//				{
//					"3"									// item ID
//					{
//						"def"		"30"				// item definition index
//						"name"		"..."				// definition name; only used if the index stops existing
//						"quality"	"5"
//						"level"		"10"
//						"origin"	"0"
//						"slot"		"12"				// backpack slot, 1-based; 0 = new, not yet placed
//						"style"		"0"					// optional
//						"custom_name"	"..."			// optional
//						"custom_desc"	"..."			// optional
//						"attributes"					// optional, by attribute name
//						{
//							"attach particle effect"	"13"
//						}
//					}
//				}
//			}
//
//			Numbers are kept small on purpose: KeyValues mangles numbers above
//			2^31 when it reads a text file on 64-bit Linux.
//
//=============================================================================

#include "cbase.h"
#include "tf_offline_inventory.h"
#include "tf_offline_inventory_shared.h"
#include "tf_offline_crates.h"
#include "tf_offline_tools.h"
#include "tool_items/decoder_ring_tool.h"
#include "tf_item_inventory.h"
#include "tf_item_system.h"
#include "tf_gc_client.h"
#include "gc_clientsystem.h"
#include "econ_item.h"
#include "econ_item_schema.h"
#include "econ_game_account_client.h"
#include "localization_provider.h"
#include "base_gcmessages.pb.h"
#include "gcsdk/gcsdk_auto.h"
#include "filesystem.h"
#include "KeyValues.h"
#include "vstdlib/random.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define OFFLINE_INVENTORY_FILE		"cfg/offline_inventory.txt"
#define OFFLINE_INVENTORY_PATH_ID	"MOD"

// Everyone gets the biggest backpack the game supports.
static const int kOfflineBackpackSlots = MAX_NUM_BACKPACK_SLOTS;

static CTFOfflineInventory g_TFOfflineInventory;
CTFOfflineInventory &TFOfflineInventory()
{
	return g_TFOfflineInventory;
}

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------
static const char *GetQualityDisplayName( int nQuality );

static itemid_t ItemIDFromKV( KeyValues *pItemKV )
{
	return V_atoui64( pItemKV->GetName() );
}

static int SlotFromKV( KeyValues *pItemKV )
{
	const int iSlot = pItemKV->GetInt( "slot", 0 );
	return ( iSlot > 0 && iSlot <= kOfflineBackpackSlots ) ? iSlot : 0;
}

static bool SerializeEconItem( const CEconItem &item, std::string &strOut )
{
	CSOEconItem msgItem;
	item.SerializeToProtoBufItem( msgItem );
	return msgItem.SerializeToString( &strOut );
}

// Parses pszValue the way the attribute's type expects and puts it on the item.
static bool ApplyAttributeFromString( CEconItem *pItem, const CEconItemAttributeDefinition *pAttrDef, const char *pszValue )
{
	const ISchemaAttributeType *pAttrType = pAttrDef->GetAttributeType();
	if ( !pAttrType )
		return false;

	attribute_data_union_t value;
	pAttrType->InitializeNewEconAttributeValue( &value );

	const bool bParsed = pAttrType->BConvertStringToEconAttributeValue( pAttrDef, pszValue, &value );
	if ( bParsed && pItem )
	{
		pAttrType->LoadEconAttributeValue( pItem, pAttrDef, value );
	}

	pAttrType->UnloadEconAttributeValue( &value );
	return bParsed;
}

static const CEconItemDefinition *FindItemDefForKV( KeyValues *pItemKV )
{
	const CEconItemDefinition *pItemDef = GetItemSchema()->GetItemDefinition( pItemKV->GetInt( "def", -1 ) );
	if ( !pItemDef )
	{
		// The index may have changed between game versions; fall back to the name.
		const char *pszName = pItemKV->GetString( "name", NULL );
		if ( pszName && pszName[0] )
		{
			pItemDef = GetItemSchema()->GetItemDefinitionByName( pszName );
		}
	}
	return pItemDef;
}

//-----------------------------------------------------------------------------
CTFOfflineInventory::CTFOfflineInventory()
	: m_pData( NULL )
	, m_bLoaded( false )
	, m_bDirty( false )
	, m_bSaveDisabled( false )
	, m_nBulkChangeDepth( 0 )
	, m_bBulkChangePending( false )
	, m_unNextItemID( 1 )
	, m_unLastGivenItemID( INVALID_ITEM_ID )
	, m_ulCacheVersion( 0 )
{
	V_memset( m_nSlotUseCount, 0, sizeof( m_nSlotUseCount ) );
}

CTFOfflineInventory::~CTFOfflineInventory()
{
	// Deliberately not freeing m_pData here: this object is a static, and by
	// the time static destructors run the KeyValues system may be gone.
	// Shutdown() does the cleanup.
}

//-----------------------------------------------------------------------------
void CTFOfflineInventory::Think()
{
	if ( !m_bLoaded )
	{
		Load();
		return;
	}

	ApplyPendingUnlocks();
	OfflineTools_ProcessPendingRequests();
	ApplyPendingMoves();

	if ( m_bDirty )
	{
		Save();
	}
}

//-----------------------------------------------------------------------------
void CTFOfflineInventory::Shutdown()
{
	if ( m_bDirty )
	{
		Save();
	}

	if ( m_pData )
	{
		m_pData->deleteThis();
		m_pData = NULL;
	}
	m_bLoaded = false;
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::Load()
{
	if ( m_bLoaded )
		return true;

	if ( !steamapicontext || !steamapicontext->SteamUser() || !g_pFullFileSystem )
		return false;

	const CSteamID steamID = steamapicontext->SteamUser()->GetSteamID();
	if ( !steamID.IsValid() )
		return false;

	// Need the item schema to turn definition indices into items.
	if ( !GetItemSchema() || GetItemSchema()->GetItemDefinitionMap().Count() == 0 )
		return false;

	// Wait until the local backpack is listening for our items, so it sees them arrive.
	CPlayerInventory *pLocalInventory = TFInventoryManager()->GetLocalInventory();
	if ( !pLocalInventory || pLocalInventory->GetOwner() != steamID )
		return false;

	KeyValues *pData = new KeyValues( "offline_inventory" );
	if ( g_pFullFileSystem->FileExists( OFFLINE_INVENTORY_FILE, OFFLINE_INVENTORY_PATH_ID ) )
	{
		if ( !pData->LoadFromFile( g_pFullFileSystem, OFFLINE_INVENTORY_FILE, OFFLINE_INVENTORY_PATH_ID ) )
		{
			// Never overwrite a save we couldn't read; the items in it would be lost.
			Warning( "Offline inventory: couldn't read %s. Starting with an empty backpack and NOT saving changes. Fix or remove the file, then restart.\n", OFFLINE_INVENTORY_FILE );
			pData->deleteThis();
			pData = new KeyValues( "offline_inventory" );
			m_bSaveDisabled = true;
		}
	}

	m_pData = pData;
	m_steamID = steamID;
	KeyValues *pItemsKV = m_pData->FindKey( "items", true );

	// Work out the next free item ID and which backpack slots are taken.
	itemid_t unHighestID = 0;
	V_memset( m_nSlotUseCount, 0, sizeof( m_nSlotUseCount ) );
	FOR_EACH_TRUE_SUBKEY( pItemsKV, pItemKV )
	{
		unHighestID = MAX( unHighestID, ItemIDFromKV( pItemKV ) );
		m_nSlotUseCount[ SlotFromKV( pItemKV ) ]++;
	}

	m_unNextItemID = MAX( (itemid_t)m_pData->GetInt( "next_item_id", 1 ), unHighestID + 1 );
	m_pData->SetInt( "next_item_id", (int)m_unNextItemID );

	m_bLoaded = true;
	Resubscribe();

	Msg( "Offline inventory: loaded %d item(s) from %s.\n", GetItemCount(), OFFLINE_INVENTORY_FILE );
	return true;
}

//-----------------------------------------------------------------------------
void CTFOfflineInventory::Save()
{
	m_bDirty = false;

	if ( !m_pData || m_bSaveDisabled )
		return;

	g_pFullFileSystem->CreateDirHierarchy( "cfg", OFFLINE_INVENTORY_PATH_ID );
	if ( !m_pData->SaveToFile( g_pFullFileSystem, OFFLINE_INVENTORY_FILE, OFFLINE_INVENTORY_PATH_ID ) )
	{
		Warning( "Offline inventory: failed to write %s.\n", OFFLINE_INVENTORY_FILE );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Rebuild the whole live backpack from the save data in one go.
//-----------------------------------------------------------------------------
bool CTFOfflineInventory::Resubscribe()
{
	if ( !m_bLoaded )
		return false;

	COfflineSOCacheMsgBuilder builder;

	int nSkipped = 0;
	FOR_EACH_TRUE_SUBKEY( GetItemsKV(), pItemKV )
	{
		CEconItem item;
		if ( BuildEconItem( pItemKV, &item ) )
		{
			builder.AddItem( item );
		}
		else
		{
			nSkipped++;
		}
	}

	if ( nSkipped )
	{
		Warning( "Offline inventory: %d item(s) in %s use item definitions this version of the game doesn't have. They are kept in the file but hidden.\n", nSkipped, OFFLINE_INVENTORY_FILE );
	}

	// Account record: this is what makes the backpack bigger than the default 300 slots.
	CSOEconGameAccountClient msgAccount;
	msgAccount.set_additional_backpack_slots( kOfflineBackpackSlots - DEFAULT_NUM_BACKPACK_SLOTS );
	std::string strAccount;
	msgAccount.SerializeToString( &strAccount );
	builder.AddObject( k_EEconTypeGameAccountClient, strAccount );

	CUtlBuffer bufMsg;
	builder.Serialize( m_steamID.ConvertToUint64(), ++m_ulCacheVersion, bufMsg );

	if ( !GCClientSystem()->GetGCClient()->AddLocalSOCache( m_steamID, bufMsg.Base(), bufMsg.TellPut() ) )
	{
		Warning( "Offline inventory: the game rejected the backpack data.\n" );
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
void CTFOfflineInventory::BeginBulkChange()
{
	m_nBulkChangeDepth++;
}

void CTFOfflineInventory::EndBulkChange()
{
	Assert( m_nBulkChangeDepth > 0 );
	if ( --m_nBulkChangeDepth > 0 )
		return;

	if ( m_bBulkChangePending )
	{
		m_bBulkChangePending = false;
		Resubscribe();
	}
}

//-----------------------------------------------------------------------------
KeyValues *CTFOfflineInventory::GetItemsKV() const
{
	return m_pData ? m_pData->FindKey( "items" ) : NULL;
}

KeyValues *CTFOfflineInventory::FindItemKV( itemid_t unItemID ) const
{
	KeyValues *pItemsKV = GetItemsKV();
	if ( !pItemsKV || unItemID == INVALID_ITEM_ID )
		return NULL;

	return pItemsKV->FindKey( CFmtStr( "%llu", (unsigned long long)unItemID ) );
}

int CTFOfflineInventory::GetItemCount() const
{
	int nCount = 0;
	KeyValues *pItemsKV = GetItemsKV();
	if ( pItemsKV )
	{
		FOR_EACH_TRUE_SUBKEY( pItemsKV, pItemKV )
		{
			nCount++;
		}
	}
	return nCount;
}

int CTFOfflineInventory::GetFreeSlotCount() const
{
	int nFree = 0;
	for ( int i = 1; i <= kOfflineBackpackSlots; ++i )
	{
		if ( m_nSlotUseCount[i] == 0 )
			nFree++;
	}
	return nFree;
}

int CTFOfflineInventory::FindFreeBackpackSlot() const
{
	for ( int i = 1; i <= kOfflineBackpackSlots; ++i )
	{
		if ( m_nSlotUseCount[i] == 0 )
			return i;
	}
	return 0;
}

//-----------------------------------------------------------------------------
GCSDK::CGCClientSharedObjectCache *CTFOfflineInventory::GetLiveSOCache() const
{
	if ( !m_bLoaded )
		return NULL;
	return GCClientSystem()->GetSOCache( m_steamID );
}

CEconItem *CTFOfflineInventory::FindLiveItem( itemid_t unItemID ) const
{
	GCSDK::CGCClientSharedObjectCache *pSOCache = GetLiveSOCache();
	if ( !pSOCache )
		return NULL;

	GCSDK::CGCClientSharedObjectTypeCache *pTypeCache = pSOCache->FindTypeCache( CEconItem::k_nTypeID );
	if ( !pTypeCache )
		return NULL;

	CEconItem soIndex;
	soIndex.SetItemID( unItemID );
	return (CEconItem *)pTypeCache->FindSharedObject( soIndex );
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::BuildEconItem( KeyValues *pItemKV, CEconItem *pItem ) const
{
	const CEconItemDefinition *pItemDef = FindItemDefForKV( pItemKV );
	if ( !pItemDef )
		return false;

	const itemid_t unItemID = ItemIDFromKV( pItemKV );
	const int iSlot = SlotFromKV( pItemKV );

	pItem->SetItemID( unItemID );
	pItem->SetOriginalID( unItemID );
	pItem->SetAccountID( m_steamID.GetAccountID() );
	pItem->SetDefinitionIndex( pItemDef->GetDefinitionIndex() );
	pItem->SetItemLevel( pItemKV->GetInt( "level", 1 ) );
	pItem->SetQuality( pItemKV->GetInt( "quality", AE_UNIQUE ) );
	pItem->SetOrigin( (eEconItemOrigin)pItemKV->GetInt( "origin", kEconItemOrigin_Drop ) );
	pItem->SetFlags( 0 );
	pItem->SetStyle( pItemKV->GetInt( "style", 0 ) );
	int nNewReason = pItemKV->GetInt( "new_reason", UNACK_ITEM_DROPPED );
	if ( nNewReason <= UNACK_ITEM_UNKNOWN || nNewReason >= UNACK_NUM_METHODS )
	{
		nNewReason = UNACK_ITEM_DROPPED;
	}
	pItem->SetInventoryToken( iSlot > 0
		? ( kBackendPosition_NewFormat | (uint32)iSlot )
		: GetUnacknowledgedPositionFor( (unacknowledged_item_inventory_positions_t)nNewReason ) );

	const char *pszCustomName = pItemKV->GetString( "custom_name", "" );
	if ( pszCustomName[0] )
	{
		pItem->SetCustomName( pszCustomName );
	}

	const char *pszCustomDesc = pItemKV->GetString( "custom_desc", "" );
	if ( pszCustomDesc[0] )
	{
		pItem->SetCustomDesc( pszCustomDesc );
	}

	KeyValues *pAttributesKV = pItemKV->FindKey( "attributes" );
	if ( pAttributesKV )
	{
		FOR_EACH_VALUE( pAttributesKV, pAttrKV )
		{
			const CEconItemAttributeDefinition *pAttrDef = GetItemSchema()->GetAttributeDefinitionByName( pAttrKV->GetName() );
			if ( !pAttrDef || !ApplyAttributeFromString( pItem, pAttrDef, pAttrKV->GetString() ) )
			{
				Warning( "Offline inventory: item %llu has an attribute this game doesn't understand: \"%s\" \"%s\"\n",
					(unsigned long long)unItemID, pAttrKV->GetName(), pAttrKV->GetString() );
			}
		}
	}

	return true;
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::CreateLiveItem( KeyValues *pItemKV )
{
	if ( m_nBulkChangeDepth > 0 )
	{
		m_bBulkChangePending = true;
		return true;
	}

	GCSDK::CGCClientSharedObjectCache *pSOCache = GetLiveSOCache();
	if ( !pSOCache )
		return false;

	CEconItem item;
	if ( !BuildEconItem( pItemKV, &item ) )
		return false;

	std::string strData;
	if ( !SerializeEconItem( item, strData ) )
		return false;

	return pSOCache->BCreateFromMsg( CEconItem::k_nTypeID, strData.data(), (uint32)strData.size() );
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::UpdateLiveItem( KeyValues *pItemKV )
{
	if ( m_nBulkChangeDepth > 0 )
	{
		m_bBulkChangePending = true;
		return true;
	}

	GCSDK::CGCClientSharedObjectCache *pSOCache = GetLiveSOCache();
	if ( !pSOCache )
		return false;

	CEconItem item;
	if ( !BuildEconItem( pItemKV, &item ) )
		return false;

	// An update replaces the whole item, so carry over where it is equipped.
	// (That lives in the local loadout file, not in our save data.)
	const CEconItem *pLiveItem = FindLiveItem( item.GetItemID() );
	if ( pLiveItem )
	{
		for ( int i = 0; i < pLiveItem->GetEquippedInstanceCount(); ++i )
		{
			const CEconItem::EquippedInstance_t &instance = pLiveItem->GetEquippedInstance( i );
			item.Equip( instance.m_unEquippedClass, instance.m_unEquippedSlot );
		}
	}

	std::string strData;
	if ( !SerializeEconItem( item, strData ) )
		return false;

	return pSOCache->BUpdateFromMsg( CEconItem::k_nTypeID, strData.data(), (uint32)strData.size() );
}

//-----------------------------------------------------------------------------
// Purpose: Tell whatever server we're on that our equipped items may have changed.
//-----------------------------------------------------------------------------
void CTFOfflineInventory::NotifyEquipmentChanged()
{
	GTFGCClientSystem()->LocalInventoryChanged();
}

//-----------------------------------------------------------------------------
itemid_t CTFOfflineInventory::GiveItem( const CEconItemDefinition *pItemDef, int nQuality )
{
	return CreateItem( pItemDef, nQuality, NULL, kEconItemOrigin_Drop, 0 );
}

//-----------------------------------------------------------------------------
itemid_t CTFOfflineInventory::CreateItem( const CEconItemDefinition *pItemDef, int nQuality, KeyValues *pAttributes,
										  eEconItemOrigin eOrigin, int nUnacknowledgedReason )
{
	if ( !m_bLoaded || !pItemDef )
		return INVALID_ITEM_ID;

	// New items wait in slot 0 until the game shows them to you and places them.
	int iSlot = 0;
	if ( nUnacknowledgedReason == 0 )
	{
		iSlot = FindFreeBackpackSlot();
		if ( !iSlot )
		{
			Warning( "Offline inventory: the backpack is full (%d slots).\n", kOfflineBackpackSlots );
			return INVALID_ITEM_ID;
		}
	}

	// Default to the quality the item normally comes in, and Unique if that's Normal or unset.
	if ( nQuality < 0 )
	{
		nQuality = pItemDef->GetQuality();
		if ( nQuality <= AE_NORMAL || nQuality >= AE_MAX_TYPES )
		{
			nQuality = AE_UNIQUE;
		}
	}

	int nMinLevel = pItemDef->GetMinLevel();
	int nMaxLevel = pItemDef->GetMaxLevel();
	if ( nMaxLevel < nMinLevel )
	{
		V_swap( nMinLevel, nMaxLevel );
	}
	if ( nMaxLevel <= 0 )
	{
		nMinLevel = nMaxLevel = 1;
	}

	const itemid_t unItemID = m_unNextItemID;

	KeyValues *pItemKV = new KeyValues( CFmtStr( "%llu", (unsigned long long)unItemID ) );
	pItemKV->SetInt( "def", pItemDef->GetDefinitionIndex() );
	pItemKV->SetString( "name", pItemDef->GetDefinitionName() );
	pItemKV->SetInt( "quality", nQuality );
	pItemKV->SetInt( "level", RandomInt( MAX( nMinLevel, 1 ), nMaxLevel ) );
	pItemKV->SetInt( "origin", eOrigin );
	pItemKV->SetInt( "slot", iSlot );
	if ( nUnacknowledgedReason != 0 )
	{
		pItemKV->SetInt( "new_reason", nUnacknowledgedReason );
	}

	if ( pAttributes )
	{
		KeyValues *pAttributesCopy = pAttributes->MakeCopy();
		pAttributesCopy->SetName( "attributes" );
		pItemKV->AddSubKey( pAttributesCopy );
	}

	// Strange items need a kill counter to show their rank.
	if ( nQuality == AE_STRANGE )
	{
		const CEconItemAttributeDefinition *pKillEater = GetItemSchema()->GetAttributeDefinitionByName( "kill eater" );
		KeyValues *pAttributesKV = pItemKV->FindKey( "attributes", true );
		if ( pKillEater && !::FindAttribute( pItemDef, pKillEater ) && !pAttributesKV->FindKey( pKillEater->GetDefinitionName() ) )
		{
			pAttributesKV->SetString( pKillEater->GetDefinitionName(), "0" );
		}
	}

	GetItemsKV()->AddSubKey( pItemKV );

	if ( !CreateLiveItem( pItemKV ) )
	{
		GetItemsKV()->RemoveSubKey( pItemKV );
		pItemKV->deleteThis();
		Warning( "Offline inventory: couldn't create %s.\n", pItemDef->GetDefinitionName() );
		return INVALID_ITEM_ID;
	}

	m_nSlotUseCount[iSlot]++;
	m_unNextItemID++;
	m_pData->SetInt( "next_item_id", (int)m_unNextItemID );
	m_unLastGivenItemID = unItemID;
	m_bDirty = true;

	return unItemID;
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::SetItemAttribute( itemid_t unItemID, const CEconItemAttributeDefinition *pAttrDef, const char *pszValue )
{
	return SetItemAttributeInternal( unItemID, pAttrDef, pszValue, true );
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::SetItemAttributeInternal( itemid_t unItemID, const CEconItemAttributeDefinition *pAttrDef, const char *pszValue, bool bNotifyServer )
{
	KeyValues *pItemKV = FindItemKV( unItemID );
	if ( !pItemKV || !pAttrDef || !pszValue )
		return false;

	// Make sure the value makes sense for this attribute before saving it.
	if ( !ApplyAttributeFromString( NULL, pAttrDef, pszValue ) )
		return false;

	pItemKV->FindKey( "attributes", true )->SetString( pAttrDef->GetDefinitionName(), pszValue );

	UpdateLiveItem( pItemKV );
	if ( bNotifyServer )
	{
		NotifyEquipmentChanged();
	}
	m_bDirty = true;
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Add to a counter attribute (e.g. a Strange kill count).
//-----------------------------------------------------------------------------
bool CTFOfflineInventory::AddToItemAttribute( itemid_t unItemID, const CEconItemAttributeDefinition *pAttrDef, int nAmount )
{
	const CEconItem *pLiveItem = FindLiveItem( unItemID );
	if ( !pLiveItem || !pAttrDef )
		return false;

	uint32 unValue = 0;
	::FindAttribute( pLiveItem, pAttrDef, &unValue );

	const int64 nNewValue = MAX( (int64)unValue + nAmount, (int64)0 );

	// Not worth re-sending the whole loadout to the server for each kill; the
	// server already counted this one in its own copy.
	return SetItemAttributeInternal( unItemID, pAttrDef, CFmtStr( "%u", (uint32)nNewValue ), false );
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::SetItemCustomText( itemid_t unItemID, bool bDescription, const char *pszText )
{
	KeyValues *pItemKV = FindItemKV( unItemID );
	if ( !pItemKV )
		return false;

	const char *pszKey = bDescription ? "custom_desc" : "custom_name";
	if ( pszText && pszText[0] )
	{
		pItemKV->SetString( pszKey, pszText );
	}
	else
	{
		KeyValues *pTextKV = pItemKV->FindKey( pszKey );
		if ( !pTextKV )
			return false;
		pItemKV->RemoveSubKey( pTextKV );
		pTextKV->deleteThis();
	}

	UpdateLiveItem( pItemKV );
	NotifyEquipmentChanged();
	m_bDirty = true;
	return true;
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::SetItemQuality( itemid_t unItemID, int nQuality )
{
	KeyValues *pItemKV = FindItemKV( unItemID );
	if ( !pItemKV || nQuality < 0 || nQuality >= AE_MAX_TYPES )
		return false;

	pItemKV->SetInt( "quality", nQuality );
	UpdateLiveItem( pItemKV );
	NotifyEquipmentChanged();
	m_bDirty = true;
	return true;
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::SetItemStyle( itemid_t unItemID, int nStyle )
{
	KeyValues *pItemKV = FindItemKV( unItemID );
	if ( !pItemKV || nStyle < 0 || nStyle > 255 )
		return false;

	pItemKV->SetInt( "style", nStyle );
	UpdateLiveItem( pItemKV );
	NotifyEquipmentChanged();
	m_bDirty = true;
	return true;
}

//-----------------------------------------------------------------------------
// Wallet (for the Mann Co. Store). Kept in cents; new saves start with $100.
//-----------------------------------------------------------------------------
static const int kStartingWalletCents = 10000;

int CTFOfflineInventory::GetWalletCents() const
{
	return m_pData ? m_pData->GetInt( "wallet_cents", kStartingWalletCents ) : 0;
}

void CTFOfflineInventory::SetWalletCents( int nCents )
{
	if ( !m_pData )
		return;

	m_pData->SetInt( "wallet_cents", MAX( nCents, 0 ) );
	m_bDirty = true;
}

//-----------------------------------------------------------------------------
const CEconItemDefinition *CTFOfflineInventory::GetItemDef( itemid_t unItemID ) const
{
	KeyValues *pItemKV = FindItemKV( unItemID );
	return pItemKV ? FindItemDefForKV( pItemKV ) : NULL;
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::RemoveItemAttribute( itemid_t unItemID, const CEconItemAttributeDefinition *pAttrDef )
{
	KeyValues *pItemKV = FindItemKV( unItemID );
	if ( !pItemKV || !pAttrDef )
		return false;

	KeyValues *pAttributesKV = pItemKV->FindKey( "attributes" );
	KeyValues *pAttrKV = pAttributesKV ? pAttributesKV->FindKey( pAttrDef->GetDefinitionName() ) : NULL;
	if ( !pAttrKV )
		return false;

	pAttributesKV->RemoveSubKey( pAttrKV );
	pAttrKV->deleteThis();

	UpdateLiveItem( pItemKV );
	NotifyEquipmentChanged();
	m_bDirty = true;
	return true;
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::SetItemPosition( itemid_t unItemID, uint32 unBackendPosition )
{
	if ( !FindItemKV( unItemID ) )
		return false;

	// Applied on the next Think(), the way a reply from an item server would
	// arrive. The backpack UI calls this while it is still looping over its
	// items, so changing them right now would pull the list out from under it.
	PendingMove_t &move = m_vecPendingMoves[ m_vecPendingMoves.AddToTail() ];
	move.m_unItemID = unItemID;
	move.m_unBackendPosition = unBackendPosition;
	return true;
}

//-----------------------------------------------------------------------------
void CTFOfflineInventory::ApplyPendingMoves()
{
	if ( m_vecPendingMoves.Count() == 0 )
		return;

	CUtlVector< PendingMove_t > vecMoves;
	vecMoves.Swap( m_vecPendingMoves );

	FOR_EACH_VEC( vecMoves, i )
	{
		ApplyItemPosition( vecMoves[i].m_unItemID, vecMoves[i].m_unBackendPosition );
	}
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::ApplyItemPosition( itemid_t unItemID, uint32 unBackendPosition )
{
	KeyValues *pItemKV = FindItemKV( unItemID );
	if ( !pItemKV )
		return false;

	int iNewSlot = 0;
	if ( !IsUnacknowledged( unBackendPosition ) )
	{
		iNewSlot = ExtractBackpackPositionFromBackend( unBackendPosition );
		if ( iNewSlot < 0 || iNewSlot > kOfflineBackpackSlots )
		{
			iNewSlot = 0;
		}
	}

	const int iOldSlot = SlotFromKV( pItemKV );
	if ( iOldSlot == iNewSlot )
		return true;

	// Counts rather than flags: during a swap two items briefly share a slot.
	m_nSlotUseCount[iOldSlot]--;
	m_nSlotUseCount[iNewSlot]++;
	pItemKV->SetInt( "slot", iNewSlot );

	// Once placed, it isn't a new item any more.
	KeyValues *pNewReasonKV = iNewSlot > 0 ? pItemKV->FindKey( "new_reason" ) : NULL;
	if ( pNewReasonKV )
	{
		pItemKV->RemoveSubKey( pNewReasonKV );
		pNewReasonKV->deleteThis();
	}

	UpdateLiveItem( pItemKV );
	m_bDirty = true;
	return true;
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::DeleteItem( itemid_t unItemID )
{
	KeyValues *pItemKV = FindItemKV( unItemID );
	if ( !pItemKV )
		return false;

	// Take it out of every loadout first, so those slots go back to stock.
	CTFPlayerInventory *pLocalInventory = TFInventoryManager()->GetLocalTFInventory();
	if ( pLocalInventory )
	{
		pLocalInventory->UnequipLocal( unItemID );
		pLocalInventory->SaveLocalLoadout();
	}

	m_nSlotUseCount[ SlotFromKV( pItemKV ) ]--;
	GetItemsKV()->RemoveSubKey( pItemKV );
	pItemKV->deleteThis();
	m_bDirty = true;

	if ( m_nBulkChangeDepth > 0 )
	{
		m_bBulkChangePending = true;
	}
	else
	{
		GCSDK::CGCClientSharedObjectCache *pSOCache = GetLiveSOCache();
		if ( pSOCache )
		{
			// Only the item ID is needed to find the object to destroy.
			CEconItem soIndex;
			soIndex.SetItemID( unItemID );

			std::string strData;
			if ( SerializeEconItem( soIndex, strData ) )
			{
				pSOCache->BDestroyFromMsg( CEconItem::k_nTypeID, strData.data(), (uint32)strData.size() );
			}
		}
	}

	NotifyEquipmentChanged();
	return true;
}

//-----------------------------------------------------------------------------
int CTFOfflineInventory::DeleteAllItems()
{
	KeyValues *pItemsKV = GetItemsKV();
	if ( !pItemsKV )
		return 0;

	const int nCount = GetItemCount();

	// Reset every loadout to stock.
	CTFPlayerInventory *pLocalInventory = TFInventoryManager()->GetLocalTFInventory();
	if ( pLocalInventory )
	{
		pLocalInventory->SaveLocalLoadout( true, false );
	}

	m_pData->RemoveSubKey( pItemsKV );
	pItemsKV->deleteThis();
	m_pData->FindKey( "items", true );
	V_memset( m_nSlotUseCount, 0, sizeof( m_nSlotUseCount ) );
	m_bDirty = true;

	BeginBulkChange();
	m_bBulkChangePending = true;
	EndBulkChange();

	NotifyEquipmentChanged();
	return nCount;
}

//-----------------------------------------------------------------------------
// Crates
//-----------------------------------------------------------------------------
void CTFOfflineInventory::RequestUnlockCrate( itemid_t unKeyID, itemid_t unCrateID )
{
	PendingUnlock_t &unlock = m_vecPendingUnlocks[ m_vecPendingUnlocks.AddToTail() ];
	unlock.m_unKeyID = unKeyID;
	unlock.m_unCrateID = unCrateID;
}

//-----------------------------------------------------------------------------
void CTFOfflineInventory::ApplyPendingUnlocks()
{
	if ( m_vecPendingUnlocks.Count() == 0 )
		return;

	CUtlVector< PendingUnlock_t > vecUnlocks;
	vecUnlocks.Swap( m_vecPendingUnlocks );

	FOR_EACH_VEC( vecUnlocks, i )
	{
		if ( UnlockCrate( vecUnlocks[i].m_unKeyID, vecUnlocks[i].m_unCrateID ) )
		{
			PlayCrateUnlockSound();
		}
	}
}

//-----------------------------------------------------------------------------
static void GetUnusualEffectName( KeyValues *pAttributes, char *pszOut, int nOutSize )
{
	pszOut[0] = '\0';
	if ( !pAttributes )
		return;

	KeyValues *pEffectKV = pAttributes->FindKey( "attach particle effect" );
	if ( !pEffectKV )
	{
		pEffectKV = pAttributes->FindKey( "on taunt attach particle index" );
	}
	if ( !pEffectKV )
		return;

	const int iEffect = (int)V_atof( pEffectKV->GetString() );
	const locchar_t *pszLocalized = GLocalizationProvider()->Find( CFmtStr( "#Attrib_Particle%d", iEffect ) );
	if ( pszLocalized )
	{
		GLocalizationProvider()->ConvertLoccharToANSI( pszLocalized, pszOut, nOutSize );
	}
	else
	{
		V_snprintf( pszOut, nOutSize, "effect %d", iEffect );
	}
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::UnlockCrate( itemid_t unKeyID, itemid_t unCrateID )
{
	const CEconItem *pCrate = FindLiveItem( unCrateID );
	if ( !pCrate || !FindItemKV( unCrateID ) )
	{
		Warning( "Offline inventory: crate %llu isn't in your backpack.\n", (unsigned long long)unCrateID );
		return false;
	}

	if ( unKeyID != INVALID_ITEM_ID && !FindItemKV( unKeyID ) )
	{
		Warning( "Offline inventory: key %llu isn't in your backpack.\n", (unsigned long long)unKeyID );
		return false;
	}

	CUtlVector< COfflineLootItem * > vecLoot;
	if ( !OfflineCrates_RollCrate( pCrate, vecLoot ) )
	{
		vecLoot.PurgeAndDeleteElements();
		Warning( "Offline inventory: this version of the game doesn't know what that crate contains, so it wasn't opened.\n" );
		return false;
	}

	// The key and the crate are used up. (pCrate is gone after this.)
	if ( unKeyID != INVALID_ITEM_ID )
	{
		DeleteItem( unKeyID );
	}
	DeleteItem( unCrateID );

	// Arriving as new "found in crate" items is what makes the backpack play
	// its unboxing reveal.
	char szName[256], szEffect[256];
	FOR_EACH_VEC( vecLoot, i )
	{
		const COfflineLootItem *pLoot = vecLoot[i];
		const itemid_t unItemID = CreateItem( pLoot->m_pItemDef, pLoot->m_nQuality, pLoot->m_pAttributes,
											  kEconItemOrigin_FoundInCrate, UNACK_ITEM_FOUND_IN_CRATE );
		if ( unItemID == INVALID_ITEM_ID )
			continue;

		OfflineInventory_GetItemDefDisplayName( pLoot->m_pItemDef, szName, sizeof( szName ) );
		GetUnusualEffectName( pLoot->m_pAttributes, szEffect, sizeof( szEffect ) );
		Msg( "%s %s %s%s%s (item ID %llu)\n", i == 0 ? "Unboxed:" : "  Bonus:",
			GetQualityDisplayName( pLoot->m_nQuality ), szName,
			szEffect[0] ? ", " : "", szEffect, (unsigned long long)unItemID );
	}

	vecLoot.PurgeAndDeleteElements();
	return true;
}

//-----------------------------------------------------------------------------
bool CTFOfflineInventory::OpenCrateWithAnyKey( itemid_t unCrateID )
{
	KeyValues *pCrateKV = FindItemKV( unCrateID );
	const CEconItemDefinition *pCrateDef = pCrateKV ? FindItemDefForKV( pCrateKV ) : NULL;
	if ( !OfflineCrates_IsCrate( pCrateDef ) )
	{
		Msg( "Item %llu isn't a crate.\n", (unsigned long long)unCrateID );
		return false;
	}

	itemid_t unKeyID = INVALID_ITEM_ID;
	if ( OfflineCrates_NeedsKey( pCrateDef ) )
	{
		const CEconItemDefinition *pKeyDef = OfflineCrates_GetKeyForCrate( pCrateDef );
		FOR_EACH_TRUE_SUBKEY( GetItemsKV(), pItemKV )
		{
			if ( pKeyDef && FindItemDefForKV( pItemKV ) == pKeyDef )
			{
				unKeyID = ItemIDFromKV( pItemKV );
				break;
			}
		}

		if ( unKeyID == INVALID_ITEM_ID )
		{
			char szKeyName[256];
			if ( pKeyDef )
			{
				OfflineInventory_GetItemDefDisplayName( pKeyDef, szKeyName, sizeof( szKeyName ) );
			}
			else
			{
				V_strncpy( szKeyName, "key", sizeof( szKeyName ) );
			}
			Msg( "You need a %s to open this. inv_givecrate gives keys along with crates.\n", szKeyName );
			return false;
		}
	}

	return UnlockCrate( unKeyID, unCrateID );
}

//-----------------------------------------------------------------------------
void CTFOfflineInventory::WriteEquippedItemsForServer( KeyValues *pKV ) const
{
	KeyValues *pItemsKV = pKV->FindKey( "items", true );

	GCSDK::CGCClientSharedObjectCache *pSOCache = GetLiveSOCache();
	if ( !pSOCache )
		return;

	GCSDK::CGCClientSharedObjectTypeCache *pTypeCache = pSOCache->FindTypeCache( CEconItem::k_nTypeID );
	if ( !pTypeCache )
		return;

	// One value per item: KeyValues strings sent over the network are capped
	// at a few KB each, so a single blob for the whole loadout could get cut off.
	int nWritten = 0;
	for ( uint32 i = 0; i < pTypeCache->GetCount(); ++i )
	{
		const CEconItem *pItem = (const CEconItem *)pTypeCache->GetObject( i );
		if ( !pItem || !pItem->IsEquipped() )
			continue;

		CUtlString strEncoded;
		if ( OfflineInventory_EncodeItem( *pItem, strEncoded ) )
		{
			pItemsKV->SetString( CFmtStr( "%d", nWritten++ ), strEncoded.Get() );
		}
	}
}

//=============================================================================
// Console commands
//=============================================================================

//-----------------------------------------------------------------------------
// Item names
//-----------------------------------------------------------------------------
void OfflineInventory_GetItemDefDisplayName( const CEconItemDefinition *pItemDef, char *pszOut, int nOutSize )
{
	const char *pszToken = pItemDef->GetItemBaseName();
	const locchar_t *pszLocalized = ( pszToken && pszToken[0] ) ? GLocalizationProvider()->Find( pszToken ) : NULL;
	if ( pszLocalized )
	{
		GLocalizationProvider()->ConvertLoccharToANSI( pszLocalized, pszOut, nOutSize );
	}
	else
	{
		V_strncpy( pszOut, pItemDef->GetDefinitionName(), nOutSize );
	}
}

static bool IsAllDigits( const char *psz )
{
	if ( !psz || !psz[0] )
		return false;

	for ( ; *psz; ++psz )
	{
		if ( !V_isdigit( *psz ) )
			return false;
	}
	return true;
}

static bool IsGiveableDef( const CEconItemDefinition *pItemDef )
{
	return !pItemDef->IsHidden() && !pItemDef->IsBaseItem();
}

// Finds item definitions by index, internal name, or English name.
// Exact matches win; otherwise every definition containing the text is returned.
void OfflineInventory_FindItemDefs( const char *pszQuery, CUtlVector< const CEconItemDefinition * > &vecOut )
{
	vecOut.Purge();

	if ( IsAllDigits( pszQuery ) )
	{
		const CEconItemDefinition *pItemDef = GetItemSchema()->GetItemDefinition( V_atoi( pszQuery ) );
		if ( pItemDef )
		{
			vecOut.AddToTail( pItemDef );
		}
		return;
	}

	CUtlVector< const CEconItemDefinition * > vecExact, vecPartial;

	char szDisplayName[256];
	const CEconItemSchema::SortedItemDefinitionMap_t &mapItems = GetItemSchema()->GetSortedItemDefinitionMap();
	FOR_EACH_MAP( mapItems, i )
	{
		const CEconItemDefinition *pItemDef = mapItems[i];
		if ( !pItemDef )
			continue;

		OfflineInventory_GetItemDefDisplayName( pItemDef, szDisplayName, sizeof( szDisplayName ) );

		if ( !V_stricmp( pItemDef->GetDefinitionName(), pszQuery ) || !V_stricmp( szDisplayName, pszQuery ) )
		{
			vecExact.AddToTail( pItemDef );
		}
		else if ( V_stristr( pItemDef->GetDefinitionName(), pszQuery ) || V_stristr( szDisplayName, pszQuery ) )
		{
			vecPartial.AddToTail( pItemDef );
		}
	}

	CUtlVector< const CEconItemDefinition * > &vecMatches = vecExact.Count() ? vecExact : vecPartial;

	// Prefer items a player could actually own over hidden/stock ones, if there are any.
	FOR_EACH_VEC( vecMatches, i )
	{
		if ( IsGiveableDef( vecMatches[i] ) )
		{
			vecOut.AddToTail( vecMatches[i] );
		}
	}
	if ( vecOut.Count() == 0 )
	{
		vecOut.AddVectorToTail( vecMatches );
	}
}

static void PrintItemDefList( const CUtlVector< const CEconItemDefinition * > &vecDefs, int nMaxToShow )
{
	char szDisplayName[256];
	for ( int i = 0; i < vecDefs.Count() && i < nMaxToShow; ++i )
	{
		OfflineInventory_GetItemDefDisplayName( vecDefs[i], szDisplayName, sizeof( szDisplayName ) );
		Msg( "  %6d  %-40s  (%s)%s\n", vecDefs[i]->GetDefinitionIndex(), szDisplayName, vecDefs[i]->GetDefinitionName(),
			vecDefs[i]->IsHidden() ? " [hidden]" : "" );
	}
	if ( vecDefs.Count() > nMaxToShow )
	{
		Msg( "  ...and %d more. Be more specific, or use the number on the left.\n", vecDefs.Count() - nMaxToShow );
	}
}

//-----------------------------------------------------------------------------
// Qualities
//-----------------------------------------------------------------------------
struct OfflineQualityName_t
{
	const char *m_pszName;
	int m_nQuality;
};

static const OfflineQualityName_t s_QualityNames[] =
{
	{ "normal",			AE_NORMAL },
	{ "genuine",		AE_RARITY1 },
	{ "vintage",		AE_VINTAGE },
	{ "unusual",		AE_UNUSUAL },
	{ "unique",			AE_UNIQUE },
	{ "community",		AE_COMMUNITY },
	{ "valve",			AE_DEVELOPER },
	{ "selfmade",		AE_SELFMADE },
	{ "self-made",		AE_SELFMADE },
	{ "strange",		AE_STRANGE },
	{ "haunted",		AE_HAUNTED },
	{ "collectors",		AE_COLLECTORS },
	{ "collector's",	AE_COLLECTORS },
	{ "decorated",		AE_PAINTKITWEAPON },
};

// Returns -1 if the text isn't a quality.
static int ParseQuality( const char *pszQuality )
{
	if ( IsAllDigits( pszQuality ) )
	{
		const int nQuality = V_atoi( pszQuality );
		return ( nQuality >= 0 && nQuality < AE_MAX_TYPES ) ? nQuality : -1;
	}

	for ( int i = 0; i < ARRAYSIZE( s_QualityNames ); ++i )
	{
		if ( !V_stricmp( s_QualityNames[i].m_pszName, pszQuality ) )
			return s_QualityNames[i].m_nQuality;
	}

	const EEconItemQuality eQuality = EconQuality_GetQualityFromString( pszQuality );
	return eQuality == AE_UNDEFINED ? -1 : eQuality;
}

static const char *GetQualityDisplayName( int nQuality )
{
	for ( int i = 0; i < ARRAYSIZE( s_QualityNames ); ++i )
	{
		if ( s_QualityNames[i].m_nQuality == nQuality )
			return s_QualityNames[i].m_pszName;
	}
	const char *pszName = EconQuality_GetQualityString( (EEconItemQuality)nQuality );
	return pszName ? pszName : "?";
}

//-----------------------------------------------------------------------------
// Item categories, for inv_give_all
//-----------------------------------------------------------------------------
EOfflineItemCategory OfflineInventory_GetItemCategory( const CEconItemDefinition *pItemDef )
{
	const GameItemDefinition_t *pTFItemDef = static_cast< const GameItemDefinition_t * >( pItemDef );
	for ( int iClass = TF_FIRST_NORMAL_CLASS; iClass < TF_LAST_NORMAL_CLASS; ++iClass )
	{
		if ( !pTFItemDef->CanBeUsedByClass( iClass ) )
			continue;

		const int iSlot = pTFItemDef->GetLoadoutSlot( iClass );
		if ( iSlot >= LOADOUT_POSITION_PRIMARY && iSlot <= LOADOUT_POSITION_PDA2 )
			return kCategory_Weapon;
		if ( iSlot == LOADOUT_POSITION_HEAD || iSlot == LOADOUT_POSITION_MISC || iSlot == LOADOUT_POSITION_MISC2 )
			return kCategory_Cosmetic;
		if ( iSlot >= LOADOUT_POSITION_TAUNT && iSlot <= LOADOUT_POSITION_TAUNT8 )
			return kCategory_Taunt;
		if ( iSlot == LOADOUT_POSITION_ACTION )
			return kCategory_Action;
	}
	return kCategory_None;
}

//-----------------------------------------------------------------------------
// Shared argument handling
//-----------------------------------------------------------------------------
static bool CheckInventoryReady()
{
	if ( TFOfflineInventory().IsLoaded() )
		return true;

	Msg( "The offline inventory hasn't loaded yet. Make sure Steam is running, then try again in a moment.\n" );
	return false;
}

// Accepts an item ID or "last" (the most recently given item).
static itemid_t ParseItemID( const char *pszArg )
{
	if ( !V_stricmp( pszArg, "last" ) )
		return TFOfflineInventory().GetLastGivenItemID();

	if ( IsAllDigits( pszArg ) )
		return V_atoui64( pszArg );

	return INVALID_ITEM_ID;
}

itemid_t OfflineInventory_ParseExistingItemID( const char *pszArg )
{
	const itemid_t unItemID = ParseItemID( pszArg );
	if ( unItemID == INVALID_ITEM_ID || !TFOfflineInventory().FindItemKV( unItemID ) )
	{
		Msg( "No item with ID \"%s\". Use inv_list to see your item IDs.\n", pszArg );
		return INVALID_ITEM_ID;
	}
	return unItemID;
}

static const CEconItemAttributeDefinition *ParseAttribute( const char *pszArg )
{
	const CEconItemAttributeDefinition *pAttrDef = IsAllDigits( pszArg )
		? GetItemSchema()->GetAttributeDefinition( V_atoi( pszArg ) )
		: GetItemSchema()->GetAttributeDefinitionByName( pszArg );

	if ( !pAttrDef )
	{
		Msg( "No attribute named \"%s\". Attribute names are the ones in items_game.txt, e.g. \"attach particle effect\".\n", pszArg );
	}
	return pAttrDef;
}

// Unusual taunts store their effect in a different attribute than hats and weapons.
static const CEconItemAttributeDefinition *GetUnusualEffectAttribute( const CEconItemDefinition *pItemDef )
{
	const char *pszAttrName = ( OfflineInventory_GetItemCategory( pItemDef ) == kCategory_Taunt )
		? "on taunt attach particle index"
		: "attach particle effect";
	return GetItemSchema()->GetAttributeDefinitionByName( pszAttrName );
}

//-----------------------------------------------------------------------------
CON_COMMAND( inv_give, "Add an item to your offline backpack.\n"
	"  Usage: inv_give <item name or number> [quality] [unusual effect number]\n"
	"  Qualities: normal genuine vintage unusual unique community valve selfmade strange haunted collectors decorated\n"
	"  Example: inv_give \"Team Captain\" unusual 13" )
{
	if ( !CheckInventoryReady() )
		return;

	if ( args.ArgC() < 2 )
	{
		Msg( "Usage: inv_give <item name or number> [quality] [unusual effect number]\n" );
		return;
	}

	CUtlVector< const CEconItemDefinition * > vecMatches;
	OfflineInventory_FindItemDefs( args[1], vecMatches );
	if ( vecMatches.Count() == 0 )
	{
		Msg( "No item matches \"%s\". Try inv_search.\n", args[1] );
		return;
	}
	if ( vecMatches.Count() > 1 )
	{
		Msg( "\"%s\" matches %d items:\n", args[1], vecMatches.Count() );
		PrintItemDefList( vecMatches, 25 );
		return;
	}

	int nQuality = -1;
	if ( args.ArgC() >= 3 )
	{
		nQuality = ParseQuality( args[2] );
		if ( nQuality < 0 )
		{
			Msg( "Unknown quality \"%s\".\n", args[2] );
			return;
		}
	}

	const CEconItemDefinition *pItemDef = vecMatches[0];

	// Crates need a series number and a key; inv_givecrate handles both.
	if ( OfflineCrates_IsCrate( pItemDef ) )
	{
		Msg( "That's a crate. Use inv_givecrate instead: it sets up the crate's contents and gives you a key. See inv_crates.\n" );
		return;
	}

	const itemid_t unItemID = TFOfflineInventory().GiveItem( pItemDef, nQuality );
	if ( unItemID == INVALID_ITEM_ID )
		return;

	if ( args.ArgC() >= 4 )
	{
		const CEconItemAttributeDefinition *pEffectAttr = GetUnusualEffectAttribute( pItemDef );
		if ( !pEffectAttr || !TFOfflineInventory().SetItemAttribute( unItemID, pEffectAttr, args[3] ) )
		{
			Msg( "Couldn't apply unusual effect \"%s\".\n", args[3] );
		}
	}

	char szDisplayName[256];
	OfflineInventory_GetItemDefDisplayName( pItemDef, szDisplayName, sizeof( szDisplayName ) );
	Msg( "Gave %s (item ID %llu).\n", szDisplayName, (unsigned long long)unItemID );
}

//-----------------------------------------------------------------------------
// Tools that can be used from the backpack in the offline inventory.
static bool IsUsableOfflineTool( const CEconItemDefinition *pItemDef )
{
	static const char *s_pszToolTypes[] =
	{
		"paint_can", "name", "desc", "strange_part", "strange_count_transfer",
		"strangifier", "killstreakifier", "paintkit_weapon_festivizer", "unusualifier",
	};

	const IEconTool *pTool = pItemDef->GetEconTool();
	if ( !pTool || !pTool->GetTypeName() )
		return false;

	for ( int i = 0; i < ARRAYSIZE( s_pszToolTypes ); ++i )
	{
		if ( !V_stricmp( pTool->GetTypeName(), s_pszToolTypes[i] ) )
			return true;
	}
	return false;
}

CON_COMMAND( inv_give_all, "Add one of every item you don't already have.\n"
	"  Usage: inv_give_all [all | weapons | cosmetics | taunts | tools]\n"
	"  \"tools\" gives paint cans, name tags, strange parts, Strangifiers, Killstreak Kits and the like." )
{
	if ( !CheckInventoryReady() )
		return;

	const char *pszFilter = args.ArgC() >= 2 ? args[1] : "all";
	const bool bAll = !V_stricmp( pszFilter, "all" );
	const bool bTools = !V_stricmp( pszFilter, "tools" );
	EOfflineItemCategory eOnly = kCategory_None;
	if ( !V_stricmp( pszFilter, "weapons" ) )
		eOnly = kCategory_Weapon;
	else if ( !V_stricmp( pszFilter, "cosmetics" ) )
		eOnly = kCategory_Cosmetic;
	else if ( !V_stricmp( pszFilter, "taunts" ) )
		eOnly = kCategory_Taunt;
	else if ( !bAll && !bTools )
	{
		Msg( "Usage: inv_give_all [all | weapons | cosmetics | taunts | tools]\n" );
		return;
	}

	// Which definitions do we already own?
	CUtlRBTree< int > setOwnedDefs( DefLessFunc( int ) );
	KeyValues *pItemsKV = TFOfflineInventory().GetItemsKV();
	FOR_EACH_TRUE_SUBKEY( pItemsKV, pItemKV )
	{
		setOwnedDefs.InsertIfNotFound( pItemKV->GetInt( "def", -1 ) );
	}

	int nGiven = 0, nFailed = 0;

	TFOfflineInventory().BeginBulkChange();

	const CEconItemSchema::SortedItemDefinitionMap_t &mapItems = GetItemSchema()->GetSortedItemDefinitionMap();
	FOR_EACH_MAP( mapItems, i )
	{
		const CEconItemDefinition *pItemDef = mapItems[i];
		if ( !pItemDef || !IsGiveableDef( pItemDef ) || pItemDef->IsBundle() )
			continue;

		if ( bTools )
		{
			if ( !IsUsableOfflineTool( pItemDef ) )
				continue;
		}
		else
		{
			if ( pItemDef->IsTool() )
				continue;

			const EOfflineItemCategory eCategory = OfflineInventory_GetItemCategory( pItemDef );
			if ( eCategory == kCategory_None )
				continue;
			if ( !bAll && eCategory != eOnly )
				continue;
		}

		if ( setOwnedDefs.Find( pItemDef->GetDefinitionIndex() ) != setOwnedDefs.InvalidIndex() )
			continue;

		if ( TFOfflineInventory().GetFreeSlotCount() == 0 )
		{
			nFailed++;
			continue;
		}

		if ( TFOfflineInventory().GiveItem( pItemDef, -1 ) != INVALID_ITEM_ID )
		{
			nGiven++;
		}
		else
		{
			nFailed++;
		}
	}

	TFOfflineInventory().EndBulkChange();

	Msg( "Gave %d item(s).\n", nGiven );
	if ( nFailed )
	{
		Warning( "%d item(s) didn't fit or couldn't be created.\n", nFailed );
	}
}

//-----------------------------------------------------------------------------
CON_COMMAND( inv_search, "Search every item in the game by name.\n"
	"  Usage: inv_search <text>" )
{
	if ( args.ArgC() < 2 )
	{
		Msg( "Usage: inv_search <text>\n" );
		return;
	}

	CUtlVector< const CEconItemDefinition * > vecMatches;
	OfflineInventory_FindItemDefs( args.ArgS(), vecMatches );
	if ( vecMatches.Count() == 0 )
	{
		Msg( "No item matches \"%s\".\n", args.ArgS() );
		return;
	}

	Msg( "%d match(es):\n", vecMatches.Count() );
	PrintItemDefList( vecMatches, 100 );
}

//-----------------------------------------------------------------------------
CON_COMMAND( inv_list, "List the items in your offline backpack, with their item IDs.\n"
	"  Usage: inv_list [text to filter by]" )
{
	if ( !CheckInventoryReady() )
		return;

	const char *pszFilter = args.ArgC() >= 2 ? args.ArgS() : NULL;
	char szDisplayName[256];
	int nShown = 0;

	KeyValues *pItemsKV = TFOfflineInventory().GetItemsKV();
	FOR_EACH_TRUE_SUBKEY( pItemsKV, pItemKV )
	{
		const CEconItemDefinition *pItemDef = FindItemDefForKV( pItemKV );
		if ( pItemDef )
		{
			OfflineInventory_GetItemDefDisplayName( pItemDef, szDisplayName, sizeof( szDisplayName ) );
		}
		else
		{
			V_snprintf( szDisplayName, sizeof( szDisplayName ), "<unknown item %d>", pItemKV->GetInt( "def", -1 ) );
		}

		if ( pszFilter && !V_stristr( szDisplayName, pszFilter ) && ( !pItemDef || !V_stristr( pItemDef->GetDefinitionName(), pszFilter ) ) )
			continue;

		Msg( "  id %-6s  slot %-4d  %-10s  %s", pItemKV->GetName(), pItemKV->GetInt( "slot", 0 ),
			GetQualityDisplayName( pItemKV->GetInt( "quality", AE_UNIQUE ) ), szDisplayName );

		KeyValues *pAttributesKV = pItemKV->FindKey( "attributes" );
		if ( pAttributesKV )
		{
			FOR_EACH_VALUE( pAttributesKV, pAttrKV )
			{
				Msg( "  [%s = %s]", pAttrKV->GetName(), pAttrKV->GetString() );
			}
		}
		Msg( "\n" );
		nShown++;
	}

	Msg( "%d item(s) shown, %d total, %d free backpack slots.\n", nShown, TFOfflineInventory().GetItemCount(), TFOfflineInventory().GetFreeSlotCount() );
}

//-----------------------------------------------------------------------------
CON_COMMAND( inv_effects, "List unusual effects and their numbers.\n"
	"  Usage: inv_effects [text to filter by]" )
{
	const char *pszFilter = args.ArgC() >= 2 ? args.ArgS() : NULL;
	char szName[256];

	const CEconItemSchema::ParticleDefinitionMap_t &mapParticles = GetItemSchema()->GetAttributeControlledParticleSystems();
	FOR_EACH_MAP( mapParticles, i )
	{
		const int iEffect = mapParticles.Key( i );
		const locchar_t *pszLocalized = GLocalizationProvider()->Find( CFmtStr( "#Attrib_Particle%d", iEffect ) );
		if ( pszLocalized )
		{
			GLocalizationProvider()->ConvertLoccharToANSI( pszLocalized, szName, sizeof( szName ) );
		}
		else
		{
			V_strncpy( szName, mapParticles[i].pszSystemName ? mapParticles[i].pszSystemName : "?", sizeof( szName ) );
		}

		if ( pszFilter && !V_stristr( szName, pszFilter ) )
			continue;

		Msg( "  %5d  %s\n", iEffect, szName );
	}
}

//-----------------------------------------------------------------------------
CON_COMMAND( inv_setattr, "Set an attribute on an item (unusual effects, paint, killstreaks, ...).\n"
	"  Usage: inv_setattr <item id | last> <attribute name> <value>\n"
	"  Example: inv_setattr last \"attach particle effect\" 13" )
{
	if ( !CheckInventoryReady() )
		return;

	if ( args.ArgC() < 4 )
	{
		Msg( "Usage: inv_setattr <item id | last> <attribute name> <value>\n" );
		return;
	}

	const itemid_t unItemID = OfflineInventory_ParseExistingItemID( args[1] );
	if ( unItemID == INVALID_ITEM_ID )
		return;

	const CEconItemAttributeDefinition *pAttrDef = ParseAttribute( args[2] );
	if ( !pAttrDef )
		return;

	if ( TFOfflineInventory().SetItemAttribute( unItemID, pAttrDef, args[3] ) )
	{
		Msg( "Set \"%s\" to %s on item %llu.\n", pAttrDef->GetDefinitionName(), args[3], (unsigned long long)unItemID );
	}
	else
	{
		Msg( "\"%s\" isn't a valid value for \"%s\".\n", args[3], pAttrDef->GetDefinitionName() );
	}
}

//-----------------------------------------------------------------------------
CON_COMMAND( inv_removeattr, "Remove an attribute you added to an item.\n"
	"  Usage: inv_removeattr <item id | last> <attribute name>" )
{
	if ( !CheckInventoryReady() )
		return;

	if ( args.ArgC() < 3 )
	{
		Msg( "Usage: inv_removeattr <item id | last> <attribute name>\n" );
		return;
	}

	const itemid_t unItemID = OfflineInventory_ParseExistingItemID( args[1] );
	if ( unItemID == INVALID_ITEM_ID )
		return;

	const CEconItemAttributeDefinition *pAttrDef = ParseAttribute( args[2] );
	if ( !pAttrDef )
		return;

	if ( TFOfflineInventory().RemoveItemAttribute( unItemID, pAttrDef ) )
	{
		Msg( "Removed \"%s\" from item %llu.\n", pAttrDef->GetDefinitionName(), (unsigned long long)unItemID );
	}
	else
	{
		Msg( "Item %llu doesn't have \"%s\" set.\n", (unsigned long long)unItemID, pAttrDef->GetDefinitionName() );
	}
}

//-----------------------------------------------------------------------------
CON_COMMAND( inv_paint, "Paint an item using the colors from a paint can.\n"
	"  Usage: inv_paint <item id | last> <paint can name>\n"
	"  Example: inv_paint last \"Team Spirit\"" )
{
	if ( !CheckInventoryReady() )
		return;

	if ( args.ArgC() < 3 )
	{
		Msg( "Usage: inv_paint <item id | last> <paint can name>\n" );
		return;
	}

	const itemid_t unItemID = OfflineInventory_ParseExistingItemID( args[1] );
	if ( unItemID == INVALID_ITEM_ID )
		return;

	// Paint cans carry their colors as these attributes; copy them onto the item.
	const CEconItemAttributeDefinition *pTintAttrs[] =
	{
		GetItemSchema()->GetAttributeDefinitionByName( "set item tint RGB" ),
		GetItemSchema()->GetAttributeDefinitionByName( "set item tint RGB 2" ),
	};

	CUtlVector< const CEconItemDefinition * > vecMatches;
	OfflineInventory_FindItemDefs( args[2], vecMatches );

	// Keep only definitions that actually carry a paint color.
	CUtlVector< const CEconItemDefinition * > vecPaints;
	FOR_EACH_VEC( vecMatches, i )
	{
		if ( pTintAttrs[0] && ::FindAttribute( vecMatches[i], pTintAttrs[0] ) )
		{
			vecPaints.AddToTail( vecMatches[i] );
		}
	}

	if ( vecPaints.Count() == 0 )
	{
		Msg( "No paint can matches \"%s\". Try inv_search paint.\n", args[2] );
		return;
	}
	if ( vecPaints.Count() > 1 )
	{
		Msg( "\"%s\" matches %d paint cans:\n", args[2], vecPaints.Count() );
		PrintItemDefList( vecPaints, 25 );
		return;
	}

	const CEconItemDefinition *pPaintDef = vecPaints[0];
	if ( !OfflineTools_PaintItem( unItemID, pPaintDef ) )
	{
		Msg( "Couldn't paint that item.\n" );
		return;
	}

	char szDisplayName[256];
	OfflineInventory_GetItemDefDisplayName( pPaintDef, szDisplayName, sizeof( szDisplayName ) );
	Msg( "Painted item %llu with %s.\n", (unsigned long long)unItemID, szDisplayName );
}

//-----------------------------------------------------------------------------
CON_COMMAND( inv_delete, "Delete an item from your offline backpack.\n"
	"  Usage: inv_delete <item id | last>" )
{
	if ( !CheckInventoryReady() )
		return;

	if ( args.ArgC() < 2 )
	{
		Msg( "Usage: inv_delete <item id | last>\n" );
		return;
	}

	const itemid_t unItemID = OfflineInventory_ParseExistingItemID( args[1] );
	if ( unItemID == INVALID_ITEM_ID )
		return;

	if ( TFOfflineInventory().DeleteItem( unItemID ) )
	{
		Msg( "Deleted item %llu.\n", (unsigned long long)unItemID );
	}
}

//-----------------------------------------------------------------------------
CON_COMMAND( inv_wipe, "Delete EVERY item in your offline backpack and reset your loadouts.\n"
	"  Usage: inv_wipe confirm" )
{
	if ( !CheckInventoryReady() )
		return;

	if ( args.ArgC() < 2 || V_stricmp( args[1], "confirm" ) )
	{
		Msg( "This deletes all %d item(s) in your offline backpack. Type \"inv_wipe confirm\" to do it.\n", TFOfflineInventory().GetItemCount() );
		return;
	}

	Msg( "Deleted %d item(s).\n", TFOfflineInventory().DeleteAllItems() );
}
