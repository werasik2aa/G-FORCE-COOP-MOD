#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <vector>

#include "ServerClient/MTypes.h"

namespace coop
{
	// A local game pointer is meaningful only in its owning process.  WorldSync
	// assigns the host a small process-neutral id, then binds it to the matching
	// native object on the client.  It deliberately does not own NPC AI, damage or
	// despawn yet; this first layer proves matching and transform replication.
	class WorldSync final
	{
	public:
		struct TriggerKey
		{
			std::uint32_t family;
			std::uint32_t subtype;
			std::int32_t definition_id;
			std::uint32_t occurrence;
		};

		static WorldSync& Instance();

		void SendToRemote(const void* data, std::uint32_t size, int flags) const;
		void OnPeerConnected();
		void OnPeerDisconnected();
		void ResetForWorldLoad();
		void ClearGameState();
		// Called from P1's first post-load tick.  The socket worker later sends one
		// WorldReady packet, which makes the host replay its current baseline.
		void NotifyLocalWorldReady();
		// Returns the host-side occurrence counter for a native trigger pointer,
		// assigning a new one on first sight.  Only meaningful on the host.
		std::uint32_t HostOccurrence(void* trigger);
		// Returns the local trigger object matching a process-neutral trigger key,
		// or NULL when this process has not built that template yet.
		void* FindTemplateTrigger(std::uint32_t family,
			std::uint32_t subtype, std::int32_t definition_id);
		// Host-only: queues one reliable trigger-event packet for the client.
		void QueueHostTriggerEvent(const TriggerKey& key, int event_code,
			int result);
		// Game-thread: the local player damaged a world-linked entity.  Broadcasts
		// a reliable damage packet; both roles may call this.
		void ReportLocalDamage(void* entity, std::uint32_t amount,
			int event_code);
		// Returns the world id of a linked entity, or zero when untracked.
		std::uint32_t WorldIdOfEntity(void* entity) const;
		// Returns the live entity currently bound to a native trigger object.
		void* EntityOfTrigger(void* trigger) const;

		// These methods run only on the game thread, from the already verified
		// trigger factory/spawn hooks and P1's post-update tick.
		void RecordTriggerTemplate(void* trigger, std::uint32_t family,
			std::uint32_t subtype);
		void RecordNativeSpawn(void* trigger, void* entity, std::uint32_t family,
			std::uint32_t subtype, std::int32_t definition_id);
		void GameTick();
		// Called from the already-installed D3D Present hook, after game simulation.
		// A client applies the latest host sample here every rendered frame, so local
		// AI may calculate but cannot leave a linked entity visually elsewhere.
		void OnRenderFrame();

		// These methods run on the socket worker.  They only copy wire data and never
		// follow game pointers or call a native game function.
		bool OnRemotePacket(const void* data, std::uint32_t size);
		void NetworkTick();

	private:
		struct WorldSpawnPacket : PacketHeader
		{
			std::uint32_t world_id;
			TriggerKey key;
			float position[4];
			float rotation[4];
		};

		struct WorldSnapshotPacket : PacketHeader
		{
			std::uint32_t world_id;
			std::uint32_t sequence;
			float position[4];
			float rotation[4];
		};

		struct WorldReadyPacket : PacketHeader
		{
			std::uint32_t sequence;
		};

		// Host -> client notification that a native trigger received a gameplay
		// event (activation, hit, timer).  The receiver replays the same event on
		// its own matching trigger template so dynamic children (spiders) spawn
		// locally through the stock code path.
		struct WorldTriggerEventPacket : PacketHeader
		{
			TriggerKey key;
			std::int32_t event_code;
			std::int32_t result;
		};

		// Either side reports that its local player damaged a world-linked entity.
			// The receiver finds the same entity by world id and replays the hit on it.
		struct WorldDamagePacket : PacketHeader
		{
			std::uint32_t world_id;
			std::uint32_t amount;
			std::int32_t event_code;
		};

		// Host -> client: entity has been despawned/died.  Client removes it.
		struct WorldDespawnPacket : PacketHeader
		{
			std::uint32_t world_id;
		};

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
		static_assert(sizeof(WorldDamagePacket) == 32,
			"damage packets must keep their fixed x86 wire layout");

		struct TriggerCounter
		{
			void* trigger;
			std::uint32_t occurrence;
		};

		struct TriggerTemplate
		{
			void* trigger;
			std::uint32_t family;
			std::uint32_t subtype;
			std::int32_t definition_id;
		};

		struct HostEntity
		{
			void* entity;
			void* trigger;
			TriggerKey key;
			std::uint32_t world_id;
			float last_position[4];
			float last_rotation[4];
			bool announced;
			bool have_transform;
		};

		struct ClientEntity
		{
			void* entity;
			TriggerKey key;
			std::uint32_t world_id;
			bool logged_snapshot;
			bool logged_render_apply;
			bool has_latest_snapshot;
			WorldSnapshotPacket latest_snapshot;
			DWORD latest_received_tick;
			bool has_presentation;
			float presentation_position[4];
			float presentation_rotation[4];
			float blend_start_position[4];
			float blend_start_rotation[4];
			DWORD blend_started_tick;
			DWORD blend_duration_ms;
		};

		struct PendingSpawn
		{
			WorldSpawnPacket packet;
			DWORD last_attempt_tick;
			bool logged_missing_template;
		};

		WorldSync();
		~WorldSync() = default;
		WorldSync(const WorldSync&) = delete;
		WorldSync& operator=(const WorldSync&) = delete;

		bool IsSupportedFamily(std::uint32_t family) const;
		bool ReadEntityTransform(void* entity, float position[4],
			float rotation[4]) const;
		bool IsLiveEntity(void* entity) const;
		void EnumerateHostEntities();
		void EnumerateClientEntities();
		void ProcessClientPackets();
		void ResolvePendingSpawns();
		void ApplyIncomingDamage();
		// SEH-isolated: the native dispatcher may fault on a dying entity.
		static void ApplyOneHit(void* trigger, std::uint32_t amount,
			std::uint32_t world_id, int event_code);
		void ApplyPendingSnapshots();
		void AcceptSnapshot(ClientEntity& entity,
			const WorldSnapshotPacket& snapshot, DWORD received_tick);
		void AdvancePresentation(ClientEntity& entity, DWORD now);
		void ApplyPresentation(ClientEntity& entity);
		void QueueHostSpawn(HostEntity& entity);
		void QueueHostSnapshot(HostEntity& entity, const float position[4],
			const float rotation[4]);
		std::uint32_t NextOccurrence(std::vector<TriggerCounter>& counters,
			void* trigger);
		HostEntity* FindHostEntity(void* entity);
		ClientEntity* FindClientEntity(void* entity);
		ClientEntity* FindClientEntityById(std::uint32_t world_id);
		ClientEntity* FindUnlinkedClientEntity(const TriggerKey& key);
		TriggerTemplate* FindTriggerTemplate(const TriggerKey& key);
		void AddClientEntity(void* entity, const TriggerKey& key,
			std::uint32_t world_id);
		bool TrySpawnClientEntity(PendingSpawn& pending);
		// Host -> client: queues a despawn packet for a dead entity.
		void QueueHostDespawn(std::uint32_t world_id);
		// Removes a dead entity from tracking and notifies clients.
		void ProcessHostDespawns();
		// Client-side: handles incoming despawn packet.
		void HandleIncomingDespawn(const WorldDespawnPacket& packet);

	public:
		// F9 debug: kills the live NPC/monster nearest to P1 by setting the same
		// controller death state the remote-damage path uses.  Diagnostic tool.
		void DebugKillNearest();

		struct PendingDamage
		{
			std::uint32_t world_id;
			std::uint32_t amount;
			std::int32_t event_code;
		};

		SRWLOCK m_packet_lock;
		mutable SRWLOCK m_damage_lock;
		std::vector<WorldDamagePacket> m_outgoing_damage;
		std::vector<WorldDamagePacket> m_incoming_damage;
		std::vector<PendingDamage> m_pending_damage;
		std::vector<WorldDespawnPacket> m_outgoing_despawns;
		std::vector<WorldDespawnPacket> m_incoming_despawns;
		std::vector<WorldSpawnPacket> m_outgoing_spawns;
		std::vector<WorldSnapshotPacket> m_outgoing_snapshots;
		std::vector<WorldTriggerEventPacket> m_outgoing_trigger_events;
		std::vector<WorldSpawnPacket> m_incoming_spawns;
		std::vector<WorldSnapshotPacket> m_incoming_snapshots;
		std::vector<WorldTriggerEventPacket> m_incoming_trigger_events;
		std::vector<TriggerCounter> m_host_trigger_counters;
		std::vector<TriggerCounter> m_client_trigger_counters;
		std::vector<TriggerTemplate> m_trigger_templates;
		std::vector<HostEntity> m_host_entities;
		std::vector<ClientEntity> m_client_entities;
		std::vector<PendingSpawn> m_pending_spawns;
		std::vector<WorldSnapshotPacket> m_pending_snapshots;
		std::uint32_t m_next_world_id;
		std::uint32_t m_snapshot_sequence;
		DWORD m_last_snapshot_tick;
		volatile LONG m_host_resync_requested;
		volatile LONG m_client_ready_pending;
		volatile LONG m_client_ready_sent;
		std::uint32_t m_client_ready_sequence;
		bool m_forced_client_spawn_active;
		WorldSpawnPacket m_forced_client_spawn;
	};
}
