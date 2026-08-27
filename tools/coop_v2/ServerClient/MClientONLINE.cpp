#include "MClientONLINE.h"

#include "SteamManager.h"

CSteamOnlineSocketClient::CSteamOnlineSocketClient() :
	m_retry_pending(false),
	m_retry_attempts(0),
	m_retry_at_tick(0),
	kMaxConnectRetries(4),
	kConnectRetryDelayMs(2000)
{}

CSteamOnlineSocketClient::~CSteamOnlineSocketClient()
{
	Disconnect();
}

bool CSteamOnlineSocketClient::CreateConnection(const char* connection_ip)
{
	if (m_connection != k_HSteamNetConnection_Invalid)
		return true;
	if (!connection_ip || !connection_ip[0])
		return false;

	m_sockets = SteamNetworkingSockets_SteamAPI();
	ISteamNetworkingUtils* utils = SteamNetworkingUtils_SteamAPI();
	if (!m_sockets || !utils)
	{
		Msg("[network-error] Steam client sockets interface unavailable");
		return false;
	}

	SteamNetworkingIPAddr address;
	if (!address.ParseString(connection_ip))
	{
		Msg("[network-error] invalid Steam server address: %s", connection_ip);
		return false;
	}

	m_connection = m_sockets->ConnectByIPAddress(address, 0, nullptr);
	if (m_connection == k_HSteamNetConnection_Invalid)
		return false;
	m_last_connection = connection_ip;
	m_state = MConnectState::Connecting;
	Msg("[network-client] Steam IP connection created: %s", connection_ip);
	return true;
}

bool CSteamOnlineSocketClient::RecreateConnection()
{
	if (m_last_steam_id.IsValid() && m_last_connection.empty())
		return ConnectToFriend(m_last_steam_id, m_last_steam_port);
	return !m_last_connection.empty() &&
		CreateConnection(m_last_connection.c_str());
}

bool CSteamOnlineSocketClient::ConnectToFriend(
	CSteamID friend_id, std::uint32_t virtual_port)
{
	if (m_connection != k_HSteamNetConnection_Invalid)
		return true;
	if (!friend_id.IsValid())
		return false;

	m_sockets = SteamNetworkingSockets_SteamAPI();
	ISteamNetworkingUtils* utils = SteamNetworkingUtils_SteamAPI();
	if (!m_sockets || !utils)
		return false;
	utils->InitRelayNetworkAccess();

	SteamNetworkingIdentity identity;
	identity.Clear();
	identity.SetSteamID(friend_id);
	m_connection = m_sockets->ConnectP2P(identity, virtual_port, 0, nullptr);
	if (m_connection == k_HSteamNetConnection_Invalid)
	{
		Msg("[network-error] Steam P2P connection creation failed");
		return false;
	}

	m_last_connection.clear();
	m_last_steam_id = friend_id;
	m_last_steam_port = virtual_port;
	m_state = MConnectState::Connecting;
	Msg("[network-client] connecting to Steam friend %llu on virtual port %u "
		"(relay=%s)",
		friend_id.ConvertToUint64(), virtual_port, SteamRelayStatusName());
	return true;
}

void CSteamOnlineSocketClient::Disconnect(bool quit)
{
	// A deliberate disconnect cancels the retry, otherwise the pending timer would
	// resurrect a connection the caller just tore down.
	m_retry_pending = false;
	CSteamOfflineSocketClient::Disconnect(quit);
}

void CSteamOnlineSocketClient::ResetConnectRetries()
{
	m_retry_pending = false;
	m_retry_attempts = 0;
}

void CSteamOnlineSocketClient::ScheduleConnectRetry()
{
	if (!m_last_steam_id.IsValid())
		return;
	if (m_retry_attempts >= kMaxConnectRetries)
	{
		Msg("[network-client] Steam connect gave up after %u attempts; the peer "
			"has no open P2P socket (host must finish loading its save)",
			m_retry_attempts);
		return;
	}
	++m_retry_attempts;
	m_retry_at_tick = static_cast<std::uint32_t>(GetTickCount()) +
		kConnectRetryDelayMs;
	m_retry_pending = true;
	Msg("[network-client] Steam connect failed (attempt %u/%u, relay=%s); "
		"retrying in %u ms",
		m_retry_attempts, kMaxConnectRetries, SteamRelayStatusName(),
		kConnectRetryDelayMs);
}

void CSteamOnlineSocketClient::OnFrame()
{
	if (m_retry_pending)
	{
		const std::int32_t remaining = static_cast<std::int32_t>(
			m_retry_at_tick - static_cast<std::uint32_t>(GetTickCount()));
		if (remaining <= 0)
		{
			m_retry_pending = false;
			if (m_connection == k_HSteamNetConnection_Invalid)
				RecreateConnection();
		}
	}
	CSteamOfflineSocketClient::OnFrame();
}

void CSteamOnlineSocketClient::OnSteamStateChange(
	SteamNetConnectionStatusChangedCallback_t* info)
{
	// OnStateChange invalidates m_connection on failure, so both the ownership test
	// and the previous state have to be sampled before delegating to it.
	const bool ours = info && info->m_hConn == m_connection &&
		m_connection != k_HSteamNetConnection_Invalid;
	const bool was_connecting = m_state == MConnectState::Connecting;

	OnStateChange(info);
	if (!info)
		return;

	if (ours && was_connecting && m_state == MConnectState::None)
	{
		// Never reached Connected: the handshake itself failed.
		ScheduleConnectRetry();
		return;
	}

	if (info->m_info.m_eState !=
		k_ESteamNetworkingConnectionState_Connected)
		return;
	m_retry_pending = false;
	m_retry_attempts = 0;

	const CSteamID remote = info->m_info.m_identityRemote.GetSteamID();
	if (remote.IsValid() && SteamFriends())
	{
		const std::string connect = std::to_string(remote.ConvertToUint64());
		SteamFriends()->SetRichPresence("connect", connect.c_str());
	}
}
