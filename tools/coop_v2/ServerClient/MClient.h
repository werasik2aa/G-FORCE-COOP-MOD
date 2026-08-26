#pragma once

#include "MTypes.h"
#include "SteamWorksSDK/include/steamnetworkingsockets.h"
#include "SteamWorksSDK/include/isteamnetworkingutils.h"

class CSteamOfflineSocketClient
{
public:
	CSteamOfflineSocketClient();
	virtual ~CSteamOfflineSocketClient();

	virtual bool CreateConnection(const char* connection_ip);
	virtual bool RecreateConnection();
	virtual bool ConnectToFriend(CSteamID, std::uint32_t = 0) { return false; }
	virtual void Disconnect(bool quit = false);
	virtual void ProcessCallbacks();
	virtual void OnFrame();
	virtual void OnStateChange(
		SteamNetConnectionStatusChangedCallback_t* info);

	bool IsSteamInterface() const;
	bool IsConnected() const;
	MConnectState GetState() const;
	void ClearConnection();

	void SendPacket(PacketHeader* message, std::uint32_t flags, bool skip = false);
	bool SendRaw(const void* data, std::uint32_t size, int flags);
	int ReceivePacket(SteamNetworkingMessage_t** data);
	SteamNetConnectionRealTimeStatus_t GetStatus() const;

	virtual bool IsSteam() const { return false; }

protected:
	virtual void OnRemotePacket(const SteamNetworkingMessage_t* message);

	ISteamNetworkingSockets* m_sockets;
	HSteamNetConnection m_connection;
	std::string m_last_connection;
	CSteamID m_last_steam_id;
	std::uint32_t m_last_steam_port;
	MConnectState m_state;
};

extern CSteamOfflineSocketClient* SteamOClient;
extern CSteamOfflineSocketClient* SteamLClient;
void OfflineClientCallback(
	SteamNetConnectionStatusChangedCallback_t* info);
