#pragma once

#include <array>
#include <cstddef>

namespace coop
{
	// Keep the key-state storage and the F1-F7/F9 dispatch table bound to one
	// explicit count instead of maintaining a second anonymous array length.
	constexpr std::size_t kDebugActionCount = 8;

	// P1's post-update hook is the only scheduling point used by these actions.
	// Network workers never execute a debug key directly against a retail object.
	class DebugActions final
	{
	public:
		static DebugActions& Instance();

		void Tick();
		void ResetForWorldLoad();

	private:
		DebugActions();
		~DebugActions() = default;
		DebugActions(const DebugActions&) = delete;
		DebugActions& operator=(const DebugActions&) = delete;

		bool ConsumePressed(int virtual_key, bool& was_down) const;

		std::array<bool, kDebugActionCount> m_key_was_down;
	};
}
