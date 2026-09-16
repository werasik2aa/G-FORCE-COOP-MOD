#include "chat_overlay.h"

#include "coop_netgame.h"
#include "coop_runtime.h"
#include "protocol/packet_view.h"
#include "world_sync.h"
#include "SteamWorksSDK/include/steamnetworkingsockets.h"

#include <algorithm>
#include <cwctype>
#include <cstring>

namespace coop
{
	namespace
	{
		constexpr std::size_t kMaxHistoryLines = 64;
		constexpr std::size_t kMaxPendingChatPackets = 128;
		constexpr std::size_t kMaxInputCharacters = 64;
		constexpr DWORD kKeyRepeatDelayMs = 400;
		constexpr DWORD kKeyRepeatIntervalMs = 50;
		constexpr int kPanelMargin = 16;
		constexpr int kPanelWidth = 560;
		constexpr int kPanelHeight = 230;
		constexpr wchar_t kPanelWindowClassName[] = L"GForceCoopChatPanel";

		bool IsStrictlyNewerSequence(std::uint32_t candidate,
			std::uint32_t previous)
		{
			return candidate != 0 && (previous == 0 ||
				static_cast<std::int32_t>(candidate - previous) > 0);
		}
	}

	ChatOverlay& ChatOverlay::Instance()
	{
		static ChatOverlay instance;
		return instance;
	}

	ChatOverlay::ChatOverlay() :
		m_key_state{},
		m_next_sequence(0),
		m_last_received_sequence(0),
		m_notification_until_tick(0),
		m_panel_window(nullptr),
		m_panel_parent(nullptr),
		m_panel_left(0),
		m_panel_top(0),
		m_panel_width(0),
		m_panel_height(0),
		m_visible(false)
	{
		InitializeSRWLock(&m_packet_lock);
	}

	void ChatOverlay::Toggle()
	{
		m_visible = !m_visible;
		const DWORD now = GetTickCount();
		for (std::size_t index = 0; index < m_key_state.size(); ++index)
		{
			KeyState& state = m_key_state[index];
			const int virtual_key = static_cast<int>(index);
			state.down = (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
			state.next_repeat_tick = now + kKeyRepeatDelayMs;
		}
		if (m_visible)
		{
			InterlockedExchange(&m_notification_until_tick, 0);
			AppendMessage(L"System", L"Chat: Enter sends, Esc or F10 closes.");
		}
		else
			DestroyPanelWindow();
	}

	bool ChatOverlay::IsInputActive() const
	{
		return m_visible;
	}

	std::wstring ChatOverlay::Trim(const std::wstring& value)
	{
		std::wstring::size_type first = 0;
		while (first < value.size() && iswspace(value[first]))
			++first;
		std::wstring::size_type last = value.size();
		while (last > first && iswspace(value[last - 1]))
			--last;
		return value.substr(first, last - first);
	}

	void ChatOverlay::HandleVirtualKey(int virtual_key,
		const BYTE keyboard_state[256])
	{
		if (virtual_key == VK_F10 || virtual_key == VK_ESCAPE)
		{
			m_visible = false;
			m_input.clear();
			DestroyPanelWindow();
			return;
		}
		if (virtual_key == VK_RETURN)
		{
			SubmitText();
			return;
		}
		if (virtual_key == VK_BACK)
		{
			if (!m_input.empty())
			{
				m_input.erase(m_input.size() - 1);
				RequestPanelRepaint();
			}
			return;
		}
		if (virtual_key == VK_TAB)
			return;

		const UINT scan_code = MapVirtualKeyW(
			static_cast<UINT>(virtual_key), MAPVK_VK_TO_VSC);
		wchar_t characters[4] = {};
		const int character_count = ToUnicodeEx(static_cast<UINT>(virtual_key),
			scan_code, keyboard_state, characters,
			static_cast<int>(_countof(characters)), 0,
			GetKeyboardLayout(0));
		if (character_count <= 0 || m_input.size() >= kMaxInputCharacters)
			return;

		const std::size_t remaining = kMaxInputCharacters - m_input.size();
		const std::size_t accepted = std::min<std::size_t>(
			static_cast<std::size_t>(character_count), remaining);
		m_input.append(characters, accepted);
		RequestPanelRepaint();
	}

	void ChatOverlay::TickInput()
	{
		if (!m_visible)
			return;

		BYTE keyboard_state[256] = {};
		if (!GetKeyboardState(keyboard_state))
			return;
		const DWORD now = GetTickCount();
		for (int virtual_key = 0; virtual_key < 256; ++virtual_key)
		{
			KeyState& state = m_key_state[virtual_key];
			const bool down = (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
			if (!down)
			{
				state = {};
				continue;
			}
			if (!state.down)
			{
				state.down = true;
				state.next_repeat_tick = now + kKeyRepeatDelayMs;
				HandleVirtualKey(virtual_key, keyboard_state);
				if (!m_visible)
					return;
				continue;
			}
			const bool allow_repeat = virtual_key != VK_F10 &&
				virtual_key != VK_ESCAPE && virtual_key != VK_RETURN;
			if (static_cast<DWORD>(now - state.next_repeat_tick) <
				0x80000000u && allow_repeat)
			{
				HandleVirtualKey(virtual_key, keyboard_state);
				state.next_repeat_tick = now + kKeyRepeatIntervalMs;
				if (!m_visible)
					return;
			}
		}
	}

	bool ChatOverlay::QueueOutgoingText(const std::wstring& raw_text)
	{
		const std::wstring text = Trim(raw_text);
		if (text.empty())
			return false;
		if (!CoopNetGame::Instance().HasRemotePeer())
		{
			AppendMessage(L"System", L"No connected peer.");
			return false;
		}

		protocol::ChatPacket packet = {};
		protocol::InitializeFixedPacket(packet, protocol::PacketKind::Chat);
		const int encoded_size = WideCharToMultiByte(CP_UTF8, 0,
			text.data(), static_cast<int>(text.size()), packet.utf8,
			static_cast<int>(sizeof(packet.utf8) - 1), nullptr, nullptr);
		if (encoded_size <= 0)
		{
			AppendMessage(L"System", L"Message is too long or invalid.");
			return false;
		}
		packet.utf8[encoded_size] = '\0';

		AcquireSRWLockExclusive(&m_packet_lock);
		if (m_outgoing.size() >= kMaxPendingChatPackets)
		{
			ReleaseSRWLockExclusive(&m_packet_lock);
			AppendMessage(L"System", L"Chat queue is full.");
			return false;
		}
		packet.sequence = ++m_next_sequence;
		if (packet.sequence == 0)
			packet.sequence = ++m_next_sequence;
		m_outgoing.push_back(packet);
		ReleaseSRWLockExclusive(&m_packet_lock);
		AppendMessage(L"You", text);
		return true;
	}

	void ChatOverlay::SubmitText()
	{
		if (QueueOutgoingText(m_input))
		{
			m_input.clear();
			RequestPanelRepaint();
		}
	}

	void ChatOverlay::AppendMessage(const wchar_t* sender,
		const std::wstring& text)
	{
		m_history.push_back(std::wstring(sender ? sender : L"Peer") +
			L": " + text);
		if (m_history.size() > kMaxHistoryLines)
			m_history.erase(m_history.begin());
		RequestPanelRepaint();
	}

	bool ChatOverlay::DecodeUtf8(const protocol::ChatPacket& packet,
		std::wstring& text)
	{
		std::size_t length = 0;
		while (length < sizeof(packet.utf8) && packet.utf8[length] != '\0')
			++length;
		if (length == 0 || length == sizeof(packet.utf8))
			return false;
		const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			packet.utf8, static_cast<int>(length), nullptr, 0);
		if (needed <= 0)
			return false;
		text.assign(static_cast<std::size_t>(needed), L'\0');
		return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			packet.utf8, static_cast<int>(length), &text[0], needed) == needed &&
			!Trim(text).empty();
	}

	bool ChatOverlay::OnRemotePacket(const void* data, std::uint32_t size)
	{
		const protocol::PacketView view(data, size);
		protocol::ChatPacket packet = {};
		if (view.Kind() != protocol::PacketKind::Chat ||
			!view.CopyUncompressedExact(packet))
		{
			return true;
		}
		std::wstring decoded;
		if (!DecodeUtf8(packet, decoded))
			return true;

		AcquireSRWLockExclusive(&m_packet_lock);
		if (IsStrictlyNewerSequence(packet.sequence, m_last_received_sequence) &&
			m_incoming.size() < kMaxPendingChatPackets)
		{
			m_last_received_sequence = packet.sequence;
			m_incoming.push_back(packet);
		}
		ReleaseSRWLockExclusive(&m_packet_lock);
		return true;
	}

	void ChatOverlay::GameTick()
	{
		std::vector<protocol::ChatPacket> incoming;
		AcquireSRWLockExclusive(&m_packet_lock);
		incoming.swap(m_incoming);
		ReleaseSRWLockExclusive(&m_packet_lock);
		for (const protocol::ChatPacket& packet : incoming)
		{
			std::wstring text;
			if (DecodeUtf8(packet, text))
			{
				AppendMessage(L"Peer", Trim(text));
				if (!m_visible)
					InterlockedExchange(&m_notification_until_tick,
						static_cast<LONG>(GetTickCount() + 6000u));
			}
		}
	}

	bool ChatOverlay::EnsurePanelWindow(HWND game_window, int left, int top,
		int width, int height)
	{
		if (!game_window || !IsWindow(game_window) || width <= 0 || height <= 0)
			return false;
		if (m_panel_window && !IsWindow(m_panel_window))
		{
			m_panel_window = nullptr;
			m_panel_parent = nullptr;
		}
		if (m_panel_window && m_panel_parent != game_window)
			DestroyPanelWindow();
		if (!m_panel_window)
		{
			WNDCLASSW window_class = {};
			window_class.lpfnWndProc = &ChatOverlay::PanelWindowProc;
			window_class.hInstance = GetModuleHandleW(nullptr);
			window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
			window_class.lpszClassName = kPanelWindowClassName;
			if (!RegisterClassW(&window_class) &&
				GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			{
				CoopRuntime::Instance().Log(
					"[chat] could not register retained GDI panel error=%lu\r\n",
					GetLastError());
				return false;
			}
			m_panel_window = CreateWindowExW(
				WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOPARENTNOTIFY |
					WS_EX_LAYERED,
				kPanelWindowClassName, L"", WS_POPUP,
				left, top, width, height, game_window, nullptr,
				GetModuleHandleW(nullptr), nullptr);
			if (!m_panel_window)
			{
				CoopRuntime::Instance().Log(
					"[chat] could not create retained GDI panel error=%lu\r\n",
					GetLastError());
				return false;
			}
			m_panel_parent = game_window;
			m_panel_left = left;
			m_panel_top = top;
			m_panel_width = width;
			m_panel_height = height;
			SetWindowPos(m_panel_window, HWND_TOP, left, top, width, height,
				SWP_NOACTIVATE | SWP_SHOWWINDOW);
			SetLayeredWindowAttributes(m_panel_window, 0,
				m_visible ? 238 : 196, LWA_ALPHA);
			UpdateWindow(m_panel_window);
			CoopRuntime::Instance().Log(
				"[chat] GDI panel visible owner=%p screen=(%d,%d) size=(%d,%d)\r\n",
				game_window, left, top, width, height);
			return true;
		}
		if (m_panel_left != left || m_panel_top != top ||
			m_panel_width != width || m_panel_height != height)
		{
			m_panel_left = left;
			m_panel_top = top;
			m_panel_width = width;
			m_panel_height = height;
			SetWindowPos(m_panel_window, HWND_TOP, left, top, width, height,
				SWP_NOACTIVATE | SWP_SHOWWINDOW);
		}
		else if (!IsWindowVisible(m_panel_window))
		{
			ShowWindow(m_panel_window, SW_SHOWNOACTIVATE);
			RequestPanelRepaint();
		}
		SetLayeredWindowAttributes(m_panel_window, 0,
			m_visible ? 238 : 196, LWA_ALPHA);
		return true;
	}

	void ChatOverlay::DestroyPanelWindow()
	{
		if (m_panel_window && IsWindow(m_panel_window))
			DestroyWindow(m_panel_window);
		m_panel_window = nullptr;
		m_panel_parent = nullptr;
		m_panel_left = 0;
		m_panel_top = 0;
		m_panel_width = 0;
		m_panel_height = 0;
	}

	void ChatOverlay::RequestPanelRepaint()
	{
		if (m_panel_window && IsWindow(m_panel_window))
		{
			RedrawWindow(m_panel_window, nullptr, nullptr,
				RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
		}
	}

	void ChatOverlay::PaintPanel(HDC device_context)
	{
		if (!device_context)
			return;
		RECT panel = {};
		if (!m_panel_window || !GetClientRect(m_panel_window, &panel))
			return;
		if (panel.right <= panel.left || panel.bottom <= panel.top)
			return;

		HBRUSH background = CreateSolidBrush(RGB(18, 22, 30));
		HBRUSH border = CreateSolidBrush(RGB(60, 170, 230));
		FillRect(device_context, &panel, background);
		FrameRect(device_context, &panel, border);
		DeleteObject(border);
		DeleteObject(background);

		SetBkMode(device_context, TRANSPARENT);
		const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
		HGDIOBJ previous_font = SelectObject(device_context, font);
		SetTextColor(device_context, RGB(225, 235, 245));

		RECT text_area = { panel.left + 10, panel.top + 8, panel.right - 10,
			m_visible ? panel.bottom - 30 : panel.bottom - 8 };
		const int text_height = static_cast<int>(
			text_area.bottom - text_area.top);
		const int visible_lines = text_height > 17 ? text_height / 17 : 1;
		const std::size_t start = m_history.size() >
			static_cast<std::size_t>(visible_lines) ?
			m_history.size() - static_cast<std::size_t>(visible_lines) : 0;
		int y = text_area.top;
		for (std::size_t index = start; index < m_history.size(); ++index)
		{
			RECT line = { text_area.left, y, text_area.right, y + 17 };
			DrawTextW(device_context, m_history[index].c_str(), -1, &line,
				DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
			y += 17;
		}

		if (m_visible)
		{
			SetTextColor(device_context, RGB(125, 220, 255));
			RECT input = { panel.left + 10, panel.bottom - 24, panel.right - 10,
				panel.bottom - 6 };
			const std::wstring prompt = L"> " + m_input + L"_";
			DrawTextW(device_context, prompt.c_str(), -1, &input,
				DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		}
		SelectObject(device_context, previous_font);
	}

	LRESULT CALLBACK ChatOverlay::PanelWindowProc(HWND window, UINT message,
		WPARAM wparam, LPARAM lparam)
	{
		switch (message)
		{
		case WM_ERASEBKGND:
			return 1;
		case WM_MOUSEACTIVATE:
			return MA_NOACTIVATE;
		case WM_NCHITTEST:
			return HTTRANSPARENT;
		case WM_PAINT:
			{
				PAINTSTRUCT paint = {};
				HDC device_context = BeginPaint(window, &paint);
				ChatOverlay::Instance().PaintPanel(device_context);
				EndPaint(window, &paint);
				return 0;
			}
		default:
			return DefWindowProcW(window, message, wparam, lparam);
		}
	}

	void ChatOverlay::Render(HWND game_window)
	{
		const DWORD now = GetTickCount();
		const DWORD notification_until = static_cast<DWORD>(
			InterlockedCompareExchange(&m_notification_until_tick, 0, 0));
		const bool notification_visible = notification_until != 0 &&
			static_cast<DWORD>(notification_until - now) < 0x80000000u;
		if (!m_visible && !notification_visible)
		{
			InterlockedExchange(&m_notification_until_tick, 0);
			DestroyPanelWindow();
			return;
		}
		if (!game_window || !IsWindow(game_window))
			return;
		if (!IsWindowVisible(game_window) || IsIconic(game_window))
		{
			if (m_panel_window && IsWindow(m_panel_window))
				ShowWindow(m_panel_window, SW_HIDE);
			return;
		}
		RECT client = {};
		if (!GetClientRect(game_window, &client))
			return;
		const int client_width = client.right - client.left;
		const int client_height = client.bottom - client.top;
		const int panel_width = std::min(kPanelWidth,
			client_width - kPanelMargin * 2);
		const int requested_height = m_visible ? kPanelHeight : 76;
		const int panel_height = std::min(requested_height,
			client_height - kPanelMargin * 2);
		if (panel_width <= 0 || panel_height <= 0)
			return;
		POINT screen_origin = { kPanelMargin,
			client_height - kPanelMargin - panel_height };
		if (!ClientToScreen(game_window, &screen_origin))
			return;
		EnsurePanelWindow(game_window, screen_origin.x, screen_origin.y, panel_width,
			panel_height);
	}

	void ChatOverlay::NetworkTick()
	{
		if (!CoopNetGame::Instance().HasRemotePeer())
			return;
		std::vector<protocol::ChatPacket> outgoing;
		AcquireSRWLockExclusive(&m_packet_lock);
		outgoing.swap(m_outgoing);
		ReleaseSRWLockExclusive(&m_packet_lock);
		for (const protocol::ChatPacket& packet : outgoing)
		{
			WorldSync::Instance().SendToRemote(&packet, sizeof(packet),
				k_nSteamNetworkingSend_Reliable);
		}
	}

	void ChatOverlay::OnPeerDisconnected()
	{
		AcquireSRWLockExclusive(&m_packet_lock);
		m_outgoing.clear();
		m_incoming.clear();
		m_next_sequence = 0;
		m_last_received_sequence = 0;
		InterlockedExchange(&m_notification_until_tick, 0);
		ReleaseSRWLockExclusive(&m_packet_lock);
	}

	void ChatOverlay::Shutdown()
	{
		OnPeerDisconnected();
		m_visible = false;
		m_input.clear();
		m_history.clear();
		for (KeyState& state : m_key_state)
			state = {};
	}
}
