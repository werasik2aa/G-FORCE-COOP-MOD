#pragma once

#include "MTypes.h"
#include "SteamWorksSDK/includeo/steam_api.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#ifndef _WIN64
#pragma comment(lib, "GameNetworkingSockets.lib")
#pragma comment(lib, "steam_api.lib")
#endif

class CSteamOnlineSocketClient;
class CSteamOfflineSocketClient;
class CSteamOnlineSocketServer;
class CSteamOfflineSocketServer;

class CSteamManager final
{
public:
	CSteamManager();
	~CSteamManager();

	bool Initialize();
	void Destroy();
	void OnFrame();

	bool InitializeSteam();
	bool IsSteamInitialized() const;
	const CSteamID& GetMySteamID() const;

	void StartServers();
	void ConnectLocalhost();

private:
	static DWORD WINAPI WorkerThread(LPVOID context);
	void WorkerLoop();
	void PollHotkeys();
	bool IsGameForeground() const;
	void WarmUpRelayNetwork();

	STEAM_CALLBACK(CSteamManager, OnGameRichPresenceJoinRequested,
		GameRichPresenceJoinRequested_t);
	STEAM_CALLBACK(CSteamManager, OnSteamServersConnected,
		SteamServersConnected_t);
	STEAM_CALLBACK(CSteamManager, OnSteamServersDisconnected,
		SteamServersDisconnected_t);

	volatile LONG m_started;
	bool m_steam_initialized;
	CSteamID m_my_steam_id;
	HANDLE m_stop_event;
	HANDLE m_worker_thread;
	bool m_f7_was_down;
	bool m_f8_was_down;
};

// Human-readable Steam Datagram Relay availability, for the one question a
// failed P2P connect always raises: were the relays even up when we tried?
const char* SteamRelayStatusName();

extern CSteamManager* SteamManager;
extern CSteamOnlineSocketServer* SteamSServer;
extern CSteamOfflineSocketServer* SteamOServer;
extern CSteamOnlineSocketClient* SteamSClient;
extern CSteamOfflineSocketClient* SteamLClient;
extern CSteamOfflineSocketClient* SteamOClient;
