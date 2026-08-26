#include "packet_dispatch.h"

#include "packet_view.h"
#include "../coop_netgame.h"
#include "../save_sync.h"
#include "../world_sync.h"

namespace coop
{
namespace protocol
{
    bool DispatchInboundPacket(const void* data, std::uint32_t size)
    {
        const PacketView view(data, size);
        switch (view.Kind())
        {
        case PacketKind::Input:
            // CoopNetGame validates and copies the input snapshot under its lock.
            CoopNetGame::Instance().OnRemotePacket(data, size);
            return true;

        case PacketKind::SaveSlot:
            return SaveSync::Instance().OnRemotePacket(data, size);

        case PacketKind::WorldSpawn:
        case PacketKind::WorldSnapshot:
        case PacketKind::WorldReady:
        case PacketKind::WorldTriggerEvent:
        case PacketKind::WorldDamage:
        case PacketKind::WorldDespawn:
        case PacketKind::TriggerP1Teleport:
            return WorldSync::Instance().OnRemotePacket(data, size);

        default:
            return false;
        }
    }
}
}
