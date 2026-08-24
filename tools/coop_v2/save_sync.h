#pragma once

#include <cstdint>

class CSteamOfflineSocketServer;

namespace coop
{
class SaveSync final
{
public:
	static SaveSync& Instance();

	// DATA4 is the fifth visible save slot. It is reserved for co-op joining so
	// the receiver's first four personal slots are never touched.
	void SendHostJoinSave(CSteamOfflineSocketServer* server,
		std::int32_t connection);
	bool OnRemotePacket(const void* data, std::uint32_t size);
	// Runs from the render/game thread after the network worker has written
	// DATA4.  Entering the stock loader here keeps all level-state work out of
	// the socket thread.
	void OnMainFrame();

private:
	SaveSync() :
		m_pending_load(0)
	{
	}
	~SaveSync() = default;
	SaveSync(const SaveSync&) = delete;
	SaveSync& operator=(const SaveSync&) = delete;

	volatile long m_pending_load;
};
}
