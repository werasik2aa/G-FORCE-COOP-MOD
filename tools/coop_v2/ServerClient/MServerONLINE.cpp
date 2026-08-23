#include "MServerONLINE.h"

#include "SteamManager.h"

CSteamOnlineSocketServer::CSteamOnlineSocketServer()
{
}

CSteamOnlineSocketServer::~CSteamOnlineSocketServer()
{
	CloseServer();
}

bool CSteamOnlineSocketServer::OpenListenSocket(std::uint32_t port)
{
	return OpenListenSocketP2P(port);
}

bool CSteamOnlineSocketServer::OpenListenSocketP2P(std::uint32_t port)
{
	if (IsSteamSocketOpen())
		return true;
	if (!SteamManager || !SteamManager->IsSteamInitialized())
	{
		Msg("[network-steam] Steam unavailable; P2P server not started");
		return false;
	}

	m_sockets = SteamNetworkingSockets_SteamAPI();
	ISteamNetworkingUtils* utils = SteamNetworkingUtils_SteamAPI();
	if (!m_sockets || !utils)
		return false;
	utils->InitRelayNetworkAccess();

	SteamNetworkingConfigValue_t callback;
	callback.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
		reinterpret_cast<void*>(OnlineServerCallback));
	m_listen_socket = m_sockets->CreateListenSocketP2P(port, 1, &callback);
	if (m_listen_socket == k_HSteamListenSocket_Invalid)
	{
		Msg("[network-error] Steam P2P listen failed on virtual port %u", port);
		return false;
	}
	m_poll_group = m_sockets->CreatePollGroup();
	if (m_poll_group == k_HSteamNetPollGroup_Invalid)
	{
		m_sockets->CloseListenSocket(m_listen_socket);
		m_listen_socket = k_HSteamListenSocket_Invalid;
		return false;
	}
	m_p2p = true;

	if (SteamFriends())
	{
		SteamFriends()->SetRichPresence("status", "Hosting G-Force co-op");
		SteamFriends()->SetRichPresence("steam_display", "#Status_Hosting");
		const std::string connect = std::to_string(
			SteamManager->GetMySteamID().ConvertToUint64());
		SteamFriends()->SetRichPresence("connect", connect.c_str());
	}
	Msg("[network-server] Steam P2P listening on virtual port %u", port);
	return true;
}

void CSteamOnlineSocketServer::CloseServer()
{
	if (SteamFriends())
		SteamFriends()->ClearRichPresence();
	CSteamOfflineSocketServer::CloseServer();
}

void CSteamOnlineSocketServer::OnStateChange(
	SteamNetConnectionStatusChangedCallback_t* info)
{
	if (!info)
		return;
	if (info->m_info.m_eState ==
		k_ESteamNetworkingConnectionState_Connecting)
	{
		const CSteamID remote = info->m_info.m_identityRemote.GetSteamID();
		if (remote.IsValid() && SteamFriends() &&
			!SteamFriends()->HasFriend(remote, k_EFriendFlagImmediate))
		{
			CloseConnection(info->m_hConn, kCoopEndNotFriend,
				"Steam user is not in host friend list");
			Msg("[network-server] rejected non-friend Steam user %llu",
				remote.ConvertToUint64());
			return;
		}
	}
	CSteamOfflineSocketServer::OnStateChange(info);
}

void OnlineServerCallback(SteamNetConnectionStatusChangedCallback_t* info)
{
	if (SteamSServer)
		SteamSServer->OnStateChange(info);
}
