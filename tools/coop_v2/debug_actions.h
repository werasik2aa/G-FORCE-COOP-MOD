#pragma once

namespace coop
{
	// P1's post-update hook is the only scheduling point used here. Network
	// workers never execute a key action directly against a retail object.
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

		bool m_enter_was_down;
	};
}
