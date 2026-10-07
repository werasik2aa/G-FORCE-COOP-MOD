#include "menu_session_hook.h"

#include "coop_netgame.h"
#include "coop_runtime.h"
#include "gforce_constants.h"
#include "retail/retail_memory.h"
#include "ServerClient/SteamManager.h"

#include <cstring>

namespace
{
	bool VerifySlot(std::uintptr_t slot, std::uintptr_t target,
		const std::uint8_t* bytes, std::size_t count)
	{
		std::uintptr_t current = 0;
		return coop::retail::TryRead(slot, current) && current == target &&
			std::memcmp(reinterpret_cast<const void*>(target), bytes, count) == 0;
	}

	bool ReplaceSlot(std::uintptr_t slot, void* replacement)
	{
		return coop::MemoryPatch::Write(reinterpret_cast<void*>(slot),
			&replacement, sizeof(replacement));
	}

	bool RestoreSlot(std::uintptr_t slot, void* replacement, void* original)
	{
		void* current = nullptr;
		return coop::retail::TryRead(slot, current) &&
			(current == original ||
				(current == replacement && ReplaceSlot(slot, original)));
	}
}

namespace coop
{
	MenuSessionHook& MenuSessionHook::Instance()
	{
		static MenuSessionHook instance;
		return instance;
	}

	bool MenuSessionHook::Install()
	{
		using namespace gforce;
		if (m_installed)
			return true;
		if (!VerifySlot(kMainMenuQuitVtableSlot, kMainMenuQuitConfirmed,
			kExpectedMainMenuQuitConfirmed, sizeof(kExpectedMainMenuQuitConfirmed)) ||
			!VerifySlot(kPauseMenuQuitVtableSlot, kPauseMenuQuitConfirmed,
				kExpectedPauseMenuQuitConfirmed, sizeof(kExpectedPauseMenuQuitConfirmed)) ||
			!VerifySlot(kMainMenuEnterVtableSlot, kMainMenuEnter,
				kExpectedMainMenuEnter, sizeof(kExpectedMainMenuEnter)))
		{
			CoopRuntime::Instance().Log("[menu-session-error] native exit ABI mismatch\r\n");
			return false;
		}
		m_original_main_quit = reinterpret_cast<MainQuitFn>(kMainMenuQuitConfirmed);
		m_original_pause_quit = reinterpret_cast<PauseQuitFn>(kPauseMenuQuitConfirmed);
		m_original_main_enter = reinterpret_cast<MainEnterFn>(kMainMenuEnter);
		if (!ReplaceSlot(kMainMenuQuitVtableSlot, reinterpret_cast<void*>(&HookMainQuit)) ||
			!ReplaceSlot(kPauseMenuQuitVtableSlot, reinterpret_cast<void*>(&HookPauseQuit)) ||
			!ReplaceSlot(kMainMenuEnterVtableSlot, reinterpret_cast<void*>(&HookMainEnter)))
		{
			Remove();
			CoopRuntime::Instance().Log("[menu-session-error] native exit slot patch failed\r\n");
			return false;
		}
		m_installed = true;
		CoopRuntime::Instance().Log("[menu-session] native exit confirmations hooked\r\n");
		return true;
	}

	void MenuSessionHook::Remove()
	{
		using namespace gforce;
		if (!m_original_main_quit)
			return;
		const bool main_restored = RestoreSlot(kMainMenuQuitVtableSlot,
			reinterpret_cast<void*>(&HookMainQuit), reinterpret_cast<void*>(m_original_main_quit));
		const bool pause_restored = RestoreSlot(kPauseMenuQuitVtableSlot,
			reinterpret_cast<void*>(&HookPauseQuit), reinterpret_cast<void*>(m_original_pause_quit));
		const bool enter_restored = RestoreSlot(kMainMenuEnterVtableSlot,
			reinterpret_cast<void*>(&HookMainEnter), reinterpret_cast<void*>(m_original_main_enter));
		if (main_restored && pause_restored && enter_restored)
			m_installed = false;
		else
			CoopRuntime::Instance().Log("[menu-session-warning] exit slots changed; foreign hooks retained\r\n");
	}

	int __fastcall MenuSessionHook::HookMainQuit(void* menu, void*, std::uint32_t answer)
	{
		if (answer == 1)
		{
			CoopRuntime::Instance().Log("[menu-session] confirmed application exit\r\n");
			CoopNetGame::Instance().QuitSessionToMainMenu();
		}
		return Instance().m_original_main_quit(menu, answer);
	}

	void __fastcall MenuSessionHook::HookPauseQuit(void* menu, void*, std::uint32_t answer)
	{
		if (answer == 1)
		{
			CoopRuntime::Instance().Log("[menu-session] confirmed quit to main menu\r\n");
			CoopNetGame::Instance().QuitSessionToMainMenu();
		}
		Instance().m_original_pause_quit(menu, answer);
	}

	int __fastcall MenuSessionHook::HookMainEnter(void* menu, void*)
	{
		const int result = Instance().m_original_main_enter(menu);
		// The native quit can leave P1 ticking during its fade. Keep automatic
		// hosting inhibited until main-menu entry, then arm the next loaded world.
		if (SteamManager)
			SteamManager->DisarmAutomaticHost();
		return result;
	}
}
