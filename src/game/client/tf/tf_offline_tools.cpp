//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Offline tools. See tf_offline_tools.h.
//
//=============================================================================

#include "cbase.h"
#include "tf_offline_tools.h"
#include "tf_offline_inventory.h"
#include "tf_offline_crates.h"
#include "econ_gcmessages.h"
#include "base_gcmessages.pb.h"
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
