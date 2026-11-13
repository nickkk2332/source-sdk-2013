//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Offline Mann Co. Store. See tf_offline_store.h.
//
//			Prices: every weapon, cosmetic, taunt and bundle in the game is for
//			sale, plus keys, cases and the tools the offline inventory can use.
//			Valve's real prices only ever lived on their servers, so these are
//			flat TF2-ish defaults. Override any of them in
//			cfg/offline_store_prices.txt:
//
//			"offline_store_prices"
//			{
//				"Team Captain"		"14.99"		// item name, internal name or number -> dollars
//				"Mann Co. Supply Crate Key"	"0"	// 0 = not for sale
//			}
//
//=============================================================================

#include "cbase.h"
#include "tf_offline_store.h"
#include "tf_offline_inventory.h"
#include "tf_offline_crates.h"
#include "econ_item_schema.h"
#include "econ_item_tools.h"
#include "store/store_panel.h"
#include "filesystem.h"
#include "KeyValues.h"
#include "vstdlib/random.h"
#include <time.h>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define OFFLINE_STORE_PRICES_FILE	"cfg/offline_store_prices.txt"

static void OnStoreSaleChanged( IConVar *pConVar, const char *pszOldValue, float flOldValue )
{
	// Rebuild the store's prices if it's already been opened.
	if ( CStorePanel::IsPricesheetLoaded() )
	{
		CStorePanel::RequestPricesheet();
	}
}

ConVar inv_store_sale( "inv_store_sale", "0", FCVAR_ARCHIVE,
	"Offline Mann Co. Store: percent off everything (0 to 90).", true, 0.f, true, 90.f, OnStoreSaleChanged );

static const int kFeaturedItemCount = 12;

//-----------------------------------------------------------------------------
void OfflineStore_FormatMoney( int nCents, char *pszOut, int nOutSize )
{
	V_snprintf( pszOut, nOutSize, "%s$%d.%02d", nCents < 0 ? "-" : "", abs( nCents ) / 100, abs( nCents ) % 100 );
}

//-----------------------------------------------------------------------------
// Default prices
//-----------------------------------------------------------------------------
static bool IsHat( const CEconItemDefinition *pItemDef )
{
	const GameItemDefinition_t *pTFItemDef = static_cast< const GameItemDefinition_t * >( pItemDef );
	for ( int iClass = TF_FIRST_NORMAL_CLASS; iClass < TF_LAST_NORMAL_CLASS; ++iClass )
	{
		if ( pTFItemDef->CanBeUsedByClass( iClass ) )
			return pTFItemDef->GetLoadoutSlot( iClass ) == LOADOUT_POSITION_HEAD;
	}
	return false;
}

static int GetToolPrice( const char *pszToolType )
{
	static const struct OfflineToolPrice_t
	{
		const char *m_pszToolType;
		int m_nCents;
	} s_ToolPrices[] =
	{
		{ "decoder_ring",				249 },	// keys
		{ "paint_can",					199 },
		{ "name",						99 },
		{ "desc",						99 },
		{ "strange_part",				299 },
		{ "strange_count_transfer",		199 },
		{ "strangifier",				299 },
		{ "killstreakifier",			299 },
		{ "paintkit_weapon_festivizer",	199 },
		{ "unusualifier",				1999 },
	};

	for ( int i = 0; i < ARRAYSIZE( s_ToolPrices ); ++i )
	{
		if ( !V_stricmp( s_ToolPrices[i].m_pszToolType, pszToolType ) )
			return s_ToolPrices[i].m_nCents;
	}
	return 0;	// tools the offline inventory can't use yet aren't sold
}

// Price in cents and the store tab it goes in, or 0 if it isn't sold.
static int GetDefaultPrice( const CEconItemDefinition *pItemDef, const char **ppszCategory )
{
	if ( pItemDef->IsHidden() || pItemDef->IsBaseItem() )
		return 0;

	if ( pItemDef->IsBundle() )
	{
		*ppszCategory = "Bundles";
		return 1999;
	}

	// Before tools: keyless cases count as tools too.
	if ( OfflineCrates_IsCrate( pItemDef ) )
	{
		*ppszCategory = "Tools";
		return OfflineCrates_HasKnownContents( pItemDef ) ? 99 : 0;
	}

	const IEconTool *pTool = pItemDef->GetEconTool();
	if ( pTool || pItemDef->IsTool() )
	{
		*ppszCategory = "Tools";
		return ( pTool && pTool->GetTypeName() ) ? GetToolPrice( pTool->GetTypeName() ) : 0;
	}

	switch ( OfflineInventory_GetItemCategory( pItemDef ) )
	{
	case kCategory_Weapon:
		*ppszCategory = "Weapons";
		return 99;
	case kCategory_Cosmetic:
		*ppszCategory = "Cosmetics";
		return IsHat( pItemDef ) ? 799 : 499;
	case kCategory_Taunt:
		*ppszCategory = "Taunts";
		return 599;
	default:
		return 0;
	}
}

//-----------------------------------------------------------------------------
// Price overrides from cfg/offline_store_prices.txt
//-----------------------------------------------------------------------------
static const CEconItemDefinition *FindItemDefExact( const char *pszName )
{
	bool bAllDigits = pszName[0] != '\0';
	for ( const char *p = pszName; *p; ++p )
	{
		bAllDigits &= ( V_isdigit( *p ) != 0 );
	}
	if ( bAllDigits )
		return GetItemSchema()->GetItemDefinition( V_atoi( pszName ) );

	const CEconItemDefinition *pItemDef = GetItemSchema()->GetItemDefinitionByName( pszName );
	if ( pItemDef )
		return pItemDef;

	// English name; the first non-hidden match wins.
	char szDisplayName[256];
	const CEconItemSchema::SortedItemDefinitionMap_t &mapItems = GetItemSchema()->GetSortedItemDefinitionMap();
	FOR_EACH_MAP( mapItems, i )
	{
		if ( !mapItems[i] || mapItems[i]->IsHidden() )
			continue;

		OfflineInventory_GetItemDefDisplayName( mapItems[i], szDisplayName, sizeof( szDisplayName ) );
		if ( !V_stricmp( szDisplayName, pszName ) )
			return mapItems[i];
	}
	return NULL;
}

static void LoadPriceOverrides( CUtlMap< int, int > &mapOverrides )
{
	if ( !g_pFullFileSystem || !g_pFullFileSystem->FileExists( OFFLINE_STORE_PRICES_FILE, "MOD" ) )
		return;

	KeyValuesAD pPrices( "offline_store_prices" );
	if ( !pPrices->LoadFromFile( g_pFullFileSystem, OFFLINE_STORE_PRICES_FILE, "MOD" ) )
	{
		Warning( "Offline store: couldn't read %s; using the default prices.\n", OFFLINE_STORE_PRICES_FILE );
		return;
	}

	FOR_EACH_VALUE( pPrices, pPriceKV )
	{
		const CEconItemDefinition *pItemDef = FindItemDefExact( pPriceKV->GetName() );
		if ( !pItemDef )
		{
			Warning( "Offline store: %s lists \"%s\", which isn't an item.\n", OFFLINE_STORE_PRICES_FILE, pPriceKV->GetName() );
			continue;
		}

		const int nCents = (int)( V_atof( pPriceKV->GetString() ) * 100.f + 0.5f );
		mapOverrides.InsertOrReplace( pItemDef->GetDefinitionIndex(), MAX( nCents, 0 ) );
	}
}

//-----------------------------------------------------------------------------
// Price sheet
//-----------------------------------------------------------------------------
static void AddCategory( KeyValues *pCategories, const char *pszName, const char *pszLabelToken, const char *pszParent, bool bHome = false )
{
	KeyValues *pCategory = pCategories->FindKey( pszName, true );
	pCategory->SetString( "label_token", pszLabelToken );
	if ( pszParent )
	{
		pCategory->SetString( "parent", pszParent );
	}
	if ( bHome )
	{
		pCategory->SetBool( "home", true );
		pCategory->SetBool( "default", true );
	}
}

KeyValues *OfflineStore_CreatePriceSheet()
{
	KeyValues *pSheet = new KeyValues( "prices" );

	// Store tabs, with the game's own labels.
	KeyValues *pCategories = pSheet->FindKey( "categories", true );
	// The home page's sub-tabs must come in this order: the store opens on the
	// third one (Featured), and New / Popular / OnSale are kept but not shown.
	AddCategory( pCategories, "Home",		"#Store_Home",		NULL, true );
	AddCategory( pCategories, "New",		"#Store_New",		"Home" );
	AddCategory( pCategories, "Popular",	"#Store_Popular",	"Home" );
	AddCategory( pCategories, "Featured",	"#Store_Featured",	"Home" );
	AddCategory( pCategories, "OnSale",		"#Store_OnSale",	"Home" );
	AddCategory( pCategories, "Items",		"#Store_Items",		NULL );
	AddCategory( pCategories, "Weapons",	"#Store_Weapons",	"Items" );
	AddCategory( pCategories, "Cosmetics",	"#Store_Cosmetics",	"Items" );
	AddCategory( pCategories, "Taunts",		"#Store_Taunts",	"Items" );
	AddCategory( pCategories, "Tools",		"#Store_Tools",		"Items" );
	AddCategory( pCategories, "Bundles",	"#Store_Bundles",	"Items" );

	CUtlMap< int, int > mapOverrides( DefLessFunc( int ) );
	LoadPriceOverrides( mapOverrides );

	const int nSalePercent = clamp( inv_store_sale.GetInt(), 0, 90 );

	// Featured items change once a day, like the real store's.
	CUtlVector< const CEconItemDefinition * > vecFeatureCandidates;

	KeyValues *pPricePoints = pSheet->FindKey( "inventoryvalvegcpricesheet", true );
	KeyValues *pEntries = pSheet->FindKey( "entries", true );
	CUtlVector< KeyValues * > vecEntryKVs;

	const CEconItemSchema::SortedItemDefinitionMap_t &mapItems = GetItemSchema()->GetSortedItemDefinitionMap();
	FOR_EACH_MAP( mapItems, i )
	{
		const CEconItemDefinition *pItemDef = mapItems[i];
		if ( !pItemDef )
			continue;

		const char *pszCategory = NULL;
		int nPrice = GetDefaultPrice( pItemDef, &pszCategory );

		const int iOverride = mapOverrides.Find( pItemDef->GetDefinitionIndex() );
		if ( iOverride != mapOverrides.InvalidIndex() )
		{
			nPrice = mapOverrides[iOverride];
			if ( !pszCategory )
			{
				pszCategory = "Tools";	// sold only because the overrides file says so
			}
		}

		if ( nPrice <= 0 || !pszCategory )
			continue;

		// Every price used needs a "price point" the store knows about.
		pPricePoints->FindKey( CFmtStr( "%d", nPrice ), true );

		KeyValues *pEntry = pEntries->FindKey( CFmtStr( "%d", pItemDef->GetDefinitionIndex() ), true );
		pEntry->SetString( "item_link", pItemDef->GetDefinitionName() );
		pEntry->SetString( "category_tags", pszCategory );
		pEntry->SetInt( "base_price", nPrice );
		if ( nSalePercent > 0 )
		{
			pEntry->SetInt( "sale_percentage", nSalePercent );
		}
		vecEntryKVs.AddToTail( pEntry );

		if ( !V_strcmp( pszCategory, "Cosmetics" ) || !V_strcmp( pszCategory, "Taunts" ) )
		{
			vecFeatureCandidates.AddToTail( pItemDef );
		}
	}

	CUniformRandomStream randomByDay;
	randomByDay.SetSeed( (int)( time( NULL ) / ( 60 * 60 * 24 ) ) );

	KeyValues *pFeaturedItems = pSheet->FindKey( "featured_items", true );
	for ( int i = 0; i < kFeaturedItemCount && vecFeatureCandidates.Count() > 0; ++i )
	{
		const int iPick = randomByDay.RandomInt( 0, vecFeatureCandidates.Count() - 1 );
		const CEconItemDefinition *pItemDef = vecFeatureCandidates[iPick];
		vecFeatureCandidates.FastRemove( iPick );

		pFeaturedItems->SetInt( pItemDef->GetDefinitionName(), 1 );

		KeyValues *pEntry = pEntries->FindKey( CFmtStr( "%d", pItemDef->GetDefinitionIndex() ) );
		if ( pEntry )
		{
			pEntry->SetString( "category_tags", CFmtStr( "%s+Featured", pEntry->GetString( "category_tags" ) ) );
		}
	}

	return pSheet;
}

//-----------------------------------------------------------------------------
// Purchases
//-----------------------------------------------------------------------------
int OfflineStore_GiveItem( const CEconItemDefinition *pItemDef, int nCount )
{
	int nCreated = 0;
	for ( int i = 0; i < nCount; ++i )
	{
		if ( pItemDef->IsBundle() )
		{
			for ( int j = 0; j < pItemDef->GetBundleItemCount(); ++j )
			{
				const CEconItemDefinition *pBundleItemDef = GetItemSchema()->GetItemDefinition( pItemDef->GetBundleItem( j ) );
				if ( pBundleItemDef && TFOfflineInventory().CreateItem( pBundleItemDef, -1, NULL, kEconItemOrigin_Purchased, UNACK_ITEM_PURCHASED ) != INVALID_ITEM_ID )
				{
					nCreated++;
				}
			}
		}
		else if ( TFOfflineInventory().CreateItem( pItemDef, -1, NULL, kEconItemOrigin_Purchased, UNACK_ITEM_PURCHASED ) != INVALID_ITEM_ID )
		{
			nCreated++;
		}
	}
	return nCreated;
}

//=============================================================================
// Console commands
//=============================================================================
static bool CheckStoreInventoryReady()
{
	if ( TFOfflineInventory().IsLoaded() )
		return true;

	Msg( "The offline inventory hasn't loaded yet. Make sure Steam is running, then try again in a moment.\n" );
	return false;
}

static void PrintWallet()
{
	char szMoney[64];
	OfflineStore_FormatMoney( TFOfflineInventory().GetWalletCents(), szMoney, sizeof( szMoney ) );
	Msg( "Your wallet has %s.\n", szMoney );
}

CON_COMMAND( inv_wallet, "Show how much money is in your offline wallet (for the Mann Co. Store)." )
{
	if ( CheckStoreInventoryReady() )
	{
		PrintWallet();
	}
}

CON_COMMAND( inv_wallet_add, "Add money to your offline wallet (for the Mann Co. Store).\n"
	"  Usage: inv_wallet_add <dollars>    e.g. inv_wallet_add 50" )
{
	if ( !CheckStoreInventoryReady() )
		return;

	if ( args.ArgC() < 2 )
	{
		Msg( "Usage: inv_wallet_add <dollars>\n" );
		return;
	}

	const int nCents = (int)( V_atof( args[1] ) * 100.f + ( V_atof( args[1] ) < 0 ? -0.5f : 0.5f ) );
	TFOfflineInventory().SetWalletCents( TFOfflineInventory().GetWalletCents() + nCents );
	PrintWallet();
}

CON_COMMAND( inv_wallet_set, "Set how much money is in your offline wallet (for the Mann Co. Store).\n"
	"  Usage: inv_wallet_set <dollars>" )
{
	if ( !CheckStoreInventoryReady() )
		return;

	if ( args.ArgC() < 2 )
	{
		Msg( "Usage: inv_wallet_set <dollars>\n" );
		return;
	}

	TFOfflineInventory().SetWalletCents( (int)( V_atof( args[1] ) * 100.f + 0.5f ) );
	PrintWallet();
}

CON_COMMAND( inv_store_reload, "Rebuild the Mann Co. Store's prices (after editing " OFFLINE_STORE_PRICES_FILE ")." )
{
	if ( CStorePanel::IsPricesheetLoaded() )
	{
		CStorePanel::RequestPricesheet();
		Msg( "Store prices reloaded.\n" );
	}
	else
	{
		Msg( "The store will use the new prices when you open it.\n" );
	}
}
