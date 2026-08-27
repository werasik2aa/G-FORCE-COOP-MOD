#include "SteamManager.h"

#include "MClient.h"
#include "MClientONLINE.h"
#include "MServer.h"
#include "MServerONLINE.h"
#include "MStandalone.h"
#include "../coop_netgame.h"
#include "../ip_connect_dialog.h"

#include <new>
#include <string.h>

CSteamManager g_manager;
CSteamManager* SteamManager = &g_manager;
CSteamOnlineSocketServer* SteamSServer = nullptr;
CSteamOfflineSocketServer* SteamOServer = nullptr;
CSteamOnlineSocketClient* SteamSClient = nullptr;
CSteamOfflineSocketClient* SteamLClient = nullptr;
CSteamOfflineSocketClient* SteamOClient = nullptr;

CSteamManager::CSteamManager() :
	m_started(0),
	m_steam_initialized(false),
	m_stop_event(NULL),
	m_worker_thread(NULL),
	m_game_world_ready(0),
	m_automatic_host_attempted(0),
	m_ip_prompt_requested(0),
	m_f8_was_down(false)
{
	lstrcpynA(m_last_ip_address, "127.0.0.1:44139",
		static_cast<int>(_countof(m_last_ip_address)));
}

CSteamManager::~CSteamManager()
{
	Destroy();
}

bool CSteamManager::Initialize()
{
	if (InterlockedCompareExchange(&m_started, 1, 0) != 0)
		return m_started == 1;

	if (!SetEnvironmentVariableA("SteamAppId", "480") ||
		!SetEnvironmentVariableA("SteamGameId", "480"))
		Msg("[network-warning] failed to set Spacewar environment variables");

	InitializeSteam();
	SteamLClient = new (std::nothrow) CSteamOfflineSocketClient();
	SteamOServer = new (std::nothrow) CSteamOfflineSocketServer();
	SteamSClient = new (std::nothrow) CSteamOnlineSocketClient();
	SteamSServer = new (std::nothrow) CSteamOnlineSocketServer();
	if (!SteamLClient || !SteamOServer || !SteamSClient || !SteamSServer)
	{
		Msg("[network-error] failed to allocate transport objects");
		Destroy();
		return false;
	}
	SteamOClient = SteamLClient;

	m_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!m_stop_event)
	{
		Msg("[network-error] failed to create network stop event");
		Destroy();
		return false;
	}
	m_worker_thread = CreateThread(
		NULL, 0, WorkerThread, this, 0, NULL);
	if (!m_worker_thread)
	{
		Msg("[network-error] failed to create network worker");
		Destroy();
		return false;
	}

	Msg("[network] worker started; world load opens host ports %u/%u, F8 connects %s",
		OfflinePort, OnlinePortS, "127.0.0.1:44139");
	return true;
}

void CSteamManager::Destroy()
{
	const LONG previous = InterlockedExchange(&m_started, 2);
	if (previous == 0 || previous == 2)
		return;

	if (m_stop_event)
		SetEvent(m_stop_event);
	if (m_worker_thread)
	{
		WaitForSingleObject(m_worker_thread, 5000);
		CloseHandle(m_worker_thread);
		m_worker_thread = NULL;
	}
	if (m_stop_event)
	{
		CloseHandle(m_stop_event);
		m_stop_event = NULL;
	}

	if (SteamOClient)
		SteamOClient->Disconnect();
	if (SteamOServer)
		SteamOServer->CloseServer();
	if (SteamSServer)
		SteamSServer->CloseServer();

	delete SteamSClient;
	delete SteamLClient;
	delete SteamSServer;
	delete SteamOServer;
	SteamSClient = nullptr;
	SteamLClient = nullptr;
	SteamSServer = nullptr;
	SteamOServer = nullptr;
	SteamOClient = nullptr;

	ShutdownStandaloneSockets();
	if (m_steam_initialized)
	{
		SteamAPI_Shutdown();
		Msg("[network-steam] Steam API stopped");
	}
	m_steam_initialized = false;
	m_my_steam_id.Clear();
	InterlockedExchange(&m_started, 0);
}

bool CSteamManager::InitializeSteam()
{
	if (m_steam_initialized)
		return true;
	if (!SteamAPI_IsSteamRunning())
	{
		Msg("[network-steam] Steam is not running; offline transport remains available");
		return false;
	}
	if (!SteamAPI_Init())
	{
		Msg("[network-steam] SteamAPI_Init failed; offline transport remains available");
		return false;
	}
	if (!SteamUser() || !SteamFriends() || !SteamUtils())
	{
		Msg("[network-steam] required Steam interfaces unavailable");
		SteamAPI_Shutdown();
		return false;
	}
	m_my_steam_id = SteamUser()->GetSteamID();
	if (!m_my_steam_id.IsValid())
	{
		Msg("[network-steam] invalid local SteamID");
		SteamAPI_Shutdown();
		return false;
	}
	m_steam_initialized = true;
	Msg("[network-steam] initialized SteamID=%llu persona=%s",
		m_my_steam_id.ConvertToUint64(), SteamFriends()->GetPersonaName());
	return true;
}

void CSteamManager::WarmUpRelayNetwork()
{
	ISteamNetworkingUtils* utils = SteamNetworkingUtils_SteamAPI();
	if (!utils)
		return;
	// This is called only after a world is loaded or for an explicit Steam Join.
	// It starts relay/NAT traversal, so doing it while merely opening the game makes
	// Steam rate-limit a path the player may never use.
	utils->InitRelayNetworkAccess();
	// The offline path tunes its timeouts through the standalone
	// GameNetworkingSockets utils; that is a different library instance and leaves
	// the Steam one on defaults.  A first-time SDR handshake between two machines
	// can legitimately need longer than the default initial timeout.
	utils->SetGlobalConfigValueInt32(
		k_ESteamNetworkingConfig_TimeoutInitial, 30000);
	utils->SetGlobalConfigValueInt32(
		k_ESteamNetworkingConfig_TimeoutConnected, 10000);
	Msg("[network-steam] relay network warm-up requested; status=%s",
		SteamRelayStatusName());
}

const char* SteamRelayStatusName()
{
	ISteamNetworkingUtils* utils = SteamNetworkingUtils_SteamAPI();
	if (!utils)
		return "no-utils";
	switch (utils->GetRelayNetworkStatus(nullptr))
	{
	case k_ESteamNetworkingAvailability_CannotTry:
		return "cannot-try";
	case k_ESteamNetworkingAvailability_Failed:
		return "failed";
	case k_ESteamNetworkingAvailability_Previously:
		return "previously-ok";
	case k_ESteamNetworkingAvailability_Retrying:
		return "retrying";
	case k_ESteamNetworkingAvailability_NeverTried:
		return "never-tried";
	case k_ESteamNetworkingAvailability_Waiting:
		return "waiting";
	case k_ESteamNetworkingAvailability_Attempting:
		return "attempting";
	case k_ESteamNetworkingAvailability_Current:
		return "ready";
	default:
		return "unknown";
	}
}

bool CSteamManager::IsSteamInitialized() const
{
	return m_steam_initialized;
}

const CSteamID& CSteamManager::GetMySteamID() const
{
	return m_my_steam_id;
}

void CSteamManager::NotifyGameWorldReady()
{
	InterlockedExchange(&m_game_world_ready, 1);
}

void CSteamManager::RequestIpConnectionPrompt()
{
	InterlockedExchange(&m_ip_prompt_requested, 1);
}

DWORD WINAPI CSteamManager::WorkerThread(LPVOID context)
{
	static_cast<CSteamManager*>(context)->WorkerLoop();
	return 0;
}

void CSteamManager::WorkerLoop()
{
	while (WaitForSingleObject(m_stop_event, 1) == WAIT_TIMEOUT)
	{
		PollHotkeys();
		OnFrame();
	}
}

void CSteamManager::OnFrame()
{
	if (m_steam_initialized)
		SteamAPI_RunCallbacks();
	if (SteamOClient)
	{
		SteamOClient->ProcessCallbacks();
		SteamOClient->OnFrame();
	}
	if (SteamOServer)
	{
		SteamOServer->ProcessCallbacks();
		SteamOServer->OnFrame();
	}
	if (SteamSServer)
	{
		SteamSServer->ProcessCallbacks();
		SteamSServer->OnFrame();
	}

	ProcessAutomaticHostRequest();
	coop::CoopNetGame::Instance().NetworkTick();
}

bool CSteamManager::IsGameForeground() const
{
	const HWND foreground = GetForegroundWindow();
	if (!foreground)
		return false;
	DWORD process_id = 0;
	GetWindowThreadProcessId(foreground, &process_id);
	return process_id == GetCurrentProcessId();
}

void CSteamManager::PollHotkeys()
{
	const bool f8_down = IsGameForeground() &&
		(GetAsyncKeyState(VK_F8) & 0x8000) != 0;
	if (f8_down && !m_f8_was_down)
		RequestIpConnectionPrompt();
	m_f8_was_down = f8_down;

	if (InterlockedExchange(&m_ip_prompt_requested, 0) != 0)
		PromptForIpConnection();
}

void CSteamManager::ProcessAutomaticHostRequest()
{
	if (!InterlockedCompareExchange(&m_game_world_ready, 0, 0) || InterlockedCompareExchange(&m_automatic_host_attempted, 0, 0))
		return;

	InterlockedExchange(&m_automatic_host_attempted, 1);
	if (coop::CoopNetGame::Instance().IsClient())
	{
		Msg("[network-host] loaded-world host skipped; Steam join selected client mode");
		return;
	}
	StartLoadedWorldServers();
}

void CSteamManager::StartLoadedWorldServers()
{
	if (!SteamOServer || !SteamSServer)
		return;

	if (m_steam_initialized)
		WarmUpRelayNetwork();

	const bool offline = SteamOServer->OpenListenSocket(OfflinePort);
	const bool online = SteamSServer->OpenListenSocket(OnlinePortS);
	Msg("[network-host] loaded world: IP=%s Steam=%s ports=%u/%u relay=%s", offline ? "ready" : "failed", online ? "ready" : "unavailable",
		OfflinePort, OnlinePortS, SteamRelayStatusName());
	if (offline || online)
		coop::CoopNetGame::Instance().SetModeHost();
}

void CSteamManager::PromptForIpConnection()
{
	if (!SteamLClient)
		return;
	if (coop::CoopNetGame::Instance().IsClient())
	{
		Msg("[network-client] F8 ignored; this process is already a client");
		return;
	}

	char address[sizeof(m_last_ip_address)] = {};
	IpConnectDialog dialog;
	if (!dialog.Prompt(GetForegroundWindow(), m_last_ip_address, address, sizeof(address)))
	{
		Msg("[network-client] IP connect cancelled");
		return;
	}

	ConnectToIpAddress(address);
}

void CSteamManager::ConnectToIpAddress(const char* address)
{
	if (!SteamLClient || !address || !address[0])
		return;

	// The UI is intentionally an IP field, not a separate port editor.  Keep
	// the familiar standalone port when a LAN user enters only an IPv4 address.
	char connection_address[sizeof(m_last_ip_address)] = {};
	if (strchr(address, ':'))
	{
		lstrcpynA(connection_address, address,
			static_cast<int>(_countof(connection_address)));
	}
	else
	{
		_snprintf_s(connection_address, sizeof(connection_address), _TRUNCATE,
			"%s:44139", address);
	}

	SteamNetworkingIPAddr parsed_address;
	if (!ParseStandaloneIPv4Address(connection_address, parsed_address))
	{
		Msg("[network-error] invalid server address: %s", connection_address);
		return;
	}

	// Explicit IP input is client intent.  A process can already have opened its
	// own listener after loading a save, so close it only after validation rather
	// than leaving both roles active or losing host state on a typo.
	coop::CoopNetGame::Instance().SetModeClient();
	if (SteamOClient && SteamOClient != SteamLClient)
		SteamOClient->Disconnect();
	StopServersForClient();
	SteamOClient = SteamLClient;
	const bool started = SteamOClient->CreateConnection(connection_address);
	if (started)
		lstrcpynA(m_last_ip_address, connection_address, static_cast<int>(_countof(m_last_ip_address)));
	Msg("[network-client] IP connect address=%s started=%s", connection_address, started ? "started" : "failed");
}

void CSteamManager::StopServersForClient()
{
	if (SteamOServer)
		SteamOServer->CloseServer();
	if (SteamSServer)
		SteamSServer->CloseServer();
}

void CSteamManager::OnGameRichPresenceJoinRequested(
	GameRichPresenceJoinRequested_t* callback)
{
	if (!callback || !SteamSClient)
		return;
	Msg("[network-steam] join request from %llu; connecting without X-Ray UI "
		"(relay=%s)",
		callback->m_steamIDFriend.ConvertToUint64(), SteamRelayStatusName());
	// A Steam Join is explicit client intent.  It wins even if the local save
	// just finished loading on the same frame, so this process never becomes a
	// temporary host while it is following a friend.
	coop::CoopNetGame::Instance().SetModeClient();
	if (SteamOClient)
		SteamOClient->Disconnect();
	StopServersForClient();
	SteamOClient = SteamSClient;
	// The relay is touched only for the explicit join action, never at startup.
	if (m_steam_initialized)
		WarmUpRelayNetwork();
	// A join request is a fresh user intent, so the retry budget starts over even
	// if a previous friend's connect had already exhausted it.
	SteamSClient->ResetConnectRetries();
	SteamOClient->ConnectToFriend(callback->m_steamIDFriend, OnlinePortS);
}

void CSteamManager::OnSteamServersConnected(SteamServersConnected_t*)
{
	Msg("[network-steam] connection to Steam backend restored");
}

void CSteamManager::OnSteamServersDisconnected(
	SteamServersDisconnected_t* callback)
{
	Msg("[network-steam] disconnected from Steam backend: %d",
		callback ? callback->m_eResult : 0);
	if (SteamOClient && SteamOClient->IsSteam())
		SteamOClient->Disconnect();
}
