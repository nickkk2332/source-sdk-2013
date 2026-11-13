//========= Copyright Valve Corporation, All rights reserved. ============//
#include "cbase.h"
#include "tf_gc_client.h"
#include "gcsdk/gcsdk_auto.h"
#include "tf_gcmessages.h"
#include "kvpacker.h"
#include "tf_party.h"
// XXX(JohnS): Eventually, we want to send a smaller lobby object to clients. For now, they use the CTFGSLobby, which is
//             in shared code for that reason.
#include "tf_lobby_server.h"
#include "base_gcmessages.pb.h"
#include "igameevents.h"
#include "netadr.h"
#include "econ_item_inventory.h"
#include "tf_item_inventory.h"
#include "tf_hud_mann_vs_machine_status.h"
#include "tf_hud_mainmenuoverride.h"
#include "econ/confirm_dialog.h"
#include "rtime.h"
#include "ienginevgui.h"
#include "clientmode_tf.h"
#include "tf_match_description.h"
#include "tf_progression_description.h"
#include "tf_matchcriteria.h"
#include "tf_xp_source.h"
#include "tf_notification.h"
#include "c_tf_notification.h"
#include "tf_gc_shared.h"
#include "tf_partyclient.h"
#include "tf_matchmaking_dashboard.h"
#include "tf_ladder_data.h"
#include "tf_rating_data.h"
#include "tf_quest_map.h"
#include "tf_quest_map_node.h"
#include "econ_quests.h"

#include "econ_item_description.h"

#include "tf_hud_disconnect_prompt.h"

#include "util_shared.h"
#include <steam/isteamnetworkingutils.h>
#include <steam/isteamnetworkingsockets.h>
#include "filesystem.h"
#include "steam/isteamuser.h"
#include "tf_offline_inventory.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#ifdef _DEBUG
	#define GCMATCHMAKING_DEBUG_LEVEL 4
#else
	#define GCMATCHMAKING_DEBUG_LEVEL 1
#endif

ConVar tf_mm_debug_level( "tf_mm_debug_level", "4" );

#define GCMatchmakingDebugSpew( lvl, ...) do { if ( lvl <= GCMATCHMAKING_DEBUG_LEVEL || lvl <= tf_mm_debug_level.GetInt() ) { Msg( __VA_ARGS__); } } while(false)




using namespace GCSDK;

static const char* GetWebBaseUrl()
{
	switch( GetUniverse() )
	{
	case k_EUniverseDev:
		return "https://teamfortress.local.steam.dev/";
	case k_EUniverseBeta:
		return "https://beta.teamfortress.com/";
	case k_EUniversePublic:
	default:
		return "https://www.teamfortress.com/";
	}
}


static CTFGCClientSystem s_TFGCClientSystem;
CTFGCClientSystem *GTFGCClientSystem() { return &s_TFGCClientSystem; }

static CTFPartyClient s_TFPartyClient;
CTFPartyClient *g_pTFPartyClient = nullptr;
CTFPartyClient* GTFPartyClient() { return g_pTFPartyClient; }

// Dialog Prompt Asking users if they want to rejoin a MvM Game
static CTFRejoinConfirmDialog *s_pRejoinLobbyDialog;


void SubscribeToLocalPlayerSOCache( ISharedObjectListener* pListener )
{
	if ( steamapicontext && steamapicontext->SteamUser() )
	{
		CSteamID steamID = steamapicontext->SteamUser()->GetSteamID();
		GCClientSystem()->GetGCClient()->AddSOCacheListener( steamID, pListener );
	}
	else
	{
		Assert( !"Failed to subscribe to local user's SOCache!" );
	}
}


//-----------------------------------------------------------------------------
// Constructor
//-----------------------------------------------------------------------------
CTFGCClientSystem::CTFGCClientSystem( void )
{
	// replace base GCClientSystem
	SetGCClientSystem( this );

	s_pRejoinLobbyDialog = NULL;

}

//-----------------------------------------------------------------------------
// Destructor
//-----------------------------------------------------------------------------
CTFGCClientSystem::~CTFGCClientSystem( void )
{
	// Prevent other system from using this pointer after it's destroyed
	SetGCClientSystem( NULL );
}


bool CTFGCClientSystem::Init()
{
	ListenForGameEvent( "client_disconnect" );
	ListenForGameEvent( "client_beginconnect" );
	ListenForGameEvent( "server_spawn" );

	// Let SDR know that we will likely want access to the relay network, so we're more
	// likely to have initial ping data to the clusters ready by the time we ask for it
	if ( SteamNetworkingUtils() )
		{ SteamNetworkingUtils()->InitRelayNetworkAccess(); }


	return true;
}


void CTFGCClientSystem::PostInit()
{
	BaseClass::PostInit();
}

//-----------------------------------------------------------------------------
void CTFGCClientSystem::PreInitGC()
{
	if ( !m_bRegisteredSharedObjects )
	{
//		REG_SHARED_OBJECT_SUBCLASS( CTFGameAccountClient );
		REG_SHARED_OBJECT_SUBCLASS( CTFParty );
		REG_SHARED_OBJECT_SUBCLASS( CTFGSLobby );
//		REG_SHARED_OBJECT_SUBCLASS( CTFBetaParticipation );
		REG_SHARED_OBJECT_SUBCLASS( CTFPartyInvite );
		REG_SHARED_OBJECT_SUBCLASS( CTFLobbyInvite );
		REG_SHARED_OBJECT_SUBCLASS( CQuestMapNode );
		REG_SHARED_OBJECT_SUBCLASS( CTFRatingData );
		REG_SHARED_OBJECT_SUBCLASS( CQuestMapRewardPurchase );

		m_bRegisteredSharedObjects = true;
	}
}


//-----------------------------------------------------------------------------
void CTFGCClientSystem::PostInitGC()
{
	GCMatchmakingDebugSpew( 1, "CTFGCClientSystem::PostInitGC\n" );

	if ( steamapicontext && steamapicontext->SteamUser() )
	{
		GCMatchmakingDebugSpew( 1, "CTFGCClientSystem - adding listener\n" );

		CSteamID steamID = steamapicontext->SteamUser()->GetSteamID();
		GCClientSystem()->FindOrAddSOCache( steamID )->AddListener( this );
	}
	else
	{
		Warning( "CTFGCClientSystem - couldn't add listener because Steam wasn't ready\n" );
	}

}


//-----------------------------------------------------------------------------
void CTFGCClientSystem::LevelShutdownPostEntity()
{
	BaseClass::LevelShutdownPostEntity();
}

//-----------------------------------------------------------------------------
void CTFGCClientSystem::LevelInitPreEntity()
{
	BaseClass::LevelInitPreEntity();
}

//-----------------------------------------------------------------------------
void CTFGCClientSystem::Shutdown()
{
	GCMatchmakingDebugSpew( 1, "CTFGCClientSystem::ShutdownGC\n" );

	if ( steamapicontext && steamapicontext->SteamUser() )
	{
		GCMatchmakingDebugSpew( 1, "CTFGCClientSystem - adding listener\n" );

		CSteamID steamID = steamapicontext->SteamUser()->GetSteamID();
		GCSDK::CGCClientSharedObjectCache	*pSOCache = GCClientSystem()->GetSOCache( steamID );
		if ( pSOCache )
		{
			pSOCache->RemoveListener( this );
		}
	}
	else
	{
		Warning( "CTFGCClientSystem - couldn't add listener because Steam wasn't ready\n" );
	}

	// Write any unsaved item changes to disk.
	TFOfflineInventory().Shutdown();

	BaseClass::Shutdown();
}

//-----------------------------------------------------------------------------
void CTFGCClientSystem::FireGameEvent( IGameEvent *event )
{
	const char *pEventName = event->GetName();
	// Disconnected from gameserver
	if ( !Q_stricmp( pEventName, "client_disconnect" ) )
	{
		m_steamIDCurrentServer.Clear();
		m_eConnectState = eConnectState_Disconnected;

		// We treat the gameserver as authoritative when we are connected to it -- disconnecting from it may change what
		// we believe about still having a live match. (e.g. if we have no pLobby anymore, but were loading into a live
		// match so didn't get the m_bAssignedMatchEnded update, we need to reconsider if we think the match is live)
		if ( UpdateAssignedLobby() )
		{
			// Fire this regardless of if our assigned lobby changed, since other SO fields did change.
			FireGameEventLobbyUpdated();
		}
		return;
	}

	// Started attempting connection to gameserver
	if ( !Q_stricmp( pEventName, "client_beginconnect" ) )
	{
		Assert( IsConnectStateDisconnected() );

		// TODO does the retry command set this source? It should go through ::ConnectToServer
		{
			m_eConnectState = eConnectState_NonmatchmadeServer;
		}
		return;
	}

	// Successfully connected to a gameserver. For MM purposes, we stay in state connecting until server spawn as that
	// ensures there's no intermediate "loading into some server but we're not sure of its steamid yet" state.
	if ( !Q_strcmp( pEventName, "server_spawn" ) )
	{
		GCMatchmakingDebugSpew( 4, "Client reached server_spawn.\n" );
		switch ( m_eConnectState )
		{
			default:
				AssertMsg1( false, "Unknown connect state %d", m_eConnectState );
			// These two can happen when doing weird things with timedemo or listen servers
			case eConnectState_Disconnected:
				m_eConnectState = eConnectState_NonmatchmadeServer;
				GCMatchmakingDebugSpew( 4, "Client connected to non-matchmade.\n" );
				break;

			case eConnectState_ConnectingToMatchmade:
				m_eConnectState = eConnectState_ConnectedToMatchmade;
				GCMatchmakingDebugSpew( 4, "Client connected to matchmade.\n" );
				break;

			case eConnectState_ConnectedToMatchmade:
				break;

			case eConnectState_NonmatchmadeServer:
				break;
		}
		m_steamIDCurrentServer.Clear();
		if ( steamapicontext && steamapicontext->SteamUser() && steamapicontext->SteamUtils() )
		{
			m_steamIDCurrentServer.SetFromString( event->GetString( "steamid", "" ), GetUniverse() );
			GCMatchmakingDebugSpew( 4, "Recognizing MM server id %s\n", m_steamIDCurrentServer.Render() );
		}

		if ( m_eConnectState == eConnectState_ConnectedToMatchmade && !m_steamIDCurrentServer.IsValid() )
		{
			Warning( "Connected to MM server but no GS steamid is set.\n" );
		}

		return;
	}

}

//-----------------------------------------------------------------------------
void CTFGCClientSystem::InvalidatePingData()
{
}


//-----------------------------------------------------------------------------
// Offline inventory
//
// Our items come from a save file on this machine (see tf_offline_inventory.h)
// rather than from Valve's servers. Whenever we're in a game, we send the
// server the items we have equipped, and send them again whenever that changes.
//-----------------------------------------------------------------------------
void CTFGCClientSystem::OfflineInventoryThink()
{
	TFOfflineInventory().Think();

	switch ( m_eOfflineInventoryState )
	{
	case kOfflineInventoryState_Loading:
		if ( !TFOfflineInventory().IsLoaded() )
			return;

		m_eOfflineInventoryState = kOfflineInventoryState_Idle;
		// fallthrough

	case kOfflineInventoryState_Idle:
		// Wait to get into a game.
		if ( !engine->IsConnected() )
			return;

		m_eOfflineInventoryState = kOfflineInventoryState_SendToServer;
		// fallthrough

	case kOfflineInventoryState_SendToServer:
	{
		if ( !engine->IsConnected() )
		{
			m_eOfflineInventoryState = kOfflineInventoryState_Idle;
			return;
		}

		// Wait until we are fully loaded to send the server our items.
		if ( !engine->IsInGame() || !SteamUser() )
			return;

		CGCClientSharedObjectCache *pSOCache = GetSOCache( SteamUser()->GetSteamID() );
		if ( !pSOCache )
			return;

		KeyValues *kv = new KeyValues( "sdk_inventory" );
		TFOfflineInventory().WriteEquippedItemsForServer( kv );

		// Which item goes in which slot for each class.
		SDK_AddServerInventoryInfo( kv, pSOCache );

		// Real TF2 respawns you in the spawn room when you change loadout; let the server do the same.
		extern ConVar tf_respawn_on_loadoutchanges;
		kv->SetBool( "respawn_on_change", tf_respawn_on_loadoutchanges.GetBool() );

		engine->ServerCmdKeyValues( kv );

		m_bLocalChangesApplied = false;
		m_eOfflineInventoryState = kOfflineInventoryState_SentToServer;
		break;
	}

	case kOfflineInventoryState_SentToServer:
		if ( !engine->IsConnected() )
		{
			m_eOfflineInventoryState = kOfflineInventoryState_Idle;
		}
		else if ( m_bLocalChangesApplied )
		{
			m_eOfflineInventoryState = kOfflineInventoryState_SendToServer;
		}
		break;
	}
}

void CTFGCClientSystem::ServerRequestEquipment()
{
	// The server lost track of our items (or never got them). Send them again.
	if ( m_eOfflineInventoryState == kOfflineInventoryState_SentToServer )
	{
		m_eOfflineInventoryState = kOfflineInventoryState_SendToServer;
	}
}

void CTFGCClientSystem::LocalInventoryChanged()
{
	m_bLocalChangesApplied = true;
}

void CTFGCClientSystem::SDK_AddServerInventoryInfo( KeyValues* pKV, CGCClientSharedObjectCache* pSOCache )
{
	// Here is where we would tell the DS which items we have equipped, along with any other info it needs to correctly update the socache

	CGCClientSharedObjectTypeCache* pItemCache = pSOCache->FindTypeCache(CEconItem::k_nTypeID);
	if (!pItemCache)
		return;

	CTFPlayerInventory *pLocalInv = dynamic_cast<CTFPlayerInventory*>(TFInventoryManager()->GetLocalInventory());
	if (pLocalInv == NULL)
		return;

	// Extract our current loadout information and record it in the key values.
	KeyValues *pLoadoutKV = new KeyValues("local_loadout");
	for (int iClass = TF_FIRST_NORMAL_CLASS; iClass < TF_LAST_NORMAL_CLASS; iClass++)
	{
		char szClass[256];
		V_snprintf(szClass, sizeof(szClass), "%i", iClass);

		KeyValues *pClassKV = new KeyValues(szClass);
		pLoadoutKV->AddSubKey(pClassKV);

		for (int iSlot = 0; iSlot < CLASS_LOADOUT_POSITION_COUNT; ++iSlot)
		{
			CEconItemView* pItemView = TFInventoryManager()->GetItemInLoadoutForClass(iClass, iSlot);
			if (!pItemView)
				continue;

			// todo: investigate why we get zeroes here...?
			if (pItemView->GetID() == INVALID_ITEM_ID || pItemView->GetID() == 0)
				continue;

			char szSlot[256];
			V_snprintf(szSlot, sizeof(szSlot), "%i", iSlot);

			pClassKV->SetUint64(szSlot, pItemView->GetID());
		}
	}
	pKV->AddSubKey(pLoadoutKV);

}

//-----------------------------------------------------------------------------
void CTFGCClientSystem::Update( float frametime )
{
	BaseClass::Update( frametime );


	OfflineInventoryThink();



	FOR_EACH_VEC_BACK( m_vecDelayedLocalPlayerSOListenersToAdd, i )
	{
		SubscribeToLocalPlayerSOCache( m_vecDelayedLocalPlayerSOListenersToAdd[ i ] );
		m_vecDelayedLocalPlayerSOListenersToAdd.Remove( i );
	}
}

void CTFGCClientSystem::SOCacheSubscribed( const CSteamID & steamIDOwner, GCSDK::ESOCacheEvent eEvent )
{
	if ( steamIDOwner == ClientSteamContext().GetLocalPlayerSteamID() )
	{
		// Assert( m_pSOCache == NULL ); // we *can* get two SOCacheSubscribed calls in a row.
		m_pSOCache = GCClientSystem()->GetSOCache( steamIDOwner );
		Assert( m_pSOCache != NULL );

		if ( gameeventmanager )
		{

			// force a lobby update whenever our SO cache arrives
			FireGameEventLobbyUpdated();
		}
	}
}

void CTFGCClientSystem::FireGameEventLobbyUpdated()
{
	IGameEvent *event2 = gameeventmanager->CreateEvent( "lobby_updated" );
	if ( event2 )
	{
		gameeventmanager->FireEventClientSide( event2 );
	}
}

void CTFGCClientSystem::SOCreated( const CSteamID & steamIDOwner, const GCSDK::CSharedObject *pObject, GCSDK::ESOCacheEvent eEvent ) { SOChanged( pObject, SOChanged_Create, eEvent ); }
void CTFGCClientSystem::SOUpdated( const CSteamID & steamIDOwner, const GCSDK::CSharedObject *pObject, GCSDK::ESOCacheEvent eEvent ) { SOChanged( pObject, SOChanged_Update, eEvent ); }
void CTFGCClientSystem::SODestroyed( const CSteamID & steamIDOwner, const GCSDK::CSharedObject *pObject, GCSDK::ESOCacheEvent eEvent ) { SOChanged( pObject, SOChanged_Destroy, eEvent ); }

void CTFGCClientSystem::SOChanged( const GCSDK::CSharedObject *pObject, SOChangeType_t changeType, GCSDK::ESOCacheEvent eEvent )
{
	// (CTFParty objects are handled by CTFPartyClient)
	if ( pObject->GetTypeID() == CTFGSLobby::k_nTypeID )
	{
		#if GCMATCHMAKING_DEBUG_LEVEL > 0
			switch ( changeType )
			{
				case SOChanged_Create: GCMatchmakingDebugSpew( 1, "Lobby created\n"); break;
				case SOChanged_Update: GCMatchmakingDebugSpew( 2, "Lobby updated\n"); break;
				case SOChanged_Destroy: GCMatchmakingDebugSpew( 1, "Lobby destroyed\n"); break;
				default: AssertMsg1( false, "Bogus change type %d", changeType );
			}
		#endif

		UpdateAssignedLobby();
		// Fire this regardless of if our assigned lobby changed, since other SO fields did change.
		FireGameEventLobbyUpdated();
	}
	// Notifications. Sync/add/delete with what's in our notification drawer
	else if ( pObject->GetTypeID() == CTFNotification::k_nTypeID )
	{
		const CTFNotification* pSONotification = ( const CTFNotification* )( pObject );
		Msg( "Notification %llu %s: \"%s\"\n",
		     (unsigned long long) pSONotification->Obj().notification_id(),
		     changeType == SOChanged_Create ? "created" : changeType == SOChanged_Destroy ? "destroyed" : "updated",
		     pSONotification->Obj().notification_string().c_str() );

		// Update existing notification if found
		bool bFound = false;
		for ( int i = NotificationQueue_GetNumNotifications() - 1; i >= 0; --i )
		{
			CClientNotification *pNotif = dynamic_cast<CClientNotification *>(NotificationQueue_GetByIndex( i ));
			if ( pNotif && pNotif->NotificationID() == pSONotification->Obj().notification_id() )
			{
				Msg( "Notification %llu already displayed, updating\n",
				     (unsigned long long) pSONotification->Obj().notification_id() );
				bFound = true;
				if ( changeType == SOChanged_Destroy )
				{
					NotificationQueue_Remove( pNotif );
				}
				else
				{
					pNotif->Update( pSONotification );
				}
			}
		}

		// Add them to our notifications drawer if not
		if ( !bFound && changeType != SOChanged_Destroy )
		{
			Msg( "New notification %llu arrived: \"%s\"\n",
			     (unsigned long long) pSONotification->Obj().notification_id(),
			     pSONotification->Obj().notification_string().c_str() );
			CClientNotification *pClientNotification = new CClientNotification();
			pClientNotification->Update( pSONotification );
			NotificationQueue_Add( pClientNotification );
		}
	}
	else if ( pObject->GetTypeID() == CTFLobbyInvite::k_nTypeID )
	{
		OnMatchInvitesUpdated();
	}

}

bool CTFGCClientSystem::UpdateAssignedLobby()
{
	return false;
}

CTFParty* CTFGCClientSystem::GetParty()
{
	return NULL;
}

CTFGSLobby* CTFGCClientSystem::GetLobby() const
{
	return NULL;
}


//-----------------------------------------------------------------------------

bool ForceCompetitiveConvars()
{

	bool anyFailures = false;

	Assert( ThreadInMainThread() );
	for ( ConCommandBase *ccb = g_pCVar->GetCommands(); ccb; ccb = ccb->GetNext() )
	{
		if ( ccb->IsCommand() )
			continue;

		ConVar *pVar = ( ConVar * ) ccb;

		if ( !pVar->IsCompetitiveRestricted() )
			continue;

		// Hack: This var is created by the dxconfig system, but it doesn't actually exist.
		// Skip it so we have no vars change when running a clean config.
		if ( V_stricmp( pVar->GetName(), "r_decal_cullsize" ) == 0 )
			continue;
		
		if ( !pVar->SetCompetitiveMode( true ) )
			anyFailures = true;
	}

	return !anyFailures;
}

void CTFGCClientSystem::ConnectToServer( const char *connect )
{

	Msg("Connecting to %s\n", connect );
	CUtlString connectCmd;
	connectCmd.Format( "connect %s matchmaking", connect );
	if ( engine )
	{
		const bool k_bUseCompetitiveConvars = false;

		bool bAllowed = !k_bUseCompetitiveConvars || ForceCompetitiveConvars();
		if ( !bAllowed )
		{
			// ForceCompetitiveConvars() shouldn't fail
			Assert( 0 );
		}

		engine->ClientCmd_Unrestricted( connectCmd.String() );
		//vgui::surface()->PlaySound( "ui/ui_findmatch_join_01.wav" );
	}
	else
	{
		Warning( "Failed to reconnect to game server as engine wasn't ready\n" );
	}
}
void CTFGCClientSystem::SetWorldStatus( CMsgTFWorldStatus &status )
{
	if ( !BMessagesAreEqual( &m_WorldStatus, &status ) )
	{
		m_WorldStatus = status;

		uint32_t nEngineVer = engine->GetClientVersion();
		uint32_t nWorldVer = status.active_client_version();
		// World version 0 is dev-universe for disabled checking
		bool bOutOfDate = ( nWorldVer > 0 && nEngineVer != nWorldVer );
		if ( !m_bClientOutOfDate && bOutOfDate )
			{ Msg( "Client out of date -- world version %u, current version %u\n", nWorldVer, nEngineVer ); }
		m_bClientOutOfDate = bOutOfDate;

		IGameEvent *pEvent = gameeventmanager->CreateEvent( "world_status_changed" );
		if ( pEvent )
		{
			gameeventmanager->FireEventClientSide( pEvent );
		}
	}
}

bool CTFGCClientSystem::BLocalPlayerInventoryHasMvmTicket( void )
{
	CPlayerInventory *pLocalInv = TFInventoryManager()->GetLocalInventory();
	if ( pLocalInv == NULL )
		return false;

	static CSchemaItemDefHandle pItemDef_MvmTicket( CTFItemSchema::k_rchMvMTicketItemDefName );
	if ( !pItemDef_MvmTicket )
		return false;

	for ( int i = 0 ; i < pLocalInv->GetItemCount() ; ++i )
	{
		CEconItemView *pItem = pLocalInv->GetItem( i );
		Assert( pItem );
		if ( pItem->GetItemDefinition() == pItemDef_MvmTicket )
			return true;
	}

	return false;
}

int CTFGCClientSystem::GetLocalPlayerInventoryMvmTicketCount( void )
{
	int nCount = 0;

	CPlayerInventory *pLocalInv = TFInventoryManager()->GetLocalInventory();
	if ( pLocalInv )
	{
		static CSchemaItemDefHandle pItemDef_MvmTicket( CTFItemSchema::k_rchMvMTicketItemDefName );
		if ( pItemDef_MvmTicket )
		{
			for ( int i = 0 ; i < pLocalInv->GetItemCount() ; ++i )
			{
				CEconItemView *pItem = pLocalInv->GetItem( i );
				Assert( pItem );
				if ( pItem->GetItemDefinition() == pItemDef_MvmTicket )
				{
					nCount++;
				}
			}
		}
	}

	return nCount;
}

bool CTFGCClientSystem::BLocalPlayerInventoryHasSquadSurplusVoucher( void )
{
	CPlayerInventory *pLocalInv = TFInventoryManager()->GetLocalInventory();
	if ( pLocalInv == NULL )
		return false;

	static CSchemaItemDefHandle k_rchMvMSquadSurplusVoucherItemDefName( CTFItemSchema::k_rchMvMSquadSurplusVoucherItemDefName );
	if ( !k_rchMvMSquadSurplusVoucherItemDefName )
		return false;

	for ( int i = 0 ; i < pLocalInv->GetItemCount() ; ++i )
	{
		CEconItemView *pItem = pLocalInv->GetItem( i );
		Assert( pItem );
		if ( pItem->GetItemDefinition() == k_rchMvMSquadSurplusVoucherItemDefName )
			return true;
	}

	return false;
}

int CTFGCClientSystem::GetLocalPlayerInventorySquadSurplusVoucherCount( void )
{
	int nCount = 0;

	CPlayerInventory *pLocalInv = TFInventoryManager()->GetLocalInventory();
	if ( pLocalInv )
	{
		static CSchemaItemDefHandle k_rchMvMSquadSurplusVoucherItemDefName( CTFItemSchema::k_rchMvMSquadSurplusVoucherItemDefName );
		if ( k_rchMvMSquadSurplusVoucherItemDefName )
		{
			for ( int i = 0 ; i < pLocalInv->GetItemCount() ; ++i )
			{
				CEconItemView *pItem = pLocalInv->GetItem( i );
				Assert( pItem );
				if ( pItem->GetItemDefinition() == k_rchMvMSquadSurplusVoucherItemDefName )
				{
					nCount++;
				}
			}
		}
	}

	return nCount;
}

bool CTFGCClientSystem::BIsBannedFromMatchmaking( EMMPenaltyPool ePool, CRTime* prtExpireTime /* = NULL */, int* pnDuration /* = NULL */ )
{
	return false;
}


#ifdef USE_MVM_TOUR
bool CTFGCClientSystem::BGetLocalPlayerBadgeInfoForTour( int iTourIndex, uint32 *pnBadgeLevel, uint32 *pnCompletedChallenges )
{
	Assert( iTourIndex >= 0 );
	Assert( iTourIndex < GetItemSchema()->GetMvmTours().Count() );
	Assert( pnBadgeLevel );
	Assert( pnCompletedChallenges );

	*pnBadgeLevel = 0;
	*pnCompletedChallenges = 0;

	CPlayerInventory *pLocalInv = TFInventoryManager()->GetLocalInventory();
	if ( pLocalInv == NULL )
		return false;

	// We can't search for a badge without knowing which attribute to look for.
	static CSchemaAttributeDefHandle pAttribDef_MvmChallengeCompleted( CTFItemSchema::k_rchMvMChallengeCompletedMaskAttribName );
	Assert( pAttribDef_MvmChallengeCompleted );
	if ( !pAttribDef_MvmChallengeCompleted )
		return false;

	if ( iTourIndex < 0 || iTourIndex >= GetItemSchema()->GetMvmTours().Count() )
	{
		AssertMsg1( false, "Invalid tour index %d", iTourIndex );
		return false;
	}
	const CEconItemDefinition *pBadgeDef = GetItemSchema()->GetMvmTours()[iTourIndex].m_pBadgeItemDef;
	if ( pBadgeDef == NULL )
	{
		Assert( pBadgeDef );
		return false;
	}

	for ( int i = 0 ; i < pLocalInv->GetItemCount() ; ++i )
	{
		CEconItemView *pBadge = pLocalInv->GetItem( i );
		Assert( pBadge );
		if ( pBadge->GetItemDefinition() != pBadgeDef )
			continue;

		if ( !pBadge->FindAttribute( pAttribDef_MvmChallengeCompleted, pnCompletedChallenges ) )
		{
			AssertMsg( false, "Badge missing challenges completed attribute?" );
			*pnCompletedChallenges = 0;
		}

		extern uint32 GetItemDescriptionDisplayLevel( const IEconItemInterface *pEconItem );
		*pnBadgeLevel = GetItemDescriptionDisplayLevel( pBadge );
		return true;
	}

	return false;
}

#endif // USE_MVM_TOUR

void CTFGCClientSystem::AbandonCurrentMatch()
{
	Msg( "Sending request to abandon current match\n" );

	if ( BConnectedToMatchServer( true ) )
	{
		// If we were connected to said match, then disconnect
		Msg( "Disconnecting from abandoned match server\n" );
		engine->ClientCmd_Unrestricted( "disconnect" );
	}
}

bool CTFGCClientSystem::IsConnectStateDisconnected()
{
	return ( m_eConnectState != eConnectState_ConnectingToMatchmade &&
	         m_eConnectState != eConnectState_ConnectedToMatchmade );
}

//-----------------------------------------------------------------------------
// Purpose: Ask the GC for the latest global casual criteria stats
//-----------------------------------------------------------------------------
void CTFGCClientSystem::RequestMatchMakerStats() const
{
}

//-----------------------------------------------------------------------------
// Purpose: Set our cached global casual criteria stats and figure out the most
//			popular map so we can do some health computations later.
//-----------------------------------------------------------------------------
void CTFGCClientSystem::SetMatchMakerStats( const CMsgGCMatchMakerStatsResponse newStats )
{
}

//-----------------------------------------------------------------------------
// Purpose: Given a health ratio, get the health data
//-----------------------------------------------------------------------------
CTFGCClientSystem::MatchMakerHealthData_t CTFGCClientSystem::GetHealthBracketForRatio( float flRatio ) const
{
	CTFGCClientSystem::MatchMakerHealthData_t data;
	data.m_flRatio = flRatio;

	static const Color colorBad( 128, 128, 128, 60 );
	static const Color colorOK( 188, 112, 0, 128 );
	static const Color colorGood( 94, 150, 49, 255 );

	// Walk through our brackets and fine where we fall and setup data accordingly
	if ( flRatio < 0.3f )
	{
		data.m_colorBar = LerpColor( colorBad, colorOK, RemapValClamped( flRatio, 0.2f, 0.3f, 0.f, 1.f ) );
		data.m_strLocToken = "TF_Casual_QueueEstimation_Bad";
	}
	else if ( flRatio < 0.7f )
	{
		data.m_colorBar = LerpColor( colorOK, colorGood, RemapValClamped( flRatio, 0.3f, 0.7f, 0.f, 1.f ) );
		data.m_strLocToken = "TF_Casual_QueueEstimation_OK";
	}
	else
	{
		data.m_colorBar = colorGood;
		data.m_strLocToken = "TF_Casual_QueueEstimation_Good";
	}

	return data;
}


//-----------------------------------------------------------------------------
// Purpose: Really here just so we can shortcircuit some staging_only debug
//-----------------------------------------------------------------------------
inline uint32 GetCountForMap( const CMsgGCMatchMakerStatsResponse msg, int nIndex )
{

	return msg.map_count( nIndex );
}

//-----------------------------------------------------------------------------
// Purpose: Get the overall health of the current local casual criteria.
//			Currently just takes the best individual map health.
//-----------------------------------------------------------------------------
// TODO(Universal Parties): Unused?
CTFGCClientSystem::MatchMakerHealthData_t CTFGCClientSystem::GetOverallHealthDataForLocalCriteria() const
{
	uint32 nMostSearchedCount = m_nMostSearchedMapCount;
	uint32 nLargestOfSelected = 0;
	CCasualCriteriaHelper helper = GTFPartyClient()->GetLocalGroupCriteria().GetCasualCriteriaHelper();


	// No data -- we assume bad
	if ( nMostSearchedCount == 0 )
		return GetHealthBracketForRatio( 0.f );

	// Go through all the locallty selected maps and find the one with the best health.
	// Use that to get our estimated overall criteria health.
	for( int i=0; i < m_MatchMakerStats.map_count_size(); ++i )
	{
		if ( helper.IsMapSelected( i ) )
		{
			nLargestOfSelected = Max( nLargestOfSelected, GetCountForMap( m_MatchMakerStats, i ) );
		}
	}

	return GetHealthBracketForRatio( (float)nLargestOfSelected / (float)nMostSearchedCount );
}

//-----------------------------------------------------------------------------
// Purpose: Gets the health of a given map
//-----------------------------------------------------------------------------
CTFGCClientSystem::MatchMakerHealthData_t CTFGCClientSystem::GetHealthDataForMap( uint32 nMapIndex ) const
{
	uint32 nMostSearchedCount = m_nMostSearchedMapCount;
	uint32 nLargestOfSelected = 0;

	// No data -- we assume bad
	if ( nMostSearchedCount == 0 )
		return GetHealthBracketForRatio( 0.f );

	if ( (int)nMapIndex < m_MatchMakerStats.map_count_size() )
	{
		nLargestOfSelected = GetCountForMap( m_MatchMakerStats, nMapIndex );
	}

	return GetHealthBracketForRatio( (float)nLargestOfSelected / (float)nMostSearchedCount );
}

//-----------------------------------------------------------------------------
// Purpose: Sends an xp acknowledge via k_EMsgGC_AcknowledgeXP to the GC if
//			we have any outstanding xp sources or a mismatch in our last
//			acknowledged xp and our current one.
//-----------------------------------------------------------------------------
void CTFGCClientSystem::AcknowledgePendingRatingAndSources( ETFMatchGroup eMatchGroup )
{
}

void CTFGCClientSystem::AcknowledgeNotification( uint32 nAccountID, uint64 ulNotificationID )
{
}

//-----------------------------------------------------------------------------
// Purpose: Hang onto the survey request
//-----------------------------------------------------------------------------
void CTFGCClientSystem::SetSurveyRequest( const CMsgGCSurveyRequest& msgSurveyRequest )
{
	m_msgSurveyRequest = msgSurveyRequest;
}

//-----------------------------------------------------------------------------
// Purpose: Send survey response and clear the stored survey request
//-----------------------------------------------------------------------------
void CTFGCClientSystem::SendSurveyResponse( int32 nResponse )
{
}

void CTFGCClientSystem::ClearSurveyRequest()
{
	m_msgSurveyRequest.Clear();
}

void CTFGCClientSystem::AddLocalPlayerSOListener( ISharedObjectListener* pListener, bool bImmediately )
{
	if ( bImmediately )
	{
		SubscribeToLocalPlayerSOCache( pListener );
	}
	else
	{
		m_vecDelayedLocalPlayerSOListenersToAdd.AddToTail( pListener );
	}
}

void CTFGCClientSystem::RemoveLocalPlayerSOListener( ISharedObjectListener* pListener )
{
	// Remove if it was a delayed add
	auto idx = m_vecDelayedLocalPlayerSOListenersToAdd.Find( pListener );
	if ( idx != m_vecDelayedLocalPlayerSOListenersToAdd.InvalidIndex() )
	{
		m_vecDelayedLocalPlayerSOListenersToAdd.Remove( idx );
	}

	if ( steamapicontext && steamapicontext->SteamUser() )
	{
		CSteamID steamID = steamapicontext->SteamUser()->GetSteamID();
		GetGCClient()->RemoveSOCacheListener( steamID, pListener );
	}
}

bool CTFGCClientSystem::BConnectedToMatchServer( bool bLiveMatch )
{
	return false;
}

bool CTFGCClientSystem::BHaveRunningMatch() const
{
	return false;
}

bool CTFGCClientSystem::BHaveLiveMatch() const
{
	return false;
}

EAbandonGameStatus CTFGCClientSystem::GetAssignedMatchAbandonStatus()
{
	return k_EAbandonGameStatus_Safe;
}

ETFMatchGroup CTFGCClientSystem::GetLiveMatchGroup() const
{

	return k_eTFMatchGroup_Invalid;
}

void CTFGCClientSystem::JoinMMMatch()
{
}

//-----------------------------------------------------------------------------
CTFGCClientSystem::MatchInvite_t CTFGCClientSystem::GetMatchInvite( int idx ) const
{
	auto *pTypeCache = m_pSOCache ? m_pSOCache->FindBaseTypeCache( CTFLobbyInvite::k_nTypeID ) : nullptr;
	int nInvites = pTypeCache ? pTypeCache->GetCount() : 0;

	if ( idx < 0 || idx >= nInvites )
	{
		AssertMsg( false, "Invalid index passed to GetMatchInvite" );
		return MatchInvite_t{ 0u, k_eTFMatchGroup_Invalid, false };
	}

	CTFLobbyInvite *pInvite = static_cast< CTFLobbyInvite * >( pTypeCache->GetObject( idx ) );
	PlayerGroupID_t nLobbyID = pInvite->GetGroupID();
	ETFMatchGroup eMatchGroup = pInvite->GetMatchGroup();
	bool bAcceptingThisMsg = ( m_nAcceptingMatchLobbyID == nLobbyID );
	return MatchInvite_t{ nLobbyID, eMatchGroup, bAcceptingThisMsg };
}

//-----------------------------------------------------------------------------
int CTFGCClientSystem::GetMatchInviteIdxByLobbyID( PlayerGroupID_t nLobbyID ) const
{
	int nNumInvites = GetNumMatchInvites();
	for( int idxInvite = 0; idxInvite < nNumInvites; ++idxInvite )
	{
		auto invite = GetMatchInvite( idxInvite );
		if ( invite.nLobbyID == nLobbyID )
			{ return idxInvite; }
	}

	return -1;
}

//-----------------------------------------------------------------------------
int CTFGCClientSystem::GetNumMatchInvites() const
{
	if ( !m_pSOCache )
		{ return 0; }

	CSharedObjectTypeCache *pTypeCache = m_pSOCache->FindBaseTypeCache( CTFLobbyInvite::k_nTypeID );
	return pTypeCache ? pTypeCache->GetCount() : 0;
}

//-----------------------------------------------------------------------------
void CTFGCClientSystem::RequestAcceptMatchInvite( PlayerGroupID_t nLobbyID )
{
}

//-----------------------------------------------------------------------------
bool CTFGCClientSystem::BIsMatchGroupDisabled( ETFMatchGroup eMatchGroup ) const
{
	for ( int i = 0; i < WorldStatus().disabled_match_groups_size(); i++ )
	{
		if ( WorldStatus().disabled_match_groups( i ) == eMatchGroup )
		{
			return true;
		}
	}

	return false;
}

//-----------------------------------------------------------------------------
void CTFGCClientSystem::OnAcceptLobbyReply( PlayerGroupID_t )
{
	Assert( m_nAcceptingMatchLobbyID != 0u );
	m_nAcceptingMatchLobbyID = 0u;
	// Update invites with whatever happened (the invite will be gone, or we will have failed and they'll want to see
	// that bAccepting went back to false)
	//
	// We also suspend updating invites while an accept is in flight.
	OnMatchInvitesUpdated();
}

//-----------------------------------------------------------------------------
void CTFGCClientSystem::OnMatchInvitesUpdated()
{
	// Suppress while accept message in flight, OnAcceptLobbyReply will fire
	if ( m_nAcceptingMatchLobbyID )
		{ return; }

	IGameEvent *event = gameeventmanager->CreateEvent( "match_invites_updated" );
	if ( event )
		{ gameeventmanager->FireEventClientSide( event ); }
}

void CTFGCClientSystem::LeaveGameAndPrepareToJoinParty( GCSDK::PlayerGroupID_t nPartyID )
{
	// Leave current server
	engine->ClientCmd_Unrestricted( "disconnect" );
}

bool CTFGCClientSystem::BIsPhoneVerified( void )
{
	return false;
}

bool CTFGCClientSystem::BIsPhoneIdentifying( void )
{
	return false;
}

bool CTFGCClientSystem::BHasCompetitiveAccess( void )
{
	return false;
}


