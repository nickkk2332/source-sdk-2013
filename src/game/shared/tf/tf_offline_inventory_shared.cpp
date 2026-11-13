//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Shared helpers for the offline inventory. See the header.
//
//=============================================================================

#include "cbase.h"
#include "tf_offline_inventory_shared.h"
#include "econ_item.h"
#include "base_gcmessages.pb.h"
#include "gcsdk/gcsdk_auto.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//-----------------------------------------------------------------------------
// Minimal protobuf wire-format writers.
//
// CMsgSOCacheSubscribed, as compiled into gcsdk_sdk:
//	optional fixed64        owner       = 1;
//	repeated SubscribedType objects     = 2;
//	optional fixed64        version     = 3;
//	(fields 4-7 are not used by the TF client)
//
// CMsgSOCacheSubscribed.SubscribedType:
//	optional int32 type_id     = 1;
//	repeated bytes object_data = 2;
//-----------------------------------------------------------------------------
namespace
{
	enum
	{
		kWireType_Varint = 0,
		kWireType_Fixed64 = 1,
		kWireType_LengthDelimited = 2,
	};

	void WriteVarint( CUtlBuffer &buf, uint64 ulValue )
	{
		while ( ulValue >= 0x80 )
		{
			buf.PutUnsignedChar( (unsigned char)( ( ulValue & 0x7F ) | 0x80 ) );
			ulValue >>= 7;
		}
		buf.PutUnsignedChar( (unsigned char)ulValue );
	}

	void WriteTag( CUtlBuffer &buf, int nField, int nWireType )
	{
		WriteVarint( buf, ( (uint64)nField << 3 ) | (uint64)nWireType );
	}

	void WriteFixed64Field( CUtlBuffer &buf, int nField, uint64 ulValue )
	{
		WriteTag( buf, nField, kWireType_Fixed64 );
		for ( int i = 0; i < 8; ++i )
		{
			buf.PutUnsignedChar( (unsigned char)( ( ulValue >> ( 8 * i ) ) & 0xFF ) );
		}
	}

	void WriteBytesField( CUtlBuffer &buf, int nField, const void *pData, int nSize )
	{
		WriteTag( buf, nField, kWireType_LengthDelimited );
		WriteVarint( buf, (uint64)nSize );
		if ( nSize > 0 )
		{
			buf.Put( pData, nSize );
		}
	}
}

//-----------------------------------------------------------------------------
void COfflineSOCacheMsgBuilder::AddObject( int nTypeID, const std::string &strObjectData )
{
	int iIndex = m_vecObjects.AddToTail();
	m_vecObjects[iIndex].m_nTypeID = nTypeID;
	m_vecObjects[iIndex].m_strData = strObjectData;
}

//-----------------------------------------------------------------------------
void COfflineSOCacheMsgBuilder::AddItem( const CEconItem &item )
{
	CSOEconItem msgItem;
	item.SerializeToProtoBufItem( msgItem );

	std::string strData;
	msgItem.SerializeToString( &strData );
	AddObject( CEconItem::k_nTypeID, strData );
}

//-----------------------------------------------------------------------------
void COfflineSOCacheMsgBuilder::Serialize( uint64 ulOwnerSteamID, uint64 ulVersion, CUtlBuffer &bufOut ) const
{
	bufOut.Purge();

	WriteFixed64Field( bufOut, 1, ulOwnerSteamID );

	// Group objects by type: one SubscribedType per type ID.
	CUtlVector< int > vecTypesWritten;
	FOR_EACH_VEC( m_vecObjects, i )
	{
		const int nTypeID = m_vecObjects[i].m_nTypeID;
		if ( vecTypesWritten.HasElement( nTypeID ) )
			continue;
		vecTypesWritten.AddToTail( nTypeID );

		CUtlBuffer bufType;
		WriteTag( bufType, 1, kWireType_Varint );
		WriteVarint( bufType, (uint64)(uint32)nTypeID );

		for ( int j = i; j < m_vecObjects.Count(); ++j )
		{
			if ( m_vecObjects[j].m_nTypeID != nTypeID )
				continue;

			const std::string &strData = m_vecObjects[j].m_strData;
			WriteBytesField( bufType, 2, strData.data(), (int)strData.size() );
		}

		WriteBytesField( bufOut, 2, bufType.Base(), bufType.TellPut() );
	}

	WriteFixed64Field( bufOut, 3, ulVersion );
}

//-----------------------------------------------------------------------------
bool OfflineInventory_EncodeItem( const CEconItem &item, CUtlString &strBase64Out )
{
	CSOEconItem msgItem;
	item.SerializeToProtoBufItem( msgItem );

	std::string strData;
	if ( !msgItem.SerializeToString( &strData ) )
		return false;

	// Zero the buffer first so the result is terminated whether or not the
	// encoder (prebuilt, in gcsdk) writes a terminator itself.
	const uint32 unCapacity = GCSDK::Base64EncodeMaxOutput( (uint32)strData.size() ) + 1;
	CUtlMemory< char > memEncoded;
	memEncoded.EnsureCapacity( unCapacity );
	V_memset( memEncoded.Base(), 0, unCapacity );

	uint32 unEncodedSize = unCapacity - 1;
	if ( !GCSDK::Base64Encode( (const uint8 *)strData.data(), (uint32)strData.size(), memEncoded.Base(), &unEncodedSize ) )
		return false;

	strBase64Out.Set( memEncoded.Base() );
	return true;
}

//-----------------------------------------------------------------------------
bool OfflineInventory_DecodeItemBytes( const char *pszBase64, std::string &strBytesOut )
{
	if ( !pszBase64 || !pszBase64[0] )
		return false;

	const uint32 unInputLength = V_strlen( pszBase64 );
	uint32 unDecodedSize = ( unInputLength / 4 ) * 3 + 4;
	CUtlMemory< uint8 > memDecoded;
	memDecoded.EnsureCapacity( unDecodedSize );
	if ( !GCSDK::Base64Decode( pszBase64, unInputLength, memDecoded.Base(), &unDecodedSize, false ) )
		return false;

	strBytesOut.assign( (const char *)memDecoded.Base(), unDecodedSize );
	return true;
}
