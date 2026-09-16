#pragma once

#include <cstdint>

class CSteamOfflineSocketServer;

namespace coop
{
	class SaveSync final
	{
	public:
		static SaveSync& Instance();

		// The host's selected native Load Game slot is copied into the same numbered
		// slot on the client, then loaded through the game's own loader.
		// Called from P1's game-thread tick after a world is loaded.  Capturing here
		// keeps the network worker from reading live menu/game state.
		void CaptureLoadedHostSlot();
		// Called before the host's validated Load Game call. The selected save is
		// sent to every connected peer before native loading starts.
		void OnHostLoadGame(std::uint32_t slot);
		void SendHostJoinSave(CSteamOfflineSocketServer* server, std::int32_t connection);
		bool OnRemotePacket(const void* data, std::uint32_t size);
		// True while a host save is queued for the native loader but not yet
		// consumed. Quit detection must not mistake this window for menu exit.
		bool HasPendingLoad() const;
		// Runs after the network worker has written selected DATA<n>: from P1's
		// game-thread tick in a loaded world, or from the post-Present front-end
		// bootstrap before that controller exists. It never runs on the socket
		// worker; the post-Present path has no active D3D call or WorldSync work.
		bool OnMainFrame();

	private:
		SaveSync() :
			m_pending_load(0),
			m_pending_slot(-1),
			m_host_slot(-1)
		{}
		~SaveSync() = default;
		SaveSync(const SaveSync&) = delete;
		SaveSync& operator=(const SaveSync&) = delete;

		volatile long m_pending_load;
		volatile long m_pending_slot;
		volatile long m_host_slot;
	};
}
