//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Offline tools: what Valve's item servers did when you used a tool
//			from the backpack (paint cans, name tags, strange parts, ...).
//
//			The backpack UI sends its requests through GCClientSystem()->
//			BSendMessage() as before. On the client, requests this file knows
//			how to handle are copied off there, and carried out on the next
//			frame against the offline inventory, the way a reply from an item
//			server would arrive.
//
//=============================================================================

#ifndef TF_OFFLINE_TOOLS_H
#define TF_OFFLINE_TOOLS_H
#ifdef _WIN32
#pragma once
#endif

#include "econ_item_constants.h"

class CEconItemDefinition;

namespace google
{
	namespace protobuf
	{
		class Message;
	}
}

// Called from GCClientSystem()->BSendMessage(). Returns true if the request is
// one we handle (it is then queued and must not go anywhere else).
// pBody is everything after the message header (the struct, then any strings).
bool OfflineTools_InterceptStructMessage( uint32 unMsgType, const void *pBody, uint32 cubBody );
bool OfflineTools_InterceptProtoMessage( uint32 unMsgType, const ::google::protobuf::Message &body );

// Carries out queued requests. Called by the offline inventory each frame.
void OfflineTools_ProcessPendingRequests();

// Copies a paint can's colors onto an item. Used by the paint can tool and inv_paint.
bool OfflineTools_PaintItem( itemid_t unItemID, const CEconItemDefinition *pPaintCanDef );

// War Paints: the paint "canvas" tools you use on a weapon type.
bool		OfflineTools_IsWarPaint( const CEconItemDefinition *pItemDef );
// Wear grades, as the "set_item_texture_wear" value: 0.2 Factory New ... 1.0 Battle Scarred.
float		OfflineTools_RandomWear();
// Accepts "fn", "factory new", "factory_new", 0.2, ..., or "random".
bool		OfflineTools_ParseWear( const char *pszText, float *pflWear );
const char	*OfflineTools_GetWearName( float flWear );

#endif // TF_OFFLINE_TOOLS_H
