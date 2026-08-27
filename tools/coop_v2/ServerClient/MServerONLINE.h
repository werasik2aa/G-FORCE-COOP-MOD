#pragma once

#include "SteamWorksSDK/includeo/steam_api.h"
#include "SteamWorksSDK/includeo/isteamnetworkingsockets.h"
#include "MServer.h"

class CSteamOnlineSocketServer : public CSteamOfflineSocketServer
{
public:
	CSteamOnlineSocketServer();
	~CSteamOnlineSocketServer() override;

	bool OpenListenSocket(std::uint32_t port) override;
	bool OpenListenSocketP2P(std::uint32_t port);
	void CloseServer() override;
	bool IsSteam() const override { return true; }
	void OnStateChange(SteamNetConnectionStatusChangedCallback_t* info) override;
};

extern CSteamOnlineSocketServer* SteamSServer;
void OnlineServerCallback(
	SteamNetConnectionStatusChangedCallback_t* info);
