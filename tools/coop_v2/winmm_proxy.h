#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

class WinmmProxy final
{
public:
	static WinmmProxy& Instance();

	void SetModule(HMODULE module);
	void StartEarlyInitialization();
	void EnsureInitialized();
	FARPROC ResolveReal(const char* procedure_name);

	UINT TimeBeginPeriod(UINT period);
	UINT TimeEndPeriod(UINT period);
	MMRESULT TimeGetDevCaps(LPTIMECAPS caps, UINT size);
	DWORD TimeGetTime();
	MMRESULT WaveOutClose(HWAVEOUT wave_out);
	MMRESULT WaveOutOpen(LPHWAVEOUT wave_out, UINT device_id,
		LPCWAVEFORMATEX format, DWORD_PTR callback,
		DWORD_PTR instance, DWORD flags);
	MMRESULT WaveOutPause(HWAVEOUT wave_out);
	MMRESULT WaveOutPrepareHeader(
		HWAVEOUT wave_out, LPWAVEHDR header, UINT size);
	MMRESULT WaveOutReset(HWAVEOUT wave_out);
	MMRESULT WaveOutRestart(HWAVEOUT wave_out);
	MMRESULT WaveOutSetVolume(HWAVEOUT wave_out, DWORD volume);
	MMRESULT WaveOutUnprepareHeader(
		HWAVEOUT wave_out, LPWAVEHDR header, UINT size);
	MMRESULT WaveOutWrite(HWAVEOUT wave_out, LPWAVEHDR header, UINT size);

private:
	typedef UINT (WINAPI* TimePeriodFn)(UINT);
	typedef MMRESULT (WINAPI* TimeGetDevCapsFn)(LPTIMECAPS, UINT);
	typedef DWORD (WINAPI* TimeGetTimeFn)();
	typedef MMRESULT (WINAPI* WaveOutHandleFn)(HWAVEOUT);
	typedef MMRESULT (WINAPI* WaveOutHeaderFn)(HWAVEOUT, LPWAVEHDR, UINT);
	typedef MMRESULT (WINAPI* WaveOutOpenFn)(LPHWAVEOUT, UINT,
		LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
	typedef MMRESULT (WINAPI* WaveOutSetVolumeFn)(HWAVEOUT, DWORD);
	typedef BOOL (WINAPI* CoopInitializeFn)();

	WinmmProxy();
	~WinmmProxy() = default;
	WinmmProxy(const WinmmProxy&) = delete;
	WinmmProxy& operator=(const WinmmProxy&) = delete;

	void Initialize();
	static bool AppendFileName(
		wchar_t path[MAX_PATH], const wchar_t* file_name);
	static BOOL CALLBACK InitializeOnce(PINIT_ONCE, PVOID, PVOID*);
	static DWORD WINAPI EarlyInitializeThread(LPVOID);

	HMODULE m_proxy_module;
	HMODULE m_real_winmm;
	HMODULE m_coop_module;
	TimePeriodFn m_time_begin_period;
	TimePeriodFn m_time_end_period;
	TimeGetDevCapsFn m_time_get_dev_caps;
	TimeGetTimeFn m_time_get_time;
	WaveOutHandleFn m_wave_out_close;
	WaveOutOpenFn m_wave_out_open;
	WaveOutHandleFn m_wave_out_pause;
	WaveOutHeaderFn m_wave_out_prepare_header;
	WaveOutHandleFn m_wave_out_reset;
	WaveOutHandleFn m_wave_out_restart;
	WaveOutSetVolumeFn m_wave_out_set_volume;
	WaveOutHeaderFn m_wave_out_unprepare_header;
	WaveOutHeaderFn m_wave_out_write;
	INIT_ONCE m_init_once;
};
