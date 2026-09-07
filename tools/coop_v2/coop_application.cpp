#include "coop_application.h"

#include "coop_runtime.h"
#include "coop_netgame.h"
#include "menu_connect_hook.h"
#include "player2.h"
#include "ServerClient/SteamManager.h"
#include "window_hook.h"

namespace coop
{
	CoopApplication& CoopApplication::Instance()
	{
		static CoopApplication instance;
		return instance;
	}

	CoopApplication::CoopApplication() :
		m_init_state(0)
	{}

	void CoopApplication::SetModule(HMODULE module)
	{
		CoopRuntime::Instance().SetModule(module);
	}

	BOOL CoopApplication::Initialize()
	{
		const LONG previous = InterlockedCompareExchange(&m_init_state, 1, 0);
		if (previous != 0)
			return previous == 2;

		CoopRuntime& runtime = CoopRuntime::Instance();
		if (!runtime.Initialize())
		{
			InterlockedExchange(&m_init_state, -1);
			return FALSE;
		}
		runtime.Log("=== G-Force network co-op prototype v1 ===\r\n");

		if (!runtime.VerifyExecutable())
		{
			InterlockedExchange(&m_init_state, -1);
			return FALSE;
		}
		runtime.LoadConfiguration();
		if (!MenuConnectHook::Instance().Install())
			runtime.Log("[menu-warning] native Connect by IP row unavailable; F8 remains available\r\n");
		if (!WindowHook::Instance().Install())
			runtime.Log("[window-warning] experimental window mode is unavailable\r\n");
		if (!Player2Module::Instance().Install())
		{
			runtime.Log("[error] hook installation failed; game left unmodified\r\n");
			InterlockedExchange(&m_init_state, -1);
			return FALSE;
		}
		if (!SteamManager->Initialize())
			runtime.Log("[network-warning] network manager is unavailable; local P2 debug route remains available\r\n");
		if (!CoopNetGame::Instance().InstallInputHook())
			runtime.Log("[network-warning] remote XInput injection is unavailable\r\n");

		InterlockedExchange(&m_init_state, 2);
		runtime.Log("[ready] F1=local Mooch dual-laser probe F2=spawn trigger F3=replay event F4=known interactive F5=P2 F6=ABR F7=pause bypass F8=IP connect F9=trigger catalog\r\n");
		return TRUE;
	}

	void CoopApplication::Shutdown()
	{
		if (InterlockedCompareExchange(&m_init_state, 3, 2) != 2)
			return;

		MenuConnectHook::Instance().Remove();
		SteamManager->Destroy();
		CoopNetGame::Instance().RemoveInputHook();
		Player2Module::Instance().Remove();
		WindowHook::Instance().Remove();
		CoopRuntime::Instance().Log("[shutdown] hooks restored\r\n");
		CoopRuntime::Instance().Shutdown();
	}
}
