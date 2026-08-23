#include "coop_application.h"

extern "C" BOOL WINAPI CoopInitialize()
{
	return coop::CoopApplication::Instance().Initialize();
}

extern "C" VOID WINAPI CoopShutdown()
{
	coop::CoopApplication::Instance().Shutdown();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		coop::CoopApplication::Instance().SetModule(instance);
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}
