#include "menu_connect_hook.h"

#include "coop_netgame.h"
#include "coop_runtime.h"
#include "gforce_constants.h"
#include "retail/retail_memory.h"
#include "retail/retail_views.h"
#include "ServerClient/SteamManager.h"

#include <cstring>
#include <limits>

namespace
{
	constexpr char kConnectByIpLabel[] = "Connect by IP";

	bool BytesMatch(std::uintptr_t address, const std::uint8_t* expected,
		std::size_t size)
	{
		return std::memcmp(reinterpret_cast<const void*>(address), expected, size) == 0;
	}

	bool MakeRelativeBranch(std::uint8_t opcode, std::uintptr_t source,
		const void* destination, BYTE output[5])
	{
		const std::intptr_t relative =
			reinterpret_cast<std::intptr_t>(destination) -
			static_cast<std::intptr_t>(source + 5u);
		if (relative < std::numeric_limits<std::int32_t>::min() ||
			relative > std::numeric_limits<std::int32_t>::max())
			return false;
		output[0] = opcode;
		const std::int32_t encoded = static_cast<std::int32_t>(relative);
		std::memcpy(output + 1, &encoded, sizeof(encoded));
		return true;
	}
}

namespace coop
{
	MenuConnectHook& MenuConnectHook::Instance()
	{
		static MenuConnectHook instance;
		return instance;
	}

	MenuConnectHook::MenuConnectHook() :
		m_installed(false),
		m_build_hooked(false),
		m_credits_call_hooked(false),
		m_text_resolver_hooked(false),
		m_build_trampoline(nullptr),
		m_original_build_main_menu(nullptr),
		m_original_add_child(nullptr),
		m_text_resolver_slot(nullptr),
		m_original_text_resolver(nullptr),
		m_building_menu(nullptr),
		m_building_menu_thread(0),
		m_button_inserted_for_build(false),
		m_reported_insert_failure(false),
		m_reported_builder_seen(false),
		m_reported_credits_seen(false),
		m_reported_label_resolved(false),
		m_click_route_enabled(0)
	{
		std::memset(m_original_build_bytes, 0, sizeof(m_original_build_bytes));
		std::memset(m_build_patch_bytes, 0, sizeof(m_build_patch_bytes));
		std::memset(m_original_credits_call_bytes, 0,
			sizeof(m_original_credits_call_bytes));
		std::memset(m_credits_call_patch_bytes, 0,
			sizeof(m_credits_call_patch_bytes));
	}

	bool MenuConnectHook::VerifyRetailAbi() const
	{
		if (!BytesMatch(gforce::kMainMenuBuild, gforce::kExpectedMainMenuBuild,
			sizeof(gforce::kExpectedMainMenuBuild)))
		{
			CoopRuntime::Instance().Log(
				"[menu-error] ABI mismatch: BuildMainMenu @0x005EECC0\r\n");
			return false;
		}
		if (!BytesMatch(gforce::kMenuCreditsAddChildCall,
			gforce::kExpectedMenuCreditsAddChildCall,
			sizeof(gforce::kExpectedMenuCreditsAddChildCall)))
		{
			CoopRuntime::Instance().Log(
				"[menu-error] ABI mismatch: Credits AddChild seam @0x005EED92\r\n");
			return false;
		}
		if (!BytesMatch(gforce::kMenuAddChild, gforce::kExpectedMenuAddChild,
			sizeof(gforce::kExpectedMenuAddChild)))
		{
			CoopRuntime::Instance().Log(
				"[menu-error] ABI mismatch: native AddChild @0x005C4850\r\n");
			return false;
		}
		if (!BytesMatch(gforce::kMenuCallbackInvoke,
			gforce::kExpectedMenuCallbackInvoke,
			sizeof(gforce::kExpectedMenuCallbackInvoke)))
		{
			CoopRuntime::Instance().Log(
				"[menu-error] ABI mismatch: menu callback invoke @0x005E8AA0\r\n");
			return false;
		}
		if (!BytesMatch(gforce::kMenuLabelResolver,
			gforce::kExpectedMenuLabelResolver,
			sizeof(gforce::kExpectedMenuLabelResolver)))
		{
			CoopRuntime::Instance().Log(
				"[menu-error] ABI mismatch: XAText resolver @0x00490900\r\n");
			return false;
		}
		if (!BytesMatch(gforce::kGameStringAssign,
			gforce::kExpectedGameStringAssign,
			sizeof(gforce::kExpectedGameStringAssign)))
		{
			CoopRuntime::Instance().Log(
				"[menu-error] ABI mismatch: EXWString assign @0x006395BA\r\n");
			return false;
		}
		if (!BytesMatch(gforce::kGameStringAssignAnsi,
			gforce::kExpectedGameStringAssignAnsi,
			sizeof(gforce::kExpectedGameStringAssignAnsi)))
		{
			CoopRuntime::Instance().Log(
				"[menu-error] ABI mismatch: EXWString ANSI assign @0x0063982E\r\n");
			return false;
		}
		if (!BytesMatch(gforce::kGameStringRelease,
			gforce::kExpectedGameStringRelease,
			sizeof(gforce::kExpectedGameStringRelease)))
		{
			CoopRuntime::Instance().Log(
				"[menu-error] ABI mismatch: EXWString release @0x0064E925\r\n");
			return false;
		}

		const void* const callback_invoke = *reinterpret_cast<void* const*>(
			gforce::kMenuCallbackInvokeVtableSlot);
		const void* const label_resolver = *reinterpret_cast<void* const*>(
			gforce::kXATextResolverVtableSlot);
		if (callback_invoke != reinterpret_cast<void*>(gforce::kMenuCallbackInvoke))
		{
			CoopRuntime::Instance().Log(
				"[menu-error] callback vtable mismatch @0x0071B09C\r\n");
			return false;
		}
		if (label_resolver != reinterpret_cast<void*>(gforce::kMenuLabelResolver))
		{
			CoopRuntime::Instance().Log(
				"[menu-error] XAText resolver vtable mismatch @0x006FA818\r\n");
			return false;
		}
		return true;
	}

	bool MenuConnectHook::InstallBuildHook()
	{
		BYTE* const target = reinterpret_cast<BYTE*>(gforce::kMainMenuBuild);
		if (m_build_hooked)
			return true;
		if (!BytesMatch(gforce::kMainMenuBuild, gforce::kExpectedMainMenuBuild,
			sizeof(m_original_build_bytes)))
			return false;

		BYTE patch[sizeof(m_build_patch_bytes)] = {};
		if (!MakeRelativeBranch(0xE9, gforce::kMainMenuBuild,
			reinterpret_cast<void*>(&HookBuildMainMenu), patch))
			return false;

		BYTE* const trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr,
			sizeof(m_original_build_bytes) + 5u, MEM_RESERVE | MEM_COMMIT,
			PAGE_EXECUTE_READWRITE));
		if (!trampoline)
			return false;
		std::memcpy(m_original_build_bytes, target, sizeof(m_original_build_bytes));
		std::memcpy(trampoline, m_original_build_bytes,
			sizeof(m_original_build_bytes));
		if (!MakeRelativeBranch(0xE9,
			reinterpret_cast<std::uintptr_t>(trampoline + sizeof(m_original_build_bytes)),
			target + sizeof(m_original_build_bytes),
			trampoline + sizeof(m_original_build_bytes)) ||
			!MemoryPatch::Write(target, patch, sizeof(patch)))
		{
			VirtualFree(trampoline, 0, MEM_RELEASE);
			return false;
		}
		std::memcpy(m_build_patch_bytes, patch, sizeof(m_build_patch_bytes));
		m_build_trampoline = trampoline;
		m_original_build_main_menu = reinterpret_cast<BuildMainMenuFn>(trampoline);
		m_build_hooked = true;
		return true;
	}

	bool MenuConnectHook::InstallCreditsAddChildHook()
	{
		BYTE* const call_site = reinterpret_cast<BYTE*>(
			gforce::kMenuCreditsAddChildCall);
		if (m_credits_call_hooked)
			return true;
		if (!BytesMatch(gforce::kMenuCreditsAddChildCall,
			gforce::kExpectedMenuCreditsAddChildCall,
			sizeof(m_original_credits_call_bytes)))
			return false;
		BYTE patch[sizeof(m_credits_call_patch_bytes)] = {};
		if (!MakeRelativeBranch(0xE8, gforce::kMenuCreditsAddChildCall,
			reinterpret_cast<void*>(&HookCreditsAddChild), patch))
			return false;
		std::memcpy(m_original_credits_call_bytes, call_site,
			sizeof(m_original_credits_call_bytes));
		if (!MemoryPatch::Write(call_site, patch, sizeof(patch)))
			return false;
		std::memcpy(m_credits_call_patch_bytes, patch,
			sizeof(m_credits_call_patch_bytes));
		m_original_add_child = reinterpret_cast<AddChildFn>(gforce::kMenuAddChild);
		m_credits_call_hooked = true;
		return true;
	}

	bool MenuConnectHook::InstallTextResolverHook()
	{
		if (m_text_resolver_hooked)
			return true;
		void** const slot = reinterpret_cast<void**>(
			gforce::kXATextResolverVtableSlot);
		if (*slot != reinterpret_cast<void*>(gforce::kMenuLabelResolver))
			return false;
		void* const replacement = reinterpret_cast<void*>(&HookTextResolver);
		m_original_text_resolver = reinterpret_cast<LabelResolverFn>(*slot);
		if (!MemoryPatch::Write(slot, &replacement, sizeof(replacement)))
		{
			m_original_text_resolver = nullptr;
			return false;
		}
		m_text_resolver_slot = slot;
		m_text_resolver_hooked = true;
		return true;
	}

	bool MenuConnectHook::Install()
	{
		if (m_installed)
			return true;
		CoopRuntime& runtime = CoopRuntime::Instance();
		runtime.Log("[menu-init] validating native main-menu ABI\r\n");
		if (!VerifyRetailAbi())
			return false;
		if (!InstallBuildHook())
		{
			runtime.Log("[menu-error] BuildMainMenu trampoline installation failed\r\n");
			return false;
		}
		runtime.Log("[menu-init] BuildMainMenu trampoline installed\r\n");
		if (!InstallCreditsAddChildHook())
		{
			runtime.Log("[menu-error] Credits AddChild seam installation failed\r\n");
			RemoveBuildHook();
			return false;
		}
		runtime.Log("[menu-init] Credits AddChild seam installed\r\n");
		if (!InstallTextResolverHook())
		{
			runtime.Log("[menu-error] XAText resolver hook installation failed\r\n");
			RemoveCreditsAddChildHook();
			RemoveBuildHook();
			return false;
		}
		runtime.Log("[menu-init] XAText resolver hook installed\r\n");
		m_installed = true;
		InterlockedExchange(&m_click_route_enabled, 1);
		runtime.Log(
			"[menu] native Connect by IP row hook installed\r\n");
		return true;
	}

	bool MenuConnectHook::RemoveTextResolverHook()
	{
		if (!m_text_resolver_hooked)
			return true;
		if (!m_text_resolver_slot ||
			*m_text_resolver_slot != reinterpret_cast<void*>(&HookTextResolver))
		{
			CoopRuntime::Instance().Log(
				"[menu-warning] text resolver changed; leaving its slot untouched\r\n");
			return false;
		}
		void* const original = reinterpret_cast<void*>(m_original_text_resolver);
		if (!MemoryPatch::Write(m_text_resolver_slot, &original, sizeof(original)))
			return false;
		m_text_resolver_hooked = false;
		m_text_resolver_slot = nullptr;
		m_original_text_resolver = nullptr;
		return true;
	}

	bool MenuConnectHook::RemoveCreditsAddChildHook()
	{
		if (!m_credits_call_hooked)
			return true;
		BYTE* const call_site = reinterpret_cast<BYTE*>(
			gforce::kMenuCreditsAddChildCall);
		if (std::memcmp(call_site, m_credits_call_patch_bytes,
			sizeof(m_credits_call_patch_bytes)) != 0)
		{
			CoopRuntime::Instance().Log(
				"[menu-warning] Credits call changed; leaving it untouched\r\n");
			return false;
		}
		if (!MemoryPatch::Write(call_site, m_original_credits_call_bytes,
			sizeof(m_original_credits_call_bytes)))
			return false;
		m_credits_call_hooked = false;
		m_original_add_child = nullptr;
		return true;
	}

	bool MenuConnectHook::RemoveBuildHook()
	{
		if (!m_build_hooked)
			return true;
		BYTE* const target = reinterpret_cast<BYTE*>(gforce::kMainMenuBuild);
		if (std::memcmp(target, m_build_patch_bytes, sizeof(m_build_patch_bytes)) != 0)
		{
			CoopRuntime::Instance().Log(
				"[menu-warning] main-menu build entry changed; keeping trampoline alive\r\n");
			return false;
		}
		if (!MemoryPatch::Write(target, m_original_build_bytes,
			sizeof(m_original_build_bytes)))
			return false;
		if (m_build_trampoline)
			VirtualFree(m_build_trampoline, 0, MEM_RELEASE);
		m_build_trampoline = nullptr;
		m_original_build_main_menu = nullptr;
		m_build_hooked = false;
		return true;
	}

	void MenuConnectHook::Remove()
	{
		InterlockedExchange(&m_click_route_enabled, 0);
		const bool text_restored = RemoveTextResolverHook();
		const bool call_restored = RemoveCreditsAddChildHook();
		const bool build_restored = RemoveBuildHook();
		if (text_restored && call_restored && build_restored)
			m_installed = false;
	}

	void* __fastcall MenuConnectHook::HookBuildMainMenu(void* menu, void*)
	{
		return Instance().HandleBuildMainMenu(menu);
	}

	void __fastcall MenuConnectHook::HookCreditsAddChild(void* parent, void*,
		void* child)
	{
		Instance().HandleCreditsAddChild(parent, child);
	}

	char __fastcall MenuConnectHook::HookTextResolver(void* text, void*,
		void* destination)
	{
		return Instance().HandleTextResolver(text, destination);
	}

	void __fastcall MenuConnectHook::HookConnectAction(void*, void*,
		std::uint32_t action)
	{
		Instance().HandleConnectAction(action);
	}

	void* MenuConnectHook::HandleBuildMainMenu(void* menu)
	{
		if (!m_original_build_main_menu)
			return nullptr;
		if (!m_reported_builder_seen)
		{
			m_reported_builder_seen = true;
			CoopRuntime::Instance().Log(
				"[menu] BuildMainMenu observed\r\n");
		}
		const void* const previous_menu = m_building_menu;
		const DWORD previous_thread = m_building_menu_thread;
		const bool previous_inserted = m_button_inserted_for_build;
		m_building_menu = menu;
		m_building_menu_thread = GetCurrentThreadId();
		m_button_inserted_for_build = false;
		void* const result = m_original_build_main_menu(menu);
		m_building_menu = const_cast<void*>(previous_menu);
		m_building_menu_thread = previous_thread;
		m_button_inserted_for_build = previous_inserted;
		CoopNetGame::Instance().QuitSessionToMainMenu();
		return result;
	}

	void MenuConnectHook::HandleCreditsAddChild(void* parent, void* child)
	{
		if (m_original_add_child)
			m_original_add_child(parent, child);

		if (!parent || !m_building_menu ||
			m_building_menu_thread != GetCurrentThreadId() ||
			m_button_inserted_for_build)
			return;
		if (!m_reported_credits_seen)
		{
			m_reported_credits_seen = true;
			CoopRuntime::Instance().Log(
				"[menu] Credits AddChild seam observed\r\n");
		}
		m_button_inserted_for_build = true;

		const retail::HudMenuMainRef menu = {
			retail::ToAddress(m_building_menu)
		};
		const retail::HudPaneRef container = { retail::ToAddress(parent) };
		retail::HudPaneRef button = {};
		if (!retail::NativeGameApi::CreateMainMenuButton(menu,
			gforce::kMenuConnectLabelResourceId, gforce::kMenuConnectAction,
			reinterpret_cast<void*>(&HookConnectAction), button) ||
			!retail::NativeGameApi::AddMenuChild(container, button))
		{
			if (!m_reported_insert_failure)
			{
				m_reported_insert_failure = true;
				CoopRuntime::Instance().Log(
					"[menu-error] native Connect by IP row creation failed\r\n");
			}
			return;
		}
		CoopRuntime::Instance().Log(
			"[menu] native Connect by IP row added\r\n");
	}

	char MenuConnectHook::HandleTextResolver(void* text, void* destination)
	{
		if (!m_original_text_resolver)
			return 0;
		const retail::LocalizedTextRef text_ref = { retail::ToAddress(text) };
		const retail::LocalizedTextView text_view(text_ref);
		std::uint32_t resource = 0;
		if (!text_ref || !destination || !text_view.ResourceId(resource) ||
			resource != gforce::kMenuConnectLabelResourceId)
		{
			return m_original_text_resolver(text, destination);
		}

		std::uint8_t language = 0;
		bool already_resolved = false;
		if (!retail::TryRead(gforce::kTextLanguage, language) ||
			!text_view.IsResolvedFor(language, already_resolved))
		{
			return m_original_text_resolver(text, destination);
		}
		if (!already_resolved)
		{
			const retail::GameStringRef output = {
				retail::ToAddress(destination)
			};
			if (!retail::NativeGameApi::AssignGameStringFromAnsi(output,
				kConnectByIpLabel) || !text_view.MarkResolvedFor(language))
			{
				text_view.Invalidate();
				return m_original_text_resolver(text, destination);
			}
		}
		if (!m_reported_label_resolved)
		{
			m_reported_label_resolved = true;
			CoopRuntime::Instance().Log(
				"[menu] Connect by IP label resolved\r\n");
		}
		return static_cast<char>(language);
	}

	void MenuConnectHook::HandleConnectAction(std::uint32_t action)
	{
		if (action != gforce::kMenuConnectAction ||
			InterlockedCompareExchange(&m_click_route_enabled, 0, 0) == 0)
			return;
		if (SteamManager)
		{
			SteamManager->RequestIpConnectionPrompt();
			CoopRuntime::Instance().Log(
				"[menu] Connect by IP clicked; F8 request queued\r\n");
		}
	}
}
