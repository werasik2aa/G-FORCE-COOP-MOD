#pragma once

#include <cstdint>
#include <string>

constexpr int kServerReadBatch = 128;
constexpr int kClientReadBatch = 32;

enum class MConnectState : std::uint8_t
{
	None,
	Connecting,
	Connected
};

enum CoopNetworkEndReason
{
	kCoopEndUnknown = 4000,
	kCoopEndServerShutdown,
	kCoopEndSessionFull,
	kCoopEndNotFriend
};

enum CoopPacketId : std::uint32_t
{
	kCoopPacketInvalid = 0,
	kCoopPacketHello = 1,
	kCoopPacketWelcome = 2,
	kCoopPacketInput = 10,
	kCoopPacketSaveSlot = 20,
	// Reliable host-created world entity event.  It carries a process-neutral
	// trigger identity, never a raw game pointer.
	kCoopPacketWorldSpawn = 30,
	// Position/rotation correction for an already linked world entity.
	kCoopPacketWorldSnapshot = 31,
	// Sent by the client after its stock save load has reached a real P1 tick.
	// It asks the host to send the current living-world baseline again.
	kCoopPacketWorldReady = 32
};

struct PacketHeader
{
	std::uint32_t m_RealSize = 0;
	std::uint32_t m_CompressSize = 0;
	std::uint32_t m_SizeOne = 0;
	std::uint32_t m_PacketID = kCoopPacketInvalid;
	std::uint32_t m_ClientID = 0;

	std::uint32_t UncompressSize() const { return m_RealSize; }
	std::uint32_t CompressedSize() const { return m_CompressSize; }
	bool IsCompressedSize() const { return m_CompressSize > 0; }
	std::uint32_t Size() const
	{
		return static_cast<std::uint32_t>(sizeof(PacketHeader)) +
			(m_CompressSize > 0 ? m_CompressSize : m_RealSize);
	}
};

// Number of logical keyboard actions the engine defines (0x43 == 67).  Rounded
// up to 68 so the per-action byte arrays keep the trailing uint32 fields
// naturally 4-byte aligned.  Index range actually used is [0, 67).
constexpr std::uint32_t kCoopActionCount = 68;
// Fly_Active uses six fixed raw 0x4008xxxx queries in addition to the normal
// logical action table.  They are controller semantics, not player key binds.
constexpr std::uint32_t kCoopFlyRawActionCount = 6;

struct CoopInput
{
	std::uint32_t virtual_keys[8];
	float position[4];
	float rotation[4];
	float look_axis[2];
	// Held level, one bit per action index.  A held key is a stable state, not a
	// one-frame event, so it survives packet pacing on the receiver.
	std::uint32_t action_down[3];
	// Monotonic per-action counters.  The sender increments an action's counter
	// once per physical press/release edge (captured after the engine's own bind
	// mapping); the receiver detects an edge by observing the counter change and
	// latches it for exactly one P2 frame.  This is bind-agnostic and never loses
	// a press between network packets.  uint8 wraps at 256 edges, which is far
	// more than can occur inside one packet interval.
	std::uint8_t action_press_seq[kCoopActionCount];
	std::uint8_t action_release_seq[kCoopActionCount];
	std::uint32_t transform_sequence;
	std::uint32_t selected_weapon_type;
	std::uint32_t weapon_sequence;
	// The engine's fire handler does not reconstruct this from look_axis. It
	// copies the cached XGamePad ray verbatim into its shot command, so send the
	// exact ray calculated on the controlling machine.
	float aim_origin[3];
	float aim_direction[3];
	// Yaw of the sender's own camera, exactly as 0x52AD20 returns it (already
	// negated by that function), plus a validity word so a receiver never turns
	// P2 towards a zero yaw published before the sender's first camera update.
	// The single shared camera handler on the receiving machine belongs to its
	// local P1, so P2's body-turn and movement-direction paths must read this
	// instead of it.
	float camera_yaw;
	std::uint32_t camera_yaw_valid;
	// Mooch exists exactly once in the game world.  While its owner controls it,
	// send its live position separately from the owner's Darwin transform.  The
	// receiver applies this after Mooch's own stock tick, so it is presentation
	// only and cannot give the other player control of the shared fly.
	float fly_position[4];
	std::uint32_t fly_transform_sequence;
	std::uint32_t fly_controlled;
	// Held and edge state for Fly_Active's raw controller queries.  Without this
	// the remote side can enter Mooch but never reaches its native scan/fire mode.
	std::uint32_t fly_raw_down;
	std::uint8_t fly_raw_press_seq[kCoopFlyRawActionCount];
	std::uint8_t fly_raw_release_seq[kCoopFlyRawActionCount];
};

static_assert(sizeof(CoopInput) == 304,
	"CoopInput is the authoritative remote-player state; host and client "
	"builds must share this exact layout");

struct CoopInputPacket : PacketHeader
{
	CoopInput input;
};

template<typename T>
struct ServerPacket : PacketHeader
{
	T SubPacket = {};

	ServerPacket()
	{
		m_RealSize = sizeof(T);
		m_SizeOne = sizeof(T);
	}
};

void __cdecl Msg(const char* format, ...);
