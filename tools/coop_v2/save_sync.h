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
	void SendHostJoinSave(CSteamOfflineSocketServer* server,
		std::int32_t connection);
	bool OnRemotePacket(const void* data, std::uint32_t size);
	// Runs from the render/game thread after the network worker has written the
	// selected DATA<n>.  Entering the stock loader here keeps all level-state
	// work out of the socket thread.
	void OnMainFrame();

private:
	SaveSync() :
		m_pending_load(0),
		m_pending_slot(-1),
		m_host_slot(-1)
	{
	}
	~SaveSync() = default;
	SaveSync(const SaveSync&) = delete;
	SaveSync& operator=(const SaveSync&) = delete;

	volatile long m_pending_load;
	volatile long m_pending_slot;
	volatile long m_host_slot;
};
}
