//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Offline tools. See tf_offline_tools.h.
//
//=============================================================================

#include "cbase.h"
#include "tf_offline_tools.h"
#include "tf_offline_inventory.h"
#include "tf_offline_crates.h"
#include "tf_offline_crafting.h"
#include "econ_gcmessages.h"
#include "base_gcmessages.pb.h"
#include "tf_gcmessages.h"
#include "econ_item_inventory.h"
#include "econ_controls.h"
#include "game_item_schema.h"
#include "tf_shareddefs.h"
#include "econ_item.h"
#include "econ_item_schema.h"
#include "econ_item_tools.h"
#include "KeyValues.h"
#include <string>
#include <math.h>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//-----------------------------------------------------------------------------
// Request queue
//-----------------------------------------------------------------------------
struct OfflineToolRequest_t
{
	uint32		m_unMsgType;
	std::string	m_strBody;		// struct messages: the struct then its strings; protobuf: the serialized body
};

static CUtlVector< OfflineToolRequest_t > s_vecPendingRequests;

static bool IsHandledStructMessage( uint32 unMsgType )
{
	switch ( unMsgType )
	{
	case k_EMsgGCPaintItem:
	case k_EMsgGCNameItem:
	case k_EMsgGCNameBaseItem:
	case k_EMsgGCRemoveItemName:
	case k_EMsgGCSetItemStyle:
	case k_EMsgGCCraft:
	case k_EMsgGCDelete:
		return true;
	}
	return false;
}

static bool IsHandledProtoMessage( uint32 unMsgType )
{
	switch ( unMsgType )
	{
	case k_EMsgGCApplyStrangePart:
	case k_EMsgGCApplyStrangeRestriction:
	case k_EMsgGCApplyStrangeCountTransfer:
	case k_EMsgGCApplyXifier:
	case k_EMsgGCApplyBaseItemXifier:
	case k_EMsgGCRemoveItemPaint:
	case k_EMsgGCRemoveMakersMark:
	case k_EMsgGCRemoveKillStreak:
	case k_EMsgGCRemoveGiftedBy:
	case k_EMsgGCRemoveFestivizer:
	case k_EMsgGCRemoveCustomTexture:
	case k_EMsgGCRemoveStrangePart:
	case k_EMsgGCResetStrangeScores:
	case k_EMsgGCRemoveUpgradeCard:
	case k_EMsgGCConsumePaintKit:
	case k_EMsgGCSortItems:
	case k_EMsgGCSetItemEffectVerticalOffset:
	case k_EMsgGCSetHatEffectUseHeadOrigin:
	case k_EMsgGCCraftCollectionUpgrade:
	case k_EMsgGCCraftCommonStatClock:
		return true;
	}
	return false;
}

bool OfflineTools_InterceptStructMessage( uint32 unMsgType, const void *pBody, uint32 cubBody )
{
	if ( !IsHandledStructMessage( unMsgType ) )
		return false;

	OfflineToolRequest_t &request = s_vecPendingRequests[ s_vecPendingRequests.AddToTail() ];
	request.m_unMsgType = unMsgType;
	request.m_strBody.assign( (const char *)pBody, cubBody );
	return true;
}

bool OfflineTools_InterceptProtoMessage( uint32 unMsgType, const ::google::protobuf::Message &body )
{
	if ( !IsHandledProtoMessage( unMsgType ) )
		return false;

	OfflineToolRequest_t &request = s_vecPendingRequests[ s_vecPendingRequests.AddToTail() ];
	request.m_unMsgType = unMsgType;
	body.SerializeToString( &request.m_strBody );
	return true;
}

// Copies the fixed-size part of a struct message (copied, since the bytes may not be aligned).
template < typename T >
static bool ReadStructBody( const OfflineToolRequest_t &request, T *pOut )
{
	if ( request.m_strBody.size() < sizeof( T ) )
		return false;

	V_memcpy( pOut, request.m_strBody.data(), sizeof( T ) );
	return true;
}

// The text that follows the struct in a message (e.g. the name from a name tag).
template < typename T >
static std::string ReadStructString( const OfflineToolRequest_t &request )
{
	if ( request.m_strBody.size() <= sizeof( T ) )
		return std::string();

	const char *pszStart = request.m_strBody.data() + sizeof( T );
	const size_t cubMax = request.m_strBody.size() - sizeof( T );
	return std::string( pszStart, strnlen( pszStart, cubMax ) );
}

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------
static const char *GetToolType( itemid_t unToolID )
{
	const CEconItemDefinition *pToolDef = TFOfflineInventory().GetItemDef( unToolID );
	const IEconTool *pTool = pToolDef ? pToolDef->GetEconTool() : NULL;
	return ( pTool && pTool->GetTypeName() ) ? pTool->GetTypeName() : "";
}

static void GetItemName( itemid_t unItemID, char *pszOut, int nOutSize )
{
	const CEconItemDefinition *pItemDef = TFOfflineInventory().GetItemDef( unItemID );
	if ( pItemDef )
	{
		OfflineInventory_GetItemDefDisplayName( pItemDef, pszOut, nOutSize );
	}
	else
	{
		V_snprintf( pszOut, nOutSize, "item %llu", (unsigned long long)unItemID );
	}
}

static bool CheckItemsExist( itemid_t unToolID, itemid_t unSubjectID )
{
	if ( !TFOfflineInventory().FindItemKV( unToolID ) || !TFOfflineInventory().FindItemKV( unSubjectID ) )
	{
		Warning( "Offline inventory: the tool or the item it was used on isn't in your backpack any more.\n" );
		return false;
	}
	return true;
}

// The tool is used up once it has been applied.
static void UseUpTool( itemid_t unToolID, itemid_t unSubjectID, const char *pszWhat )
{
	char szSubject[256];
	GetItemName( unSubjectID, szSubject, sizeof( szSubject ) );
	TFOfflineInventory().DeleteItem( unToolID );
	Msg( "%s %s.\n", pszWhat, szSubject );
}

static const CEconItemAttributeDefinition *Attr( const char *pszName )
{
	return GetItemSchema()->GetAttributeDefinitionByName( pszName );
}

static bool SetAttr( itemid_t unItemID, const char *pszAttrName, const char *pszValue )
{
	const CEconItemAttributeDefinition *pAttrDef = Attr( pszAttrName );
	return pAttrDef && TFOfflineInventory().SetItemAttribute( unItemID, pAttrDef, pszValue );
}

static void RemoveAttrs( itemid_t unItemID, const char **ppszAttrNames, int nCount )
{
	for ( int i = 0; i < nCount; ++i )
	{
		const CEconItemAttributeDefinition *pAttrDef = Attr( ppszAttrNames[i] );
		if ( pAttrDef )
		{
			TFOfflineInventory().RemoveItemAttribute( unItemID, pAttrDef );
		}
	}
}

// A number attribute's value on an item (from its definition or added later).
// Integer attributes can hold either a plain integer (values we set) or the bits
// of a float (values from the schema, which parses them as floats), so tell
// those apart the same way the crate code does.
static bool GetItemAttrNumber( itemid_t unItemID, const CEconItemAttributeDefinition *pAttrDef, float *pflValue )
{
	const CEconItem *pItem = TFOfflineInventory().GetLiveItem( unItemID );
	attrib_value_t unBits = 0;
	if ( !pItem || !pAttrDef || !::FindAttribute( pItem, pAttrDef, &unBits ) )
		return false;

	float flAsFloat;
	V_memcpy( &flAsFloat, &unBits, sizeof( flAsFloat ) );

	const bool bLooksLikeFloatBits = unBits >= 0x01000000 && flAsFloat >= 0.f && flAsFloat < 16777216.f && flAsFloat == floorf( flAsFloat );
	*pflValue = ( pAttrDef->IsStoredAsInteger() && !bLooksLikeFloatBits ) ? (float)unBits : flAsFloat;
	return true;
}

static bool ItemHasAttr( itemid_t unItemID, const CEconItemAttributeDefinition *pAttrDef )
{
	const CEconItem *pItem = TFOfflineInventory().GetLiveItem( unItemID );
	return pItem && pAttrDef && ::FindAttribute( pItem, pAttrDef );
}

// The strange counter type in a counter slot. Slot 0 with no type set counts kills.
static bool GetKillEaterSlotType( itemid_t unItemID, int iSlot, int *pnType )
{
	if ( !ItemHasAttr( unItemID, GetKillEaterAttr_Score( iSlot ) ) )
		return false;

	float flType;
	*pnType = GetItemAttrNumber( unItemID, GetKillEaterAttr_Type( iSlot ), &flType ) ? (int)flType : kKillEaterEvent_PlayerKill;
	return true;
}

static void EnsureKillCounter( itemid_t unItemID )
{
	const CEconItemAttributeDefinition *pKillEater = Attr( "kill eater" );
	if ( pKillEater && !ItemHasAttr( unItemID, pKillEater ) )
	{
		TFOfflineInventory().SetItemAttribute( unItemID, pKillEater, "0" );
	}
}

// Stock items (the ones every player has) have no item ID, so tools used on
// them first make a real copy of the item in your backpack.
static itemid_t CreateItemFromBaseItem( uint32 unBaseDefIndex )
{
	const CEconItemDefinition *pBaseDef = GetItemSchema()->GetItemDefinition( unBaseDefIndex );
	if ( !pBaseDef )
		return INVALID_ITEM_ID;

	return TFOfflineInventory().GiveItem( pBaseDef, AE_UNIQUE );
}

//-----------------------------------------------------------------------------
// Paint
//-----------------------------------------------------------------------------
bool OfflineTools_PaintItem( itemid_t unItemID, const CEconItemDefinition *pPaintCanDef )
{
	CUtlString strColor, strTeamColor;
	if ( !OfflineCrates_GetStaticAttribValue( pPaintCanDef, "set item tint RGB", strColor ) )
		return false;

	if ( !SetAttr( unItemID, "set item tint RGB", strColor.Get() ) )
		return false;

	// Team paints have a second color for BLU; single-color paints mustn't
	// keep one from an earlier paint job.
	if ( OfflineCrates_GetStaticAttribValue( pPaintCanDef, "set item tint RGB 2", strTeamColor ) )
	{
		SetAttr( unItemID, "set item tint RGB 2", strTeamColor.Get() );
	}
	else
	{
		const char *pszTeamColor[] = { "set item tint RGB 2" };
		RemoveAttrs( unItemID, pszTeamColor, ARRAYSIZE( pszTeamColor ) );
	}
	return true;
}

static void ApplyPaintCan( itemid_t unToolID, itemid_t unSubjectID )
{
	if ( !CheckItemsExist( unToolID, unSubjectID ) )
		return;

	if ( !OfflineTools_PaintItem( unSubjectID, TFOfflineInventory().GetItemDef( unToolID ) ) )
	{
		Warning( "Offline inventory: that paint can has no color.\n" );
		return;
	}
	UseUpTool( unToolID, unSubjectID, "Painted" );
}

//-----------------------------------------------------------------------------
// Name and description tags
//-----------------------------------------------------------------------------
static void ApplyNameTag( itemid_t unToolID, itemid_t unSubjectID, const std::string &strText, bool bDescription )
{
	if ( !CheckItemsExist( unToolID, unSubjectID ) )
		return;

	bDescription |= !V_stricmp( GetToolType( unToolID ), "desc" );
	if ( strText.empty() || !TFOfflineInventory().SetItemCustomText( unSubjectID, bDescription, strText.c_str() ) )
		return;

	UseUpTool( unToolID, unSubjectID, bDescription ? "Wrote a description on" : "Renamed" );
}

//-----------------------------------------------------------------------------
// Strange parts and filters
//-----------------------------------------------------------------------------
static void ApplyStrangePart( itemid_t unPartID, itemid_t unSubjectID )
{
	if ( !CheckItemsExist( unPartID, unSubjectID ) )
		return;

	CUtlString strCounterType;
	if ( !OfflineCrates_GetStaticAttribValue( TFOfflineInventory().GetItemDef( unPartID ), "strange part new counter ID", strCounterType ) )
	{
		Warning( "Offline inventory: that strange part doesn't say what it counts.\n" );
		return;
	}
	const int nCounterType = (int)V_atof( strCounterType.Get() );

	// An item can only count each thing once.
	for ( int i = 0; i < GetKillEaterAttrCount(); ++i )
	{
		int nType;
		if ( GetKillEaterSlotType( unSubjectID, i, &nType ) && nType == nCounterType )
		{
			Msg( "That item already counts this.\n" );
			return;
		}
	}

	for ( int i = 0; i < GetKillEaterAttrCount(); ++i )
	{
		if ( !GetKillEaterAttr_IsUserCustomizable( i ) || ItemHasAttr( unSubjectID, GetKillEaterAttr_Score( i ) ) )
			continue;

		TFOfflineInventory().SetItemAttribute( unSubjectID, GetKillEaterAttr_Type( i ), CFmtStr( "%d", nCounterType ) );
		TFOfflineInventory().SetItemAttribute( unSubjectID, GetKillEaterAttr_Score( i ), "0" );
		UseUpTool( unPartID, unSubjectID, "Added a strange part to" );
		return;
	}

	Msg( "That item has no free strange part slots.\n" );
}

static void ApplyStrangeFilter( itemid_t unFilterID, itemid_t unSubjectID, uint32 unSlot )
{
	if ( !CheckItemsExist( unFilterID, unSubjectID ) )
		return;

	const CEconItemDefinition *pFilterDef = TFOfflineInventory().GetItemDef( unFilterID );
	const CEconTool_StrangePartRestriction *pFilter = pFilterDef ? pFilterDef->GetTypedEconTool< CEconTool_StrangePartRestriction >() : NULL;
	if ( !pFilter || unSlot >= (uint32)GetKillEaterAttrCount() || !ItemHasAttr( unSubjectID, GetKillEaterAttr_Score( unSlot ) ) )
	{
		Warning( "Offline inventory: can't apply that strange filter there.\n" );
		return;
	}

	TFOfflineInventory().SetItemAttribute( unSubjectID, GetKillEaterAttr_Restriction( unSlot ), CFmtStr( "%u", pFilter->GetRestrictionType() ) );
	TFOfflineInventory().SetItemAttribute( unSubjectID, GetKillEaterAttr_RestrictionValue( unSlot ), CFmtStr( "%u", pFilter->GetRestrictionValue() ) );
	UseUpTool( unFilterID, unSubjectID, "Added a strange filter to" );
}

// Moves the counts from one item to another of the same kind; the first starts over.
static void ApplyStrangeCountTransfer( itemid_t unToolID, itemid_t unSourceID, itemid_t unDestID )
{
	if ( !CheckItemsExist( unToolID, unSourceID ) || !CheckItemsExist( unToolID, unDestID ) )
		return;

	bool bMovedAny = false;
	for ( int i = 0; i < GetKillEaterAttrCount(); ++i )
	{
		const CEconItemAttributeDefinition *pScoreAttr = GetKillEaterAttr_Score( i );
		const CEconItem *pSource = TFOfflineInventory().GetLiveItem( unSourceID );
		uint32 unScore = 0;
		if ( !pSource || !pScoreAttr || !::FindAttribute( pSource, pScoreAttr, &unScore ) || !ItemHasAttr( unDestID, pScoreAttr ) )
			continue;

		TFOfflineInventory().SetItemAttribute( unDestID, pScoreAttr, CFmtStr( "%u", unScore ) );
		TFOfflineInventory().SetItemAttribute( unSourceID, pScoreAttr, "0" );
		bMovedAny = true;
	}

	if ( !bMovedAny )
	{
		Msg( "Neither item has counts the other can take.\n" );
		return;
	}
	UseUpTool( unToolID, unDestID, "Moved strange counts to" );
}

//-----------------------------------------------------------------------------
// Strangifiers, Killstreak Kits, the Festivizer and the Taunt Unusualifier
//-----------------------------------------------------------------------------
static void CopyToolAttr( itemid_t unToolID, itemid_t unSubjectID, const char *pszAttrName, const char *pszRollTemplate )
{
	const CEconItemAttributeDefinition *pAttrDef = Attr( pszAttrName );
	float flValue;
	if ( GetItemAttrNumber( unToolID, pAttrDef, &flValue ) )
	{
		// Killstreak attributes are integers stored the float way, like most of the schema's.
		TFOfflineInventory().SetItemAttribute( unSubjectID, pAttrDef, CFmtStr( "%d", (int)flValue ) );
		return;
	}

	// Specialized and Professional kits got their sheen and effect when they
	// dropped. Kits from inv_give don't have one yet, so roll it now.
	CUtlString strAttrName, strValue;
	if ( pszRollTemplate && OfflineCrates_RollAttributeTemplate( pszRollTemplate, TFOfflineInventory().GetItemDef( unSubjectID ), strAttrName, strValue ) )
	{
		SetAttr( unSubjectID, strAttrName.Get(), strValue.Get() );
	}
}

static void ApplyXifier( itemid_t unToolID, itemid_t unSubjectID )
{
	if ( !CheckItemsExist( unToolID, unSubjectID ) )
		return;

	const char *pszToolType = GetToolType( unToolID );
	const CEconItem *pSubject = TFOfflineInventory().GetLiveItem( unSubjectID );
	if ( !pSubject )
		return;

	if ( !V_stricmp( pszToolType, "strangifier" ) )
	{
		const int nQuality = pSubject->GetQuality();
		if ( nQuality == AE_NORMAL || nQuality == AE_UNIQUE )
		{
			TFOfflineInventory().SetItemQuality( unSubjectID, AE_STRANGE );
		}
		EnsureKillCounter( unSubjectID );
		UseUpTool( unToolID, unSubjectID, "Strangified" );
	}
	else if ( !V_stricmp( pszToolType, "killstreakifier" ) )
	{
		float flTier = 1.f;
		GetItemAttrNumber( unToolID, Attr( "killstreak tier" ), &flTier );
		const int nTier = MAX( (int)flTier, 1 );

		SetAttr( unSubjectID, "killstreak tier", CFmtStr( "%d", nTier ) );
		if ( nTier >= 2 )
		{
			CopyToolAttr( unToolID, unSubjectID, "killstreak idleeffect", "killstreak_idleeffects" );
		}
		if ( nTier >= 3 )
		{
			CopyToolAttr( unToolID, unSubjectID, "killstreak effect", "killstreak_effects" );
		}
		UseUpTool( unToolID, unSubjectID, "Applied a killstreak kit to" );
	}
	else if ( V_stristr( pszToolType, "festivizer" ) )
	{
		SetAttr( unSubjectID, "is_festivized", "1" );
		UseUpTool( unToolID, unSubjectID, "Festivized" );
	}
	else if ( !V_stricmp( pszToolType, "unusualifier" ) )
	{
		CUtlString strTemplate, strAttrName, strEffect;
		if ( !OfflineCrates_GetStaticAttribValue( TFOfflineInventory().GetItemDef( unToolID ), "unusualifier_attribute_template_name", strTemplate )
			|| !OfflineCrates_RollAttributeTemplate( strTemplate.Get(), TFOfflineInventory().GetItemDef( unSubjectID ), strAttrName, strEffect ) )
		{
			Warning( "Offline inventory: that Unusualifier has no effects for this item.\n" );
			return;
		}

		SetAttr( unSubjectID, "on taunt attach particle index", strEffect.Get() );
		TFOfflineInventory().SetItemQuality( unSubjectID, AE_UNUSUAL );
		UseUpTool( unToolID, unSubjectID, "Unusualified" );
	}
	else
	{
		Warning( "Offline inventory: using \"%s\" tools isn't supported yet.\n", pszToolType );
	}
}

//-----------------------------------------------------------------------------
// The backpack's "Restore" options
//-----------------------------------------------------------------------------
static void RemoveStrangePart( itemid_t unItemID, int nCounterType )
{
	for ( int i = 0; i < GetKillEaterAttrCount(); ++i )
	{
		int nType;
		if ( !GetKillEaterAttr_IsUserCustomizable( i ) || !GetKillEaterSlotType( unItemID, i, &nType ) || nType != nCounterType )
			continue;

		const char *pszAttrs[] =
		{
			GetKillEaterAttr_Score( i ) ? GetKillEaterAttr_Score( i )->GetDefinitionName() : "",
			GetKillEaterAttr_Type( i ) ? GetKillEaterAttr_Type( i )->GetDefinitionName() : "",
			GetKillEaterAttr_Restriction( i ) ? GetKillEaterAttr_Restriction( i )->GetDefinitionName() : "",
			GetKillEaterAttr_RestrictionValue( i ) ? GetKillEaterAttr_RestrictionValue( i )->GetDefinitionName() : "",
		};
		RemoveAttrs( unItemID, pszAttrs, ARRAYSIZE( pszAttrs ) );
		return;
	}
}

static void ResetStrangeScores( itemid_t unItemID )
{
	for ( int i = 0; i < GetKillEaterAttrCount(); ++i )
	{
		if ( ItemHasAttr( unItemID, GetKillEaterAttr_Score( i ) ) )
		{
			TFOfflineInventory().SetItemAttribute( unItemID, GetKillEaterAttr_Score( i ), "0" );
		}
	}
}

static void ApplyRestore( uint32 unMsgType, itemid_t unItemID )
{
	static const char *s_pszPaint[] = { "set item tint RGB", "set item tint RGB 2" };
	static const char *s_pszMakersMark[] = { "makers mark id" };
	static const char *s_pszKillStreak[] = { "killstreak tier", "killstreak effect", "killstreak idleeffect" };
	static const char *s_pszGiftedBy[] = { "gifter account id", "event date" };
	static const char *s_pszFestivizer[] = { "is_festivized" };
	static const char *s_pszCustomTexture[] = { "custom texture lo", "custom texture hi" };

	switch ( unMsgType )
	{
	case k_EMsgGCRemoveItemPaint:		RemoveAttrs( unItemID, s_pszPaint, ARRAYSIZE( s_pszPaint ) ); break;
	case k_EMsgGCRemoveMakersMark:		RemoveAttrs( unItemID, s_pszMakersMark, ARRAYSIZE( s_pszMakersMark ) ); break;
	case k_EMsgGCRemoveKillStreak:		RemoveAttrs( unItemID, s_pszKillStreak, ARRAYSIZE( s_pszKillStreak ) ); break;
	case k_EMsgGCRemoveGiftedBy:		RemoveAttrs( unItemID, s_pszGiftedBy, ARRAYSIZE( s_pszGiftedBy ) ); break;
	case k_EMsgGCRemoveFestivizer:		RemoveAttrs( unItemID, s_pszFestivizer, ARRAYSIZE( s_pszFestivizer ) ); break;
	case k_EMsgGCRemoveCustomTexture:	RemoveAttrs( unItemID, s_pszCustomTexture, ARRAYSIZE( s_pszCustomTexture ) ); break;
	}
}

//-----------------------------------------------------------------------------
// War Paints
//-----------------------------------------------------------------------------
static const struct OfflineWear_t
{
	const char	*m_pszName;
	const char	*m_pszShort;
	float		m_flWear;
} s_OfflineWears[] =
{
	{ "Factory New",	"fn",	0.2f },
	{ "Minimal Wear",	"mw",	0.4f },
	{ "Field-Tested",	"ft",	0.6f },
	{ "Well-Worn",		"ww",	0.8f },
	{ "Battle Scarred",	"bs",	1.0f },
};

bool OfflineTools_IsWarPaint( const CEconItemDefinition *pItemDef )
{
	const IEconTool *pTool = pItemDef ? pItemDef->GetEconTool() : NULL;
	if ( !pTool || !pTool->GetTypeName() || V_stricmp( pTool->GetTypeName(), "paintkit" ) )
		return false;

	// The generic "Paintkit" definition has no paint of its own.
	CUtlString strPaintKit;
	return OfflineCrates_GetStaticAttribValue( pItemDef, "paintkit_proto_def_index", strPaintKit );
}

float OfflineTools_RandomWear()
{
	return s_OfflineWears[ RandomInt( 0, ARRAYSIZE( s_OfflineWears ) - 1 ) ].m_flWear;
}

bool OfflineTools_ParseWear( const char *pszText, float *pflWear )
{
	if ( !pszText || !pszText[0] )
		return false;

	if ( !V_stricmp( pszText, "random" ) )
	{
		*pflWear = OfflineTools_RandomWear();
		return true;
	}

	for ( int i = 0; i < ARRAYSIZE( s_OfflineWears ); ++i )
	{
		// "Field-Tested", "field tested", "field_tested" and "ft" all work.
		char szLoose[64];
		V_strncpy( szLoose, pszText, sizeof( szLoose ) );
		for ( char *p = szLoose; *p; ++p )
		{
			if ( *p == '_' || *p == '-' )
				*p = ' ';
		}
		char szName[64];
		V_strncpy( szName, s_OfflineWears[i].m_pszName, sizeof( szName ) );
		for ( char *p = szName; *p; ++p )
		{
			if ( *p == '-' )
				*p = ' ';
		}

		if ( !V_stricmp( szLoose, szName ) || !V_stricmp( pszText, s_OfflineWears[i].m_pszShort ) )
		{
			*pflWear = s_OfflineWears[i].m_flWear;
			return true;
		}
	}

	const float flValue = V_atof( pszText );
	if ( flValue > 0.f && flValue <= 1.f )
	{
		*pflWear = flValue;
		return true;
	}
	return false;
}

const char *OfflineTools_GetWearName( float flWear )
{
	// Same buckets the game uses (EconWear_ToIntCategory).
	for ( int i = 0; i < ARRAYSIZE( s_OfflineWears ); ++i )
	{
		if ( flWear <= s_OfflineWears[i].m_flWear + 0.0001f )
			return s_OfflineWears[i].m_pszName;
	}
	return s_OfflineWears[ ARRAYSIZE( s_OfflineWears ) - 1 ].m_pszName;
}

// The pattern seed: painted items lay their paint out from it. Items without one
// use their original item ID, so that's the default.
static uint64 GetPaintSeed( itemid_t unItemID )
{
	const CEconItem *pItem = TFOfflineInventory().GetLiveItem( unItemID );
	if ( !pItem )
		return 0;

	uint64 ulSeed = pItem->GetOriginalID();
	attrib_value_t unBits = 0;
	if ( ::FindAttribute( pItem, Attr( "custom_paintkit_seed_lo" ), &unBits ) )
	{
		ulSeed = unBits;
	}
	if ( ::FindAttribute( pItem, Attr( "custom_paintkit_seed_hi" ), &unBits ) )
	{
		ulSeed |= (uint64)unBits << 32;
	}
	return ulSeed;
}

static void SetPaintSeed( itemid_t unItemID, uint64 ulSeed )
{
	SetAttr( unItemID, "custom_paintkit_seed_lo", CFmtStr( "%u", (uint32)( ulSeed & 0xFFFFFFFF ) ) );
	SetAttr( unItemID, "custom_paintkit_seed_hi", CFmtStr( "%u", (uint32)( ulSeed >> 32 ) ) );
}

// What the item server did with a War Paint: use it up, and make a new
// decorated weapon of the chosen type with the paint, its wear and the
// pattern the preview showed. A Strange or Unusual War Paint makes a Strange
// or Unusual weapon.
static void ConsumeWarPaint( itemid_t unPaintID, uint32 unTargetDefIndex )
{
	const CEconItem *pPaint = TFOfflineInventory().GetLiveItem( unPaintID );
	const CEconItemDefinition *pTargetDef = GetItemSchema()->GetItemDefinition( unTargetDefIndex );
	if ( !pPaint || !pTargetDef || !OfflineTools_IsWarPaint( pPaint->GetItemDefinition() ) )
	{
		Warning( "Offline inventory: couldn't use that War Paint.\n" );
		return;
	}

	uint32 unPaintKit = 0;
	if ( !GetPaintKitDefIndex( pPaint, &unPaintKit ) )
		return;

	float flWear = 0.f;
	if ( !GetItemAttrNumber( unPaintID, Attr( "set_item_texture_wear" ), &flWear ) || flWear <= 0.f )
	{
		flWear = OfflineTools_RandomWear();
	}

	// Keep the War Paint's own extras (kill counter, unusual effect, ...).
	KeyValues *pPaintKV = TFOfflineInventory().FindItemKV( unPaintID );
	KeyValues *pPaintAttrs = pPaintKV ? pPaintKV->FindKey( "attributes" ) : NULL;
	KeyValuesAD pAttributes( pPaintAttrs ? pPaintAttrs->MakeCopy() : new KeyValues( "attributes" ) );

	const uint64 ulSeed = GetPaintSeed( unPaintID );
	pAttributes->SetString( "paintkit_proto_def_index", CFmtStr( "%u", unPaintKit ) );
	pAttributes->SetString( "set_item_texture_wear", CFmtStr( "%g", flWear ) );
	pAttributes->SetString( "custom_paintkit_seed_lo", CFmtStr( "%u", (uint32)( ulSeed & 0xFFFFFFFF ) ) );
	pAttributes->SetString( "custom_paintkit_seed_hi", CFmtStr( "%u", (uint32)( ulSeed >> 32 ) ) );

	int nQuality = AE_PAINTKITWEAPON;
	if ( pPaint->GetQuality() == AE_STRANGE || BIsItemStrange( pPaint ) )
	{
		nQuality = AE_STRANGE;
	}
	if ( pPaint->GetQuality() == AE_UNUSUAL || pAttributes->FindKey( "attach particle effect" ) )
	{
		nQuality = AE_UNUSUAL;	// keeps its kill counter too, if it had one
	}

	const itemid_t unNewItemID = TFOfflineInventory().CreateItem( pTargetDef, nQuality, pAttributes, kEconItemOrigin_Paintkit, UNACK_ITEM_PAINTKIT );
	if ( unNewItemID == INVALID_ITEM_ID )
		return;

	char szName[256];
	OfflineInventory_GetItemDefDisplayName( pTargetDef, szName, sizeof( szName ) );
	TFOfflineInventory().DeleteItem( unPaintID );
	Msg( "Painted a new %s (%s, item ID %llu).\n", szName, OfflineTools_GetWearName( flWear ), (unsigned long long)unNewItemID );
}

//-----------------------------------------------------------------------------
// Backpack actions
//-----------------------------------------------------------------------------
static void DeleteFromBackpack( itemid_t unItemID )
{
	if ( ItemHasAttr( unItemID, Attr( "cannot delete" ) ) )
		return;

	char szName[256];
	GetItemName( unItemID, szName, sizeof( szName ) );
	if ( TFOfflineInventory().DeleteItem( unItemID ) )
	{
		Msg( "Deleted %s.\n", szName );
	}
}

// Unusual effect height (the loadout screen's effect position controls).
static void SetEffectVerticalOffset( itemid_t unItemID, float flOffset )
{
	if ( flOffset == 0.f )
	{
		const char *pszAttr = "particle effect vertical offset";
		RemoveAttrs( unItemID, &pszAttr, 1 );
	}
	else
	{
		SetAttr( unItemID, "particle effect vertical offset", CFmtStr( "%g", flOffset ) );
	}
}

static void SetEffectUseHeadOrigin( itemid_t unItemID, bool bUseHead )
{
	if ( bUseHead )
	{
		SetAttr( unItemID, "particle effect use head origin", "1" );
	}
	else
	{
		const char *pszAttr = "particle effect use head origin";
		RemoveAttrs( unItemID, &pszAttr, 1 );
	}
}

//-----------------------------------------------------------------------------
// Backpack sorting
//-----------------------------------------------------------------------------
struct OfflineSortEntry_t
{
	itemid_t	m_unItemID;
	int			m_nKey1;
	int			m_nKey2;
	CUtlString	m_strGroup;
	CUtlString	m_strName;
};

static int s_OfflineSortType = kGCItemSort_NoSort;

static int CompareSortEntries( const OfflineSortEntry_t *pA, const OfflineSortEntry_t *pB )
{
	if ( s_OfflineSortType == kGCItemSort_SortByDate )
	{
		// Newest first.
		if ( pA->m_unItemID != pB->m_unItemID )
			return pA->m_unItemID > pB->m_unItemID ? -1 : 1;
		return 0;
	}

	if ( pA->m_nKey1 != pB->m_nKey1 )
		return pA->m_nKey1 < pB->m_nKey1 ? -1 : 1;

	const int nGroup = V_stricmp( pA->m_strGroup.Get(), pB->m_strGroup.Get() );
	if ( nGroup != 0 )
		return nGroup;

	if ( pA->m_nKey2 != pB->m_nKey2 )
		return pA->m_nKey2 < pB->m_nKey2 ? -1 : 1;

	const int nName = V_stricmp( pA->m_strName.Get(), pB->m_strName.Get() );
	if ( nName != 0 )
		return nName;

	if ( pA->m_unItemID != pB->m_unItemID )
		return pA->m_unItemID < pB->m_unItemID ? -1 : 1;
	return 0;
}

// Best qualities first, the way the backpack's "rarity" sort always worked.
static int GetQualitySortRank( int nQuality )
{
	static const int s_nOrder[] =
	{
		AE_UNUSUAL, AE_STRANGE, AE_COLLECTORS, AE_HAUNTED, AE_RARITY1 /* genuine */, AE_VINTAGE,
		AE_PAINTKITWEAPON, AE_SELFMADE, AE_COMMUNITY, AE_DEVELOPER, AE_UNIQUE, AE_NORMAL,
	};
	for ( int i = 0; i < ARRAYSIZE( s_nOrder ); ++i )
	{
		if ( s_nOrder[i] == nQuality )
			return i;
	}
	return ARRAYSIZE( s_nOrder );
}

// Loadout screen class order: Scout, Soldier, Pyro, Demoman, Heavy, Engineer, Medic, Sniper, Spy.
static int GetClassSortRank( const CEconItemDefinition *pItemDef )
{
	static const int s_nClassOrder[] =
	{
		TF_CLASS_SCOUT, TF_CLASS_SOLDIER, TF_CLASS_PYRO, TF_CLASS_DEMOMAN, TF_CLASS_HEAVYWEAPONS,
		TF_CLASS_ENGINEER, TF_CLASS_MEDIC, TF_CLASS_SNIPER, TF_CLASS_SPY,
	};

	const GameItemDefinition_t *pTFItemDef = static_cast< const GameItemDefinition_t * >( pItemDef );
	int nFirst = -1;
	int nCount = 0;
	for ( int i = 0; i < ARRAYSIZE( s_nClassOrder ); ++i )
	{
		if ( pTFItemDef->CanBeUsedByClass( s_nClassOrder[i] ) )
		{
			if ( nFirst < 0 )
			{
				nFirst = i;
			}
			nCount++;
		}
	}

	if ( nCount == 0 )
		return ARRAYSIZE( s_nClassOrder ) + 1;	// tools, crates, ...
	if ( nCount > 1 )
		return ARRAYSIZE( s_nClassOrder );		// multi-class and all-class items after single-class ones
	return nFirst;
}

static int GetSlotSortRank( const CEconItemDefinition *pItemDef )
{
	const GameItemDefinition_t *pTFItemDef = static_cast< const GameItemDefinition_t * >( pItemDef );
	for ( int iClass = TF_FIRST_NORMAL_CLASS; iClass < TF_LAST_NORMAL_CLASS; ++iClass )
	{
		if ( pTFItemDef->CanBeUsedByClass( iClass ) )
		{
			const int iSlot = pTFItemDef->GetLoadoutSlot( iClass );
			if ( iSlot != LOADOUT_POSITION_INVALID )
				return iSlot;
		}
	}
	return CLASS_LOADOUT_POSITION_COUNT;	// tools, crates, ...
}

static void LocalizeToAnsi( const char *pszToken, CUtlString &strOut )
{
	strOut = pszToken ? pszToken : "";
	if ( pszToken && pszToken[0] == '#' )
	{
		const locchar_t *pszLocalized = GLocalizationProvider()->Find( pszToken );
		if ( pszLocalized )
		{
			char szAnsi[256];
			GLocalizationProvider()->ConvertLoccharToANSI( pszLocalized, szAnsi, sizeof( szAnsi ) );
			strOut = szAnsi;
		}
	}
}

static void SortBackpack( uint32 unSortType )
{
	CUtlVector< OfflineSortEntry_t > vecEntries;

	KeyValues *pItemsKV = TFOfflineInventory().GetItemsKV();
	if ( pItemsKV )
	{
		FOR_EACH_TRUE_SUBKEY( pItemsKV, pItemKV )
		{
			// Leave new items (not placed yet) alone.
			if ( pItemKV->GetInt( "slot", 0 ) <= 0 )
				continue;

			const itemid_t unItemID = V_atoui64( pItemKV->GetName() );
			const CEconItem *pItem = TFOfflineInventory().GetLiveItem( unItemID );
			const CEconItemDefinition *pItemDef = pItem ? pItem->GetItemDefinition() : NULL;
			if ( !pItemDef )
				continue;

			OfflineSortEntry_t &entry = vecEntries[ vecEntries.AddToTail() ];
			entry.m_unItemID = unItemID;
			entry.m_nKey1 = 0;
			entry.m_nKey2 = 0;

			char szName[256];
			OfflineInventory_GetItemDefDisplayName( pItemDef, szName, sizeof( szName ) );
			entry.m_strName = szName;

			switch ( unSortType )
			{
			case kGCItemSort_SortByRarity:
				entry.m_nKey1 = GetQualitySortRank( pItem->GetQuality() );
				entry.m_nKey2 = -(int)pItem->GetRarity();
				break;
			case kGCItemSort_SortByType:
				LocalizeToAnsi( pItemDef->GetItemTypeName(), entry.m_strGroup );
				break;
			case kTFGCItemSort_SortByClass:
				entry.m_nKey1 = GetClassSortRank( pItemDef );
				entry.m_nKey2 = GetSlotSortRank( pItemDef );
				break;
			case kTFGCItemSort_SortBySlot:
				entry.m_nKey1 = GetSlotSortRank( pItemDef );
				entry.m_nKey2 = GetClassSortRank( pItemDef );
				break;
			case kGCItemSort_SortByDefIndex:
				entry.m_nKey1 = pItemDef->GetDefinitionIndex();
				break;
			default:	// by name
				break;
			}
		}
	}

	s_OfflineSortType = unSortType;
	vecEntries.Sort( CompareSortEntries );

	CUtlVector< itemid_t > vecOrder;
	FOR_EACH_VEC( vecEntries, i )
	{
		vecOrder.AddToTail( vecEntries[i].m_unItemID );
	}
	TFOfflineInventory().SetBackpackOrder( vecOrder );

	// What the item server's "sort finished" reply does.
	CloseWaitingDialog();
	InventoryManager()->SortBackpackFinished();
}

//-----------------------------------------------------------------------------
// Dispatch
//-----------------------------------------------------------------------------
static void ProcessRequest( const OfflineToolRequest_t &request )
{
	switch ( request.m_unMsgType )
	{
	case k_EMsgGCPaintItem:
	{
		MsgGCPaintItem_t body;
		if ( ReadStructBody( request, &body ) )
		{
			ApplyPaintCan( body.m_unToolItemID, body.m_unSubjectItemID );
		}
		break;
	}

	case k_EMsgGCNameItem:
	{
		MsgGCNameItem_t body;
		if ( ReadStructBody( request, &body ) )
		{
			ApplyNameTag( body.m_unToolItemID, body.m_unSubjectItemID, ReadStructString< MsgGCNameItem_t >( request ), body.m_bDescription );
		}
		break;
	}

	case k_EMsgGCNameBaseItem:
	{
		MsgGCNameBaseItem_t body;
		if ( ReadStructBody( request, &body ) && TFOfflineInventory().FindItemKV( body.m_unToolItemID ) )
		{
			const itemid_t unNewItemID = CreateItemFromBaseItem( body.m_unBaseItemDefinitionID );
			if ( unNewItemID != INVALID_ITEM_ID )
			{
				ApplyNameTag( body.m_unToolItemID, unNewItemID, ReadStructString< MsgGCNameBaseItem_t >( request ), body.m_bDescription );
			}
		}
		break;
	}

	case k_EMsgGCRemoveItemName:
	{
		MsgGCRemoveItemName_t body;
		if ( ReadStructBody( request, &body ) )
		{
			TFOfflineInventory().SetItemCustomText( body.m_unItemID, body.m_bDescription, NULL );
		}
		break;
	}

	case k_EMsgGCSetItemStyle:
	{
		MsgGCSetItemStyle_t body;
		if ( ReadStructBody( request, &body ) )
		{
			TFOfflineInventory().SetItemStyle( body.m_unItemID, body.m_iStyle );
		}
		break;
	}

	case k_EMsgGCCraft:
		OfflineCrafting_ProcessCraftRequest( request.m_strBody.data(), (uint32)request.m_strBody.size() );
		break;

	case k_EMsgGCDelete:
	{
		MsgGCDelete_t body;
		if ( ReadStructBody( request, &body ) )
		{
			DeleteFromBackpack( body.m_unItemID );
		}
		break;
	}

	case k_EMsgGCConsumePaintKit:
	{
		CMsgConsumePaintkit msg;
		if ( msg.ParseFromString( request.m_strBody ) )
		{
			ConsumeWarPaint( msg.source_id(), msg.target_defindex() );
		}
		break;
	}

	case k_EMsgGCSortItems:
	{
		CMsgSortItems msg;
		if ( msg.ParseFromString( request.m_strBody ) )
		{
			SortBackpack( msg.sort_type() );
		}
		break;
	}

	case k_EMsgGCSetItemEffectVerticalOffset:
	{
		CMsgSetItemEffectVerticalOffset msg;
		if ( msg.ParseFromString( request.m_strBody ) )
		{
			SetEffectVerticalOffset( msg.item_id(), msg.offset() );
		}
		break;
	}

	case k_EMsgGCSetHatEffectUseHeadOrigin:
	{
		CMsgSetHatEffectUseHeadOrigin msg;
		if ( msg.ParseFromString( request.m_strBody ) )
		{
			SetEffectUseHeadOrigin( msg.item_id(), msg.use_head() );
		}
		break;
	}

	case k_EMsgGCCraftCollectionUpgrade:
	{
		CMsgCraftCollectionUpgrade msg;
		if ( msg.ParseFromString( request.m_strBody ) )
		{
			CUtlVector< itemid_t > vecItemIDs;
			for ( int i = 0; i < msg.item_id_size(); ++i )
			{
				vecItemIDs.AddToTail( msg.item_id( i ) );
			}
			OfflineCrafting_TradeUp( vecItemIDs );
		}
		break;
	}

	case k_EMsgGCCraftCommonStatClock:
	{
		CMsgCraftCommonStatClock msg;
		if ( msg.ParseFromString( request.m_strBody ) )
		{
			CUtlVector< itemid_t > vecItemIDs;
			for ( int i = 0; i < msg.item_id_size(); ++i )
			{
				vecItemIDs.AddToTail( msg.item_id( i ) );
			}
			OfflineCrafting_CraftStatClock( vecItemIDs );
		}
		break;
	}

	case k_EMsgGCApplyStrangePart:
	{
		CMsgApplyStrangePart msg;
		if ( msg.ParseFromString( request.m_strBody ) )
		{
			ApplyStrangePart( msg.strange_part_item_id(), msg.item_item_id() );
		}
		break;
	}

	case k_EMsgGCApplyStrangeRestriction:
	{
		CMsgApplyStrangeRestriction msg;
		if ( msg.ParseFromString( request.m_strBody ) )
		{
			ApplyStrangeFilter( msg.strange_part_item_id(), msg.item_item_id(), msg.strange_attr_index() );
		}
		break;
	}

	case k_EMsgGCApplyStrangeCountTransfer:
	{
		CMsgApplyStrangeCountTransfer msg;
		if ( msg.ParseFromString( request.m_strBody ) )
		{
			ApplyStrangeCountTransfer( msg.tool_item_id(), msg.item_src_item_id(), msg.item_dest_item_id() );
		}
		break;
	}

	case k_EMsgGCApplyXifier:
	{
		CMsgApplyToolToItem msg;
		if ( msg.ParseFromString( request.m_strBody ) )
		{
			ApplyXifier( msg.tool_item_id(), msg.subject_item_id() );
		}
		break;
	}

	case k_EMsgGCApplyBaseItemXifier:
	{
		CMsgApplyToolToBaseItem msg;
		if ( msg.ParseFromString( request.m_strBody ) && TFOfflineInventory().FindItemKV( msg.tool_item_id() ) )
		{
			const itemid_t unNewItemID = CreateItemFromBaseItem( msg.baseitem_def_index() );
			if ( unNewItemID != INVALID_ITEM_ID )
			{
				ApplyXifier( msg.tool_item_id(), unNewItemID );
			}
		}
		break;
	}

	case k_EMsgGCRemoveItemPaint:
	case k_EMsgGCRemoveMakersMark:
	case k_EMsgGCRemoveKillStreak:
	case k_EMsgGCRemoveGiftedBy:
	case k_EMsgGCRemoveFestivizer:
	case k_EMsgGCRemoveCustomTexture:
	{
		CMsgGCRemoveCustomizationAttributeSimple msg;
		if ( msg.ParseFromString( request.m_strBody ) )
		{
			ApplyRestore( request.m_unMsgType, msg.item_id() );
		}
		break;
	}

	case k_EMsgGCRemoveStrangePart:
	{
		CMsgGCRemoveStrangePart msg;
		if ( msg.ParseFromString( request.m_strBody ) )
		{
			RemoveStrangePart( msg.item_id(), msg.strange_part_score_type() );
		}
		break;
	}

	case k_EMsgGCResetStrangeScores:
	{
		CMsgGCResetStrangeScores msg;
		if ( msg.ParseFromString( request.m_strBody ) )
		{
			ResetStrangeScores( msg.item_id() );
		}
		break;
	}

	case k_EMsgGCRemoveUpgradeCard:
	{
		CMsgGCRemoveUpgradeCard msg;
		if ( msg.ParseFromString( request.m_strBody ) )
		{
			TFOfflineInventory().RemoveItemAttribute( msg.item_id(), GetItemSchema()->GetAttributeDefinition( msg.attribute_index() ) );
		}
		break;
	}
	}
}

void OfflineTools_ProcessPendingRequests()
{
	if ( s_vecPendingRequests.Count() == 0 )
		return;

	CUtlVector< OfflineToolRequest_t > vecRequests;
	vecRequests.Swap( s_vecPendingRequests );

	FOR_EACH_VEC( vecRequests, i )
	{
		ProcessRequest( vecRequests[i] );
	}
}

//=============================================================================
// Console commands: War Paint patterns and wear
//=============================================================================
static itemid_t ParsePaintedItem( const CCommand &args, const char *pszUsage )
{
	if ( !TFOfflineInventory().IsLoaded() )
	{
		Msg( "The offline inventory hasn't loaded yet. Make sure Steam is running, then try again in a moment.\n" );
		return INVALID_ITEM_ID;
	}
	if ( args.ArgC() < 2 )
	{
		Msg( "%s", pszUsage );
		return INVALID_ITEM_ID;
	}

	const itemid_t unItemID = OfflineInventory_ParseExistingItemID( args[1] );
	if ( unItemID == INVALID_ITEM_ID )
		return INVALID_ITEM_ID;

	const CEconItem *pItem = TFOfflineInventory().GetLiveItem( unItemID );
	uint32 unPaintKit = 0;
	if ( !pItem || !GetPaintKitDefIndex( pItem, &unPaintKit ) )
	{
		Msg( "That item has no war paint.\n" );
		return INVALID_ITEM_ID;
	}
	return unItemID;
}

static float GetItemWear( itemid_t unItemID )
{
	float flWear = 0.f;
	if ( !GetItemAttrNumber( unItemID, Attr( "set_item_texture_wear" ), &flWear ) || flWear <= 0.f )
	{
		const CEconItem *pItem = TFOfflineInventory().GetLiveItem( unItemID );
		if ( !pItem || !GetPaintKitWear( pItem, flWear ) || flWear <= 0.f )
		{
			flWear = 0.6f;
		}
	}
	return flWear;
}

static const char s_szRerollUsage[] =
	"Usage: inv_reroll <item ID | last> [pattern | wear | both]\n"
	"  Re-rolls a War Paint's or painted weapon's pattern (default), wear grade, or both.\n"
	"  Re-rolling a War Paint before you use it changes the weapon you'll get.\n";

CON_COMMAND( inv_reroll, "Re-roll a War Paint's or painted weapon's pattern and/or wear.\n"
	"  Usage: inv_reroll <item ID | last> [pattern | wear | both]" )
{
	const itemid_t unItemID = ParsePaintedItem( args, s_szRerollUsage );
	if ( unItemID == INVALID_ITEM_ID )
		return;

	const char *pszWhat = args.ArgC() >= 3 ? args[2] : "pattern";
	const bool bPattern = !V_stricmp( pszWhat, "pattern" ) || !V_stricmp( pszWhat, "both" ) || !V_stricmp( pszWhat, "seed" );
	const bool bWear = !V_stricmp( pszWhat, "wear" ) || !V_stricmp( pszWhat, "both" );
	if ( !bPattern && !bWear )
	{
		Msg( "%s", s_szRerollUsage );
		return;
	}

	char szName[256];
	GetItemName( unItemID, szName, sizeof( szName ) );

	if ( bPattern )
	{
		const uint64 ulSeed = (uint64)RandomInt( 1, 0x7FFFFFFF );
		SetPaintSeed( unItemID, ulSeed );
		Msg( "New pattern for %s (seed %llu).\n", szName, (unsigned long long)ulSeed );
	}

	if ( bWear )
	{
		const float flOldWear = GetItemWear( unItemID );
		float flNewWear = flOldWear;
		for ( int nTries = 0; nTries < 20 && fabsf( flNewWear - flOldWear ) < 0.01f; ++nTries )
		{
			flNewWear = OfflineTools_RandomWear();
		}
		SetAttr( unItemID, "set_item_texture_wear", CFmtStr( "%g", flNewWear ) );
		Msg( "%s is now %s.\n", szName, OfflineTools_GetWearName( flNewWear ) );
	}
}

CON_COMMAND( inv_wear, "Set a War Paint's or painted weapon's wear.\n"
	"  Usage: inv_wear <item ID | last> <fn | mw | ft | ww | bs | random>\n"
	"  (Factory New, Minimal Wear, Field-Tested, Well-Worn, Battle Scarred)" )
{
	static const char s_szUsage[] = "Usage: inv_wear <item ID | last> <fn | mw | ft | ww | bs | random>\n";
	const itemid_t unItemID = ParsePaintedItem( args, s_szUsage );
	if ( unItemID == INVALID_ITEM_ID )
		return;

	// "field tested" may arrive as two words.
	char szWear[64] = "";
	for ( int i = 2; i < args.ArgC(); ++i )
	{
		if ( i > 2 )
		{
			V_strncat( szWear, " ", sizeof( szWear ) );
		}
		V_strncat( szWear, args[i], sizeof( szWear ) );
	}

	float flWear = 0.f;
	if ( !OfflineTools_ParseWear( szWear, &flWear ) )
	{
		Msg( "%s", s_szUsage );
		return;
	}

	char szName[256];
	GetItemName( unItemID, szName, sizeof( szName ) );
	SetAttr( unItemID, "set_item_texture_wear", CFmtStr( "%g", flWear ) );
	Msg( "%s is now %s.\n", szName, OfflineTools_GetWearName( flWear ) );
}
