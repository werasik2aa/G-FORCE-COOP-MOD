#pragma once

#include "MTypes.h"
#include "SteamWorksSDK/include/steamnetworkingsockets.h"

#include <vector>

class CSteamOfflineSocketServer
{
public:
	CSteamOfflineSocketServer();
	virtual ~CSteamOfflineSocketServer();

	virtual bool OpenListenSocket(std::uint32_t port);
	virtual void CloseServer();
	virtual void ProcessCallbacks();
	virtual void OnFrame();
	virtual void OnStateChange(
		SteamNetConnectionStatusChangedCallback_t* info);

	bool IsSteamSocketOpen() const;
	bool IsSteamInterface() const;
	size_t GetPlayerCount() const;
	std::uint32_t GetOpenConnection() const;
	const std::vector<HSteamNetConnection>& GetPlayers() const;

	void CloseConnection(HSteamNetConnection connection,
		int reason = 0, const char* message = "Server closed connection");
	void SendDataTo(
		HSteamNetConnection connection, PacketHeader* data,
		std::uint32_t flags);
	bool SendRaw(HSteamNetConnection connection,
		const void* data, std::uint32_t size, int flags);
	int ReceivePacket(SteamNetworkingMessage_t** data);
	bool IsClientExist(HSteamNetConnection client_id) const;
	SteamNetConnectionRealTimeStatus_t GetStatus(
		HSteamNetConnection client_id) const;

	virtual bool IsSteam() const { return false; }

protected:
	void ResetState();
	void EraseClient(HSteamNetConnection connection);
	virtual void OnLocalPacket(const PacketHeader* data);
	virtual void OnRemotePacket(const SteamNetworkingMessage_t* message);

	ISteamNetworkingSockets* m_sockets;
	HSteamNetPollGroup m_poll_group;
	HSteamListenSocket m_listen_socket;
	std::vector<HSteamNetConnection> m_players;
	std::uint8_t m_max_players;
	bool m_p2p;
};

extern CSteamOfflineSocketServer* SteamOServer;
void OfflineServerCallback(
	SteamNetConnectionStatusChangedCallback_t* info);
