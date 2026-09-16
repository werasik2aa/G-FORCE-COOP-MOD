#pragma once

#include <cstddef>
#include <cstdint>

#include "../ServerClient/MTypes.h"

namespace coop
{
namespace protocol
{
	constexpr std::size_t kChatUtf8Capacity = 192;

	// The payload is a nul-terminated UTF-8 string. A fixed packet keeps the
	// socket worker validation simple and bounds text before it reaches the UI.
	struct ChatPacket final : PacketHeader
	{
		std::uint32_t sequence;
		char utf8[kChatUtf8Capacity];
	};

	static_assert(sizeof(ChatPacket) == 216,
		"chat packets must keep their fixed x86 wire layout");
}
}
