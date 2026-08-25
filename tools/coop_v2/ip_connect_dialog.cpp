#include "ip_connect_dialog.h"

namespace
{
const char kDialogClassName[] = "GForceCoopIpConnectDialog";
const int kDialogWidth = 440;
const int kDialogHeight = 154;
const int kAddressControlId = 1001;

int CenteredCoordinate(int origin, int extent, int size)
{
	return origin + (extent - size) / 2;
}
}

IpConnectDialog::IpConnectDialog() :
	m_window(NULL),
	m_edit(NULL),
	m_owner(NULL),
	m_output(NULL),
	m_output_size(0),
	m_owner_was_enabled(false),
	m_finished(false),
	m_accepted(false)
{
}

bool IpConnectDialog::RegisterWindowClass()
{
	static ATOM atom = 0;
	if (atom)
		return true;

	WNDCLASSEXA window_class = {};
	window_class.cbSize = sizeof(window_class);
	window_class.lpfnWndProc = WindowProc;
	window_class.hInstance = GetModuleHandleA(NULL);
	window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
	window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	window_class.lpszClassName = kDialogClassName;
	atom = RegisterClassExA(&window_class);
	return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool IpConnectDialog::Prompt(HWND owner, const char* default_address,
	char* address, std::size_t address_size)
{
	if (!address || address_size < 2 || !RegisterWindowClass())
		return false;

	address[0] = '\0';
	m_owner = owner;
	m_output = address;
	m_output_size = address_size;
	m_finished = false;
	m_accepted = false;

	RECT bounds = { 0, 0, kDialogWidth, kDialogHeight };
	if (m_owner && GetWindowRect(m_owner, &bounds))
	{
		const int width = bounds.right - bounds.left;
		const int height = bounds.bottom - bounds.top;
		bounds.left = CenteredCoordinate(bounds.left, width, kDialogWidth);
		bounds.top = CenteredCoordinate(bounds.top, height, kDialogHeight);
	}
	else
	{
		bounds.left = CenteredCoordinate(0, GetSystemMetrics(SM_CXSCREEN),
			kDialogWidth);
		bounds.top = CenteredCoordinate(0, GetSystemMetrics(SM_CYSCREEN),
			kDialogHeight);
	}

	m_window = CreateWindowExA(WS_EX_DLGMODALFRAME,
		kDialogClassName, "G-Force Co-op: Connect by IP",
		WS_POPUP | WS_CAPTION | WS_SYSMENU,
		bounds.left, bounds.top, kDialogWidth, kDialogHeight, m_owner, NULL,
		GetModuleHandleA(NULL), this);
	if (!m_window)
		return false;

	CreateControls();
	SetWindowTextA(m_edit, default_address ? default_address : "");
	// Only disable the owner if we share its thread; cross-thread
	// EnableWindow sends a synchronous message that can deadlock the
	// game's main thread while the worker's dialog pumps messages.
	m_owner_was_enabled = m_owner && IsWindowEnabled(m_owner) &&
		GetWindowThreadProcessId(m_owner, NULL) == GetCurrentThreadId();
	if (m_owner_was_enabled)
		EnableWindow(m_owner, FALSE);
	ShowWindow(m_window, SW_SHOW);
	SetForegroundWindow(m_window);
	SetFocus(m_edit);
	SendMessageA(m_edit, EM_SETSEL, 0, -1);

	MSG message = {};
	while (!m_finished)
	{
		const BOOL result = GetMessageA(&message, NULL, 0, 0);
		if (result <= 0)
		{
			if (result == 0)
				PostQuitMessage(static_cast<int>(message.wParam));
			Complete(false);
			break;
		}
		if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN)
		{
			Complete(true);
			continue;
		}
		if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE)
		{
			Complete(false);
			continue;
		}
		TranslateMessage(&message);
		DispatchMessageA(&message);
	}

	if (m_owner_was_enabled && m_owner)
	{
		EnableWindow(m_owner, TRUE);
		SetForegroundWindow(m_owner);
	}
	return m_accepted;
}

void IpConnectDialog::CreateControls()
{
	CreateWindowExA(0, "STATIC", "Host IP address and port:",
		WS_CHILD | WS_VISIBLE, 20, 18, 390, 18, m_window, NULL,
		GetModuleHandleA(NULL), NULL);
	m_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
		20, 40, 400, 24, m_window,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddressControlId)),
		GetModuleHandleA(NULL), NULL);
	CreateWindowExA(0, "STATIC", "Enter - connect    Esc - cancel",
		WS_CHILD | WS_VISIBLE, 20, 82, 390, 18, m_window, NULL,
		GetModuleHandleA(NULL), NULL);
	CreateWindowExA(0, "STATIC",
		"Port is optional (44139). Example: 192.168.1.25",
		WS_CHILD | WS_VISIBLE, 20, 105, 390, 18, m_window, NULL,
		GetModuleHandleA(NULL), NULL);
}

void IpConnectDialog::Complete(bool accepted)
{
	if (m_finished)
		return;

	if (accepted && m_edit && m_output && m_output_size > 0)
	{
		GetWindowTextA(m_edit, m_output, static_cast<int>(m_output_size));
		m_accepted = m_output[0] != '\0';
	}
	m_finished = true;
	if (m_window)
		DestroyWindow(m_window);
}

LRESULT CALLBACK IpConnectDialog::WindowProc(HWND window, UINT message,
	WPARAM wparam, LPARAM lparam)
{
	IpConnectDialog* dialog = reinterpret_cast<IpConnectDialog*>(
		GetWindowLongPtrA(window, GWLP_USERDATA));
	if (message == WM_NCCREATE)
	{
		CREATESTRUCTA* create = reinterpret_cast<CREATESTRUCTA*>(lparam);
		dialog = static_cast<IpConnectDialog*>(create->lpCreateParams);
		SetWindowLongPtrA(window, GWLP_USERDATA,
			reinterpret_cast<LONG_PTR>(dialog));
		if (dialog)
			dialog->m_window = window;
	}

	if (dialog && message == WM_CLOSE)
	{
		dialog->Complete(false);
		return 0;
	}
	if (message == WM_DESTROY)
	{
		if (dialog)
			dialog->m_window = NULL;
		return 0;
	}
	return DefWindowProcA(window, message, wparam, lparam);
}
