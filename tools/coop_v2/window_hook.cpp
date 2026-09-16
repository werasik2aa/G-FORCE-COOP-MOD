#include "window_hook.h"

#include "chat_overlay.h"
#include "coop_netgame.h"
#include "coop_runtime.h"
#include "save_sync.h"
#include "world_sync.h"

namespace coop
{
	WindowHook& WindowHook::Instance()
	{
		static WindowHook instance;
		return instance;
	}

	WindowHook::WindowHook() :
		m_original_direct3d_create9(nullptr),
		m_original_d3d_create_device(nullptr),
		m_original_d3d_reset(nullptr),
		m_original_d3d_present(nullptr),
		m_direct3d_create9_iat(nullptr),
		m_d3d_create_device_slot(nullptr),
		m_d3d_reset_slot(nullptr),
		m_d3d_present_slot(nullptr),
		m_game_window(nullptr),
		m_present_count(0)
	{}

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

		RECT rectangle = { 0, 0, CoopRuntime::Instance().Config().window_width, CoopRuntime::Instance().Config().window_height };
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
		if (focus_window)
			m_game_window = focus_window;
		else if (parameters && parameters->hDeviceWindow)
			m_game_window = parameters->hDeviceWindow;
		// The presentation rewrite remains opt-in. This hook intentionally does
		// not subclass the WndProc or alter focus/minimise state: those operations
		// caused a fullscreen shutdown regression and never removed retail pause.
		if (!CoopRuntime::Instance().Config().test_windowed || !parameters)
			return;
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

	HRESULT WINAPI WindowHook::HookD3DReset(IDirect3DDevice9* device,
		D3DPRESENT_PARAMETERS* parameters)
	{
		return Instance().Reset(device, parameters);
	}

	HRESULT WindowHook::Reset(IDirect3DDevice9* device,
		D3DPRESENT_PARAMETERS* parameters)
	{
		ConfigureWindowedPresentation(m_game_window, nullptr, parameters);
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
		WorldSync::Instance().OnRenderFrame();
		const HRESULT result = m_original_d3d_present ? m_original_d3d_present(device,
			source_rectangle, destination_rectangle, destination_window,
			dirty_region) : D3DERR_INVALIDCALL;
		// A joining client receives DATA<n> while still in the front end, before a
		// P1 controller exists and TickPlayer1 can consume the pending load. Keep
		// that bootstrap path, but only after the stock Present has returned: no
		// WorldSync work or active D3D call can then observe objects destroyed by
		// the native loader. Once a world exists, TickPlayer1 consumes the same
		// one-shot queue before this fallback is reached.
		if (SUCCEEDED(result))
		{
			// Chat owns a retained non-activating GDI HWND over the game client. Keeping its
			// layout here after Present avoids D3D state changes while avoiding the
			// direct-client-DC flashing caused by a following D3D flip.
			ChatOverlay::Instance().Render(
				destination_window ? destination_window : m_game_window);
			SaveSync::Instance().OnMainFrame();
			// Present also fires in menus, roughly every frame: reuse it as the
			// quit-to-menu watchdog tick (~1/sec).
			if (InterlockedIncrement(&m_present_count) % 60 == 0)
				CoopNetGame::Instance().CheckMenuQuitTick();
		}
		return result;
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
		CoopRuntime::Instance().Log("[window] D3D9 device ready windowed=%d client=%dx%d hwnd=%p\r\n",
			CoopRuntime::Instance().Config().test_windowed ? 1 : 0,
			CoopRuntime::Instance().Config().window_width, CoopRuntime::Instance().Config().window_height, m_game_window);
		return result;
	}

	IDirect3D9* WINAPI WindowHook::HookDirect3DCreate9(UINT sdk_version)
	{
		return Instance().CreateDirect3D9(sdk_version);
	}

	IDirect3D9* WindowHook::CreateDirect3D9(UINT sdk_version)
	{
		IDirect3D9* direct3d = m_original_direct3d_create9 ?
			m_original_direct3d_create9(sdk_version) : nullptr;
		if (!direct3d)
			return nullptr;

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

	bool WindowHook::Install()
	{
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

		CoopRuntime::Instance().Log(
			"[window] D3D hook installed windowed=%d; focus/minimise bypass disabled\r\n",
			CoopRuntime::Instance().Config().test_windowed ? 1 : 0);
		return true;
	}

	void WindowHook::Remove()
	{
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
