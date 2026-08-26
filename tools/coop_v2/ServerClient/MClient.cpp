#include "MClient.h"

#include "MStandalone.h"
#include "../coop_netgame.h"
#include "../save_sync.h"
#include "../world_sync.h"

#include <string.h>

namespace
{
const char kHelloMessage[] = "GFCOOP_HELLO_v1";
const char kWelcomeMessage[] = "GFCOOP_WELCOME_v1";
}

CSteamOfflineSocketClient::CSteamOfflineSocketClient() :
	m_sockets(nullptr),
	m_connection(k_HSteamNetConnection_Invalid),
	m_last_steam_port(0),
	m_state(MConnectState::None)
{
}

CSteamOfflineSocketClient::~CSteamOfflineSocketClient()
{
	Disconnect();
}

bool CSteamOfflineSocketClient::CreateConnection(const char* connection_ip)
{
	if (m_connection != k_HSteamNetConnection_Invalid)
		return true;
	if (!connection_ip || !connection_ip[0])
		return false;

	m_sockets = AcquireStandaloneSockets();
	ISteamNetworkingUtils* utils = GetStandaloneNetworkingUtils();
	if (!m_sockets || !utils)
		return false;

	SteamNetworkingIPAddr address;
	if (!ParseStandaloneIPv4Address(connection_ip, address))
	{
		Msg("[network-error] invalid server address: %s", connection_ip);
		return false;
	}

	utils->SetGlobalConfigValueInt32(
		k_ESteamNetworkingConfig_SendBufferSize, 24 * 1024 * 1024);
	utils->SetGlobalConfigValueInt32(
		k_ESteamNetworkingConfig_TimeoutConnected, 5000);
	utils->SetGlobalConfigValueInt32(
		k_ESteamNetworkingConfig_TimeoutInitial, 15000);

	SteamNetworkingConfigValue_t callback;
	callback.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
		reinterpret_cast<void*>(OfflineClientCallback));
	m_connection = m_sockets->ConnectByIPAddress(address, 1, &callback);
	if (m_connection == k_HSteamNetConnection_Invalid)
	{
		Msg("[network-error] unable to create client connection");
		return false;
	}

	m_last_connection = connection_ip;
	m_state = MConnectState::Connecting;
	Msg("[network-client] connecting to %s", connection_ip);
	return true;
}

bool CSteamOfflineSocketClient::RecreateConnection()
{
	return !m_last_connection.empty() &&
		CreateConnection(m_last_connection.c_str());
}

void CSteamOfflineSocketClient::Disconnect(bool)
{
	const bool was_connected = m_state == MConnectState::Connected;
	if (m_sockets && m_connection != k_HSteamNetConnection_Invalid)
		m_sockets->CloseConnection(
			m_connection, 0, "Client disconnect", false);
	if (m_connection != k_HSteamNetConnection_Invalid)
		Msg("[network-client] disconnected");
	m_connection = k_HSteamNetConnection_Invalid;
	m_state = MConnectState::None;
	if (was_connected)
		coop::CoopNetGame::Instance().OnPeerDisconnected();
}

void CSteamOfflineSocketClient::OnStateChange(
	SteamNetConnectionStatusChangedCallback_t* info)
{
	if (!m_sockets || !info || info->m_hConn != m_connection)
		return;

	switch (info->m_info.m_eState)
	{
	case k_ESteamNetworkingConnectionState_Connecting:
		m_state = MConnectState::Connecting;
		break;

	case k_ESteamNetworkingConnectionState_Connected:
		m_state = MConnectState::Connected;
		Msg("[network-client] connected");
		SendHello();
		coop::CoopNetGame::Instance().OnPeerConnected();
		break;

	case k_ESteamNetworkingConnectionState_ClosedByPeer:
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		Msg("[network-client] connection closed: reason=%d debug=%s",
			info->m_info.m_eEndReason, info->m_info.m_szEndDebug);
		m_sockets->CloseConnection(m_connection, 0, nullptr, false);
		m_connection = k_HSteamNetConnection_Invalid;
		m_state = MConnectState::None;
		coop::CoopNetGame::Instance().OnPeerDisconnected();
		break;

	default:
		break;
	}
}

void CSteamOfflineSocketClient::ProcessCallbacks()
{
	if (m_sockets)
		m_sockets->RunCallbacks();
}

void CSteamOfflineSocketClient::OnFrame()
{
	if (!m_sockets || m_connection == k_HSteamNetConnection_Invalid)
		return;

	SteamNetworkingMessage_t* messages[kClientReadBatch] = {};
	const int count = ReceivePacket(messages);
	for (int index = 0; index < count; ++index)
	{
		OnRemotePacket(messages[index]);
		messages[index]->Release();
	}
}

void CSteamOfflineSocketClient::OnRemotePacket(
	const SteamNetworkingMessage_t* message)
{
	if (!message)
		return;
	const bool welcome = message->m_cbSize == sizeof(kWelcomeMessage) &&
		memcmp(message->m_pData, kWelcomeMessage, sizeof(kWelcomeMessage)) == 0;
	if (welcome)
		Msg("[network-client] received welcome");

	if (!welcome && message->m_cbSize >= sizeof(PacketHeader))
	{
		const PacketHeader* hdr =
			static_cast<const PacketHeader*>(message->m_pData);
		if (hdr->m_PacketID == kCoopPacketInput)
		{
			coop::CoopNetGame::Instance().OnRemotePacket(
				message->m_pData, static_cast<std::uint32_t>(message->m_cbSize));
			return;
		}
		if (coop::SaveSync::Instance().OnRemotePacket(message->m_pData,
			static_cast<std::uint32_t>(message->m_cbSize)))
			return;
		if (coop::WorldSync::Instance().OnRemotePacket(message->m_pData,
			static_cast<std::uint32_t>(message->m_cbSize)))
			return;
	}
	if (!welcome)
		Msg("[network-client] ignored unknown packet: %d bytes",
			message->m_cbSize);
}

void CSteamOfflineSocketClient::SendHello()
{
	SendRaw(kHelloMessage, static_cast<std::uint32_t>(sizeof(kHelloMessage)),
		k_nSteamNetworkingSend_Reliable);
}

void CSteamOfflineSocketClient::SendPacket(
	PacketHeader* message, std::uint32_t flags, bool skip)
{
	if (!skip && message)
		SendRaw(message, message->Size(), static_cast<int>(flags));
}

bool CSteamOfflineSocketClient::SendRaw(
	const void* data, std::uint32_t size, int flags)
{
	if (!m_sockets || m_connection == k_HSteamNetConnection_Invalid ||
		!data || size == 0)
		return false;
	const EResult result = m_sockets->SendMessageToConnection(
		m_connection, data, size, flags, nullptr);
	if (result != k_EResultOK)
		Msg("[network-error] client send failed: %d", result);
	return result == k_EResultOK;
}

int CSteamOfflineSocketClient::ReceivePacket(
	SteamNetworkingMessage_t** data)
{
	if (!m_sockets || m_connection == k_HSteamNetConnection_Invalid)
		return 0;
	const int count = m_sockets->ReceiveMessagesOnConnection(
		m_connection, data, kClientReadBatch);
	return count > 0 ? count : 0;
}

SteamNetConnectionRealTimeStatus_t CSteamOfflineSocketClient::GetStatus() const
{
	SteamNetConnectionRealTimeStatus_t status = {};
	status.m_eState = k_ESteamNetworkingConnectionState_None;
	if (m_sockets && m_connection != k_HSteamNetConnection_Invalid)
		m_sockets->GetConnectionRealTimeStatus(
			m_connection, &status, 0, nullptr);
	return status;
}

bool CSteamOfflineSocketClient::IsSteamInterface() const
{
	return m_sockets != nullptr;
}

bool CSteamOfflineSocketClient::IsConnected() const
{
	return m_state == MConnectState::Connected;
}

MConnectState CSteamOfflineSocketClient::GetState() const
{
	return m_state;
}

void CSteamOfflineSocketClient::ClearConnection()
{
	m_last_connection.clear();
	m_last_steam_id.Clear();
	m_last_steam_port = 0;
}

void OfflineClientCallback(SteamNetConnectionStatusChangedCallback_t* info)
{
	if (SteamOClient && !SteamOClient->IsSteam())
		SteamOClient->OnStateChange(info);
}
