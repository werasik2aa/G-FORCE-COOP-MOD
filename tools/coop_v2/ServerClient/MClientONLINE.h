#pragma once

#include "SteamWorksSDK/includeo/steam_api.h"
#include "SteamWorksSDK/includeo/isteamnetworkingsockets.h"
#include "MClient.h"

class CSteamOnlineSocketClient : public CSteamOfflineSocketClient
{
public:
	CSteamOnlineSocketClient();
	~CSteamOnlineSocketClient() override;

	bool CreateConnection(const char* connection_ip) override;
	bool RecreateConnection() override;
	bool ConnectToFriend(CSteamID friend_id, std::uint32_t virtual_port = 0) override;
	void Disconnect(bool quit = false) override;
	void OnFrame() override;
	bool IsSteam() const override { return true; }
	// Clears the retry budget.  Called when the player expresses fresh intent to
	// join, so an earlier exhausted attempt cannot block a new one.
	void ResetConnectRetries();

private:
	// A P2P connect that dies while still Connecting almost always means the peer
	// had no listen socket yet - it opens only after the host's save has loaded,
	// so it can legitimately appear seconds after the join click.
	// Retrying turns that ordering problem into a non-issue instead of a dead run.
	void ScheduleConnectRetry();

	bool m_retry_pending;
	std::uint32_t m_retry_attempts;
	std::uint32_t m_retry_at_tick;
	std::uint32_t kMaxConnectRetries;
	std::uint32_t kConnectRetryDelayMs;

	STEAM_CALLBACK(CSteamOnlineSocketClient, OnSteamStateChange, SteamNetConnectionStatusChangedCallback_t);
};
