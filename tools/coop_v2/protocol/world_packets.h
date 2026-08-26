#pragma once

#include <cstdint>

#include "../ServerClient/MTypes.h"

namespace coop
{
namespace protocol
{
    struct WorldTriggerKey final
    {
        std::uint32_t family;
        std::uint32_t subtype;
        std::int32_t definition_id;
        std::uint32_t occurrence;
    };

    struct WorldSpawnPacket final : PacketHeader
    {
        std::uint32_t world_id;
        WorldTriggerKey key;
        float position[4];
        float rotation[4];
    };

    struct WorldSnapshotPacket final : PacketHeader
    {
        std::uint32_t world_id;
        std::uint32_t sequence;
        float position[4];
        float rotation[4];
    };

    struct WorldReadyPacket final : PacketHeader
    {
        std::uint32_t sequence;
    };

    struct WorldTriggerEventPacket final : PacketHeader
    {
        WorldTriggerKey key;
        std::int32_t event_code;
        std::int32_t result;
    };

    struct TriggerP1TeleportPacket final : PacketHeader
    {
        std::uint32_t sequence;
        float position[4];
    };

    struct WorldDamagePacket final : PacketHeader
    {
        std::uint32_t world_id;
        std::uint32_t hp_bits;
        std::int32_t event_code;
    };

    struct WorldDespawnPacket final : PacketHeader
    {
        std::uint32_t world_id;
    };

    static_assert(sizeof(WorldTriggerKey) == 16,
        "world trigger identity must stay process-neutral and wire-stable");
    static_assert(sizeof(WorldDamagePacket) == 32,
        "damage packets must keep their fixed x86 wire layout");
    static_assert(sizeof(WorldDespawnPacket) == 24,
        "despawn packets must keep their fixed x86 wire layout");
    static_assert(sizeof(WorldSpawnPacket) == 72,
        "world spawn packets must keep their fixed x86 wire layout");
    static_assert(sizeof(WorldSnapshotPacket) == 60,
        "world snapshot packets must keep their fixed x86 wire layout");
    static_assert(sizeof(WorldReadyPacket) == 24,
        "world-ready packets must keep their fixed x86 wire layout");
    static_assert(sizeof(WorldTriggerEventPacket) == 44,
        "trigger-event packets must keep their fixed x86 wire layout");
    static_assert(sizeof(TriggerP1TeleportPacket) == 40,
        "trigger pulse packets must keep their fixed x86 wire layout");
}
}
