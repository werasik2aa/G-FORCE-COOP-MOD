#include "MStandalone.h"

#include "MTypes.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
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

	ISteamNetworkingSockets* Acquire()
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

	ISteamNetworkingUtils* Utils()
	{
		return Acquire() ? m_utils : nullptr;
	}

	void Shutdown()
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

private:
	CRITICAL_SECTION m_lock;
	bool m_initialized;
	ISteamNetworkingSockets* m_sockets;
	ISteamNetworkingUtils* m_utils;
};

StandaloneSocketsRuntime& Runtime()
{
	static StandaloneSocketsRuntime runtime;
	return runtime;
}
}

ISteamNetworkingSockets* AcquireStandaloneSockets()
{
	return Runtime().Acquire();
}

ISteamNetworkingUtils* GetStandaloneNetworkingUtils()
{
	return Runtime().Utils();
}

void ShutdownStandaloneSockets()
{
	Runtime().Shutdown();
}
