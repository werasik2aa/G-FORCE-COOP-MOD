#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>

namespace coop
{
	// Hooks the confirmed answer=Yes callbacks behind stock Exit Game rows.
	class MenuSessionHook final
	{
	public:
		static MenuSessionHook& Instance();
		bool Install();
		void Remove();

	private:
		using MainQuitFn = int(__thiscall*)(void*, std::uint32_t);
		using PauseQuitFn = void(__thiscall*)(void*, std::uint32_t);
		using MainEnterFn = int(__thiscall*)(void*);
		static int __fastcall HookMainQuit(void* menu, void*, std::uint32_t answer);
		static void __fastcall HookPauseQuit(void* menu, void*, std::uint32_t answer);
		static int __fastcall HookMainEnter(void* menu, void*);
		MenuSessionHook() = default;
		bool m_installed = false;
		MainQuitFn m_original_main_quit = nullptr;
		PauseQuitFn m_original_pause_quit = nullptr;
		MainEnterFn m_original_main_enter = nullptr;
	};
}
