//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Offline crafting. See tf_offline_crafting.h.
//
//=============================================================================

#include "cbase.h"
#include "tf_offline_crafting.h"
#include "tf_offline_inventory.h"
#include "econ_gcmessages.h"
#include "econ_item.h"
#include "econ_item_schema.h"
#include "econ_item_system.h"
#include "item_selection_criteria.h"
#include "tf_item_schema.h"
#include "tf_shareddefs.h"
#include "econ_ui.h"
#include "crafting_panel.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//-----------------------------------------------------------------------------
// What the inputs decided about the outputs: the class, slot and item set they
// must fit (from the recipe's add_class_usage_to_output etc.), if any.
//-----------------------------------------------------------------------------
struct OfflineCraftParams_t
{
	OfflineCraftParams_t() : m_iClass( TF_CLASS_UNDEFINED ), m_iSlot( LOADOUT_POSITION_INVALID ), m_pItemSet( NULL ) {}

	int								m_iClass;
	int								m_iSlot;
	const CEconItemSetDefinition	*m_pItemSet;
};

struct OfflineCraftOutput_t
{
	const CTFItemDefinition	*m_pItemDef;
	int						m_nQuality;		// -1 for the item's usual quality
};

//-----------------------------------------------------------------------------
static const CTFItemDefinition *GetTFItemDef( const CEconItem *pItem )
{
	return pItem ? GetItemSchema()->GetTFItemDefinition( pItem->GetDefinitionIndex() ) : NULL;
}

static const CEconItem *FindCraftItem( const CUtlVector< CEconItem * > &vecItems, uint64 unItemID )
{
	FOR_EACH_VEC( vecItems, i )
	{
		if ( vecItems[i]->GetItemID() == unItemID )
			return vecItems[i];
	}
	return NULL;
}

// Every loadout slot an item can go in, for any class.
static void GetItemSlots( const CTFItemDefinition *pItemDef, CUtlVector< int > &vecSlots )
{
	vecSlots.RemoveAll();
	if ( pItemDef->GetDefaultLoadoutSlot() != LOADOUT_POSITION_INVALID )
	{
		vecSlots.AddToTail( pItemDef->GetDefaultLoadoutSlot() );
	}
	for ( int iClass = TF_FIRST_NORMAL_CLASS; iClass < TF_LAST_NORMAL_CLASS; iClass++ )
	{
		if ( !pItemDef->CanBeUsedByClass( iClass ) )
			continue;
		const int iSlot = pItemDef->GetLoadoutSlot( iClass );
		if ( iSlot != LOADOUT_POSITION_INVALID && !vecSlots.HasElement( iSlot ) )
		{
			vecSlots.AddToTail( iSlot );
		}
	}
}

static bool AllItemsUsableByClass( const CUtlVector< CEconItem * > &vecItems, int iClass )
{
	FOR_EACH_VEC( vecItems, i )
	{
		const CTFItemDefinition *pItemDef = GetTFItemDef( vecItems[i] );
		if ( !pItemDef || !pItemDef->CanBeUsedByClass( iClass ) )
			return false;
	}
	return true;
}

static bool AllItemsFitSlot( const CUtlVector< CEconItem * > &vecItems, int iSlot )
{
	CUtlVector< int > vecSlots;
	FOR_EACH_VEC( vecItems, i )
	{
		const CTFItemDefinition *pItemDef = GetTFItemDef( vecItems[i] );
		if ( !pItemDef )
			return false;
		GetItemSlots( pItemDef, vecSlots );
		if ( !vecSlots.HasElement( iSlot ) )
			return false;
	}
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Finds the recipe the items make. nRecipeDefIndex is the recipe picked
//			in the crafting window, or RECIPE_CUSTOM to try every recipe.
//			vecChosen gets the item that matched each of the recipe's inputs.
//-----------------------------------------------------------------------------
static const CEconCraftingRecipeDefinition *FindMatchingRecipe( int nRecipeDefIndex, CUtlVector< CEconItem * > &vecItems, CUtlVector< uint64 > &vecChosen )
{
	const CEconItemSchema::RecipeDefinitionMap_t &mapRecipes = GetItemSchema()->GetRecipeDefinitionMap();

	FOR_EACH_MAP( mapRecipes, i )
	{
		const CEconCraftingRecipeDefinition *pRecipe = mapRecipes[i];
		if ( !pRecipe || pRecipe->IsDisabled() )
			continue;

		if ( nRecipeDefIndex != RECIPE_CUSTOM && pRecipe->GetDefinitionIndex() != nRecipeDefIndex )
			continue;

		vecChosen.RemoveAll();
		if ( pRecipe->ItemListMatchesInputs( &vecItems, NULL, false, &vecChosen ) )
			return pRecipe;
	}

	return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: Works out which class / slot / item set the output has to fit.
//			Where the inputs allow more than one class or slot, one is picked at
//			random (e.g. three Shotguns smelt into a token for any class that
//			uses the Shotgun).
//-----------------------------------------------------------------------------
static void GetCraftParams( const CEconCraftingRecipeDefinition *pRecipe, const CUtlVector< CEconItem * > &vecItems,
							const CUtlVector< uint64 > &vecChosen, OfflineCraftParams_t &params )
{
	const int iClassInput = pRecipe->GetClassUsageForOutputFromInput();
	if ( iClassInput >= 0 && iClassInput < vecChosen.Count() )
	{
		const CTFItemDefinition *pItemDef = GetTFItemDef( FindCraftItem( vecItems, vecChosen[iClassInput] ) );
		if ( pItemDef )
		{
			const char *pszToken = pItemDef->GetClassToken();
			if ( pszToken && pszToken[0] )
			{
				params.m_iClass = StringFieldToInt( pszToken, GetItemSchema()->GetClassUsabilityStrings() );
			}
			else
			{
				CUtlVector< int > vecClasses;
				for ( int iClass = TF_FIRST_NORMAL_CLASS; iClass < TF_LAST_NORMAL_CLASS; iClass++ )
				{
					if ( !pItemDef->CanBeUsedByClass( iClass ) )
						continue;
					if ( pRecipe->RequiresAllSameClass() && !AllItemsUsableByClass( vecItems, iClass ) )
						continue;
					vecClasses.AddToTail( iClass );
				}
				if ( vecClasses.Count() )
				{
					params.m_iClass = vecClasses[ RandomInt( 0, vecClasses.Count() - 1 ) ];
				}
			}
		}
	}

	const int iSlotInput = pRecipe->GetSlotUsageForOutputFromInput();
	if ( iSlotInput >= 0 && iSlotInput < vecChosen.Count() )
	{
		const CTFItemDefinition *pItemDef = GetTFItemDef( FindCraftItem( vecItems, vecChosen[iSlotInput] ) );
		if ( pItemDef )
		{
			const char *pszToken = pItemDef->GetSlotToken();
			if ( pszToken && pszToken[0] )
			{
				params.m_iSlot = StringFieldToInt( pszToken, GetItemSchema()->GetLoadoutStrings( EQUIP_TYPE_CLASS ) );
			}
			else if ( params.m_iClass != TF_CLASS_UNDEFINED && pItemDef->GetLoadoutSlot( params.m_iClass ) != LOADOUT_POSITION_INVALID )
			{
				params.m_iSlot = pItemDef->GetLoadoutSlot( params.m_iClass );
			}
			else
			{
				// Slot tokens have no class; weapons can sit in different slots for different classes.
				CUtlVector< int > vecSlots;
				GetItemSlots( pItemDef, vecSlots );
				if ( pRecipe->RequiresAllSameSlot() )
				{
					FOR_EACH_VEC_BACK( vecSlots, i )
					{
						if ( !AllItemsFitSlot( vecItems, vecSlots[i] ) )
						{
							vecSlots.Remove( i );
						}
					}
				}
				if ( vecSlots.Count() )
				{
					params.m_iSlot = vecSlots[ RandomInt( 0, vecSlots.Count() - 1 ) ];
				}
			}
		}
	}

	const int iSetInput = pRecipe->GetSetForOutputFromInput();
	if ( iSetInput >= 0 && iSetInput < vecChosen.Count() )
	{
		const CTFItemDefinition *pItemDef = GetTFItemDef( FindCraftItem( vecItems, vecChosen[iSetInput] ) );
		if ( pItemDef )
		{
			params.m_pItemSet = pItemDef->GetItemSetDefinition();
		}
	}
}

//-----------------------------------------------------------------------------
static bool FitsCraftParams( const CTFItemDefinition *pItemDef, const OfflineCraftParams_t &params )
{
	if ( params.m_iClass != TF_CLASS_UNDEFINED && !pItemDef->CanBeUsedByClass( params.m_iClass ) )
		return false;

	if ( params.m_iSlot != LOADOUT_POSITION_INVALID )
	{
		if ( params.m_iClass != TF_CLASS_UNDEFINED )
		{
			if ( pItemDef->GetLoadoutSlot( params.m_iClass ) != params.m_iSlot )
				return false;
		}
		else
		{
			CUtlVector< int > vecSlots;
			GetItemSlots( pItemDef, vecSlots );
			if ( !vecSlots.HasElement( params.m_iSlot ) )
				return false;
		}
	}

	if ( params.m_pItemSet && pItemDef->GetItemSetDefinition() != params.m_pItemSet )
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: The schema marks more items as craftable than the item servers ever
//			handed out from a random recipe. Leave out the odd ones: stock
//			weapons, war paints, festives and other limited editions, promo
//			all-class melees, and items that only show during a holiday.
//-----------------------------------------------------------------------------
static bool IsGoodRandomCraftResult( const CTFItemDefinition *pItemDef )
{
	if ( pItemDef->IsBaseItem() || pItemDef->GetQuality() == AE_NORMAL )
		return false;

	if ( !V_strnicmp( pItemDef->GetDefinitionName(), "Upgradeable ", 12 ) )
		return false;

	const char *pszHoliday = pItemDef->GetHolidayRestriction();
	if ( pszHoliday && pszHoliday[0] )
		return false;

	const char *pszItemClass = pItemDef->GetItemClass();
	if ( pszItemClass && !V_stricmp( pszItemClass, "saxxy" ) )
		return false;

	static const char *s_pszExcludingAttrs[] =
	{
		"limited quantity item",
		"paintkit_proto_def_index",
	};
	for ( int i = 0; i < ARRAYSIZE( s_pszExcludingAttrs ); i++ )
	{
		const CEconItemAttributeDefinition *pAttrDef = GetItemSchema()->GetAttributeDefinitionByName( s_pszExcludingAttrs[i] );
		if ( pAttrDef && ::FindAttribute( pItemDef, pAttrDef ) )
			return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Picks the item for one of the recipe's outputs.
//-----------------------------------------------------------------------------
static bool PickOutput( const CItemSelectionCriteria &criteria, const OfflineCraftParams_t &params, OfflineCraftOutput_t &output )
{
	// "Class hat" recipes ask for a hat that isn't for every class.
	const char *pszNotEqualField = criteria.GetFieldForFirstConditionOfType( k_EOperator_Float_Not_EQ );
	const bool bExcludeAllClassItems = pszNotEqualField && !V_stricmp( pszNotEqualField, "all_classes" );

	CUtlVector< const CTFItemDefinition * > vecCandidates;
	const CEconItemSchema::SortedItemDefinitionMap_t &mapItems = GetItemSchema()->GetSortedItemDefinitionMap();
	FOR_EACH_MAP( mapItems, i )
	{
		const CTFItemDefinition *pItemDef = GetItemSchema()->GetTFItemDefinition( mapItems[i]->GetDefinitionIndex() );
		if ( !pItemDef || !criteria.BEvaluate( pItemDef ) )
			continue;

		if ( !FitsCraftParams( pItemDef, params ) )
			continue;

		if ( bExcludeAllClassItems && pItemDef->CanBeUsedByAllClasses() )
			continue;

		vecCandidates.AddToTail( pItemDef );
	}

	if ( vecCandidates.Count() == 0 )
		return false;

	// A random result: only pick from the items you could normally get that way.
	if ( vecCandidates.Count() > 1 )
	{
		CUtlVector< const CTFItemDefinition * > vecGood;
		FOR_EACH_VEC( vecCandidates, i )
		{
			if ( IsGoodRandomCraftResult( vecCandidates[i] ) )
			{
				vecGood.AddToTail( vecCandidates[i] );
			}
		}
		if ( vecGood.Count() )
		{
			vecCandidates.Swap( vecGood );
		}
	}

	output.m_pItemDef = vecCandidates[ RandomInt( 0, vecCandidates.Count() - 1 ) ];
	output.m_nQuality = -1;
	if ( criteria.BQualitySet() )
	{
		const int nQuality = criteria.GetQuality();
		if ( nQuality != k_unItemQuality_Any && nQuality != AE_USE_SCRIPT_VALUE && nQuality > AE_NORMAL && nQuality < AE_MAX_TYPES )
		{
			output.m_nQuality = nQuality;
		}
	}
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Answers the crafting window, the way CGCCraftResponse does.
//-----------------------------------------------------------------------------
static void SendCraftResponse( EGCMsgResponse eResponse, CUtlVector< uint64 > &vecNewItems, int nRecipeUsed )
{
	if ( EconUI() && EconUI()->GetCraftingPanel() )
	{
		EconUI()->GetCraftingPanel()->OnCraftResponse( eResponse, &vecNewItems, nRecipeUsed );
	}
}

//-----------------------------------------------------------------------------
void OfflineCrafting_ProcessCraftRequest( const void *pBody, uint32 cubBody )
{
	CUtlVector< uint64 > vecNewItems;

	MsgGCCraft_t header;
	if ( !pBody || cubBody < sizeof( header ) )
	{
		SendCraftResponse( k_EGCMsgResponseInvalid, vecNewItems, -1 );
		return;
	}
	V_memcpy( &header, pBody, sizeof( header ) );

	const int nItemCount = header.m_nItemCount;
	if ( nItemCount == 0 || cubBody < sizeof( header ) + nItemCount * sizeof( uint64 ) )
	{
		SendCraftResponse( k_EGCMsgResponseInvalid, vecNewItems, -1 );
		return;
	}

	// The items, which must all be ours, different, and craftable.
	CUtlVector< CEconItem * > vecItems;
	const uint8 *pubIDs = (const uint8 *)pBody + sizeof( header );
	for ( int i = 0; i < nItemCount; i++ )
	{
		uint64 unItemID;
		V_memcpy( &unItemID, pubIDs + i * sizeof( uint64 ), sizeof( unItemID ) );

		CEconItem *pItem = const_cast< CEconItem * >( TFOfflineInventory().GetLiveItem( unItemID ) );
		if ( !pItem || vecItems.HasElement( pItem ) || !GetTFItemDef( pItem ) )
		{
			SendCraftResponse( k_EGCMsgResponseInvalid, vecNewItems, -1 );
			return;
		}

		if ( !pItem->IsUsableInCrafting() )
		{
			Msg( "Offline crafting: an item can't be used in crafting.\n" );
			SendCraftResponse( k_EGCMsgResponseDenied, vecNewItems, -1 );
			return;
		}

		vecItems.AddToTail( pItem );
	}

	CUtlVector< uint64 > vecChosen;
	const CEconCraftingRecipeDefinition *pRecipe = FindMatchingRecipe( header.m_nRecipeDefIndex, vecItems, vecChosen );
	if ( !pRecipe )
	{
		SendCraftResponse( k_EGCMsgResponseNoMatch, vecNewItems, -1 );
		return;
	}

	// Decide every output before using anything up.
	OfflineCraftParams_t params;
	GetCraftParams( pRecipe, vecItems, vecChosen, params );

	CUtlVector< OfflineCraftOutput_t > vecOutputs;
	const CUtlVector< CItemSelectionCriteria > &vecOutputCriteria = pRecipe->GetOutputItems();
	FOR_EACH_VEC( vecOutputCriteria, i )
	{
		OfflineCraftOutput_t &output = vecOutputs[ vecOutputs.AddToTail() ];
		if ( !PickOutput( vecOutputCriteria[i], params, output ) )
		{
			Warning( "Offline crafting: recipe %d has no item to give for output %d.\n", pRecipe->GetDefinitionIndex(), i + 1 );
			SendCraftResponse( k_EGCMsgResponseUnknownError, vecNewItems, -1 );
			return;
		}
	}

	if ( vecOutputs.Count() == 0 )
	{
		SendCraftResponse( k_EGCMsgResponseNoMatch, vecNewItems, -1 );
		return;
	}

	// Use up the inputs, then make the outputs.
	CUtlVector< itemid_t > vecInputIDs;
	FOR_EACH_VEC( vecItems, i )
	{
		vecInputIDs.AddToTail( vecItems[i]->GetItemID() );
	}
	vecItems.RemoveAll();	// the items are about to go away

	TFOfflineInventory().BeginBulkChange();

	FOR_EACH_VEC( vecInputIDs, i )
	{
		TFOfflineInventory().DeleteItem( vecInputIDs[i] );
	}

	FOR_EACH_VEC( vecOutputs, i )
	{
		const itemid_t unNewItemID = TFOfflineInventory().CreateItem( vecOutputs[i].m_pItemDef, vecOutputs[i].m_nQuality, NULL,
																	  kEconItemOrigin_Crafted, UNACK_ITEM_CRAFTED );
		if ( unNewItemID != INVALID_ITEM_ID )
		{
			vecNewItems.AddToTail( unNewItemID );

			char szName[256];
			OfflineInventory_GetItemDefDisplayName( vecOutputs[i].m_pItemDef, szName, sizeof( szName ) );
			Msg( "Offline crafting: crafted %s.\n", szName );
		}
	}

	TFOfflineInventory().EndBulkChange();

	SendCraftResponse( vecNewItems.Count() ? k_EGCMsgResponseOK : k_EGCMsgResponseUnknownError, vecNewItems, pRecipe->GetDefinitionIndex() );
}
