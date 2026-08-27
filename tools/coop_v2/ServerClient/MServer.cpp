#include "MServer.h"

#include "MStandalone.h"
#include "../coop_netgame.h"
#include "../protocol/packet_view.h"
#include "../save_sync.h"
#include "../world_sync.h"

#include <algorithm>
#include <string.h>

CSteamOfflineSocketServer::CSteamOfflineSocketServer() :
	m_sockets(nullptr),
	m_poll_group(k_HSteamNetPollGroup_Invalid),
	m_listen_socket(k_HSteamListenSocket_Invalid),
	m_max_players(1),
	m_p2p(false)
{}

CSteamOfflineSocketServer::~CSteamOfflineSocketServer()
{
	CloseServer();
}

void CSteamOfflineSocketServer::ResetState()
{
	m_sockets = nullptr;
	m_poll_group = k_HSteamNetPollGroup_Invalid;
	m_listen_socket = k_HSteamListenSocket_Invalid;
	m_players.clear();
	m_p2p = false;
}

bool CSteamOfflineSocketServer::OpenListenSocket(std::uint32_t port)
{
	if (IsSteamSocketOpen())
		return true;

	m_sockets = AcquireStandaloneSockets();
	ISteamNetworkingUtils* utils = GetStandaloneNetworkingUtils();
	if (!m_sockets || !utils)
		return false;

	utils->SetGlobalConfigValueInt32(
		k_ESteamNetworkingConfig_SendBufferSize, 24 * 1024 * 1024);
	utils->SetGlobalConfigValueInt32(
		k_ESteamNetworkingConfig_TimeoutConnected, 5000);
	utils->SetGlobalConfigValueInt32(
		k_ESteamNetworkingConfig_TimeoutInitial, 15000);

	SteamNetworkingIPAddr address;
	address.Clear();
	address.m_port = static_cast<uint16>(port);
	SteamNetworkingConfigValue_t callback;
	callback.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
		reinterpret_cast<void*>(OfflineServerCallback));
	m_listen_socket = m_sockets->CreateListenSocketIP(address, 1, &callback);
	if (m_listen_socket == k_HSteamListenSocket_Invalid)
	{
		Msg("[network-error] offline server failed to bind port %u", port);
		return false;
	}

	m_poll_group = m_sockets->CreatePollGroup();
	if (m_poll_group == k_HSteamNetPollGroup_Invalid)
	{
		m_sockets->CloseListenSocket(m_listen_socket);
		m_listen_socket = k_HSteamListenSocket_Invalid;
		Msg("[network-error] offline server failed to create poll group");
		return false;
	}

	Msg("[network-server] offline listening on 0.0.0.0:%u", port);
	return true;
}

void CSteamOfflineSocketServer::CloseServer()
{
	if (!m_sockets)
		return;

	for (const HSteamNetConnection connection : m_players)
		m_sockets->CloseConnection(
			connection, kCoopEndServerShutdown, "Server shutdown", false);
	m_players.clear();

	if (m_listen_socket != k_HSteamListenSocket_Invalid)
		m_sockets->CloseListenSocket(m_listen_socket);
	if (m_poll_group != k_HSteamNetPollGroup_Invalid)
		m_sockets->DestroyPollGroup(m_poll_group);

	Msg("[network-server] %s server stopped", IsSteam() ? "Steam" : "offline");
	ResetState();
}

void CSteamOfflineSocketServer::OnStateChange(
	SteamNetConnectionStatusChangedCallback_t* info)
{
	if (!m_sockets || !info)
		return;

	switch (info->m_info.m_eState)
	{
	case k_ESteamNetworkingConnectionState_Connecting:
		if (m_players.size() >= m_max_players)
		{
			CloseConnection(info->m_hConn, kCoopEndSessionFull, "Server full");
			Msg("[network-server] rejected connection %u: server full",
				info->m_hConn);
			return;
		}
		if (m_sockets->AcceptConnection(info->m_hConn) != k_EResultOK ||
			!m_sockets->SetConnectionPollGroup(info->m_hConn, m_poll_group))
		{
			CloseConnection(info->m_hConn, kCoopEndUnknown, "Accept failed");
			Msg("[network-server] failed to accept connection %u",
				info->m_hConn);
			return;
		}
		m_players.push_back(info->m_hConn);
		Msg("[network-server] accepted connection %u", info->m_hConn);
		break;

	case k_ESteamNetworkingConnectionState_Connected:
		Msg("[network-server] connection %u ready", info->m_hConn);
		coop::CoopNetGame::Instance().OnPeerConnected();
		coop::SaveSync::Instance().SendHostJoinSave(this, info->m_hConn);
		break;

	case k_ESteamNetworkingConnectionState_ClosedByPeer:
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		Msg("[network-server] connection %u closed: %s", info->m_hConn,
			info->m_info.m_szEndDebug);
		EraseClient(info->m_hConn);
		if (m_players.empty())
			coop::CoopNetGame::Instance().OnPeerDisconnected();
		m_sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
		break;

	default:
		break;
	}
}

void CSteamOfflineSocketServer::ProcessCallbacks()
{
	if (m_sockets)
		m_sockets->RunCallbacks();
}

void CSteamOfflineSocketServer::OnFrame()
{
	if (!m_sockets || m_poll_group == k_HSteamNetPollGroup_Invalid)
		return;

	SteamNetworkingMessage_t* messages[kServerReadBatch] = {};
	const int count = ReceivePacket(messages);
	for (int index = 0; index < count; ++index)
	{
		OnRemotePacket(messages[index]);
		messages[index]->Release();
	}
}

void CSteamOfflineSocketServer::OnRemotePacket(
	const SteamNetworkingMessage_t* message)
{
	if (!message)
		return;
	const void* const data = message->m_pData;
	const std::uint32_t size = static_cast<std::uint32_t>(message->m_cbSize);
	const coop::protocol::PacketView view(data, size);
	switch (view.Kind())
	{
	case coop::protocol::PacketKind::Input:
		coop::CoopNetGame::Instance().OnRemotePacket(data, size);
		return;

	case coop::protocol::PacketKind::SaveSlot:
		if (coop::SaveSync::Instance().OnRemotePacket(data, size))
			return;
		break;

	case coop::protocol::PacketKind::WorldSpawn:
	case coop::protocol::PacketKind::WorldSnapshot:
	case coop::protocol::PacketKind::WorldReady:
	case coop::protocol::PacketKind::WorldTriggerEvent:
	case coop::protocol::PacketKind::WorldDamage:
	case coop::protocol::PacketKind::WorldDespawn:

		if (coop::WorldSync::Instance().OnRemotePacket(data, size))
			return;
		break;

	default:
		break;
	}
	Msg("[network-server] ignored unknown packet from %u: %d bytes",
		message->m_conn, message->m_cbSize);
}

void CSteamOfflineSocketServer::OnLocalPacket(const PacketHeader* data)
{
	Msg("[network-server] local host packet id=%u size=%u",
		data ? data->m_PacketID : 0, data ? data->Size() : 0);
}

void CSteamOfflineSocketServer::SendDataTo(
	HSteamNetConnection connection, PacketHeader* data,
	std::uint32_t flags)
{
	if (!data)
		return;
	if (connection == static_cast<HSteamNetConnection>(m_listen_socket))
	{
		OnLocalPacket(data);
		return;
	}
	SendRaw(connection, data, data->Size(), static_cast<int>(flags));
}

bool CSteamOfflineSocketServer::SendRaw(HSteamNetConnection connection,
	const void* data, std::uint32_t size, int flags)
{
	if (!m_sockets || !data || size == 0 || !IsClientExist(connection))
		return false;
	const EResult result = m_sockets->SendMessageToConnection(
		connection, data, size, flags, nullptr);
	if (result != k_EResultOK)
		Msg("[network-error] server send to %u failed: %d", connection, result);
	return result == k_EResultOK;
}

int CSteamOfflineSocketServer::ReceivePacket(SteamNetworkingMessage_t** data)
{
	if (!m_sockets || m_poll_group == k_HSteamNetPollGroup_Invalid)
		return 0;
	const int count = m_sockets->ReceiveMessagesOnPollGroup(
		m_poll_group, data, kServerReadBatch);
	return count > 0 ? count : 0;
}

void CSteamOfflineSocketServer::EraseClient(HSteamNetConnection connection)
{
	const auto found = std::find(m_players.begin(), m_players.end(), connection);
	if (found != m_players.end())
		m_players.erase(found);
}

void CSteamOfflineSocketServer::CloseConnection(
	HSteamNetConnection connection, int reason, const char* message)
{
	if (m_sockets && connection != k_HSteamNetConnection_Invalid)
		m_sockets->CloseConnection(connection, reason, message, false);
	EraseClient(connection);
}

bool CSteamOfflineSocketServer::IsClientExist(
	HSteamNetConnection client_id) const
{
	return std::find(m_players.begin(), m_players.end(), client_id) !=
		m_players.end();
}

SteamNetConnectionRealTimeStatus_t CSteamOfflineSocketServer::GetStatus(
	HSteamNetConnection client_id) const
{
	SteamNetConnectionRealTimeStatus_t status = {};
	status.m_eState = k_ESteamNetworkingConnectionState_None;
	if (m_sockets)
		m_sockets->GetConnectionRealTimeStatus(client_id, &status, 0, nullptr);
	return status;
}

bool CSteamOfflineSocketServer::IsSteamSocketOpen() const
{
	return m_listen_socket != k_HSteamListenSocket_Invalid;
}

bool CSteamOfflineSocketServer::IsSteamInterface() const
{
	return m_sockets != nullptr;
}

size_t CSteamOfflineSocketServer::GetPlayerCount() const
{
	return m_players.size();
}

std::uint32_t CSteamOfflineSocketServer::GetOpenConnection() const
{
	return static_cast<std::uint32_t>(m_listen_socket);
}

const std::vector<HSteamNetConnection>&
CSteamOfflineSocketServer::GetPlayers() const
{
	return m_players;
}

void OfflineServerCallback(SteamNetConnectionStatusChangedCallback_t* info)
{
	if (SteamOServer)
		SteamOServer->OnStateChange(info);
}
