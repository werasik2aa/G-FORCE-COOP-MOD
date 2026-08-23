#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace coop
{
class CoopApplication final
{
public:
	static CoopApplication& Instance();

	void SetModule(HMODULE module);
	BOOL Initialize();
	void Shutdown();

private:
	CoopApplication();
	~CoopApplication() = default;
	CoopApplication(const CoopApplication&) = delete;
	CoopApplication& operator=(const CoopApplication&) = delete;

	volatile LONG m_init_state;
};
}
