#pragma once

#include "SteamWorksSDK/include/steamnetworkingsockets.h"
#include "SteamWorksSDK/include/isteamnetworkingutils.h"

#include "MTypes.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

ISteamNetworkingSockets* AcquireStandaloneSockets();
ISteamNetworkingUtils* GetStandaloneNetworkingUtils();
// SteamNetworkingIPAddr::ParseString delegates through Steam API state in this
// SDK build.  The IP-only transport must not touch that state when Steam is off.
void ShutdownStandaloneSockets();

class StandaloneSocketsRuntime final
{
public:
	StandaloneSocketsRuntime() :
		m_initialized(false),
		m_sockets(nullptr),
		m_utils(nullptr)
	{
		InitializeCriticalSection(&m_lock);
	}

	~StandaloneSocketsRuntime()
	{
		Shutdown();
		DeleteCriticalSection(&m_lock);
	}

	ISteamNetworkingSockets* Acquire();
	ISteamNetworkingUtils* Utils() { return Acquire() ? m_utils : nullptr; }

	void Shutdown();

private:
	CRITICAL_SECTION m_lock;
	bool m_initialized;
	ISteamNetworkingSockets* m_sockets;
	ISteamNetworkingUtils* m_utils;
};