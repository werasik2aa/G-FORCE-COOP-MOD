#pragma once

#include <cstdint>

#include "../ServerClient/MTypes.h"

namespace coop
{
namespace protocol
{
	// The reason is diagnostic and policy metadata; position/rotation remain the
	// complete cross-process contract. No retail object address travels here.
	enum class ProgressionRallyReason : std::uint32_t
	{
		Invalid = 0,
		Cutscene = 1,
		Checkpoint = 2
	};

	struct ProgressionRallyPacket final : PacketHeader
	{
		std::uint32_t sequence;
		ProgressionRallyReason reason;
		float position[4];
		float rotation[4];
	};

	static_assert(sizeof(ProgressionRallyPacket) == 60,
		"progression rally packets must keep their fixed x86 wire layout");
}
}
