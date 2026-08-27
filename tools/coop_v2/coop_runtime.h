#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stddef.h>

namespace coop
{
struct CoopConfig
{
	volatile LONG enabled;
	volatile LONG test_windowed;
	volatile LONG keep_active_in_background;
	int window_width;
	int window_height;
};

class CoopRuntime final
{
public:
	static CoopRuntime& Instance();

	void SetModule(HMODULE module);
	bool Initialize();
	void Shutdown();

	bool VerifyExecutable();
	void LoadConfiguration();
	void Log(const char* format, ...);
	LONG LogException(EXCEPTION_POINTERS* exception, const char* stage);

	const CoopConfig& Config() const;
	CoopConfig& Config();

private:
	CoopRuntime();
	~CoopRuntime() = default;
	CoopRuntime(const CoopRuntime&) = delete;
	CoopRuntime& operator=(const CoopRuntime&) = delete;

	bool BuildModulePaths();
	bool HashExecutable(BYTE digest[32]);
	HMODULE m_module;
		HANDLE m_log;
		HANDLE m_log_mutex;
		CRITICAL_SECTION m_log_lock;
	bool m_log_lock_ready;
	wchar_t m_module_directory[MAX_PATH];
	wchar_t m_log_path[MAX_PATH];
	wchar_t m_ini_path[MAX_PATH];
	wchar_t m_game_ini_path[MAX_PATH];
	CoopConfig m_config;
};

class MemoryPatch final
{
public:
	static bool Write(void* destination, const void* source, size_t size);
	static void** FindImportAddress(
		const char* module_name, const char* function_name);

private:
	MemoryPatch() = delete;
};
}
