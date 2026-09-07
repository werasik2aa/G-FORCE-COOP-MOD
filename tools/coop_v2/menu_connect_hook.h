#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>

namespace coop
{
	// Adds one native XHudMenuMain row for the existing IP-connect request. It
	// never owns a custom renderer or a socket; the click only queues the same
	// request used by F8 for CSteamManager's worker.
	class MenuConnectHook final
	{
	public:
		static MenuConnectHook& Instance();

		bool Install();
		void Remove();

	private:
		using BuildMainMenuFn = void* (__thiscall*)(void*);
		using AddChildFn = void(__thiscall*)(void*, void*);
		using LabelResolverFn = char(__thiscall*)(void*, void*);

		MenuConnectHook();
		~MenuConnectHook() = default;
		MenuConnectHook(const MenuConnectHook&) = delete;
		MenuConnectHook& operator=(const MenuConnectHook&) = delete;

		static void* __fastcall HookBuildMainMenu(void* menu, void* unused);
		static void __fastcall HookCreditsAddChild(void* parent, void* unused,
			void* child);
		static char __fastcall HookTextResolver(void* text, void* unused,
			void* destination);
		static void __fastcall HookConnectAction(void* menu, void* unused,
			std::uint32_t action);

		void* HandleBuildMainMenu(void* menu);
		void HandleCreditsAddChild(void* parent, void* child);
		char HandleTextResolver(void* text, void* destination);
		void HandleConnectAction(std::uint32_t action);

		bool VerifyRetailAbi() const;
		bool InstallBuildHook();
		bool InstallCreditsAddChildHook();
		bool InstallTextResolverHook();
		bool RemoveBuildHook();
		bool RemoveCreditsAddChildHook();
		bool RemoveTextResolverHook();

		bool m_installed;
		bool m_build_hooked;
		bool m_credits_call_hooked;
		bool m_text_resolver_hooked;
		BYTE m_original_build_bytes[5];
		BYTE m_build_patch_bytes[5];
		BYTE* m_build_trampoline;
		BYTE m_original_credits_call_bytes[5];
		BYTE m_credits_call_patch_bytes[5];
		BuildMainMenuFn m_original_build_main_menu;
		AddChildFn m_original_add_child;
		void** m_text_resolver_slot;
		LabelResolverFn m_original_text_resolver;
		void* m_building_menu;
		DWORD m_building_menu_thread;
		bool m_button_inserted_for_build;
		bool m_reported_insert_failure;
		bool m_reported_builder_seen;
		bool m_reported_credits_seen;
		bool m_reported_label_resolved;
		volatile LONG m_click_route_enabled;
	};
}
