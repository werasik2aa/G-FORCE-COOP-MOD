#include "MTypes.h"

#include "../coop_runtime.h"

#include <stdarg.h>
#include <stdio.h>

void __cdecl Msg(const char* format, ...)
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
