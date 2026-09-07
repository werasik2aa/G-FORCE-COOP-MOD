#include "debug_actions.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "coop_netgame.h"
#include "coop_runtime.h"
#include "gforce_constants.h"
#include "player2.h"
#include "retail/retail_views.h"
#include "window_hook.h"
#include "world_sync.h"

namespace coop
{
	using namespace gforce;

	namespace
	{
		enum DebugKeyIndex
		{
			DebugFlyDualLaser,
			DebugTriggerSpawn,
			DebugRecordedEvent,
			DebugKnownInteractive,
			DebugPlayer2Spawn,
			DebugAbr,
			DebugPauseBypass,
			DebugInteractiveCatalog,
			DebugKeyCount = static_cast<int>(kDebugActionCount)
		};

		const std::array<int, kDebugActionCount> kDebugVirtualKeys = {
			VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F9
		};
		static_assert(static_cast<std::size_t>(DebugKeyCount) ==
			kDebugActionCount, "debug key table and state storage must match");

		bool IsCurrentProcessForeground()
		{
			const HWND foreground = ::GetForegroundWindow();
			if (!foreground)
				return false;
			DWORD process_id = 0;
			GetWindowThreadProcessId(foreground, &process_id);
			return process_id == GetCurrentProcessId();
		}
	}

	DebugActions& DebugActions::Instance()
	{
		static DebugActions instance;
		return instance;
	}

	DebugActions::DebugActions() : m_key_was_down{}
	{}

	bool DebugActions::ConsumePressed(int virtual_key, bool& was_down) const
	{
		const bool is_down = (::GetAsyncKeyState(virtual_key) & 0x8000) != 0;
		const bool pressed = is_down && !was_down;
		was_down = is_down;
		return pressed;
	}

	void DebugActions::ResetForWorldLoad()
	{
		for (bool& was_down : m_key_was_down)
			was_down = false;
	}

	bool Player2Module::EnsureLocalPlayer2ForDebug()
	{
		retail::EntitySlotRepository players;
		retail::EntitySlotBinding player2 = {};
		if (players.GetBinding(retail::EntitySlot::RemoteP2, player2))
		{
			m_debug_player2_enabled = true;
			InterlockedExchange(&m_player2_ready, 1);
			CoopRuntime::Instance().Log(
				"[debug-F5] P2 already exists entity=%p controller=%p\r\n",
				retail::ToPointer(player2.entity.value),
				retail::ToPointer(player2.controller.value));
			return true;
		}

		retail::EntitySlotBinding player1 = {};
		if (!players.GetBinding(retail::EntitySlot::LocalP1, player1))
		{
			CoopRuntime::Instance().Log(
				"[debug-F5] ignored: P1 controller is not ready\r\n");
			return false;
		}
		const std::uint32_t mode = GetModeId(
			retail::ToPointer(player1.controller.value));
		if (mode != kDefaultModeId && mode != kAbrModeId)
		{
			CoopRuntime::Instance().Log(
				"[debug-F5] ignored: P1 mode 0x%08X is not a safe P2 factory boundary\r\n",
				mode);
			return false;
		}

		m_debug_player2_enabled = true;
		const bool spawned = SpawnPlayer2FromSnapshot("debug-F5", true);
		if (!spawned)
			m_debug_player2_enabled = false;
		return spawned;
	}

	bool Player2Module::EnableLocalAbrForDebug()
	{
		if (!EnsureLocalPlayer2ForDebug())
			return false;

		retail::EntitySlotRepository players;
		const retail::EntityRef player1 = players.GetSelectable(
			retail::EntitySlot::LocalP1);
		void* const controller = GetController(retail::ToPointer(player1.value));
		if (!controller)
		{
			CoopRuntime::Instance().Log(
				"[debug-F6] ignored: P1 controller is not ready\r\n");
			return false;
		}
		if (GetModeId(controller) == kAbrModeId)
		{
			CoopRuntime::Instance().Log("[debug-F6] P1 is already in ABR\r\n");
			return true;
		}
		const retail::ControllerRef controller_ref = {
			retail::ToAddress(controller)
		};
		if (!retail::ControllerView(controller_ref).SelectMode(kAbrModeId))
		{
			CoopRuntime::Instance().Log("[debug-F6] native P1 ABR mode request failed\r\n");
			return false;
		}
		const bool entered = GetModeId(controller) == kAbrModeId;
		CoopRuntime::Instance().Log(
			"[debug-F6] P1 ABR mode request result=%u\r\n", entered ? 1u : 0u);
		return entered;
	}

	void DebugActions::Tick()
	{
		CoopNetGame::Instance().TickDebugFlyDualLaser();
		if (!IsCurrentProcessForeground())
		{
			// Keep the edge detector in sync while the other game process is active,
			// so changing window with an already-held F key cannot run an action here.
			for (std::size_t index = 0; index < m_key_was_down.size(); ++index)
			{
				m_key_was_down[index] =
					(::GetAsyncKeyState(kDebugVirtualKeys[index]) & 0x8000) != 0;
			}
			return;
		}

		if (ConsumePressed(VK_F1, m_key_was_down[DebugFlyDualLaser]))
			CoopNetGame::Instance().RequestDebugFlyDualLaser();
		if (ConsumePressed(VK_F2, m_key_was_down[DebugTriggerSpawn]))
			WorldSync::Instance().DebugSpawnNearestTrigger();
		if (ConsumePressed(VK_F3, m_key_was_down[DebugRecordedEvent]))
			WorldSync::Instance().DebugDispatchNearestRecordedEvent();
		if (ConsumePressed(VK_F4, m_key_was_down[DebugKnownInteractive]))
			WorldSync::Instance().DebugActivateNearestKnownInteractive();
		if (ConsumePressed(VK_F5, m_key_was_down[DebugPlayer2Spawn]))
			Player2Module::Instance().EnsureLocalPlayer2ForDebug();
		if (ConsumePressed(VK_F6, m_key_was_down[DebugAbr]))
			Player2Module::Instance().EnableLocalAbrForDebug();
		if (ConsumePressed(VK_F7, m_key_was_down[DebugPauseBypass]))
			WindowHook::Instance().EnableDebugPauseBypass();
		if (ConsumePressed(VK_F9, m_key_was_down[DebugInteractiveCatalog]))
			WorldSync::Instance().DebugLogInteractiveCandidates();
	}
}
