#include "window_hook.h"

#include "coop_runtime.h"
#include "save_sync.h"

namespace coop
{
WindowHook& WindowHook::Instance()
{
	static WindowHook instance;
	return instance;
}

WindowHook::WindowHook() :
	m_original_direct3d_create9(NULL),
	m_original_d3d_create_device(NULL),
	m_original_d3d_reset(NULL),
	m_original_d3d_present(NULL),
	m_original_get_foreground_window(NULL),
	m_original_is_iconic(NULL),
	m_direct3d_create9_iat(NULL),
	m_d3d_create_device_slot(NULL),
	m_d3d_reset_slot(NULL),
	m_d3d_present_slot(NULL),
	m_get_foreground_window_iat(NULL),
	m_is_iconic_iat(NULL),
	m_original_window_procedure(NULL),
	m_window_procedure_window(NULL),
	m_game_window(NULL)
{
}

void WindowHook::ApplyExperimentalWindowStyle(HWND window)
{
	if (!window || !CoopRuntime::Instance().Config().test_windowed)
		return;

	const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
		WS_MINIMIZEBOX;
	const DWORD extended_style = WS_EX_APPWINDOW;
	SetWindowLongPtrA(window, GWL_STYLE, static_cast<LONG_PTR>(style));
	SetWindowLongPtrA(window, GWL_EXSTYLE,
		static_cast<LONG_PTR>(extended_style));

	RECT rectangle = {0, 0, CoopRuntime::Instance().Config().window_width, CoopRuntime::Instance().Config().window_height};
	AdjustWindowRectEx(&rectangle, style, FALSE, extended_style);
	const int outer_width = rectangle.right - rectangle.left;
	const int outer_height = rectangle.bottom - rectangle.top;

	MONITORINFO monitor_info = {};
	monitor_info.cbSize = sizeof(monitor_info);
	HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
	int x = 0;
	int y = 0;
	if (GetMonitorInfoA(monitor, &monitor_info))
	{
		x = monitor_info.rcWork.left +
			(monitor_info.rcWork.right - monitor_info.rcWork.left -
				outer_width) / 2;
		y = monitor_info.rcWork.top +
			(monitor_info.rcWork.bottom - monitor_info.rcWork.top -
				outer_height) / 2;
	}
	SetWindowPos(window, HWND_NOTOPMOST, x, y, outer_width, outer_height,
		SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void WindowHook::ConfigureWindowedPresentation(HWND focus_window, DWORD* behavior_flags,
	D3DPRESENT_PARAMETERS* parameters)
{
	if (!CoopRuntime::Instance().Config().test_windowed || !parameters)
		return;

	if (focus_window)
		m_game_window = focus_window;
	InstallWindowProcedureHook();
	if (!parameters->hDeviceWindow)
		parameters->hDeviceWindow = m_game_window;
	parameters->BackBufferWidth = static_cast<UINT>(CoopRuntime::Instance().Config().window_width);
	parameters->BackBufferHeight = static_cast<UINT>(CoopRuntime::Instance().Config().window_height);
	parameters->Windowed = TRUE;
	parameters->FullScreen_RefreshRateInHz = 0;
	if (behavior_flags)
		*behavior_flags |= D3DCREATE_NOWINDOWCHANGES;
	ApplyExperimentalWindowStyle(m_game_window);
}

void WindowHook::InstallWindowProcedureHook()
{
	if (!m_game_window || m_window_procedure_window)
		return;
	SetLastError(0);
	const LONG_PTR original = SetWindowLongPtrA(m_game_window, GWLP_WNDPROC,
		reinterpret_cast<LONG_PTR>(&HookWindowProcedure));
	if (original == 0 && GetLastError() != 0)
	{
		CoopRuntime::Instance().Log("[window-warning] unable to hook window procedure\r\n");
		return;
	}
	m_original_window_procedure = reinterpret_cast<WNDPROC>(original);
	m_window_procedure_window = m_game_window;
	ForceGameActiveState();
	CoopRuntime::Instance().Log("[window] background activation message hook installed\r\n");
}

void WindowHook::ForceGameActiveState()
{
	if (!CoopRuntime::Instance().Config().keep_active_in_background ||
		!m_game_window)
	{
		return;
	}
	// 0x5F4850 computes this byte from foreground/minimized state.  It is the
	// flag the frame loop uses to pause its simulation and renderer.
	*reinterpret_cast<volatile BYTE*>(0x0091AACCu) = 1;
}

HRESULT WINAPI WindowHook::HookD3DReset(IDirect3DDevice9* device,
	D3DPRESENT_PARAMETERS* parameters)
{
	return Instance().Reset(device, parameters);
}

HRESULT WindowHook::Reset(IDirect3DDevice9* device,
	D3DPRESENT_PARAMETERS* parameters)
{
	ConfigureWindowedPresentation(m_game_window, NULL, parameters);
	const HRESULT result = m_original_d3d_reset ?
		m_original_d3d_reset(device, parameters) : D3DERR_INVALIDCALL;
	if (SUCCEEDED(result))
		ApplyExperimentalWindowStyle(m_game_window);
	return result;
}

HRESULT WINAPI WindowHook::HookD3DPresent(IDirect3DDevice9* device,
	const RECT* source_rectangle, const RECT* destination_rectangle,
	HWND destination_window, const RGNDATA* dirty_region)
{
	return Instance().Present(device, source_rectangle, destination_rectangle,
		destination_window, dirty_region);
}

HRESULT WindowHook::Present(IDirect3DDevice9* device,
	const RECT* source_rectangle, const RECT* destination_rectangle,
	HWND destination_window, const RGNDATA* dirty_region)
{
	ForceGameActiveState();
	SaveSync::Instance().OnMainFrame();
	return m_original_d3d_present ? m_original_d3d_present(device,
		source_rectangle, destination_rectangle, destination_window,
		dirty_region) : D3DERR_INVALIDCALL;
}

HRESULT WINAPI WindowHook::HookD3DCreateDevice(
	IDirect3D9* direct3d, UINT adapter, D3DDEVTYPE device_type,
	HWND focus_window, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device)
{
	return Instance().CreateDevice(direct3d, adapter, device_type,
		focus_window, behavior_flags, parameters, device);
}

HRESULT WindowHook::CreateDevice(
	IDirect3D9* direct3d, UINT adapter, D3DDEVTYPE device_type,
	HWND focus_window, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device)
{
	ConfigureWindowedPresentation(focus_window, &behavior_flags, parameters);
	const HRESULT result = m_original_d3d_create_device ?
		m_original_d3d_create_device(direct3d, adapter, device_type,
			focus_window, behavior_flags, parameters, device) :
		D3DERR_INVALIDCALL;
	if (FAILED(result) || !device || !*device)
	{
		CoopRuntime::Instance().Log("[window] CreateDevice failed result=0x%08X\r\n",
			static_cast<unsigned>(result));
		return result;
	}

	void** vtable = *reinterpret_cast<void***>(*device);
	void** reset_slot = &vtable[16];
	if (*reset_slot != reinterpret_cast<void*>(&HookD3DReset))
	{
		m_original_d3d_reset = reinterpret_cast<D3DResetFn>(*reset_slot);
		void* replacement = reinterpret_cast<void*>(&HookD3DReset);
		if (MemoryPatch::Write(reset_slot, &replacement, sizeof(replacement)))
		{
			m_d3d_reset_slot = reset_slot;
		}
	}
	void** present_slot = &vtable[17];
	if (*present_slot != reinterpret_cast<void*>(&HookD3DPresent))
	{
		m_original_d3d_present = reinterpret_cast<D3DPresentFn>(*present_slot);
		void* replacement = reinterpret_cast<void*>(&HookD3DPresent);
		if (MemoryPatch::Write(present_slot, &replacement, sizeof(replacement)))
			m_d3d_present_slot = present_slot;
	}
	ApplyExperimentalWindowStyle(m_game_window);
	CoopRuntime::Instance().Log("[window] D3D9 windowed device ready client=%dx%d hwnd=%p\r\n",
		CoopRuntime::Instance().Config().window_width, CoopRuntime::Instance().Config().window_height, m_game_window);
	return result;
}

IDirect3D9* WINAPI WindowHook::HookDirect3DCreate9(UINT sdk_version)
{
	return Instance().CreateDirect3D9(sdk_version);
}

HWND WINAPI WindowHook::HookGetForegroundWindow()
{
	return Instance().GetForegroundWindow();
}

BOOL WINAPI WindowHook::HookIsIconic(HWND window)
{
	return Instance().IsIconic(window);
}

IDirect3D9* WindowHook::CreateDirect3D9(UINT sdk_version)
{
	IDirect3D9* direct3d = m_original_direct3d_create9 ?
		m_original_direct3d_create9(sdk_version) : NULL;
	if (!direct3d)
		return NULL;

	void** vtable = *reinterpret_cast<void***>(direct3d);
	void** create_device_slot = &vtable[16];
	if (*create_device_slot != reinterpret_cast<void*>(&HookD3DCreateDevice))
	{
		m_original_d3d_create_device =
			reinterpret_cast<D3DCreateDeviceFn>(*create_device_slot);
		void* replacement = reinterpret_cast<void*>(&HookD3DCreateDevice);
		if (MemoryPatch::Write(create_device_slot, &replacement, sizeof(replacement)))
			m_d3d_create_device_slot = create_device_slot;
	}
	CoopRuntime::Instance().Log("[window] Direct3DCreate9 intercepted\r\n");
	return direct3d;
}

HWND WindowHook::GetForegroundWindow()
{
	if (CoopRuntime::Instance().Config().keep_active_in_background &&
		m_game_window)
	{
		return m_game_window;
	}
	return m_original_get_foreground_window ?
		m_original_get_foreground_window() : NULL;
}

BOOL WindowHook::IsIconic(HWND window)
{
	if (CoopRuntime::Instance().Config().keep_active_in_background &&
		window && window == m_game_window)
	{
		return FALSE;
	}
	return m_original_is_iconic ? m_original_is_iconic(window) : FALSE;
}

LRESULT CALLBACK WindowHook::HookWindowProcedure(HWND window, UINT message,
	WPARAM w_param, LPARAM l_param)
{
	return Instance().WindowProcedure(window, message, w_param, l_param);
}

LRESULT WindowHook::WindowProcedure(HWND window, UINT message,
	WPARAM w_param, LPARAM l_param)
{
	const bool changes_activation = message == WM_ACTIVATE ||
		message == WM_ACTIVATEAPP || message == WM_SETFOCUS ||
		message == WM_KILLFOCUS || message == WM_SIZE;
	if (changes_activation)
		ForceGameActiveState();
	const LRESULT result = m_original_window_procedure ?
		CallWindowProcA(m_original_window_procedure, window, message, w_param,
			l_param) : DefWindowProcA(window, message, w_param, l_param);
	if (changes_activation)
		ForceGameActiveState();
	return result;
}

bool WindowHook::Install()
{
	if (!CoopRuntime::Instance().Config().test_windowed)
		return true;
	m_direct3d_create9_iat = MemoryPatch::FindImportAddress("d3d9.dll", "Direct3DCreate9");
	if (!m_direct3d_create9_iat)
	{
		CoopRuntime::Instance().Log("[window-error] Direct3DCreate9 import not found\r\n");
		return false;
	}
	m_original_direct3d_create9 =
		reinterpret_cast<Direct3DCreate9Fn>(*m_direct3d_create9_iat);
	void* replacement = reinterpret_cast<void*>(&HookDirect3DCreate9);
	if (!MemoryPatch::Write(m_direct3d_create9_iat, &replacement,
		sizeof(replacement)))
	{
		CoopRuntime::Instance().Log("[window-error] unable to patch Direct3DCreate9 import\r\n");
		return false;
	}

	if (CoopRuntime::Instance().Config().keep_active_in_background)
	{
		m_get_foreground_window_iat =
			MemoryPatch::FindImportAddress("user32.dll", "GetForegroundWindow");
		m_is_iconic_iat = MemoryPatch::FindImportAddress("user32.dll", "IsIconic");
		if (!m_get_foreground_window_iat || !m_is_iconic_iat)
		{
			CoopRuntime::Instance().Log("[window-warning] focus pause imports not found\r\n");
			m_get_foreground_window_iat = NULL;
			m_is_iconic_iat = NULL;
		}
		else
		{
			m_original_get_foreground_window =
				reinterpret_cast<GetForegroundWindowFn>(*m_get_foreground_window_iat);
			m_original_is_iconic = reinterpret_cast<IsIconicFn>(*m_is_iconic_iat);
			void* foreground_replacement =
				reinterpret_cast<void*>(&HookGetForegroundWindow);
			void* iconic_replacement = reinterpret_cast<void*>(&HookIsIconic);
			if (!MemoryPatch::Write(m_get_foreground_window_iat,
				&foreground_replacement, sizeof(foreground_replacement)) ||
				!MemoryPatch::Write(m_is_iconic_iat, &iconic_replacement,
					sizeof(iconic_replacement)))
			{
				void* foreground_original =
					reinterpret_cast<void*>(m_original_get_foreground_window);
				MemoryPatch::Write(m_get_foreground_window_iat, &foreground_original,
					sizeof(foreground_original));
				m_get_foreground_window_iat = NULL;
				m_is_iconic_iat = NULL;
				m_original_get_foreground_window = NULL;
				m_original_is_iconic = NULL;
				CoopRuntime::Instance().Log("[window-warning] unable to patch focus pause imports\r\n");
			}
			else
			{
				CoopRuntime::Instance().Log("[window] foreground-window imports intercepted\r\n");
			}
		}
	}
	CoopRuntime::Instance().Log("[window] experimental 1280x720 window hook installed\r\n");
	return true;
}

void WindowHook::Remove()
{
	if (m_window_procedure_window && m_original_window_procedure)
	{
		SetWindowLongPtrA(m_window_procedure_window, GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(m_original_window_procedure));
	}
	if (m_get_foreground_window_iat && m_original_get_foreground_window)
	{
		void* original = reinterpret_cast<void*>(m_original_get_foreground_window);
		MemoryPatch::Write(m_get_foreground_window_iat, &original, sizeof(original));
	}
	if (m_is_iconic_iat && m_original_is_iconic)
	{
		void* original = reinterpret_cast<void*>(m_original_is_iconic);
		MemoryPatch::Write(m_is_iconic_iat, &original, sizeof(original));
	}
	if (m_direct3d_create9_iat && m_original_direct3d_create9)
	{
		void* original = reinterpret_cast<void*>(m_original_direct3d_create9);
		MemoryPatch::Write(m_direct3d_create9_iat, &original, sizeof(original));
	}
	if (m_d3d_create_device_slot && m_original_d3d_create_device)
	{
		void* original = reinterpret_cast<void*>(m_original_d3d_create_device);
		MemoryPatch::Write(m_d3d_create_device_slot, &original, sizeof(original));
	}
	if (m_d3d_reset_slot && m_original_d3d_reset)
	{
		void* original = reinterpret_cast<void*>(m_original_d3d_reset);
		MemoryPatch::Write(m_d3d_reset_slot, &original, sizeof(original));
	}
	if (m_d3d_present_slot && m_original_d3d_present)
	{
		void* original = reinterpret_cast<void*>(m_original_d3d_present);
		MemoryPatch::Write(m_d3d_present_slot, &original, sizeof(original));
	}
}
}
