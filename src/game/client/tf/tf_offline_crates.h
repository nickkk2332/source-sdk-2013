//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Offline crates: decide what comes out of a crate, the way Valve's
//			item servers used to.
//
//			Crate contents come from the game's own item schema (the same lists
//			the "Contents" view in the backpack shows). The schema that ships
//			with the game has the odds stripped out, so odds come from the
//			inv_crate_* console variables instead.
//
//=============================================================================

#ifndef TF_OFFLINE_CRATES_H
#define TF_OFFLINE_CRATES_H
#ifdef _WIN32
#pragma once
#endif

#include "utlvector.h"

class KeyValues;
class CEconItemDefinition;
class IEconItemInterface;

//-----------------------------------------------------------------------------
// One item that came out of a crate, not yet in the backpack.
//-----------------------------------------------------------------------------
class COfflineLootItem
{
public:
	COfflineLootItem();
	~COfflineLootItem();

	void		SetAttribute( const char *pszName, const char *pszValue );
	bool		HasAttribute( const char *pszName ) const;

	const CEconItemDefinition	*m_pItemDef;
	int							m_nQuality;
	KeyValues					*m_pAttributes;		// attribute name -> value as text; NULL if none

private:
	COfflineLootItem( const COfflineLootItem & );	// not copyable
	void operator=( const COfflineLootItem & );
};

// Rolls what comes out of a crate. The first item is the main prize; any after
// it are bonus items. The caller owns the items (PurgeAndDeleteElements).
// Returns false if the game doesn't know what the crate contains.
bool OfflineCrates_RollCrate( const IEconItemInterface *pCrate, CUtlVector< COfflineLootItem * > &vecOut );

// Crates and keys
bool OfflineCrates_IsCrate( const CEconItemDefinition *pItemDef );
bool OfflineCrates_NeedsKey( const CEconItemDefinition *pCrateDef );
const CEconItemDefinition *OfflineCrates_GetKeyForCrate( const CEconItemDefinition *pCrateDef );

#endif // TF_OFFLINE_CRATES_H
