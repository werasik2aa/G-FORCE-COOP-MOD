#include "MTypes.h"

#include "../coop_runtime.h"

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