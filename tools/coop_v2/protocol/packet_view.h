#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "../ServerClient/MTypes.h"

namespace coop
{
namespace protocol
{
    enum class PacketKind : std::uint32_t
    {
        Invalid = kCoopPacketInvalid,
        Hello = kCoopPacketHello,
        Welcome = kCoopPacketWelcome,
        Input = kCoopPacketInput,
        SaveSlot = kCoopPacketSaveSlot,
        WorldSpawn = kCoopPacketWorldSpawn,
        WorldSnapshot = kCoopPacketWorldSnapshot,
        WorldReady = kCoopPacketWorldReady,
        WorldTriggerEvent = kCoopPacketWorldTriggerEvent,
        WorldDamage = kCoopPacketWorldDamage,
        WorldDespawn = kCoopPacketWorldDespawn,
        TriggerP1Teleport = kCoopPacketTriggerP1Teleport
    };

	template<typename T>
	void InitializeFixedPacket(T& packet, PacketKind kind)
	{
		static_assert(std::is_base_of<PacketHeader, T>::value,
			"Fixed packet types must inherit PacketHeader");
		packet.m_PacketID = static_cast<std::uint32_t>(kind);
		packet.m_RealSize = static_cast<std::uint32_t>(
			sizeof(T) - sizeof(PacketHeader));
		packet.m_SizeOne = packet.m_RealSize;
		packet.m_CompressSize = 0;
	}

	class PacketView final
	{
    public:
        PacketView(const void* data, std::uint32_t size) : data_(data), size_(size) {}

        bool ReadHeader(PacketHeader& out) const
        {
            if (!data_ || size_ < sizeof(PacketHeader))
                return false;
            std::memcpy(&out, data_, sizeof(out));
            return true;
        }

        PacketKind Kind() const
        {
            PacketHeader header = {};
            return ReadHeader(header) ?
                static_cast<PacketKind>(header.m_PacketID) : PacketKind::Invalid;
        }

        template<typename T>
        bool CopyUncompressedExact(T& out) const
        {
            static_assert(std::is_trivially_copyable<T>::value,
                "Wire packets must be trivially copyable");
            PacketHeader header = {};
            if (!ReadHeader(header) || size_ != sizeof(T) ||
                header.m_CompressSize != 0 || header.Size() != size_)
            {
                return false;
            }
            std::memcpy(&out, data_, sizeof(T));
            return true;
        }

        std::uint32_t size() const { return size_; }

    private:
        const void* data_;
        std::uint32_t size_;
    };
}
}
