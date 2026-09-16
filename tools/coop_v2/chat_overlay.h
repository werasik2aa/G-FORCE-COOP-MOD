#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "protocol/chat_packets.h"

namespace coop
{
	// F10 opens a retained GDI owned panel over the game client. It deliberately
	// does not paint straight into the D3D Present DC: the next flip can erase
	// that transient drawing and make the chat flash.  Transport stays independent
	// from game input: only this class consumes text while the panel is visible.
	class ChatOverlay final
	{
	public:
		static ChatOverlay& Instance();

		void Toggle();
		bool IsInputActive() const;
		// Called after P1's stock tick while this process owns foreground input.
		void TickInput();
		void GameTick();
		// Called after the original D3D9 Present has completed successfully. It
		// maintains the owned panel layout; the panel paints itself through WM_PAINT.
		void Render(HWND game_window);
		void NetworkTick();
		bool OnRemotePacket(const void* data, std::uint32_t size);
		void OnPeerDisconnected();
		void Shutdown();

	private:
		struct KeyState
		{
			bool down;
			DWORD next_repeat_tick;
		};

		ChatOverlay();
		~ChatOverlay() = default;
		ChatOverlay(const ChatOverlay&) = delete;
		ChatOverlay& operator=(const ChatOverlay&) = delete;

		void HandleVirtualKey(int virtual_key, const BYTE keyboard_state[256]);
		void SubmitText();
		bool QueueOutgoingText(const std::wstring& text);
		void AppendMessage(const wchar_t* sender, const std::wstring& text);
		bool EnsurePanelWindow(HWND game_window, int left, int top,
			int width, int height);
		void DestroyPanelWindow();
		void RequestPanelRepaint();
		void PaintPanel(HDC device_context);
		static LRESULT CALLBACK PanelWindowProc(HWND window, UINT message,
			WPARAM wparam, LPARAM lparam);

		static bool DecodeUtf8(const protocol::ChatPacket& packet,
			std::wstring& text);
		static std::wstring Trim(const std::wstring& value);

		SRWLOCK m_packet_lock;
		std::vector<protocol::ChatPacket> m_outgoing;
		std::vector<protocol::ChatPacket> m_incoming;
		std::vector<std::wstring> m_history;
		std::array<KeyState, 256> m_key_state;
		std::wstring m_input;
		std::uint32_t m_next_sequence;
		std::uint32_t m_last_received_sequence;
		volatile LONG m_notification_until_tick;
		HWND m_panel_window;
		HWND m_panel_parent;
		int m_panel_left;
		int m_panel_top;
		int m_panel_width;
		int m_panel_height;
		bool m_visible;
	};
}
