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

	ISteamNetworkingSockets* AcquireStandaloneSockets()
	{
		return Runtime().Acquire();
	}

	ISteamNetworkingUtils* GetStandaloneNetworkingUtils()
	{
		return Runtime().Utils();
	}

	bool ParseStandaloneIPv4Address(const char* text, SteamNetworkingIPAddr& address)
	{
		if (!text || !text[0])
			return false;

		const char* cursor = text;
		std::uint32_t octet[4] = {};
		for (std::uint32_t index = 0; index != 4; ++index)
		{
			if (!ParseUnsignedDecimal(cursor, 255, octet[index]))
				return false;
			if (index != 3)
			{
				if (*cursor != '.')
					return false;
				++cursor;
			}
		}

		std::uint32_t port = 0;
		if (*cursor == ':')
		{
			++cursor;
			if (!ParseUnsignedDecimal(cursor, 65535, port) || port == 0)
				return false;
		}
		if (*cursor != '\0')
			return false;

		const std::uint32_t ipv4 = (octet[0] << 24) | (octet[1] << 16) |
			(octet[2] << 8) | octet[3];
		address.SetIPv4(ipv4, static_cast<std::uint16_t>(port));
		return true;
	}

	void ShutdownStandaloneSockets()
	{
		Runtime().Shutdown();
	}
}