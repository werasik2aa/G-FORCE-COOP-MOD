#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

namespace coop
{
	class WindowHook final
	{
	public:
		static WindowHook& Instance();

		bool Install();
		void Remove();

	private:
		typedef IDirect3D9* (WINAPI* Direct3DCreate9Fn)(UINT);
		typedef HRESULT(WINAPI* D3DCreateDeviceFn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
		typedef HRESULT(WINAPI* D3DResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
		typedef HWND(WINAPI* GetForegroundWindowFn)();
		typedef BOOL(WINAPI* IsIconicFn)(HWND);
		typedef HRESULT(WINAPI* D3DPresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

		WindowHook();
		~WindowHook() = default;
		WindowHook(const WindowHook&) = delete;
		WindowHook& operator=(const WindowHook&) = delete;

		void ApplyExperimentalWindowStyle(HWND window);
		void ConfigureWindowedPresentation(HWND focus_window, DWORD* behavior_flags, D3DPRESENT_PARAMETERS* parameters);
		void InstallWindowProcedureHook();
		void ForceGameActiveState();
		HRESULT Reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters);
		HRESULT Present(IDirect3DDevice9* device, const RECT* source_rectangle, const RECT* destination_rectangle, HWND destination_window, const RGNDATA* dirty_region);
		HRESULT CreateDevice(IDirect3D9* direct3d, UINT adapter, D3DDEVTYPE device_type, HWND focus_window, DWORD behavior_flags, D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device);
		IDirect3D9* CreateDirect3D9(UINT sdk_version);
		HWND GetForegroundWindow();
		BOOL IsIconic(HWND window);
		LRESULT WindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param);

		static HRESULT WINAPI HookD3DReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters);
		static HRESULT WINAPI HookD3DCreateDevice(IDirect3D9* direct3d, UINT adapter, D3DDEVTYPE device_type, HWND focus_window, DWORD behavior_flags, D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device);
		static IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdk_version);
		static HRESULT WINAPI HookD3DPresent(IDirect3DDevice9* device, const RECT* source_rectangle, const RECT* destination_rectangle, HWND destination_window, const RGNDATA* dirty_region);
		static HWND WINAPI HookGetForegroundWindow();
		static BOOL WINAPI HookIsIconic(HWND window);
		static LRESULT CALLBACK HookWindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param);

		Direct3DCreate9Fn m_original_direct3d_create9;
		D3DCreateDeviceFn m_original_d3d_create_device;
		D3DResetFn m_original_d3d_reset;
		D3DPresentFn m_original_d3d_present;
		GetForegroundWindowFn m_original_get_foreground_window;
		IsIconicFn m_original_is_iconic;
		void** m_direct3d_create9_iat;
		void** m_d3d_create_device_slot;
		void** m_d3d_reset_slot;
		void** m_d3d_present_slot;
		void** m_get_foreground_window_iat;
		void** m_is_iconic_iat;
		WNDPROC m_original_window_procedure;
		HWND m_window_procedure_window;
		HWND m_game_window;
	};
}
