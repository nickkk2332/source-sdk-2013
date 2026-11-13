//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Offline crates. See tf_offline_crates.h.
//
//			How a crate is rolled:
//
//			1. The crate's loot list comes from its series number (or a loot
//			   list name), exactly like the "Contents" view in the backpack.
//			2. One entry is picked from the list. Nested lists are followed
//			   down. Item collections (cases) are split into rarity tiers, and
//			   each tier is 5x rarer than the one below it, which gives roughly
//			   TF2's case odds: Mercenary 80%, Commando 16%, Assassin 3.2%,
//			   Elite 0.6%.
//			3. "Jobs" attached to the lists along the way and to the crate
//			   itself run next. They add attributes (kill counters, killstreak
//			   sheens, war paint wear, unusual effects, ...), raise the quality
//			   (Strange, Unusual) or add bonus items. The game's copy of the
//			   schema has no odds for them, so chance-based jobs use the
//			   inv_crate_* console variables and the rest always run.
//			4. Old crates whose contents say "or an Exceedingly Rare Special
//			   Item!" can instead give an Unusual hat with an effect from that
//			   crate's effect series.
//
//=============================================================================

#include "cbase.h"
#include "tf_offline_crates.h"
#include "tf_offline_inventory.h"
#include "econ_item.h"
#include "econ_item_schema.h"
#include "econ_item_tools.h"
#include "item_selection_criteria.h"
#include "KeyValues.h"
#include "vstdlib/random.h"
#include <math.h>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar inv_crate_unusual_chance( "inv_crate_unusual_chance", "0.01", FCVAR_ARCHIVE,
	"Offline inventory: chance (0 to 1) that a crate which can hold an Unusual gives one. TF2 is about 1%." );
ConVar inv_crate_strange_chance( "inv_crate_strange_chance", "0.1", FCVAR_ARCHIVE,
	"Offline inventory: chance (0 to 1) that an item from a case comes out Strange." );
ConVar inv_crate_bonus_chance( "inv_crate_bonus_chance", "0.05", FCVAR_ARCHIVE,
	"Offline inventory: chance (0 to 1) of each possible bonus item (paint, tools, strange parts, ...) from a case." );

static const int kMaxLootListDepth = 16;
static const int kMaxBonusDepth = 4;

//-----------------------------------------------------------------------------
// COfflineLootItem
//-----------------------------------------------------------------------------
COfflineLootItem::COfflineLootItem()
	: m_pItemDef( NULL )
	, m_nQuality( -1 )
	, m_pAttributes( NULL )
{
}

COfflineLootItem::~COfflineLootItem()
{
	if ( m_pAttributes )
	{
		m_pAttributes->deleteThis();
	}
}

void COfflineLootItem::SetAttribute( const char *pszName, const char *pszValue )
{
	if ( !m_pAttributes )
	{
		m_pAttributes = new KeyValues( "attributes" );
	}
	m_pAttributes->SetString( pszName, pszValue );
}

bool COfflineLootItem::HasAttribute( const char *pszName ) const
{
	return m_pAttributes && m_pAttributes->FindKey( pszName ) != NULL;
}

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------

// Picks an index from vecWeights in proportion to the weights. -1 if none are positive.
static int PickWeighted( const CUtlVector< float > &vecWeights )
{
	float flTotal = 0.f;
	FOR_EACH_VEC( vecWeights, i )
	{
		if ( vecWeights[i] > 0.f )
		{
			flTotal += vecWeights[i];
		}
	}
	if ( flTotal <= 0.f )
		return -1;

	float flRoll = RandomFloat( 0.f, flTotal );
	int iPicked = -1;
	FOR_EACH_VEC( vecWeights, i )
	{
		if ( vecWeights[i] <= 0.f )
			continue;

		iPicked = i;
		if ( flRoll < vecWeights[i] )
			break;
		flRoll -= vecWeights[i];
	}
	return iPicked;
}

static bool RollChance( float flChance )
{
	if ( flChance >= 1.f )
		return true;
	if ( flChance <= 0.f )
		return false;
	return RandomFloat( 0.f, 1.f ) < flChance;
}

// The text form of a schema attribute value, ready for the save file.
static void GetStaticAttribValueString( const CEconItemAttributeDefinition *pAttrDef, const attribute_data_union_t &value, CUtlString &strOut )
{
	const ISchemaAttributeType *pAttrType = pAttrDef->GetAttributeType();
	if ( pAttrType && pAttrType->GetTypeUniqueIdentifier() == GetAttributeTypeUniqueIdentifier< attrib_value_t >() && pAttrDef->IsStoredAsInteger() )
	{
		// Multi-line attribute blocks in the schema are parsed as floats even for
		// integer attributes, so "1" is stored as the bits of 1.0f. Undo that.
		uint32 unValue = value.asUint32;
		const float flValue = value.asFloat;
		if ( unValue >= 0x01000000 && flValue >= 0.f && flValue < 16777216.f && flValue == floorf( flValue ) )
		{
			unValue = (uint32)flValue;
		}
		strOut.Format( "%u", unValue );
		return;
	}

	if ( pAttrType )
	{
		std::string strValue;
		pAttrType->ConvertEconAttributeValueToString( pAttrDef, value, &strValue );
		strOut = strValue.c_str();
		return;
	}

	strOut.Format( "%f", value.asFloat );
}

static int GetStaticAttribValueInt( const CEconItemAttributeDefinition *pAttrDef, const attribute_data_union_t &value )
{
	CUtlString strValue;
	GetStaticAttribValueString( pAttrDef, value, strValue );
	return (int)V_atof( strValue.Get() );
}

// Finds a static (definition) attribute on an item definition.
static const static_attrib_t *FindStaticAttrib( const CEconItemDefinition *pItemDef, const char *pszAttrName )
{
	const CEconItemAttributeDefinition *pAttrDef = GetItemSchema()->GetAttributeDefinitionByName( pszAttrName );
	if ( !pItemDef || !pAttrDef )
		return NULL;

	const CUtlVector< static_attrib_t > &vecAttribs = pItemDef->GetStaticAttributes();
	FOR_EACH_VEC( vecAttribs, i )
	{
		if ( vecAttribs[i].iDefIndex == pAttrDef->GetDefinitionIndex() )
			return &vecAttribs[i];
	}
	return NULL;
}

// The schema has "hat only" and "taunt only" variants of the unusual effect
// attributes for its rolls; on an actual item they are the regular ones.
static const char *GetUnusualEffectTargetAttribute( const char *pszAttrName )
{
	if ( !V_stricmp( pszAttrName, "attach particle effect" ) || !V_stricmp( pszAttrName, "hat only unusual effect" ) )
		return "attach particle effect";
	if ( !V_stricmp( pszAttrName, "on taunt attach particle index" ) || !V_stricmp( pszAttrName, "taunt only unusual effect" ) )
		return "on taunt attach particle index";
	return NULL;
}

//-----------------------------------------------------------------------------
// Rolling loot lists
//-----------------------------------------------------------------------------

// Collections (cases) are split into one nested list per rarity tier. The
// schema registers each as "<collection>_<rarity>", but the list's own name is
// just the rarity ("rare", "mythical", ...), so accept either.
// Returns that rarity, or k_unItemRarity_Any for ordinary nested lists.
static uint8 GetRarityTierOfNestedList( const CEconLootListDefinition *pNestedList )
{
	const CUtlVector< drop_item_t > &vecContents = pNestedList->GetLootListContents();
	FOR_EACH_VEC( vecContents, i )
	{
		if ( vecContents[i].m_iItemOrLootlistDef < 0 )
			continue;

		const CEconItemDefinition *pItemDef = GetItemSchema()->GetItemDefinition( vecContents[i].m_iItemOrLootlistDef );
		if ( !pItemDef || pItemDef->GetRarity() == k_unItemRarity_Any )
			continue;

		const char *pszRarityName = GetItemSchema()->GetRarityName( pItemDef->GetRarity() );
		const char *pszListName = pNestedList->GetName();
		if ( pszRarityName && pszListName
			&& ( !V_stricmp( pszListName, pszRarityName ) || V_strEndsWith( pszListName, CFmtStr( "_%s", pszRarityName ) ) ) )
			return pItemDef->GetRarity();

		return k_unItemRarity_Any;
	}
	return k_unItemRarity_Any;
}

static float GetLootListEntryWeight( const drop_item_t &entry )
{
	const int iID = entry.m_iItemOrLootlistDef;
	if ( iID < 0 )
	{
		const CEconLootListDefinition *pNestedList = GetItemSchema()->GetLootListByIndex( -iID - 1 );
		if ( !pNestedList )
			return 0.f;

		// Rarity tiers: rare (Mercenary) 125, mythical (Commando) 25, legendary (Assassin) 5,
		// ancient (Elite) 1, and likewise below rare for war paints.
		const uint8 unRarity = GetRarityTierOfNestedList( pNestedList );
		if ( unRarity >= 1 && unRarity <= 6 )
			return powf( 5.f, (float)( 6 - unRarity ) );
	}
	else if ( !GetItemSchema()->GetItemDefinition( iID ) )
	{
		return 0.f;
	}

	// The game's schema loader throws entry weights away (they're all equal in
	// the public data anyway). Use one if a modified schema keeps it.
	return entry.m_flWeight > 0.f ? entry.m_flWeight : 1.f;
}

// Picks one item from a loot list, following nested lists, and collects each
// list's jobs along the way.
static const CEconItemDefinition *RollLootList( const CEconLootListDefinition *pLootList, CUtlVector< const CLootlistJob * > &vecJobs, int nDepth = 0 )
{
	if ( !pLootList || nDepth > kMaxLootListDepth )
		return NULL;

	const CUtlVector< drop_item_t > &vecContents = pLootList->GetLootListContents();
	CUtlVector< float > vecWeights;
	FOR_EACH_VEC( vecContents, i )
	{
		vecWeights.AddToTail( GetLootListEntryWeight( vecContents[i] ) );
	}

	const int iPicked = PickWeighted( vecWeights );
	if ( iPicked < 0 )
		return NULL;

	const CUtlVector< CLootlistJob * > &vecListJobs = pLootList->GetLootlistJobs();
	FOR_EACH_VEC( vecListJobs, i )
	{
		vecJobs.AddToTail( vecListJobs[i] );
	}

	const int iID = vecContents[iPicked].m_iItemOrLootlistDef;
	if ( iID < 0 )
		return RollLootList( GetItemSchema()->GetLootListByIndex( -iID - 1 ), vecJobs, nDepth + 1 );

	return GetItemSchema()->GetItemDefinition( iID );
}

// For loot lists that aren't regular schema lists (e.g. items listed in crate attributes).
static const CEconItemDefinition *RollFromPublicContents( const IEconLootList *pLootList )
{
	class CCollectItems : public IEconLootList::IEconLootListIterator
	{
	public:
		virtual void OnIterate( item_definition_index_t unItemDefIndex ) OVERRIDE
		{
			m_vecItems.AddToTail( unItemDefIndex );
		}
		CUtlVector< item_definition_index_t > m_vecItems;
	};

	CCollectItems collector;
	pLootList->EnumerateUserFacingPotentialDrops( &collector );
	if ( collector.m_vecItems.Count() == 0 )
		return NULL;

	return GetItemSchema()->GetItemDefinition( collector.m_vecItems[ RandomInt( 0, collector.m_vecItems.Count() - 1 ) ] );
}

//-----------------------------------------------------------------------------
// Jobs
//-----------------------------------------------------------------------------
struct OfflineRollState_t
{
	OfflineRollState_t() : m_bStrange( false ), m_bUnusual( false ) {}

	bool m_bStrange;
	bool m_bUnusual;
};

static bool JobAddsUnusualEffect( const CLootlistJob *pJob )
{
	const CUtlVector< CLootlistJob::RandomAttributeInfo_t > &vecGroups = pJob->GetAttributes();
	FOR_EACH_VEC( vecGroups, i )
	{
		const random_attrib_t *pGroup = vecGroups[i].m_pRandomAttributes;
		if ( !pGroup )
			continue;

		FOR_EACH_VEC( pGroup->m_RandomAttributes, j )
		{
			const CEconItemAttributeDefinition *pAttrDef = GetItemSchema()->GetAttributeDefinition( pGroup->m_RandomAttributes[j].m_staticAttrib.iDefIndex );
			if ( pAttrDef && GetUnusualEffectTargetAttribute( pAttrDef->GetDefinitionName() ) )
				return true;
		}
	}
	return false;
}

static float GetJobChance( const CLootlistJob *pJob )
{
	// Use real odds if the schema has them.
	if ( pJob->GetChanceToRunJob() > 0.f )
		return pJob->GetChanceToRunJob();

	// Bonus items from cases.
	if ( pJob->GetAdditionalDrops().Count() > 0 )
		return inv_crate_bonus_chance.GetFloat();

	const char *pszTemplateName = pJob->GetTemplateName();
	if ( pszTemplateName && V_stristr( pszTemplateName, "guaranteed" ) )
		return 1.f;
	if ( pszTemplateName && V_stristr( pszTemplateName, "strange_quality_chance" ) )
		return inv_crate_strange_chance.GetFloat();

	if ( JobAddsUnusualEffect( pJob ) )
		return inv_crate_unusual_chance.GetFloat();

	// Everything else (kill counters on Strange-only lists, killstreak sheens,
	// war paint wear, ...) is part of what the item is, so it always happens.
	return 1.f;
}

static void ApplyRolledAttribute( const static_attrib_t &attrib, COfflineLootItem *pItem, OfflineRollState_t &state )
{
	const CEconItemAttributeDefinition *pAttrDef = GetItemSchema()->GetAttributeDefinition( attrib.iDefIndex );
	if ( !pAttrDef )
		return;

	const char *pszAttrName = pAttrDef->GetDefinitionName();

	// These two are instructions to the item server, not attributes an item keeps.
	if ( !V_stricmp( pszAttrName, "elevate quality" ) )
	{
		const int nQuality = GetStaticAttribValueInt( pAttrDef, attrib.m_value );
		if ( nQuality == AE_STRANGE )
		{
			state.m_bStrange = true;
		}
		else if ( nQuality == AE_UNUSUAL )
		{
			state.m_bUnusual = true;
		}
		else if ( nQuality > AE_NORMAL && nQuality < AE_MAX_TYPES )
		{
			pItem->m_nQuality = nQuality;
		}
		return;
	}
	if ( !V_stricmp( pszAttrName, "elevate to unusual if applicable" ) )
		return;	// handled below: anything that gets an unusual effect becomes Unusual

	CUtlString strValue;
	GetStaticAttribValueString( pAttrDef, attrib.m_value, strValue );

	const char *pszEffectAttr = GetUnusualEffectTargetAttribute( pszAttrName );
	if ( pszEffectAttr )
	{
		pItem->SetAttribute( pszEffectAttr, strValue.Get() );
		state.m_bUnusual = true;
		return;
	}

	pItem->SetAttribute( pszAttrName, strValue.Get() );
}

// Picks one attribute from a group (e.g. one unusual effect out of a list) and applies it,
// if the item qualifies (e.g. hat effects only go on hats).
static void ApplyRandomAttributeGroup( const random_attrib_t *pGroup, COfflineLootItem *pItem, OfflineRollState_t &state )
{
	FOR_EACH_VEC( pGroup->m_vecCriteria, i )
	{
		if ( pGroup->m_vecCriteria[i] && !pGroup->m_vecCriteria[i]->BEvaluate( pItem->m_pItemDef ) )
			return;
	}

	CUtlVector< float > vecWeights;
	FOR_EACH_VEC( pGroup->m_RandomAttributes, i )
	{
		const lootlist_attrib_t &attrib = pGroup->m_RandomAttributes[i];
		const bool bAllowed = !attrib.BHasAnyCriteria() || attrib.BItemPassAllCriteria( pItem->m_pItemDef );
		vecWeights.AddToTail( bAllowed ? ( attrib.m_flWeight > 0.f ? attrib.m_flWeight : 1.f ) : 0.f );
	}

	const int iPicked = PickWeighted( vecWeights );
	if ( iPicked >= 0 )
	{
		ApplyRolledAttribute( pGroup->m_RandomAttributes[iPicked].m_staticAttrib, pItem, state );
	}
}

static int GetDefaultQuality( const CEconItemDefinition *pItemDef )
{
	const int nQuality = pItemDef->GetQuality();
	return ( nQuality <= AE_NORMAL || nQuality >= AE_MAX_TYPES ) ? AE_UNIQUE : nQuality;
}

static void FinishItemQuality( COfflineLootItem *pItem, const OfflineRollState_t &state )
{
	if ( state.m_bUnusual )
	{
		pItem->m_nQuality = AE_UNUSUAL;
	}

	if ( state.m_bStrange )
	{
		// A Strange roll on something that already has a special quality (e.g.
		// Unusual) makes it "Strange Unusual": same quality, plus a kill counter.
		if ( pItem->m_nQuality == AE_NORMAL || pItem->m_nQuality == AE_UNIQUE || pItem->m_nQuality == AE_PAINTKITWEAPON )
		{
			pItem->m_nQuality = AE_STRANGE;
		}

		if ( !pItem->HasAttribute( "kill eater" ) && !FindStaticAttrib( pItem->m_pItemDef, "kill eater" ) )
		{
			pItem->SetAttribute( "kill eater", "0" );
		}
	}
}

static COfflineLootItem *RollAdditionalDrop( const char *pszLootListName, CUtlVector< COfflineLootItem * > &vecBonusItems, int nDepth );

// Runs each job once (jobs from templates can show up twice through prefabs),
// rolling its chance first.
static void RunJobs( const CUtlVector< const CLootlistJob * > &vecJobs, COfflineLootItem *pItem, CUtlVector< COfflineLootItem * > &vecBonusItems, int nDepth )
{
	OfflineRollState_t state;
	CUtlVector< const char * > vecTemplatesRun;

	FOR_EACH_VEC( vecJobs, i )
	{
		const CLootlistJob *pJob = vecJobs[i];
		if ( !pJob )
			continue;

		const char *pszTemplateName = pJob->GetTemplateName();
		if ( pszTemplateName )
		{
			if ( vecTemplatesRun.HasElement( pszTemplateName ) )
				continue;
			vecTemplatesRun.AddToTail( pszTemplateName );
		}

		if ( !RollChance( GetJobChance( pJob ) ) )
			continue;

		const CUtlVector< CLootlistJob::RandomAttributeInfo_t > &vecGroups = pJob->GetAttributes();
		FOR_EACH_VEC( vecGroups, j )
		{
			if ( vecGroups[j].m_pRandomAttributes )
			{
				ApplyRandomAttributeGroup( vecGroups[j].m_pRandomAttributes, pItem, state );
			}
		}

		if ( nDepth < kMaxBonusDepth )
		{
			const CUtlVector< loot_list_additional_drop_t > &vecDrops = pJob->GetAdditionalDrops();
			FOR_EACH_VEC( vecDrops, j )
			{
				COfflineLootItem *pBonus = RollAdditionalDrop( vecDrops[j].m_pszLootListDefName, vecBonusItems, nDepth + 1 );
				if ( pBonus )
				{
					vecBonusItems.AddToTail( pBonus );
				}
			}
		}
	}

	FinishItemQuality( pItem, state );
}

// Most of the bonus lists cases use (paint cans, strange parts, ...) only exist
// on Valve's item servers. Rebuild the simple ones from the items' tool types.
struct OfflineBonusListFallback_t
{
	const char *m_pszLootListName;
	const char *m_pszToolTypes;		// space separated
	const char *m_pszItemName;		// or a single item
};

static const OfflineBonusListFallback_t s_BonusListFallbacks[] =
{
	{ "all_paint_cans",			"paint_can",					NULL },
	{ "strange_parts",			"strange_part",					NULL },
	{ "strange_transfer_tool",	"strange_count_transfer",		NULL },
	{ "case_bonus_tools",		"name desc",					NULL },
	{ "festivizer_lootlist",	"paintkit_weapon_festivizer",	NULL },
	{ "mvm_ticket",				NULL,							"Tour of Duty Ticket" },
};

static const CEconItemDefinition *RollBonusListFallback( const char *pszLootListName )
{
	for ( int i = 0; i < ARRAYSIZE( s_BonusListFallbacks ); ++i )
	{
		if ( V_stricmp( s_BonusListFallbacks[i].m_pszLootListName, pszLootListName ) )
			continue;

		if ( s_BonusListFallbacks[i].m_pszItemName )
			return GetItemSchema()->GetItemDefinitionByName( s_BonusListFallbacks[i].m_pszItemName );

		CUtlVector< CUtlString > vecToolTypes;
		V_SplitString( s_BonusListFallbacks[i].m_pszToolTypes, " ", vecToolTypes );

		CUtlVector< const CEconItemDefinition * > vecCandidates;
		const CEconItemSchema::SortedItemDefinitionMap_t &mapItems = GetItemSchema()->GetSortedItemDefinitionMap();
		FOR_EACH_MAP( mapItems, j )
		{
			const CEconItemDefinition *pItemDef = mapItems[j];
			const IEconTool *pTool = pItemDef ? pItemDef->GetEconTool() : NULL;
			if ( !pTool || !pTool->GetTypeName() || pItemDef->IsHidden() )
				continue;

			FOR_EACH_VEC( vecToolTypes, k )
			{
				if ( !V_stricmp( pTool->GetTypeName(), vecToolTypes[k].Get() ) )
				{
					vecCandidates.AddToTail( pItemDef );
					break;
				}
			}
		}

		return vecCandidates.Count() ? vecCandidates[ RandomInt( 0, vecCandidates.Count() - 1 ) ] : NULL;
	}
	return NULL;
}

static COfflineLootItem *RollAdditionalDrop( const char *pszLootListName, CUtlVector< COfflineLootItem * > &vecBonusItems, int nDepth )
{
	CUtlVector< const CLootlistJob * > vecJobs;
	const CEconLootListDefinition *pLootList = GetItemSchema()->GetLootListByName( pszLootListName );
	const CEconItemDefinition *pItemDef = pLootList ? RollLootList( pLootList, vecJobs ) : RollBonusListFallback( pszLootListName );
	if ( !pItemDef )
		return NULL;

	COfflineLootItem *pItem = new COfflineLootItem;
	pItem->m_pItemDef = pItemDef;
	pItem->m_nQuality = GetDefaultQuality( pItemDef );
	RunJobs( vecJobs, pItem, vecBonusItems, nDepth );
	return pItem;
}

//-----------------------------------------------------------------------------
// Old crates' "Exceedingly Rare Special Item"
//-----------------------------------------------------------------------------
static const char *GetClassicUnusualEffectTemplate( const char *pszFooterKey )
{
	if ( !pszFooterKey )
		return NULL;

	if ( !V_stricmp( pszFooterKey, "#Econ_Revolving_Loot_List_Rare_Series_1" ) || !V_stricmp( pszFooterKey, "#Econ_Revolving_Loot_List_Rare_Item" ) )
		return "unusual_hat_effects_series_1";
	if ( !V_stricmp( pszFooterKey, "#Econ_Revolving_Loot_List_Rare_Series_2" ) )
		return "unusual_hat_effects_series_2";
	if ( !V_stricmp( pszFooterKey, "#Econ_Revolving_Loot_List_Rare_Series_3" ) )
		return "unusual_hat_effects_series_3";
	if ( !V_stricmp( pszFooterKey, "#TF_Halloween_Rare_Loot_List_Footer" ) )
		return "unusual_hat_chance_classic_halloween";
	return NULL;
}

// An Unusual hat: any hat the effect template allows, with an effect from it.
static bool RollClassicUnusual( const char *pszEffectTemplate, COfflineLootItem *pItem )
{
	const random_attrib_t *pEffects = GetItemSchema()->GetRandomAttributeTemplateByName( pszEffectTemplate );
	if ( !pEffects )
		return false;

	CUtlVector< const CEconItemDefinition * > vecHats;
	const CEconItemSchema::SortedItemDefinitionMap_t &mapItems = GetItemSchema()->GetSortedItemDefinitionMap();
	FOR_EACH_MAP( mapItems, i )
	{
		const CEconItemDefinition *pItemDef = mapItems[i];
		if ( !pItemDef || pItemDef->IsHidden() || pItemDef->IsBaseItem() )
			continue;

		bool bPasses = pEffects->m_vecCriteria.Count() > 0;
		FOR_EACH_VEC( pEffects->m_vecCriteria, j )
		{
			if ( pEffects->m_vecCriteria[j] && !pEffects->m_vecCriteria[j]->BEvaluate( pItemDef ) )
			{
				bPasses = false;
				break;
			}
		}

		if ( bPasses )
		{
			vecHats.AddToTail( pItemDef );
		}
	}

	if ( vecHats.Count() == 0 )
		return false;

	pItem->m_pItemDef = vecHats[ RandomInt( 0, vecHats.Count() - 1 ) ];
	pItem->m_nQuality = AE_UNUSUAL;

	OfflineRollState_t state;
	ApplyRandomAttributeGroup( pEffects, pItem, state );
	FinishItemQuality( pItem, state );
	return true;
}

//-----------------------------------------------------------------------------
bool OfflineCrates_RollCrate( const IEconItemInterface *pCrate, CUtlVector< COfflineLootItem * > &vecOut )
{
	CCrateLootListWrapper lootListWrapper( pCrate );
	const IEconLootList *pEconLootList = lootListWrapper.GetEconLootList();
	if ( !pEconLootList )
		return false;

	const CEconLootListDefinition *pLootList = dynamic_cast< const CEconLootListDefinition * >( pEconLootList );
	const CEconItemDefinition *pCrateDef = pCrate->GetItemDefinition();

	COfflineLootItem *pMainItem = new COfflineLootItem;
	CUtlVector< COfflineLootItem * > vecBonusItems;

	const char *pszClassicTemplate = pLootList ? GetClassicUnusualEffectTemplate( pLootList->GetLootListFooterLocalizationKey() ) : NULL;
	const bool bClassicUnusual = pszClassicTemplate
		&& RollChance( inv_crate_unusual_chance.GetFloat() )
		&& RollClassicUnusual( pszClassicTemplate, pMainItem );

	if ( !bClassicUnusual )
	{
		CUtlVector< const CLootlistJob * > vecJobs;
		pMainItem->m_pItemDef = pLootList ? RollLootList( pLootList, vecJobs ) : RollFromPublicContents( pEconLootList );
		if ( !pMainItem->m_pItemDef )
		{
			delete pMainItem;
			return false;
		}
		pMainItem->m_nQuality = GetDefaultQuality( pMainItem->m_pItemDef );

		// The crate's own jobs (Strange / Unusual / bonus item chances on cases).
		if ( pCrateDef )
		{
			const CUtlVector< CLootlistJob * > &vecCrateJobs = pCrateDef->GetLootlistJobs();
			FOR_EACH_VEC( vecCrateJobs, i )
			{
				vecJobs.AddToTail( vecCrateJobs[i] );
			}
		}

		RunJobs( vecJobs, pMainItem, vecBonusItems, 0 );
	}

	vecOut.AddToTail( pMainItem );
	vecOut.AddVectorToTail( vecBonusItems );
	return true;
}

//-----------------------------------------------------------------------------
// Crates and keys
//-----------------------------------------------------------------------------
bool OfflineCrates_IsCrate( const CEconItemDefinition *pItemDef )
{
	return pItemDef && pItemDef->GetItemClass() && !V_stricmp( pItemDef->GetItemClass(), "supply_crate" );
}

bool OfflineCrates_NeedsKey( const CEconItemDefinition *pCrateDef )
{
	const IEconTool *pTool = pCrateDef ? pCrateDef->GetEconTool() : NULL;
	return !( pTool && pTool->GetTypeName() && !V_stricmp( pTool->GetTypeName(), "keyless_case" ) );
}

const CEconItemDefinition *OfflineCrates_GetKeyForCrate( const CEconItemDefinition *pCrateDef )
{
	// Special crates name their key; everything else takes a Mann Co. Supply Crate Key.
	const static_attrib_t *pDecodedBy = FindStaticAttrib( pCrateDef, "decoded by itemdefindex" );
	if ( pDecodedBy )
	{
		const CEconItemAttributeDefinition *pAttrDef = GetItemSchema()->GetAttributeDefinition( pDecodedBy->iDefIndex );
		const CEconItemDefinition *pKeyDef = GetItemSchema()->GetItemDefinition( GetStaticAttribValueInt( pAttrDef, pDecodedBy->m_value ) );
		if ( pKeyDef )
			return pKeyDef;
	}

	return GetItemSchema()->GetItemDefinitionByName( "Decoder Ring" );
}

// Crate series number fixed in a crate's definition, or 0.
static int GetCrateDefSeries( const CEconItemDefinition *pCrateDef )
{
	const static_attrib_t *pSeries = FindStaticAttrib( pCrateDef, "set supply crate series" );
	if ( !pSeries )
		return 0;

	const CEconItemAttributeDefinition *pAttrDef = GetItemSchema()->GetAttributeDefinition( pSeries->iDefIndex );
	return GetStaticAttribValueInt( pAttrDef, pSeries->m_value );
}

// Some old crate series list contents that aren't in the game's copy of the
// schema (they only ever existed on Valve's item servers). Those can't be opened.
static bool SeriesHasKnownContents( int iSeries )
{
	if ( iSeries <= 0 )
		return false;

	const CEconItemSchema::RevolvingLootListDefinitionMap_t &mapSeries = GetItemSchema()->GetRevolvingLootLists();
	const int iIndex = mapSeries.Find( iSeries );
	return mapSeries.IsValidIndex( iIndex ) && GetItemSchema()->GetLootListByName( mapSeries[iIndex].Get() ) != NULL;
}

static const CEconItemDefinition *FindCrateDefForSeries( int iSeries )
{
	const CEconItemSchema::SortedItemDefinitionMap_t &mapItems = GetItemSchema()->GetSortedItemDefinitionMap();
	FOR_EACH_MAP( mapItems, i )
	{
		const CEconItemDefinition *pItemDef = mapItems[i];
		if ( OfflineCrates_IsCrate( pItemDef ) && GetCrateDefSeries( pItemDef ) == iSeries )
			return pItemDef;
	}
	return NULL;
}

//=============================================================================
// Console commands
//=============================================================================
CON_COMMAND( inv_crates, "List crate series and the crate each one comes in.\n"
	"  Usage: inv_crates [text to filter by]" )
{
	const char *pszFilter = args.ArgC() >= 2 ? args.ArgS() : NULL;
	char szName[256];

	const CEconItemSchema::RevolvingLootListDefinitionMap_t &mapSeries = GetItemSchema()->GetRevolvingLootLists();
	FOR_EACH_MAP( mapSeries, i )
	{
		const int iSeries = mapSeries.Key( i );
		if ( !SeriesHasKnownContents( iSeries ) )
			continue;

		const CEconItemDefinition *pCrateDef = FindCrateDefForSeries( iSeries );
		if ( pCrateDef )
		{
			OfflineInventory_GetItemDefDisplayName( pCrateDef, szName, sizeof( szName ) );
		}
		else
		{
			V_snprintf( szName, sizeof( szName ), "Mann Co. Supply Crate Series #%d", iSeries );
		}

		if ( pszFilter && !V_stristr( szName, pszFilter ) && !V_stristr( mapSeries[i].Get(), pszFilter ) )
			continue;

		Msg( "  %4d  %s\n", iSeries, szName );
	}
	Msg( "Get one with: inv_givecrate <number> [how many]\n" );
}

//-----------------------------------------------------------------------------
CON_COMMAND( inv_givecrate, "Add crates (and keys to open them) to your offline backpack.\n"
	"  Usage: inv_givecrate <series number or crate name> [how many]\n"
	"  See inv_crates for series numbers." )
{
	if ( !TFOfflineInventory().IsLoaded() )
	{
		Msg( "The offline inventory hasn't loaded yet. Make sure Steam is running, then try again in a moment.\n" );
		return;
	}

	if ( args.ArgC() < 2 )
	{
		Msg( "Usage: inv_givecrate <series number or crate name> [how many]\n" );
		return;
	}

	const int nCount = clamp( args.ArgC() >= 3 ? V_atoi( args[2] ) : 1, 1, 100 );

	const CEconItemDefinition *pCrateDef = NULL;
	int iDynamicSeries = 0;	// for crates whose series is set per crate rather than in the definition

	const char *pszArg = args[1];
	bool bIsNumber = pszArg[0] != '\0';
	for ( const char *p = pszArg; *p; ++p )
	{
		bIsNumber &= ( V_isdigit( *p ) != 0 );
	}

	if ( bIsNumber )
	{
		const int iSeries = V_atoi( pszArg );
		if ( !SeriesHasKnownContents( iSeries ) )
		{
			Msg( "There's no crate series %d with contents this game knows about. See inv_crates.\n", iSeries );
			return;
		}

		pCrateDef = FindCrateDefForSeries( iSeries );
		if ( !pCrateDef )
		{
			// Classic Mann Co. Supply Crates share one definition and get their series when they drop.
			pCrateDef = GetItemSchema()->GetItemDefinitionByName( "Supply Crate" );
			iDynamicSeries = iSeries;
		}
	}
	else
	{
		CUtlVector< const CEconItemDefinition * > vecMatches, vecCrates;
		OfflineInventory_FindItemDefs( args.ArgS(), vecMatches );
		FOR_EACH_VEC( vecMatches, i )
		{
			if ( OfflineCrates_IsCrate( vecMatches[i] ) )
			{
				vecCrates.AddToTail( vecMatches[i] );
			}
		}

		// The count can't be used together with a multi-word name typed without quotes.
		if ( vecCrates.Count() == 0 )
		{
			OfflineInventory_FindItemDefs( pszArg, vecMatches );
			FOR_EACH_VEC( vecMatches, i )
			{
				if ( OfflineCrates_IsCrate( vecMatches[i] ) )
				{
					vecCrates.AddToTail( vecMatches[i] );
				}
			}
		}

		if ( vecCrates.Count() == 0 )
		{
			Msg( "No crate matches \"%s\". See inv_crates.\n", pszArg );
			return;
		}
		if ( vecCrates.Count() > 1 )
		{
			Msg( "\"%s\" matches %d crates. Be more specific, or use a series number from inv_crates.\n", pszArg, vecCrates.Count() );
			return;
		}

		pCrateDef = vecCrates[0];
		const int iSeries = GetCrateDefSeries( pCrateDef );
		if ( !iSeries && !FindStaticAttrib( pCrateDef, "loot list name" ) )
		{
			Msg( "That crate gets its contents from a series number. Use inv_givecrate <series number> (see inv_crates).\n" );
			return;
		}
		if ( iSeries && !SeriesHasKnownContents( iSeries ) )
		{
			Msg( "This game doesn't know what that crate contains (only Valve's item servers did), so it can't be opened here.\n" );
			return;
		}
	}

	if ( !pCrateDef )
	{
		Msg( "This version of the game has no crate definition to use.\n" );
		return;
	}

	const CEconItemAttributeDefinition *pSeriesAttr = GetItemSchema()->GetAttributeDefinitionByName( "set supply crate series" );
	const CEconItemDefinition *pKeyDef = OfflineCrates_NeedsKey( pCrateDef ) ? OfflineCrates_GetKeyForCrate( pCrateDef ) : NULL;

	int nCrates = 0, nKeys = 0;
	for ( int i = 0; i < nCount; ++i )
	{
		// Key first, so "last" in other commands means the crate.
		if ( pKeyDef && TFOfflineInventory().GiveItem( pKeyDef, -1 ) != INVALID_ITEM_ID )
		{
			nKeys++;
		}

		const itemid_t unCrateID = TFOfflineInventory().GiveItem( pCrateDef, -1 );
		if ( unCrateID == INVALID_ITEM_ID )
			break;
		nCrates++;

		if ( iDynamicSeries && pSeriesAttr )
		{
			TFOfflineInventory().SetItemAttribute( unCrateID, pSeriesAttr, CFmtStr( "%d", iDynamicSeries ) );
		}
	}

	char szCrateName[256], szKeyName[256];
	OfflineInventory_GetItemDefDisplayName( pCrateDef, szCrateName, sizeof( szCrateName ) );
	if ( iDynamicSeries )
	{
		V_snprintf( szCrateName, sizeof( szCrateName ), "Mann Co. Supply Crate Series #%d", iDynamicSeries );
	}

	if ( nKeys > 0 )
	{
		OfflineInventory_GetItemDefDisplayName( pKeyDef, szKeyName, sizeof( szKeyName ) );
		Msg( "Gave %d x %s and %d x %s. Open them from your backpack.\n", nCrates, szCrateName, nKeys, szKeyName );
	}
	else
	{
		Msg( "Gave %d x %s. Open them from your backpack.\n", nCrates, szCrateName );
	}
}

//-----------------------------------------------------------------------------
CON_COMMAND( inv_opencrate, "Open a crate from the console, using a key from your backpack if it needs one.\n"
	"  Usage: inv_opencrate <item id | last>" )
{
	if ( !TFOfflineInventory().IsLoaded() )
	{
		Msg( "The offline inventory hasn't loaded yet. Make sure Steam is running, then try again in a moment.\n" );
		return;
	}

	if ( args.ArgC() < 2 )
	{
		Msg( "Usage: inv_opencrate <item id | last>\n" );
		return;
	}

	const itemid_t unCrateID = OfflineInventory_ParseExistingItemID( args[1] );
	if ( unCrateID == INVALID_ITEM_ID )
		return;

	TFOfflineInventory().OpenCrateWithAnyKey( unCrateID );
}
