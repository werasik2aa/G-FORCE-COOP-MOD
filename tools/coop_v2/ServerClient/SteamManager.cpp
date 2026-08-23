#include "SteamManager.h"

#include "MClient.h"
#include "MClientONLINE.h"
#include "MServer.h"
#include "MServerONLINE.h"
#include "MStandalone.h"
#include "../coop_netgame.h"

#include <new>

namespace
{
constexpr std::uint32_t kOfflinePort = 44139;
constexpr std::uint32_t kSteamVirtualPort = 44140;
const char kLocalAddress[] = "127.0.0.1:44139";

CSteamManager g_manager;
}

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
	m_f7_was_down(false),
	m_f8_was_down(false)
{
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

	Msg("[network] worker started; F7=host ports %u/%u, F8=connect %s",
		kOfflinePort, kSteamVirtualPort, kLocalAddress);
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
	// InitRelayNetworkAccess only STARTS the relay ping measurement, and ConnectP2P
	// cannot route through Steam Datagram Relay until that finishes.  Both the
	// listen socket and the join-request handler used to call it on the same line as
	// the connect, so the first Steam connect of a process raced a relay network
	// that was still being measured - which ends as
	// "reason=5003 Timed out attempting to connect" even though the peer is fine.
	// Starting it here gives the relays the whole menu/loading time to come up.
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

DWORD WINAPI CSteamManager::WorkerThread(LPVOID context)
{
	static_cast<CSteamManager*>(context)->WorkerLoop();
	return 0;
}

void CSteamManager::WorkerLoop()
{
	// The relay warm-up has to happen HERE and not in InitializeSteam.  Initialize
	// runs on the game's startup path, and the very first touch of the Steam
	// networking interface brings up the SDR library from that context - which hung
	// the process before it could even print the next log line.  The worker thread
	// is the context that already drives every other Steam networking call, and it
	// starts at game launch, so the relays still get the whole menu/loading time.
	if (m_steam_initialized)
		WarmUpRelayNetwork();
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
	const bool f7_down = IsGameForeground() &&
		(GetAsyncKeyState(VK_F7) & 0x8000) != 0;
	const bool f8_down = IsGameForeground() &&
		(GetAsyncKeyState(VK_F8) & 0x8000) != 0;
	if (f7_down && !m_f7_was_down)
		StartServers();
	if (f8_down && !m_f8_was_down)
		ConnectLocalhost();
	m_f7_was_down = f7_down;
	m_f8_was_down = f8_down;
}

void CSteamManager::StartServers()
{
	if (!SteamOServer || !SteamSServer)
		return;
	if (SteamOClient && SteamOClient->IsConnected())
		SteamOClient->Disconnect();

	const bool offline = SteamOServer->OpenListenSocket(kOfflinePort);
	const bool online = SteamSServer->OpenListenSocket(kSteamVirtualPort);
	Msg("[network-host] F7 result: offline=%s Steam=%s relay=%s",
		offline ? "ready" : "failed", online ? "ready" : "unavailable",
		SteamRelayStatusName());
	if (offline || online)
		coop::CoopNetGame::Instance().SetModeHost();
}

void CSteamManager::ConnectLocalhost()
{
	if (!SteamLClient || !SteamOServer)
		return;
	if (SteamOServer->IsSteamSocketOpen())
	{
		Msg("[network-client] F8 ignored in host process");
		return;
	}
	if (SteamOClient && SteamOClient != SteamLClient)
		SteamOClient->Disconnect();
	SteamOClient = SteamLClient;
	const bool started = SteamOClient->CreateConnection(kLocalAddress);
	Msg("[network-client] F8 localhost connect=%s",
		started ? "started" : "failed");
	if (started)
		coop::CoopNetGame::Instance().SetModeClient();
}

void CSteamManager::OnGameRichPresenceJoinRequested(
	GameRichPresenceJoinRequested_t* callback)
{
	if (!callback || !SteamSClient)
		return;
	Msg("[network-steam] join request from %llu; connecting without X-Ray UI "
		"(relay=%s)",
		callback->m_steamIDFriend.ConvertToUint64(), SteamRelayStatusName());
	if (SteamOClient)
		SteamOClient->Disconnect();
	if (SteamOServer)
		SteamOServer->CloseServer();
	if (SteamSServer)
		SteamSServer->CloseServer();
	SteamOClient = SteamSClient;
	// A join request is a fresh user intent, so the retry budget starts over even
	// if a previous friend's connect had already exhausted it.
	SteamSClient->ResetConnectRetries();
	if (SteamOClient->ConnectToFriend(
		callback->m_steamIDFriend, kSteamVirtualPort))
		coop::CoopNetGame::Instance().SetModeClient();
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
