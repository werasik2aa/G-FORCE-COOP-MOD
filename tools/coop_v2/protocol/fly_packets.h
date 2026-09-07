#pragma once

#include <cstdint>

#include "../ServerClient/MTypes.h"

namespace coop
{
namespace protocol
{
	enum class FlyAbility : std::uint32_t
	{
		Invalid = 0,
		// User-observed dual Mooch laser. The static route is one exact
		// Fly_Active raw pressed-edge query; it is never a P1 scanner action.
		DualLaser = 1
	};

	struct FlyAbilityPacket final : PacketHeader
	{
		std::uint32_t sequence;
		FlyAbility ability;
		// The transform epoch produced by this same native Fly tick. Its raw action
		// happens before the tick publishes body rotation, so the sender tags that
		// next post-tick epoch and the receiver waits for it before rendering.
		// Zero is allowed only during initial native Fly entry.
		std::uint32_t source_fly_transform_sequence;
		// Stock Fly_Active turns its current ray into a world-space target point
		// before enabling the two emitters. The receiver applies this target to
		// Mooch's own controller-local presentation task; it is neither a pointer
		// nor input/HUD/camera state.
		float laser_target[3];
	};

	static_assert(sizeof(FlyAbilityPacket) == 44,
		"Fly ability packet must remain a fixed x86 wire record");
}
}
