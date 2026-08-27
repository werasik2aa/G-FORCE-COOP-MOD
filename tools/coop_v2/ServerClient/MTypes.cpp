#include "MTypes.h"

#include "../coop_runtime.h"
#include "SteamWorksSDK/include/isteamnetworkingutils.h"
#include <stdarg.h>
#include <stdio.h>

void Msg(const char* format, ...)
{
	char buffer[1024] = {};
	va_list arguments;
	va_start(arguments, format);
	const int length = _vsnprintf_s(
		buffer, sizeof(buffer), _TRUNCATE, format, arguments);
	va_end(arguments);
	if (length <= 0)
		return;

	OutputDebugStringA(buffer);
	OutputDebugStringA("\n");
	coop::CoopRuntime::Instance().Log("[pid=%lu] %s\r\n",
		GetCurrentProcessId(), buffer);
}

bool ParseUnsignedDecimal(const char*& cursor, std::uint32_t maximum, std::uint32_t& value)
{
	if (!cursor || *cursor < '0' || *cursor > '9')
		return false;
	std::uint32_t result = 0;
	do
	{
		const std::uint32_t digit = static_cast<std::uint32_t>(*cursor - '0');
		if (result > (maximum - digit) / 10)
			return false;
		result = result * 10 + digit;
		++cursor;
	} while (*cursor >= '0' && *cursor <= '9');
	value = result;
	return true;
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