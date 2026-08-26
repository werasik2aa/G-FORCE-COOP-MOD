#pragma once

#include <cstdint>

namespace coop
{
namespace protocol
{
    // Worker-thread-only. The dispatcher selects a feature queue by wire type;
    // individual features must not follow retail game pointers from this call.
    bool DispatchInboundPacket(const void* data, std::uint32_t size);
}
}
