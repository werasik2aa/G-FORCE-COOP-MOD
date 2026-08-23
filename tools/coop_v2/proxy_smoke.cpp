#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef UINT (WINAPI* TimePeriodFn)(UINT period);
typedef bool (__cdecl* SteamInitializeFn)();
typedef void (__cdecl* SteamShutdownFn)();

int main()
{
	HMODULE winmm = LoadLibraryW(L"winmm.dll");
	if (!winmm)
	{
		printf("LoadLibrary failed: %lu\n", GetLastError());
		return 2;
	}
	wchar_t loaded_path[MAX_PATH] = {};
	GetModuleFileNameW(winmm, loaded_path, MAX_PATH);
	wprintf(L"loaded=%ls\n", loaded_path);
	TimePeriodFn begin = reinterpret_cast<TimePeriodFn>(
		GetProcAddress(winmm, "timeBeginPeriod"));
	TimePeriodFn end = reinterpret_cast<TimePeriodFn>(
		GetProcAddress(winmm, "timeEndPeriod"));
	if (!begin || !end)
	{
		printf("GetProcAddress failed: %lu\n", GetLastError());
		return 3;
	}
	const UINT begin_result = begin(1);
	const UINT end_result = end(1);
	printf("timeBeginPeriod=%u timeEndPeriod=%u\n", begin_result, end_result);

	SetEnvironmentVariableA("SteamAppId", "480");
	SetEnvironmentVariableA("SteamGameId", "480");
	HMODULE steam_api = LoadLibraryW(L"steam_api.dll");
	if (!steam_api)
	{
		printf("steam_api LoadLibrary failed: %lu\n", GetLastError());
		return 4;
	}
	SteamInitializeFn steam_initialize = reinterpret_cast<SteamInitializeFn>(
		GetProcAddress(steam_api, "SteamAPI_InitSafe"));
	SteamShutdownFn steam_shutdown = reinterpret_cast<SteamShutdownFn>(
		GetProcAddress(steam_api, "SteamAPI_Shutdown"));
	if (!steam_initialize || !steam_shutdown)
	{
		printf("Steam API exports missing: %lu\n", GetLastError());
		return 5;
	}
	const bool steam_ready = steam_initialize();
	printf("SteamAPI_Init=%s\n", steam_ready ? "true" : "false");
	if (steam_ready)
		steam_shutdown();
	return begin_result == 0 && end_result == 0 ? 0 : 1;
}
