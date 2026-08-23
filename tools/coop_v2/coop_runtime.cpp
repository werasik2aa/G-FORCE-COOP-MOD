#include "coop_runtime.h"

#include "gforce_constants.h"

#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "bcrypt.lib")

namespace coop
{
using namespace gforce;

CoopRuntime& CoopRuntime::Instance()
{
	static CoopRuntime instance;
	return instance;
}

CoopRuntime::CoopRuntime() :
	m_module(NULL),
	m_log(INVALID_HANDLE_VALUE),
	m_log_lock_ready(false)
{
	ZeroMemory(&m_log_lock, sizeof(m_log_lock));
	ZeroMemory(m_module_directory, sizeof(m_module_directory));
	ZeroMemory(m_log_path, sizeof(m_log_path));
	ZeroMemory(m_ini_path, sizeof(m_ini_path));
	ZeroMemory(m_game_ini_path, sizeof(m_game_ini_path));
	ZeroMemory(&m_config, sizeof(m_config));
	m_config.enabled = 1;
	m_config.activate_player2 = 1;
	m_config.test_windowed = 1;
	m_config.keep_active_in_background = 1;
	m_config.player2_device = 1;
	m_config.spawn_key = VK_F6;
	m_config.window_width = 1280;
	m_config.window_height = 720;
	m_config.spawn_offset_x = 0.5f;
	m_config.spawn_offset_y = 0.5f;
	m_config.spawn_offset_z = 0.0f;
}

void CoopRuntime::SetModule(HMODULE module)
{
	m_module = module;
}

const CoopConfig& CoopRuntime::Config() const
{
	return m_config;
}

CoopConfig& CoopRuntime::Config()
{
	return m_config;
}

void CoopRuntime::Log(const char* format, ...)
{
	if (!m_log_lock_ready)
		return;

	EnterCriticalSection(&m_log_lock);
	if (m_log == INVALID_HANDLE_VALUE)
	{
		m_log = CreateFileW(m_log_path, FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);
	}

	if (m_log != INVALID_HANDLE_VALUE)
	{
		char buffer[1024];
		va_list args;
		va_start(args, format);
		const int length = _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
		va_end(args);
		if (length > 0)
		{
			DWORD written = 0;
			WriteFile(m_log, buffer, static_cast<DWORD>(length), &written, NULL);
		}
	}
	LeaveCriticalSection(&m_log_lock);
}

LONG CoopRuntime::LogException(EXCEPTION_POINTERS* exception, const char* stage)
{
	const DWORD code = exception && exception->ExceptionRecord ?
		exception->ExceptionRecord->ExceptionCode : 0;
	void* address = exception && exception->ExceptionRecord ?
		exception->ExceptionRecord->ExceptionAddress : NULL;
	Log("[exception] stage=%s code=0x%08X address=%p\r\n",
		stage, code, address);
	return EXCEPTION_EXECUTE_HANDLER;
}

bool CoopRuntime::BuildModulePaths()
{
	wchar_t path[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameW(m_module, path, _countof(path));
	if (length == 0 || length >= _countof(path))
		return false;

	wchar_t* slash = wcsrchr(path, L'\\');
	if (!slash)
		return false;
	*slash = L'\0';

	if (lstrlenW(path) + 1 >= static_cast<int>(_countof(m_module_directory)))
		return false;
	lstrcpyW(m_module_directory, path);

	if (lstrlenW(path) + 1 + lstrlenW(L"gforce_coop.log") >= static_cast<int>(_countof(m_log_path)))
		return false;
	lstrcpyW(m_log_path, path);
	lstrcatW(m_log_path, L"\\gforce_coop.log");

	if (lstrlenW(path) + 1 + lstrlenW(L"coop.ini") >= static_cast<int>(_countof(m_ini_path)))
		return false;
	lstrcpyW(m_ini_path, path);
	lstrcatW(m_ini_path, L"\\coop.ini");

	if (lstrlenW(path) + 1 + lstrlenW(L"GForce.ini") >=
		static_cast<int>(_countof(m_game_ini_path)))
		return false;
	lstrcpyW(m_game_ini_path, path);
	lstrcatW(m_game_ini_path, L"\\GForce.ini");
	return true;
}

bool CoopRuntime::HashExecutable(BYTE digest[32])
{
	wchar_t executable[MAX_PATH] = {};
	if (!GetModuleFileNameW(NULL, executable, _countof(executable)))
		return false;

	HANDLE file = CreateFileW(executable, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER size = {};
	if (!GetFileSizeEx(file, &size) || size.QuadPart != kFileSize)
	{
		CloseHandle(file);
		return false;
	}

	BCRYPT_ALG_HANDLE algorithm = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	PUCHAR hash_object = NULL;
	DWORD object_length = 0;
	DWORD result_length = 0;
	bool success = false;

	if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
		&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
		goto cleanup;
	if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
		reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length),
		&result_length, 0)))
		goto cleanup;

	hash_object = static_cast<PUCHAR>(HeapAlloc(GetProcessHeap(), 0, object_length));
	if (!hash_object)
		goto cleanup;
	if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, hash_object,
		object_length, NULL, 0, 0)))
		goto cleanup;

	BYTE buffer[64 * 1024];
	for (;;)
	{
		DWORD bytes_read = 0;
		if (!ReadFile(file, buffer, sizeof(buffer), &bytes_read, NULL))
			goto cleanup;
		if (bytes_read == 0)
			break;
		if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer, bytes_read, 0)))
			goto cleanup;
	}

	if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, 32, 0)))
		goto cleanup;
	success = true;

cleanup:
	SecureZeroMemory(buffer, sizeof(buffer));
	if (hash)
		BCryptDestroyHash(hash);
	if (hash_object)
	{
		SecureZeroMemory(hash_object, object_length);
		HeapFree(GetProcessHeap(), 0, hash_object);
	}
	if (algorithm)
		BCryptCloseAlgorithmProvider(algorithm, 0);
	CloseHandle(file);
	return success;
}

bool CoopRuntime::VerifyExecutable()
{
	BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(NULL));
	if (reinterpret_cast<uintptr_t>(base) != kImageBase)
	{
		Log("[error] unexpected image base: %p\r\n", base);
		return false;
	}

	IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	IMAGE_NT_HEADERS32* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE ||
		nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
		nt->FileHeader.TimeDateStamp != kTimeDateStamp ||
		nt->OptionalHeader.SizeOfImage != kSizeOfImage ||
		nt->OptionalHeader.CheckSum != kCheckSum ||
		nt->OptionalHeader.AddressOfEntryPoint != kEntryPoint)
	{
		Log("[error] PE fingerprint mismatch\r\n");
		return false;
	}

	BYTE digest[32] = {};
	if (!HashExecutable(digest))
	{
		Log("[error] unable to hash GForce.exe\r\n");
		return false;
	}
	if (memcmp(digest, kExpectedSha256, sizeof(digest)) != 0)
	{
		Log("[error] SHA-256 mismatch; no patches installed\r\n");
		return false;
	}

	Log("[ok] exact GForce.exe fingerprint verified\r\n");
	return true;
}

float CoopRuntime::ReadIniFloat(const wchar_t* key, float fallback)
{
	wchar_t default_value[64] = {};
	wchar_t value[64] = {};
	swprintf_s(default_value, _countof(default_value), L"%.6g",
		static_cast<double>(fallback));
	GetPrivateProfileStringW(L"coop", key, default_value, value,
		_countof(value), m_ini_path);
	wchar_t* end = NULL;
	const float parsed = wcstof(value, &end);
	return end == value ? fallback : parsed;
}

void CoopRuntime::LoadConfiguration()
{
	InterlockedExchange(&m_config.enabled,
		GetPrivateProfileIntW(L"coop", L"enabled", 1, m_ini_path) ? 1 : 0);
	InterlockedExchange(&m_config.activate_player2,
		GetPrivateProfileIntW(L"coop", L"activate_player2", 1, m_ini_path) ? 1 : 0);
	m_config.player2_device = GetPrivateProfileIntW(L"coop", L"player2_device", 1, m_ini_path);
	if (m_config.player2_device < 0)
		m_config.player2_device = 0;
	if (m_config.player2_device > 3)
		m_config.player2_device = 3;
	m_config.spawn_key = GetPrivateProfileIntW(L"coop", L"spawn_key", VK_F6, m_ini_path);
	if (m_config.spawn_key < 0 || m_config.spawn_key > 255)
		m_config.spawn_key = VK_F6;
	m_config.spawn_offset_x = ReadIniFloat(L"spawn_offset_x", 0.5f);
	m_config.spawn_offset_y = ReadIniFloat(L"spawn_offset_y", 0.5f);
	m_config.spawn_offset_z = ReadIniFloat(L"spawn_offset_z", 0.0f);
	InterlockedExchange(&m_config.test_windowed,
		GetPrivateProfileIntW(L"window", L"experimental_windowed", 1,
			m_ini_path) ? 1 : 0);
	InterlockedExchange(&m_config.keep_active_in_background,
		GetPrivateProfileIntW(L"window", L"keep_active_in_background", 1,
			m_ini_path) ? 1 : 0);
	m_config.window_width = GetPrivateProfileIntW(L"window", L"width", 1280,
		m_ini_path);
	m_config.window_height = GetPrivateProfileIntW(L"window", L"height", 720,
		m_ini_path);
	if (m_config.window_width < 640 || m_config.window_width > 7680)
		m_config.window_width = 1280;
	if (m_config.window_height < 480 || m_config.window_height > 4320)
		m_config.window_height = 720;
	if (m_config.test_windowed)
	{
		wchar_t width[16] = {};
		wchar_t height[16] = {};
		_itow_s(m_config.window_width, width, _countof(width), 10);
		_itow_s(m_config.window_height, height, _countof(height), 10);
		WritePrivateProfileStringW(L"RenderMode", L"FBWidth", width,
			m_game_ini_path);
		WritePrivateProfileStringW(L"RenderMode", L"FBHeight", height,
			m_game_ini_path);
	}
	Log("[config] enabled=%ld activate_player2=%ld player2_device=%d spawn_key=0x%02X offset=(%.3f, %.3f, %.3f)\r\n",
		m_config.enabled, m_config.activate_player2, m_config.player2_device, m_config.spawn_key,
		m_config.spawn_offset_x, m_config.spawn_offset_y, m_config.spawn_offset_z);
	Log("[config-window] experimental_windowed=%ld keep_active_in_background=%ld client=%dx%d\r\n",
		m_config.test_windowed, m_config.keep_active_in_background,
		m_config.window_width, m_config.window_height);
}

bool MemoryPatch::Write(void* destination, const void* source, size_t size)
{
	DWORD old_protection = 0;
	if (!VirtualProtect(destination, size, PAGE_EXECUTE_READWRITE, &old_protection))
		return false;
	memcpy(destination, source, size);
	FlushInstructionCache(GetCurrentProcess(), destination, size);
	DWORD ignored = 0;
	VirtualProtect(destination, size, old_protection, &ignored);
	return true;
}

void** MemoryPatch::FindImportAddress(const char* module_name, const char* function_name)
{
	BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(NULL));
	if (!base)
		return NULL;

	IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
	IMAGE_NT_HEADERS32* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(
		base + dos->e_lfanew);
	const IMAGE_DATA_DIRECTORY& imports =
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!imports.VirtualAddress)
		return NULL;

	IMAGE_IMPORT_DESCRIPTOR* descriptor =
		reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
			base + imports.VirtualAddress);
	for (; descriptor->Name; ++descriptor)
	{
		const char* imported_module = reinterpret_cast<const char*>(
			base + descriptor->Name);
		if (_stricmp(imported_module, module_name) != 0)
			continue;

		IMAGE_THUNK_DATA32* original = descriptor->OriginalFirstThunk ?
			reinterpret_cast<IMAGE_THUNK_DATA32*>(
				base + descriptor->OriginalFirstThunk) : NULL;
		IMAGE_THUNK_DATA32* current = reinterpret_cast<IMAGE_THUNK_DATA32*>(
			base + descriptor->FirstThunk);
		if (!original)
			return NULL;
		for (; original->u1.AddressOfData; ++original, ++current)
		{
			if (IMAGE_SNAP_BY_ORDINAL32(original->u1.Ordinal))
				continue;
			IMAGE_IMPORT_BY_NAME* import_name =
				reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
					base + original->u1.AddressOfData);
			if (strcmp(reinterpret_cast<const char*>(import_name->Name),
				function_name) == 0)
			{
				return reinterpret_cast<void**>(&current->u1.Function);
			}
		}
	}
	return NULL;
}

bool CoopRuntime::Initialize()
{
	InitializeCriticalSection(&m_log_lock);
	m_log_lock_ready = true;
	if (!BuildModulePaths())
	{
		m_log_lock_ready = false;
		DeleteCriticalSection(&m_log_lock);
		return false;
	}
	DeleteFileW(m_log_path);
	return true;
}

void CoopRuntime::Shutdown()
{
	if (!m_log_lock_ready)
		return;

	EnterCriticalSection(&m_log_lock);
	if (m_log != INVALID_HANDLE_VALUE)
	{
		CloseHandle(m_log);
		m_log = INVALID_HANDLE_VALUE;
	}
	LeaveCriticalSection(&m_log_lock);
	m_log_lock_ready = false;
	DeleteCriticalSection(&m_log_lock);
}
}
