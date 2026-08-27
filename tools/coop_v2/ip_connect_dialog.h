#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstddef>

// A deliberately small native dialog for the one value the standalone socket
// needs.  It owns a private message loop on the network worker, so it never
// injects synthetic keys into the game's main-menu input path.
class IpConnectDialog final
{
public:
	IpConnectDialog();

	bool Prompt(HWND owner, const char* default_address, char* address, std::size_t address_size);
private:
	const int CenteredCoordinate(int origin, int extent, int size) { return origin + (extent - size) / 2; }
	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
	static bool RegisterWindowClass();

	void CreateControls();
	void Complete(bool accepted);

	HWND m_window;
	HWND m_edit;
	HWND m_owner;
	char* m_output;
	std::size_t m_output_size;
	bool m_owner_was_enabled;
	bool m_finished;
	bool m_accepted;

	const int kDialogWidth = 440;
	const int kDialogHeight = 154;
	const int kAddressControlId = 1001;
};
