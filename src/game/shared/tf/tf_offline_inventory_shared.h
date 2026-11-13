//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Shared helpers for the offline inventory.
//
//			The offline inventory replaces Valve's item servers with a save file
//			on the local machine. The client builds a "shared object cache"
//			(the in-memory backpack) from that file, and sends the items it has
//			equipped to whatever server it joins. The server trusts that list.
//
//			The message format that loads a whole cache at once
//			(CMsgSOCacheSubscribed) lives inside the prebuilt gcsdk library and
//			has no header in the SDK, so we write its few fields by hand here.
//
//=============================================================================

#ifndef TF_OFFLINE_INVENTORY_SHARED_H
#define TF_OFFLINE_INVENTORY_SHARED_H
#ifdef _WIN32
#pragma once
#endif

#include "utlbuffer.h"
#include "utlvector.h"
#include "utlstring.h"
#include <string>

class CEconItem;

//-----------------------------------------------------------------------------
// Builds a serialized CMsgSOCacheSubscribed that can be handed to
// GCSDK::CGCClient::AddLocalSOCache().
//-----------------------------------------------------------------------------
class COfflineSOCacheMsgBuilder
{
public:
	// strObjectData is the serialized protobuf for one shared object (e.g. a CSOEconItem).
	void AddObject( int nTypeID, const std::string &strObjectData );

	// Adds an already serialized CEconItem.
	void AddItem( const CEconItem &item );

	int GetObjectCount() const { return m_vecObjects.Count(); }

	void Serialize( uint64 ulOwnerSteamID, uint64 ulVersion, CUtlBuffer &bufOut ) const;

private:
	struct Object_t
	{
		int m_nTypeID;
		std::string m_strData;
	};
	CUtlVector< Object_t > m_vecObjects;
};

namespace google
{
	namespace protobuf
	{
		class Message;
	}
}

// Server: called from GCClientSystem()->BSendMessage(). Strange counter
// updates go to the item's owner (whose game keeps the save file) instead of
// an item server. Returns true if the message was handled.
bool OfflineInventory_ServerInterceptGCMessage( uint32 unMsgType, const ::google::protobuf::Message &body );

// Item <-> text, used to carry items from client to server inside KeyValues.
bool OfflineInventory_EncodeItem( const CEconItem &item, CUtlString &strBase64Out );
bool OfflineInventory_DecodeItemBytes( const char *pszBase64, std::string &strBytesOut );

#endif // TF_OFFLINE_INVENTORY_SHARED_H
