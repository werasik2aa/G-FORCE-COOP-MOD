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
        // `definition_id` and occurrence are not enough for dynamically built
        // trigger families: several map objects can share both.  This is the
        // exact fixed trigger transform hash already used by WorldObjectEvent.
        std::uint32_t trigger_signature;
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

    // `sub_41E890` is a cdecl wrapper around the object forwarder, while some
    // later map actions call that forwarder directly.  The receiver must retain
    // this distinction to reproduce the native chain in its original order.
    constexpr std::uint32_t kWorldObjectEventRouteRelay = 1u;
    constexpr std::uint32_t kWorldObjectEventRouteForwarder = 2u;
    // Monster/NPC map triggers use their own dispatcher. The host emits
    // Activation; a client sends Request instead of independently firing the
    // same local spawner, which is what previously produced duplicate mobs.
    constexpr std::uint32_t kWorldObjectEventRouteEntityTriggerActivation = 3u;
    constexpr std::uint32_t kWorldObjectEventRouteEntityTriggerRequest = 4u;

    // Native event routes do not expose a process-neutral object pointer. The
    // receiver resolves the matching map template by family/subtype/definition,
    // static transform signature, and exact executable vtable before replaying
    // the route (relay, forwarder, or entity dispatcher request/activation).
    struct WorldObjectEventPacket final : PacketHeader
    {
        std::uint32_t sequence;
        std::uint32_t source_vtable;
        std::uint32_t family;
        std::uint32_t subtype;
        std::int32_t definition_id;
        std::uint32_t transform_signature;
        std::int32_t event_code;
        std::uint32_t route;
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

    struct WorldModePacket final : PacketHeader
    {
        std::uint32_t world_id;
        std::uint32_t mode;
    };

    static_assert(sizeof(WorldTriggerKey) == 16,
        "world trigger identity must stay process-neutral and wire-stable");
    static_assert(sizeof(WorldDamagePacket) == 32,
        "damage packets must keep their fixed x86 wire layout");
    static_assert(sizeof(WorldDespawnPacket) == 24,
        "despawn packets must keep their fixed x86 wire layout");
    static_assert(sizeof(WorldModePacket) == 28,
        "mode packets must keep their fixed x86 wire layout");
    static_assert(sizeof(WorldSpawnPacket) == 76,
        "world spawn packets must keep their fixed x86 wire layout");
    static_assert(sizeof(WorldSnapshotPacket) == 60,
        "world snapshot packets must keep their fixed x86 wire layout");
    static_assert(sizeof(WorldReadyPacket) == 24,
        "world-ready packets must keep their fixed x86 wire layout");
    static_assert(sizeof(WorldTriggerEventPacket) == 44,
        "trigger-event packets must keep their fixed x86 wire layout");
    static_assert(sizeof(WorldObjectEventPacket) == 52,
        "object-event packets must keep their fixed x86 wire layout");
}
}
