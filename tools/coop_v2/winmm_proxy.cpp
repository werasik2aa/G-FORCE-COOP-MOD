#include "winmm_proxy.h"

#include <stdlib.h>
#include <wchar.h>

WinmmProxy& WinmmProxy::Instance()
{
	static WinmmProxy instance;
	return instance;
}

WinmmProxy::WinmmProxy() :
	m_proxy_module(NULL),
	m_real_winmm(NULL),
	m_coop_module(NULL),
	m_time_begin_period(NULL),
	m_time_end_period(NULL),
	m_time_get_dev_caps(NULL),
	m_time_get_time(NULL),
	m_wave_out_close(NULL),
	m_wave_out_open(NULL),
	m_wave_out_pause(NULL),
	m_wave_out_prepare_header(NULL),
	m_wave_out_reset(NULL),
	m_wave_out_restart(NULL),
	m_wave_out_set_volume(NULL),
	m_wave_out_unprepare_header(NULL),
	m_wave_out_write(NULL)
{
	InitOnceInitialize(&m_init_once);
}

void WinmmProxy::SetModule(HMODULE module)
{
	m_proxy_module = module;
}

bool WinmmProxy::AppendFileName(
	wchar_t path[MAX_PATH], const wchar_t* file_name)
{
	wchar_t* slash = wcsrchr(path, L'\\');
	if (!slash)
		return false;
	if ((slash - path) + 1 + lstrlenW(file_name) >= MAX_PATH)
		return false;
	lstrcpyW(slash + 1, file_name);
	return true;
}

void WinmmProxy::Initialize()
{
	wchar_t system_winmm[MAX_PATH] = {};
	const UINT system_length = GetSystemDirectoryW(
		system_winmm, _countof(system_winmm));
	if (system_length == 0 || system_length >= _countof(system_winmm) ||
		system_length + lstrlenW(L"\\winmm.dll") >= _countof(system_winmm))
		return;
	lstrcatW(system_winmm, L"\\winmm.dll");

	m_real_winmm = LoadLibraryW(system_winmm);
	if (!m_real_winmm)
		return;
	m_time_begin_period = reinterpret_cast<TimePeriodFn>(
		GetProcAddress(m_real_winmm, "timeBeginPeriod"));
	m_time_end_period = reinterpret_cast<TimePeriodFn>(
		GetProcAddress(m_real_winmm, "timeEndPeriod"));
	m_time_get_dev_caps = reinterpret_cast<TimeGetDevCapsFn>(
		GetProcAddress(m_real_winmm, "timeGetDevCaps"));
	m_time_get_time = reinterpret_cast<TimeGetTimeFn>(
		GetProcAddress(m_real_winmm, "timeGetTime"));
	m_wave_out_close = reinterpret_cast<WaveOutHandleFn>(
		GetProcAddress(m_real_winmm, "waveOutClose"));
	m_wave_out_open = reinterpret_cast<WaveOutOpenFn>(
		GetProcAddress(m_real_winmm, "waveOutOpen"));
	m_wave_out_pause = reinterpret_cast<WaveOutHandleFn>(
		GetProcAddress(m_real_winmm, "waveOutPause"));
	m_wave_out_prepare_header = reinterpret_cast<WaveOutHeaderFn>(
		GetProcAddress(m_real_winmm, "waveOutPrepareHeader"));
	m_wave_out_reset = reinterpret_cast<WaveOutHandleFn>(
		GetProcAddress(m_real_winmm, "waveOutReset"));
	m_wave_out_restart = reinterpret_cast<WaveOutHandleFn>(
		GetProcAddress(m_real_winmm, "waveOutRestart"));
	m_wave_out_set_volume = reinterpret_cast<WaveOutSetVolumeFn>(
		GetProcAddress(m_real_winmm, "waveOutSetVolume"));
	m_wave_out_unprepare_header = reinterpret_cast<WaveOutHeaderFn>(
		GetProcAddress(m_real_winmm, "waveOutUnprepareHeader"));
	m_wave_out_write = reinterpret_cast<WaveOutHeaderFn>(
		GetProcAddress(m_real_winmm, "waveOutWrite"));

	wchar_t coop_path[MAX_PATH] = {};
	const DWORD path_length = GetModuleFileNameW(
		m_proxy_module, coop_path, _countof(coop_path));
	if (path_length == 0 || path_length >= _countof(coop_path) ||
		!AppendFileName(coop_path, L"coop_dll.dll"))
		return;

	m_coop_module = LoadLibraryW(coop_path);
	if (!m_coop_module)
		return;
	CoopInitializeFn initialize = reinterpret_cast<CoopInitializeFn>(
		GetProcAddress(m_coop_module, "CoopInitialize"));
	if (!initialize || !initialize())
	{
		FreeLibrary(m_coop_module);
		m_coop_module = NULL;
	}
}

BOOL CALLBACK WinmmProxy::InitializeOnce(PINIT_ONCE, PVOID, PVOID*)
{
	Instance().Initialize();
	return TRUE;
}

void WinmmProxy::EnsureInitialized()
{
	InitOnceExecuteOnce(&m_init_once, InitializeOnce, NULL, NULL);
}

FARPROC WinmmProxy::ResolveReal(const char* procedure_name)
{
	if (!m_real_winmm)
		EnsureInitialized();
	return m_real_winmm && procedure_name ?
		GetProcAddress(m_real_winmm, procedure_name) : NULL;
}

DWORD WINAPI WinmmProxy::EarlyInitializeThread(LPVOID)
{
	// The first exported WinMM call happens after Direct3DCreate9. Start the
	// co-op DLL immediately after loader lock is released instead.
	Instance().EnsureInitialized();
	return 0;
}

void WinmmProxy::StartEarlyInitialization()
{
	HANDLE thread = CreateThread(
		NULL, 0, EarlyInitializeThread, NULL, 0, NULL);
	if (thread)
		CloseHandle(thread);
}

UINT WinmmProxy::TimeBeginPeriod(UINT period)
{
	EnsureInitialized();
	return m_time_begin_period ? m_time_begin_period(period) : 97u;
}

UINT WinmmProxy::TimeEndPeriod(UINT period)
{
	EnsureInitialized();
	return m_time_end_period ? m_time_end_period(period) : 97u;
}

MMRESULT WinmmProxy::TimeGetDevCaps(LPTIMECAPS caps, UINT size)
{
	EnsureInitialized();
	return m_time_get_dev_caps ?
		m_time_get_dev_caps(caps, size) : TIMERR_NOCANDO;
}

DWORD WinmmProxy::TimeGetTime()
{
	EnsureInitialized();
	return m_time_get_time ? m_time_get_time() : GetTickCount();
}

MMRESULT WinmmProxy::WaveOutClose(HWAVEOUT wave_out)
{
	EnsureInitialized();
	return m_wave_out_close ? m_wave_out_close(wave_out) : MMSYSERR_ERROR;
}

MMRESULT WinmmProxy::WaveOutOpen(LPHWAVEOUT wave_out, UINT device_id,
	LPCWAVEFORMATEX format, DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
{
	EnsureInitialized();
	return m_wave_out_open ? m_wave_out_open(
		wave_out, device_id, format, callback, instance, flags) : MMSYSERR_ERROR;
}

MMRESULT WinmmProxy::WaveOutPause(HWAVEOUT wave_out)
{
	EnsureInitialized();
	return m_wave_out_pause ? m_wave_out_pause(wave_out) : MMSYSERR_ERROR;
}

MMRESULT WinmmProxy::WaveOutPrepareHeader(
	HWAVEOUT wave_out, LPWAVEHDR header, UINT size)
{
	EnsureInitialized();
	return m_wave_out_prepare_header ?
		m_wave_out_prepare_header(wave_out, header, size) : MMSYSERR_ERROR;
}

MMRESULT WinmmProxy::WaveOutReset(HWAVEOUT wave_out)
{
	EnsureInitialized();
	return m_wave_out_reset ? m_wave_out_reset(wave_out) : MMSYSERR_ERROR;
}

MMRESULT WinmmProxy::WaveOutRestart(HWAVEOUT wave_out)
{
	EnsureInitialized();
	return m_wave_out_restart ? m_wave_out_restart(wave_out) : MMSYSERR_ERROR;
}

MMRESULT WinmmProxy::WaveOutSetVolume(HWAVEOUT wave_out, DWORD volume)
{
	EnsureInitialized();
	return m_wave_out_set_volume ?
		m_wave_out_set_volume(wave_out, volume) : MMSYSERR_ERROR;
}

MMRESULT WinmmProxy::WaveOutUnprepareHeader(
	HWAVEOUT wave_out, LPWAVEHDR header, UINT size)
{
	EnsureInitialized();
	return m_wave_out_unprepare_header ?
		m_wave_out_unprepare_header(wave_out, header, size) : MMSYSERR_ERROR;
}

MMRESULT WinmmProxy::WaveOutWrite(
	HWAVEOUT wave_out, LPWAVEHDR header, UINT size)
{
	EnsureInitialized();
	return m_wave_out_write ?
		m_wave_out_write(wave_out, header, size) : MMSYSERR_ERROR;
}

extern "C" UINT WINAPI proxy_timeBeginPeriod(UINT period)
{
	return WinmmProxy::Instance().TimeBeginPeriod(period);
}

extern "C" MMRESULT WINAPI proxy_mixerClose(HMIXER mixer)
{
	typedef MMRESULT (WINAPI* Function)(HMIXER);
	Function function = reinterpret_cast<Function>(
		WinmmProxy::Instance().ResolveReal("mixerClose"));
	return function ? function(mixer) : MMSYSERR_ERROR;
}

extern "C" MMRESULT WINAPI proxy_mixerGetControlDetailsA(
	HMIXEROBJ mixer, LPMIXERCONTROLDETAILS details, DWORD flags)
{
	typedef MMRESULT (WINAPI* Function)(
		HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD);
	Function function = reinterpret_cast<Function>(
		WinmmProxy::Instance().ResolveReal("mixerGetControlDetailsA"));
	return function ? function(mixer, details, flags) : MMSYSERR_ERROR;
}

extern "C" MMRESULT WINAPI proxy_mixerGetDevCapsA(
	UINT_PTR mixer_id, LPMIXERCAPSA caps, UINT size)
{
	typedef MMRESULT (WINAPI* Function)(UINT_PTR, LPMIXERCAPSA, UINT);
	Function function = reinterpret_cast<Function>(
		WinmmProxy::Instance().ResolveReal("mixerGetDevCapsA"));
	return function ? function(mixer_id, caps, size) : MMSYSERR_ERROR;
}

extern "C" MMRESULT WINAPI proxy_mixerGetID(
	HMIXEROBJ mixer, UINT* mixer_id, DWORD flags)
{
	typedef MMRESULT (WINAPI* Function)(HMIXEROBJ, UINT*, DWORD);
	Function function = reinterpret_cast<Function>(
		WinmmProxy::Instance().ResolveReal("mixerGetID"));
	return function ? function(mixer, mixer_id, flags) : MMSYSERR_ERROR;
}

extern "C" MMRESULT WINAPI proxy_mixerGetLineControlsA(
	HMIXEROBJ mixer, LPMIXERLINECONTROLSA controls, DWORD flags)
{
	typedef MMRESULT (WINAPI* Function)(
		HMIXEROBJ, LPMIXERLINECONTROLSA, DWORD);
	Function function = reinterpret_cast<Function>(
		WinmmProxy::Instance().ResolveReal("mixerGetLineControlsA"));
	return function ? function(mixer, controls, flags) : MMSYSERR_ERROR;
}

extern "C" MMRESULT WINAPI proxy_mixerGetLineInfoA(
	HMIXEROBJ mixer, LPMIXERLINEA line, DWORD flags)
{
	typedef MMRESULT (WINAPI* Function)(HMIXEROBJ, LPMIXERLINEA, DWORD);
	Function function = reinterpret_cast<Function>(
		WinmmProxy::Instance().ResolveReal("mixerGetLineInfoA"));
	return function ? function(mixer, line, flags) : MMSYSERR_ERROR;
}

extern "C" MMRESULT WINAPI proxy_mixerOpen(LPHMIXER mixer,
	UINT mixer_id, DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
{
	typedef MMRESULT (WINAPI* Function)(
		LPHMIXER, UINT, DWORD_PTR, DWORD_PTR, DWORD);
	Function function = reinterpret_cast<Function>(
		WinmmProxy::Instance().ResolveReal("mixerOpen"));
	return function ? function(
		mixer, mixer_id, callback, instance, flags) : MMSYSERR_ERROR;
}

extern "C" MMRESULT WINAPI proxy_mixerSetControlDetails(
	HMIXEROBJ mixer, LPMIXERCONTROLDETAILS details, DWORD flags)
{
	typedef MMRESULT (WINAPI* Function)(
		HMIXEROBJ, LPMIXERCONTROLDETAILS, DWORD);
	Function function = reinterpret_cast<Function>(
		WinmmProxy::Instance().ResolveReal("mixerSetControlDetails"));
	return function ? function(mixer, details, flags) : MMSYSERR_ERROR;
}

extern "C" MMRESULT WINAPI proxy_waveInClose(HWAVEIN wave_in)
{
	typedef MMRESULT (WINAPI* Function)(HWAVEIN);
	Function function = reinterpret_cast<Function>(
		WinmmProxy::Instance().ResolveReal("waveInClose"));
	return function ? function(wave_in) : MMSYSERR_ERROR;
}

extern "C" MMRESULT WINAPI proxy_waveInMessage(
	HWAVEIN wave_in, UINT message, DWORD_PTR first, DWORD_PTR second)
{
	typedef MMRESULT (WINAPI* Function)(
		HWAVEIN, UINT, DWORD_PTR, DWORD_PTR);
	Function function = reinterpret_cast<Function>(
		WinmmProxy::Instance().ResolveReal("waveInMessage"));
	return function ? function(
		wave_in, message, first, second) : MMSYSERR_ERROR;
}

extern "C" MMRESULT WINAPI proxy_waveInOpen(LPHWAVEIN wave_in,
	UINT device_id, LPCWAVEFORMATEX format, DWORD_PTR callback,
	DWORD_PTR instance, DWORD flags)
{
	typedef MMRESULT (WINAPI* Function)(
		LPHWAVEIN, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
	Function function = reinterpret_cast<Function>(
		WinmmProxy::Instance().ResolveReal("waveInOpen"));
	return function ? function(wave_in, device_id, format,
		callback, instance, flags) : MMSYSERR_ERROR;
}

extern "C" MMRESULT WINAPI proxy_waveOutGetDevCapsW(
	UINT_PTR device_id, LPWAVEOUTCAPSW caps, UINT size)
{
	typedef MMRESULT (WINAPI* Function)(UINT_PTR, LPWAVEOUTCAPSW, UINT);
	Function function = reinterpret_cast<Function>(
		WinmmProxy::Instance().ResolveReal("waveOutGetDevCapsW"));
	return function ? function(device_id, caps, size) : MMSYSERR_ERROR;
}

extern "C" MMRESULT WINAPI proxy_waveOutMessage(
	HWAVEOUT wave_out, UINT message, DWORD_PTR first, DWORD_PTR second)
{
	typedef MMRESULT (WINAPI* Function)(
		HWAVEOUT, UINT, DWORD_PTR, DWORD_PTR);
	Function function = reinterpret_cast<Function>(
		WinmmProxy::Instance().ResolveReal("waveOutMessage"));
	return function ? function(
		wave_out, message, first, second) : MMSYSERR_ERROR;
}

extern "C" UINT WINAPI proxy_timeEndPeriod(UINT period)
{
	return WinmmProxy::Instance().TimeEndPeriod(period);
}

extern "C" MMRESULT WINAPI proxy_timeGetDevCaps(LPTIMECAPS caps, UINT size)
{
	return WinmmProxy::Instance().TimeGetDevCaps(caps, size);
}

extern "C" DWORD WINAPI proxy_timeGetTime()
{
	return WinmmProxy::Instance().TimeGetTime();
}

extern "C" MMRESULT WINAPI proxy_waveOutClose(HWAVEOUT wave_out)
{
	return WinmmProxy::Instance().WaveOutClose(wave_out);
}

extern "C" MMRESULT WINAPI proxy_waveOutOpen(LPHWAVEOUT wave_out,
	UINT device_id, LPCWAVEFORMATEX format, DWORD_PTR callback,
	DWORD_PTR instance, DWORD flags)
{
	return WinmmProxy::Instance().WaveOutOpen(
		wave_out, device_id, format, callback, instance, flags);
}

extern "C" MMRESULT WINAPI proxy_waveOutPause(HWAVEOUT wave_out)
{
	return WinmmProxy::Instance().WaveOutPause(wave_out);
}

extern "C" MMRESULT WINAPI proxy_waveOutPrepareHeader(
	HWAVEOUT wave_out, LPWAVEHDR header, UINT size)
{
	return WinmmProxy::Instance().WaveOutPrepareHeader(wave_out, header, size);
}

extern "C" MMRESULT WINAPI proxy_waveOutReset(HWAVEOUT wave_out)
{
	return WinmmProxy::Instance().WaveOutReset(wave_out);
}

extern "C" MMRESULT WINAPI proxy_waveOutRestart(HWAVEOUT wave_out)
{
	return WinmmProxy::Instance().WaveOutRestart(wave_out);
}

extern "C" MMRESULT WINAPI proxy_waveOutSetVolume(
	HWAVEOUT wave_out, DWORD volume)
{
	return WinmmProxy::Instance().WaveOutSetVolume(wave_out, volume);
}

extern "C" MMRESULT WINAPI proxy_waveOutUnprepareHeader(
	HWAVEOUT wave_out, LPWAVEHDR header, UINT size)
{
	return WinmmProxy::Instance().WaveOutUnprepareHeader(wave_out, header, size);
}

extern "C" MMRESULT WINAPI proxy_waveOutWrite(
	HWAVEOUT wave_out, LPWAVEHDR header, UINT size)
{
	return WinmmProxy::Instance().WaveOutWrite(wave_out, header, size);
}

// ---------------------------------------------------------------------------
// Complete winmm forwarding surface.
//
// This DLL occupies the module name "winmm.dll" for the WHOLE process, so every
// other module - Steam's DLLs, the overlay, any codec - resolves its winmm
// imports against us, not against System32.  Exporting only the functions
// GForce.exe itself imports therefore turned into a hard load failure for
// anything else: Steam's steamwebrtc.dll imports timeSetEvent and timeKillEvent,
// and with those absent Windows refused to load it at all ("entry point
// timeKillEvent not found in E:\steam\steamwebrtc.dll"), which took Steam
// Datagram Relay - and with it every P2P connect - down as collateral.
//
// The generated list below forwards every remaining winmm export verbatim.  Each
// thunk is a naked x86 tail-jump, which makes it signature-agnostic: the
// caller's stack frame passes through untouched and the real function does its
// own __stdcall cleanup.  Only eax is written before the jump, and that is a
// scratch register in every convention winmm uses.
#define WINMM_FORWARD(index, name) +1
const int kWinmmForwardCount = 0
#include "winmm_forward.inl"
	;
#undef WINMM_FORWARD

#define WINMM_FORWARD(index, name) #name,
static const char* const kWinmmForwardNames[kWinmmForwardCount] = {
#include "winmm_forward.inl"
};
#undef WINMM_FORWARD

// Resolved lazily and cached, so a thunk costs one indirect jump after the first
// call.  Deliberately not in an anonymous namespace: the thunks below address it
// from inline assembly.
FARPROC g_winmm_forward_slots[kWinmmForwardCount];

// Unreachable in practice - the export list is generated from the real DLL, so a
// name that exists here always exists there.  Present only so a thunk never
// jumps through a null slot.
extern "C" __declspec(naked) void WinmmForwardMissing()
{
	__asm xor eax, eax
	__asm ret
}

extern "C" FARPROC __stdcall WinmmResolveForward(int index)
{
	if (index < 0 || index >= kWinmmForwardCount)
		return reinterpret_cast<FARPROC>(&WinmmForwardMissing);
	FARPROC real = WinmmProxy::Instance().ResolveReal(kWinmmForwardNames[index]);
	if (!real)
		real = reinterpret_cast<FARPROC>(&WinmmForwardMissing);
	g_winmm_forward_slots[index] = real;
	return real;
}

#define WINMM_FORWARD(index, name) \
	extern "C" __declspec(naked) void proxy_fwd_##name() \
	{ \
		__asm mov eax, dword ptr [g_winmm_forward_slots + index * 4] \
		__asm test eax, eax \
		__asm jne L_have_slot \
		__asm push index \
		__asm call WinmmResolveForward \
		__asm L_have_slot: \
		__asm jmp eax \
	}
#include "winmm_forward.inl"
#undef WINMM_FORWARD

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		WinmmProxy::Instance().SetModule(instance);
		DisableThreadLibraryCalls(instance);
		WinmmProxy::Instance().StartEarlyInitialization();
	}
	return TRUE;
}
