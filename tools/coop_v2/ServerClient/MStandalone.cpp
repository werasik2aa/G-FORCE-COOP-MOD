#include "MStandalone.h"
#include "SteamWorksSDK/include/steamnetworkingsockets.h"
#include "SteamWorksSDK/include/isteamnetworkingsockets.h"
#include "SteamWorksSDK/include/isteamnetworkingutils.h"

StandaloneSocketsRuntime& Runtime()
{
	static StandaloneSocketsRuntime runtime;
	return runtime;
}

ISteamNetworkingSockets* AcquireStandaloneSockets()
{
	return Runtime().Acquire();
}

ISteamNetworkingUtils* GetStandaloneNetworkingUtils()
{
	return Runtime().Utils();
}

ISteamNetworkingSockets* StandaloneSocketsRuntime::Acquire()
{
	EnterCriticalSection(&m_lock);
	if (!m_initialized)
	{
		SteamDatagramErrMsg error = {};
		if (!GameNetworkingSockets_Init(nullptr, error))
		{
			Msg("[network-error] GameNetworkingSockets_Init: %s", error);
			LeaveCriticalSection(&m_lock);
			return nullptr;
		}
		m_sockets = SteamNetworkingSockets_Lib();
		m_utils = SteamNetworkingUtils_Lib();
		m_initialized = m_sockets != nullptr && m_utils != nullptr;
		Msg("[network] standalone GameNetworkingSockets initialized=%s",
			m_initialized ? "yes" : "no");
	}
	ISteamNetworkingSockets* result = m_initialized ? m_sockets : nullptr;
	LeaveCriticalSection(&m_lock);
	return result;
}

void StandaloneSocketsRuntime::Shutdown()
{
	EnterCriticalSection(&m_lock);
	if (m_initialized)
	{
		GameNetworkingSockets_Kill();
		Msg("[network] standalone GameNetworkingSockets stopped");
	}
	m_initialized = false;
	m_sockets = nullptr;
	m_utils = nullptr;
	LeaveCriticalSection(&m_lock);
}

void ShutdownStandaloneSockets()
{
	Runtime().Shutdown();
}