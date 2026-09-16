#include "debug_actions.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "coop_netgame.h"
#include "coop_runtime.h"
#include "chat_overlay.h"
#include "gforce_constants.h"
#include "player2.h"
#include "retail/retail_views.h"
#include "world_sync.h"

namespace coop
{
	using namespace gforce;

	namespace
	{
		bool IsCurrentProcessForeground()
		{
			const HWND foreground = ::GetForegroundWindow();
			if (!foreground)
				return false;
			DWORD process_id = 0;
			GetWindowThreadProcessId(foreground, &process_id);
			return process_id == GetCurrentProcessId();
		}

		bool LocalPlayer1Exists()
		{
			retail::EntitySlotRepository players;
			retail::EntitySlotBinding player1 = {};
			return players.GetBinding(retail::EntitySlot::LocalP1, player1);
		}
	}

	DebugActions& DebugActions::Instance()
	{
		static DebugActions instance;
		return instance;
	}

	DebugActions::DebugActions() : m_enter_was_down(false)
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
		m_enter_was_down = false;
	}

	void DebugActions::Tick()
	{
		if (!IsCurrentProcessForeground())
		{
			// Keep the edge detector in sync while the other game process is active,
			// so changing window with an already-held ENTER cannot toggle chat here.
			m_enter_was_down =
				(::GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
			return;
		}
		if (ChatOverlay::Instance().IsInputActive())
		{
			// The in-client GDI panel owns text keys while it is visible. It has no
			// child HWND and no WndProc hook, so the active game process polls it
			// at this normal post-P1 seam.
			ChatOverlay::Instance().TickInput();
			m_enter_was_down =
				(::GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
			return;
		}

		if (ConsumePressed(VK_RETURN, m_enter_was_down) && LocalPlayer1Exists())
			ChatOverlay::Instance().Toggle();
	}
}
