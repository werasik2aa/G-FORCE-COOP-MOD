#include "world_sync.h"

#include "coop_netgame.h"
#include "coop_runtime.h"
#include "gforce_constants.h"
#include "ServerClient/MClient.h"
#include "ServerClient/MServer.h"
#include "ServerClient/MServerONLINE.h"
#include "ServerClient/SteamManager.h"

#include <math.h>
#include <string.h>

namespace coop
{
	constexpr DWORD kWorldSnapshotIntervalMs = 350;
	constexpr DWORD kMissingSpawnRetryMs = 1000;
	constexpr size_t kWorldRegistryWalkLimit = 512;
		constexpr size_t kMaxPendingWorldPackets = 1024;

	bool SameTriggerKey(const WorldSync::TriggerKey& left, const WorldSync::TriggerKey& right)
	{
		return left.family == right.family && left.subtype == right.subtype &&
			left.definition_id == right.definition_id &&
			left.occurrence == right.occurrence;
	}

	bool SameTriggerTemplate(const coop::WorldSync::TriggerKey& key,
		std::uint32_t family, std::uint32_t subtype, std::int32_t definition_id)
	{
		return key.family == family && key.subtype == subtype &&
			key.definition_id == definition_id;
	}

		bool TransformChanged(const float old_position[4], const float old_rotation[4],
			const float position[4], const float rotation[4])
		{
			for (size_t index = 0; index != 3; ++index)
			{
				if (fabsf(old_position[index] - position[index]) > 0.015f)
					return true;
			}
			for (size_t index = 0; index != 4; ++index)
			{
				if (fabsf(old_rotation[index] - rotation[index]) > 0.0025f)
					return true;
			}
			return false;
		}

	WorldSync& WorldSync::Instance()
	{
		static WorldSync instance;
		return instance;
	}

	WorldSync::WorldSync() :
		m_next_world_id(1),
		m_snapshot_sequence(0),
		m_last_snapshot_tick(0),
		m_host_resync_requested(1),
		m_client_ready_pending(0),
		m_client_ready_sent(0),
				m_client_ready_sequence(0),
		m_forced_client_spawn_active(false),
		m_trigger_p1_teleport_sequence(0),
		m_last_trigger_p1_teleport_sequence(0),
		m_remote_p1_teleport_active(false),
		m_remote_p1_teleport_restore_tick(0)
	{

		InitializeSRWLock(&m_packet_lock);
		InitializeSRWLock(&m_damage_lock);
		ZeroMemory(&m_forced_client_spawn, sizeof(m_forced_client_spawn));
	}

	bool WorldSync::IsSupportedFamily(std::uint32_t family) const
	{
		return family == gforce::kMonsterTriggerFamily ||
			family == gforce::kNpcTriggerFamily;
	}

	void WorldSync::OnPeerConnected()
	{
		// The socket worker cannot enumerate game objects.  It only asks the next
		// game tick to resend every host object with a fresh reliable spawn event.
		InterlockedExchange(&m_host_resync_requested, 1);
		// A client may already have a loaded world when it connects.  Its next P1
		// tick will turn this into one WorldReady request for the host.
		InterlockedExchange(&m_client_ready_sent, 0);
	}

	void WorldSync::OnPeerDisconnected()
	{
		InterlockedExchange(&m_host_resync_requested, 1);
		InterlockedExchange(&m_client_ready_pending, 0);
		InterlockedExchange(&m_client_ready_sent, 0);
		AcquireSRWLockExclusive(&m_packet_lock);
		m_outgoing_spawns.clear();
		m_outgoing_snapshots.clear();
				m_outgoing_trigger_events.clear();
		m_outgoing_p1_teleports.clear();
		m_outgoing_despawns.clear();

		m_incoming_spawns.clear();
		m_incoming_snapshots.clear();
				m_incoming_trigger_events.clear();
		m_incoming_p1_teleports.clear();
		m_pending_p1_teleports.clear();
		m_incoming_despawns.clear();

		ReleaseSRWLockExclusive(&m_packet_lock);
	}

	void WorldSync::ClearGameState()
	{
		m_host_trigger_counters.clear();
		m_client_trigger_counters.clear();
		m_trigger_templates.clear();
		m_host_entities.clear();
		m_client_entities.clear();
		m_pending_spawns.clear();
				m_pending_snapshots.clear();
		m_pending_p1_teleports.clear();
		m_activated_local_p1_triggers.clear();
		m_next_world_id = 1;

		m_snapshot_sequence = 0;
		m_last_snapshot_tick = 0;
		m_client_ready_sequence = 0;
		InterlockedExchange(&m_client_ready_pending, 0);
		InterlockedExchange(&m_client_ready_sent, 0);
		m_forced_client_spawn_active = false;
		ZeroMemory(&m_forced_client_spawn, sizeof(m_forced_client_spawn));
		m_trigger_p1_teleport_sequence = 0;
		m_last_trigger_p1_teleport_sequence = 0;
		m_remote_p1_teleport_active = false;
		m_remote_p1_teleport_restore_tick = 0;
		ZeroMemory(m_remote_p1_saved_position, sizeof(m_remote_p1_saved_position));
		ZeroMemory(m_remote_p1_saved_rotation, sizeof(m_remote_p1_saved_rotation));
		ZeroMemory(m_remote_p1_target_position, sizeof(m_remote_p1_target_position));
		AcquireSRWLockExclusive(&m_damage_lock);
		m_outgoing_damage.clear();
		m_incoming_damage.clear();
		m_pending_damage.clear();
		m_outgoing_despawns.clear();
		m_incoming_despawns.clear();
		ReleaseSRWLockExclusive(&m_damage_lock);
	}

	void WorldSync::ResetForWorldLoad()
	{
		ClearGameState();
		AcquireSRWLockExclusive(&m_packet_lock);
		m_outgoing_spawns.clear();
		m_outgoing_snapshots.clear();
				m_outgoing_trigger_events.clear();
		m_outgoing_p1_teleports.clear();
		m_incoming_spawns.clear();

		m_incoming_snapshots.clear();
				m_incoming_trigger_events.clear();
		m_incoming_p1_teleports.clear();
		ReleaseSRWLockExclusive(&m_packet_lock);

		AcquireSRWLockExclusive(&m_damage_lock);
		m_outgoing_damage.clear();
		m_incoming_damage.clear();
		m_pending_damage.clear();
		m_outgoing_despawns.clear();
		m_incoming_despawns.clear();
		ReleaseSRWLockExclusive(&m_damage_lock);
		InterlockedExchange(&m_host_resync_requested, 1);
	}

	void WorldSync::NotifyLocalWorldReady()
	{
		if (!CoopNetGame::Instance().IsClient())
			return;
		if (InterlockedCompareExchange(&m_client_ready_sent, 0, 0) == 0)
			InterlockedExchange(&m_client_ready_pending, 1);
	}

	std::uint32_t WorldSync::NextOccurrence(
		std::vector<TriggerCounter>& counters, void* trigger)
	{
		for (TriggerCounter& counter : counters)
		{
			if (counter.trigger == trigger)
				return ++counter.occurrence;
		}
		TriggerCounter counter = {};
		counter.trigger = trigger;
		counter.occurrence = 1;
		counters.push_back(counter);
		return counter.occurrence;
	}

	std::uint32_t WorldSync::HostOccurrence(void* trigger)
	{
		return NextOccurrence(m_host_trigger_counters, trigger);
	}

	void* WorldSync::FindTemplateTrigger(std::uint32_t family,
		std::uint32_t subtype, std::int32_t definition_id)
	{
		TriggerKey key = {};
		key.family = family;
		key.subtype = subtype;
		key.definition_id = definition_id;
		TriggerTemplate* found = FindTriggerTemplate(key);
		if (!found)
		{
			// Spawn-definition ids are per-process table indices and can differ
			// between the two loads of the same map.  Fall back to the only
			// stable parts of the identity: family plus subtype.
			for (TriggerTemplate& existing : m_trigger_templates)
			{
				if (existing.family == family && existing.subtype == subtype)
				{
					found = &existing;
					CoopRuntime::Instance().Log(
						"[world-trigger] loose template match family=%08X subtype=%08X host_def=%d local_def=%d\r\n",
						family, subtype, definition_id, existing.definition_id);
					break;
				}
			}
		}
		return found ? found->trigger : NULL;
	}

	bool WorldSync::ReadEntityTransform(void* entity, float position[4],
		float rotation[4]) const
	{
		if (!entity || !position || !rotation)
			return false;
		__try
		{
			const BYTE* const bytes = static_cast<const BYTE*>(entity);
			memcpy(position, bytes + gforce::kEntityPositionOffset, sizeof(float) * 4);
			memcpy(rotation, bytes + gforce::kEntityRotationOffset, sizeof(float) * 4);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool WorldSync::ReadEntityHealth(void* entity, float& health) const
	{
		if (!entity)
			return false;
		__try
		{
			const BYTE* const entity_bytes = static_cast<const BYTE*>(entity);
			const BYTE* const handler = *reinterpret_cast<const BYTE* const*>(
				entity_bytes + gforce::kEntityHandlerOffset);
			if (!handler)
				return false;
			health = *reinterpret_cast<const float*>(handler +
				gforce::kHandlerHealthOffset);
			return health == health && health >= 0.0f && health <= 100000.0f;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool WorldSync::IsLiveEntity(void* entity) const
	{
		if (!entity)
			return false;
		__try
		{
			BYTE* const registry = *reinterpret_cast<BYTE**>(gforce::kEntityRegistry);
			const size_t list_offsets[] = {
				gforce::kEntityRegistryMonsterListOffset,
				gforce::kEntityRegistryNpcListOffset
			};
			for (size_t list_index = 0; registry &&
				list_index != _countof(list_offsets); ++list_index)
			{
				BYTE* node = *reinterpret_cast<BYTE**>(registry + list_offsets[list_index]);
				for (size_t visited = 0; node && visited != kWorldRegistryWalkLimit;
					++visited)
				{
					if (*reinterpret_cast<void**>(node +
						gforce::kIntrusiveListValueOffset) == entity)
					{
						return true;
					}
					node = *reinterpret_cast<BYTE**>(node +
						gforce::kIntrusiveListNextOffset);
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
		return false;
	}

	void WorldSync::RecordTriggerTemplate(void* trigger, std::uint32_t family,
		std::uint32_t subtype)
	{
		// Keep every map-defined trigger: F9 world-event experiments include
		// breakables and doors, not only live NPC/monster spawners.
		if (!trigger)
			return;

		std::int32_t definition_id = -1;
		__try
		{
			definition_id = *reinterpret_cast<const std::int32_t*>(
				static_cast<const BYTE*>(trigger) + gforce::kTriggerSpawnIdOffset);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return;
		}
		

		for (TriggerTemplate& existing : m_trigger_templates)
		{
			if (existing.trigger == trigger)
			{
				existing.family = family;
				existing.subtype = subtype;
				existing.definition_id = definition_id;
				return;
			}
		}
		TriggerTemplate entry = {};
		entry.trigger = trigger;
		entry.family = family;
		entry.subtype = subtype;
		entry.definition_id = definition_id;
		m_trigger_templates.push_back(entry);
	}

	WorldSync::HostEntity* WorldSync::FindHostEntity(void* entity)
	{
		for (HostEntity& existing : m_host_entities)
		{
			if (existing.entity == entity)
				return &existing;
		}
		return NULL;
	}

	WorldSync::ClientEntity* WorldSync::FindClientEntityById(
		std::uint32_t world_id)
	{
		if (!world_id)
			return NULL;
		for (ClientEntity& existing : m_client_entities)
		{
			if (existing.world_id == world_id)
				return &existing;
		}
		return NULL;
	}

	WorldSync::ClientEntity* WorldSync::FindClientEntity(void* entity)
	{
		if (!entity)
			return NULL;
		for (ClientEntity& existing : m_client_entities)
		{
			if (existing.entity == entity)
				return &existing;
		}
		return NULL;
	}

	WorldSync::ClientEntity* WorldSync::FindUnlinkedClientEntity(
		const TriggerKey& key)
	{
		for (ClientEntity& existing : m_client_entities)
		{
			if (existing.world_id == 0 && SameTriggerKey(existing.key, key))
				return &existing;
		}
		return NULL;
	}

	WorldSync::TriggerTemplate* WorldSync::FindTriggerTemplate(
		const TriggerKey& key)
	{
		for (TriggerTemplate& existing : m_trigger_templates)
		{
			if (SameTriggerTemplate(key, existing.family, existing.subtype,
				existing.definition_id))
			{
				return &existing;
			}
		}
		return NULL;
	}

	void WorldSync::QueueHostSpawn(HostEntity& entity)
	{
		if (!CoopNetGame::Instance().HasRemotePeer())
			return;

		WorldSpawnPacket packet = {};
		packet.m_PacketID = kCoopPacketWorldSpawn;
		packet.m_RealSize = sizeof(packet) - sizeof(PacketHeader);
		packet.m_SizeOne = packet.m_RealSize;
		packet.world_id = entity.world_id;
		packet.key = entity.key;
		memcpy(packet.position, entity.last_position, sizeof(packet.position));
		memcpy(packet.rotation, entity.last_rotation, sizeof(packet.rotation));
		AcquireSRWLockExclusive(&m_packet_lock);
		m_outgoing_spawns.push_back(packet);
		ReleaseSRWLockExclusive(&m_packet_lock);
		entity.announced = true;
		CoopRuntime::Instance().Log(
			"[world-id] host id=%u def=%d occ=%u entity=%p\r\n",
			entity.world_id, entity.key.definition_id, entity.key.occurrence,
			entity.entity);
	}

	void WorldSync::QueueHostDespawn(std::uint32_t world_id)
	{
		if (!CoopNetGame::Instance().IsHost() ||
			!CoopNetGame::Instance().HasRemotePeer())
			return;

		WorldDespawnPacket packet = {};
		packet.m_PacketID = kCoopPacketWorldDespawn;
		packet.m_RealSize = sizeof(packet) - sizeof(PacketHeader);
		packet.m_SizeOne = packet.m_RealSize;
		packet.world_id = world_id;
		AcquireSRWLockExclusive(&m_packet_lock);
		m_outgoing_despawns.push_back(packet);
		ReleaseSRWLockExclusive(&m_packet_lock);
		CoopRuntime::Instance().Log(
			"[world-despawn] queued id=%u\n", world_id);
	}

	void WorldSync::QueueHostSnapshot(HostEntity& entity,
		const float position[4], const float rotation[4])
	{
		if (!entity.announced)
			return;
		WorldSnapshotPacket packet = {};
		packet.m_PacketID = kCoopPacketWorldSnapshot;
		packet.m_RealSize = sizeof(packet) - sizeof(PacketHeader);
		packet.m_SizeOne = packet.m_RealSize;
		packet.world_id = entity.world_id;
		packet.sequence = ++m_snapshot_sequence;
		memcpy(packet.position, position, sizeof(packet.position));
		memcpy(packet.rotation, rotation, sizeof(packet.rotation));
		AcquireSRWLockExclusive(&m_packet_lock);
		m_outgoing_snapshots.push_back(packet);
		ReleaseSRWLockExclusive(&m_packet_lock);
		memcpy(entity.last_position, position, sizeof(entity.last_position));
		memcpy(entity.last_rotation, rotation, sizeof(entity.last_rotation));
		entity.have_transform = true;
	}

	void WorldSync::QueueHostTriggerEvent(const TriggerKey& key, int event_code,
		int result)
	{
		if (!CoopNetGame::Instance().IsHost() ||
			!CoopNetGame::Instance().HasRemotePeer() || !key.occurrence)
			return;

		WorldTriggerEventPacket packet = {};
		packet.m_PacketID = kCoopPacketWorldTriggerEvent;
		packet.m_RealSize = sizeof(packet) - sizeof(PacketHeader);
		packet.m_SizeOne = packet.m_RealSize;
		packet.key = key;
		packet.event_code = event_code;
		packet.result = result;
		AcquireSRWLockExclusive(&m_packet_lock);
		m_outgoing_trigger_events.push_back(packet);
		ReleaseSRWLockExclusive(&m_packet_lock);
	}

	std::uint32_t WorldSync::WorldIdOfEntity(void* entity) const
	{
		if (!entity)
			return 0;
		if (CoopNetGame::Instance().IsHost())
		{
			for (const HostEntity& tracked : m_host_entities)
			{
				if (tracked.entity == entity && tracked.world_id)
					return tracked.world_id;
			}
		}
		else if (CoopNetGame::Instance().IsClient())
		{
			for (const ClientEntity& tracked : m_client_entities)
			{
				if (tracked.entity == entity && tracked.world_id)
					return tracked.world_id;
			}
		}
		return 0;
	}

		bool WorldSync::ReportLocalDamage(void* entity, int event_code)
		{
			if (!entity || !CoopNetGame::Instance().HasRemotePeer())
				return false;
			const std::uint32_t world_id = WorldIdOfEntity(entity);
			if (!world_id)
				return false;
			float health = 0.0f;
			if (!ReadEntityHealth(entity, health))
				return false;

			WorldDamagePacket packet = {};
			packet.m_PacketID = kCoopPacketWorldDamage;
			packet.m_RealSize = sizeof(packet) - sizeof(PacketHeader);
			packet.m_SizeOne = packet.m_RealSize;
			packet.world_id = world_id;
			memcpy(&packet.hp_bits, &health, sizeof(packet.hp_bits));
			packet.event_code = event_code;
			AcquireSRWLockExclusive(&m_damage_lock);
			bool queued = false;
			if (m_outgoing_damage.size() < kMaxPendingWorldPackets)
			{
				m_outgoing_damage.push_back(packet);
				queued = true;
			}
			ReleaseSRWLockExclusive(&m_damage_lock);
			return queued;
		}

	void* WorldSync::EntityOfTrigger(void* trigger) const
	{
		if (!trigger)
			return NULL;
		if (CoopNetGame::Instance().IsHost())
		{
			for (const HostEntity& tracked : m_host_entities)
			{
				if (tracked.trigger == trigger && tracked.entity &&
					tracked.world_id)
				{
					return tracked.entity;
				}
			}
		}
		else if (CoopNetGame::Instance().IsClient())
		{
			for (const ClientEntity& tracked : m_client_entities)
			{
				void* const entity_trigger = *reinterpret_cast<void* const*>(
					static_cast<const BYTE*>(tracked.entity) +
					gforce::kEntityTriggerOffset);
				if (entity_trigger == trigger && tracked.entity && tracked.world_id)
					return tracked.entity;
			}
		}
		return NULL;
	}

		void WorldSync::ObserveLocalP1Trigger(void* trigger, bool require_p1_proximity)
	{
		constexpr float kTriggerProximityRange = 4.0f;
		constexpr float kTriggerProximityRangeSq =
			kTriggerProximityRange * kTriggerProximityRange;
		if (!trigger || !CoopNetGame::Instance().HasRemotePeer())
			return;
		// The temporary P1 move deliberately makes local triggers fire.  Those
		// effects must stay local; otherwise the receiver would echo the same pulse
		// back and create a cross-process trigger loop.
		if (m_remote_p1_teleport_active)
			return;
		// A hit event on a linked NPC is not a map proximity trigger.  Never pulse
		// the peer's camera for ordinary combat damage.
		if (EntityOfTrigger(trigger))
			return;

		if (std::find(m_activated_local_p1_triggers.begin(),
			m_activated_local_p1_triggers.end(), trigger) !=
			m_activated_local_p1_triggers.end())
		{
			return;
		}

		__try
		{
			void* const p1 = reinterpret_cast<void**>(gforce::kGPigEntityArray)[1];
			if (!p1)
				return;
			const float* const p1_position = reinterpret_cast<const float*>(
				static_cast<const BYTE*>(p1) + gforce::kEntityPositionOffset);
			const float* const trigger_position = reinterpret_cast<const float*>(
				static_cast<const BYTE*>(trigger) + gforce::kTriggerPositionOffset);
			const float dx = p1_position[0] - trigger_position[0];
			const float dy = p1_position[1] - trigger_position[1];
			const float dz = p1_position[2] - trigger_position[2];
			const float distance_sq = dx * dx + dy * dy + dz * dz;
			if (require_p1_proximity && distance_sq > kTriggerProximityRangeSq)
				return;

			TriggerP1TeleportPacket packet = {};
			packet.m_PacketID = kCoopPacketTriggerP1Teleport;
			packet.m_RealSize = sizeof(packet) - sizeof(PacketHeader);
			packet.m_SizeOne = packet.m_RealSize;
			packet.sequence = ++m_trigger_p1_teleport_sequence;
			memcpy(packet.position, trigger_position, sizeof(packet.position));
			AcquireSRWLockExclusive(&m_packet_lock);
			if (m_outgoing_p1_teleports.size() < kMaxPendingWorldPackets)
				m_outgoing_p1_teleports.push_back(packet);
			ReleaseSRWLockExclusive(&m_packet_lock);
			m_activated_local_p1_triggers.push_back(trigger);
			CoopRuntime::Instance().Log(
				"[trigger-p1-teleport] queued seq=%u pos=(%.2f,%.2f,%.2f) dist=%.2f\r\n",
				packet.sequence, packet.position[0], packet.position[1], packet.position[2],
				sqrtf(distance_sq));
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log("[trigger-p1-teleport] source trigger read fault\r\n");
		}
	}

	void WorldSync::RecordNativeSpawn(void* trigger, void* entity,

		std::uint32_t family, std::uint32_t subtype, std::int32_t definition_id)
	{
		if (!trigger || !entity || !IsSupportedFamily(family))
			return;
		if (definition_id >= 0)
			RecordTriggerTemplate(trigger, family, subtype);

		if (CoopNetGame::Instance().IsHost())
		{
			HostEntity* existing = FindHostEntity(entity);
			if (existing && existing->trigger == trigger)
				return;
			if (existing)
			{
				existing->entity = entity;
				existing->trigger = trigger;
				existing->announced = false;
				existing->have_transform = false;
				existing->world_id = m_next_world_id++;
				existing->key.family = family;
				existing->key.subtype = subtype;
				existing->key.definition_id = definition_id;
				existing->key.occurrence = NextOccurrence(m_host_trigger_counters,
					trigger);
				ReadEntityTransform(entity, existing->last_position,
					existing->last_rotation);
				existing->have_transform = true;
				QueueHostSpawn(*existing);
				return;
			}

			HostEntity added = {};
			added.entity = entity;
			added.trigger = trigger;
			added.key.family = family;
			added.key.subtype = subtype;
			added.key.definition_id = definition_id;
			added.key.occurrence = NextOccurrence(m_host_trigger_counters, trigger);
			added.world_id = m_next_world_id++;
			added.have_transform = ReadEntityTransform(entity, added.last_position,
				added.last_rotation);
			m_host_entities.push_back(added);
			if (m_host_entities.back().have_transform)
				QueueHostSpawn(m_host_entities.back());
			return;
		}

		if (!CoopNetGame::Instance().IsClient())
			return;

		TriggerKey key = {};
		key.family = family;
		key.subtype = subtype;
		key.definition_id = definition_id;
		std::uint32_t world_id = 0;
		if (m_forced_client_spawn_active && SameTriggerTemplate(
			m_forced_client_spawn.key, family, subtype, definition_id))
		{
			key = m_forced_client_spawn.key;
			world_id = m_forced_client_spawn.world_id;
			m_forced_client_spawn_active = false;
		}
		else
		{
			key.occurrence = NextOccurrence(m_client_trigger_counters, trigger);
		}
		AddClientEntity(entity, key, world_id);
	}

	void WorldSync::AddClientEntity(void* entity, const TriggerKey& key,
		std::uint32_t world_id)
	{
		for (ClientEntity& existing : m_client_entities)
		{
			if (existing.entity != entity)
				continue;
			existing.key = key;
			if (world_id)
				existing.world_id = world_id;
			return;
		}
		ClientEntity added = {};
		added.entity = entity;
		added.key = key;
		added.world_id = world_id;
		m_client_entities.push_back(added);
		if (world_id)
		{
			CoopRuntime::Instance().Log(
				"[world-link] client id=%u def=%d occ=%u entity=%p\r\n",
				world_id, key.definition_id, key.occurrence, entity);
		}
		else
		{
			CoopRuntime::Instance().Log(
				"[world-id] client candidate def=%d occ=%u entity=%p\r\n",
				key.definition_id, key.occurrence, entity);
		}
	}

	void WorldSync::EnumerateHostEntities()
	{
		if (!CoopNetGame::Instance().HasRemotePeer())
			return;

		if (InterlockedExchange(&m_host_resync_requested, 0) != 0)
		{
			for (HostEntity& entity : m_host_entities)
				entity.announced = false;
		}

		const DWORD now = GetTickCount();
		const bool send_snapshots = m_last_snapshot_tick == 0 ||
			static_cast<DWORD>(now - m_last_snapshot_tick) >=
			kWorldSnapshotIntervalMs;
		if (send_snapshots)
			m_last_snapshot_tick = now;

		try
		{
			BYTE* const registry = *reinterpret_cast<BYTE**>(gforce::kEntityRegistry);
			const size_t list_offsets[] = {
				gforce::kEntityRegistryMonsterListOffset,
				gforce::kEntityRegistryNpcListOffset
			};
			for (size_t list_index = 0; registry &&
				list_index != _countof(list_offsets); ++list_index)
			{
				BYTE* node = *reinterpret_cast<BYTE**>(registry + list_offsets[list_index]);
				for (size_t visited = 0; node && visited != kWorldRegistryWalkLimit;
					++visited)
				{
					void* const entity = *reinterpret_cast<void**>(node +
						gforce::kIntrusiveListValueOffset);
					node = *reinterpret_cast<BYTE**>(node +
						gforce::kIntrusiveListNextOffset);
					if (!entity)
						continue;
					BYTE* const trigger = *reinterpret_cast<BYTE**>(
						static_cast<BYTE*>(entity) + gforce::kEntityTriggerOffset);
					if (!trigger)
						continue;
					const std::uint32_t family = *reinterpret_cast<std::uint32_t*>(
						trigger + gforce::kTriggerFamilyOffset);
					if (!IsSupportedFamily(family))
						continue;
					const std::uint32_t subtype = *reinterpret_cast<std::uint32_t*>(
						trigger + gforce::kTriggerSubtypeOffset);
					const std::int32_t definition_id = *reinterpret_cast<std::int32_t*>(
						trigger + gforce::kTriggerSpawnIdOffset);
					HostEntity* tracked = FindHostEntity(entity);
					if (!tracked || tracked->trigger != trigger)
					{
						RecordNativeSpawn(trigger, entity, family, subtype, definition_id);
						tracked = FindHostEntity(entity);
					}
					if (!tracked)
						continue;

					float position[4] = {};
					float rotation[4] = {};
					if (!ReadEntityTransform(entity, position, rotation))
						continue;
					if (!tracked->announced)
					{
						memcpy(tracked->last_position, position,
							sizeof(tracked->last_position));
						memcpy(tracked->last_rotation, rotation,
							sizeof(tracked->last_rotation));
						tracked->have_transform = true;
						QueueHostSpawn(*tracked);
					}
					else if (send_snapshots && (!tracked->have_transform ||
						TransformChanged(tracked->last_position, tracked->last_rotation,
							position, rotation)))
					{
						QueueHostSnapshot(*tracked, position, rotation);
					}
				}
			}
		}
		catch (...)
		{
			CoopRuntime::Instance().Log(
				"[world-sync] host registry read fault; frame skipped\r\n");
		}

		// A missing host-registry entry is not a native removal instruction for the
		// other process.  The legacy world-despawn packet only removed bookkeeping
		// on the client and could arrive before the final HP=0 update.  Let each
		// game's own death path remove its local object after synchronized HP.
		for (auto it = m_host_entities.begin(); it != m_host_entities.end();)
		{
			if (!IsLiveEntity(it->entity))
			{
				it = m_host_entities.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	void WorldSync::EnumerateClientEntities()
	{
		// The trigger's native spawn callback runs before its new entity has joined
		// this registry.  Looking again on a later game tick is therefore the first
		// point at which the local pointer can safely be connected to a host world id.
		__try
		{
			BYTE* const registry = *reinterpret_cast<BYTE**>(gforce::kEntityRegistry);
			const size_t list_offsets[] = {
				gforce::kEntityRegistryMonsterListOffset,
				gforce::kEntityRegistryNpcListOffset
			};
			for (size_t list_index = 0; registry &&
				list_index != _countof(list_offsets); ++list_index)
			{
				BYTE* node = *reinterpret_cast<BYTE**>(registry + list_offsets[list_index]);
				for (size_t visited = 0; node && visited != kWorldRegistryWalkLimit;
					++visited)
				{
					void* const entity = *reinterpret_cast<void**>(node +
						gforce::kIntrusiveListValueOffset);
					node = *reinterpret_cast<BYTE**>(node +
						gforce::kIntrusiveListNextOffset);
					if (!entity || FindClientEntity(entity))
						continue;

					BYTE* const trigger = *reinterpret_cast<BYTE**>(
						static_cast<BYTE*>(entity) + gforce::kEntityTriggerOffset);
					if (!trigger)
						continue;
					const std::uint32_t family = *reinterpret_cast<std::uint32_t*>(
						trigger + gforce::kTriggerFamilyOffset);
					if (!IsSupportedFamily(family))
						continue;
					const std::uint32_t subtype = *reinterpret_cast<std::uint32_t*>(
						trigger + gforce::kTriggerSubtypeOffset);
					const std::int32_t definition_id = *reinterpret_cast<std::int32_t*>(
						trigger + gforce::kTriggerSpawnIdOffset);
					RecordTriggerTemplate(trigger, family, subtype);
					TriggerKey key = {};
					key.family = family;
					key.subtype = subtype;
					key.definition_id = definition_id;
					key.occurrence = NextOccurrence(m_client_trigger_counters, trigger);
					AddClientEntity(entity, key, 0);
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log(
				"[world-sync] client registry read fault; frame skipped\r\n");
		}
	}

	bool WorldSync::TrySpawnClientEntity(PendingSpawn& pending)
	{
		// Dynamic spawns are produced by the local game itself (the computer
		// spits spiders out on its own).  They only need proximity linking,
		// never a native trigger replay.
		if (pending.packet.key.definition_id < 0)
			return false;

		TriggerTemplate* const template_trigger =
			FindTriggerTemplate(pending.packet.key);
		if (!template_trigger)
		{
			if (!pending.logged_missing_template)
			{
				pending.logged_missing_template = true;
				CoopRuntime::Instance().Log(
					"[world-trigger] client missing id=%u def=%d: no matching template\r\n",
					pending.packet.world_id, pending.packet.key.definition_id);
			}
			return false;
		}

		m_forced_client_spawn = pending.packet;
		m_forced_client_spawn_active = true;
		CoopRuntime::Instance().Log(
			"[world-trigger] client invoking native trigger for host id=%u def=%d\r\n",
			pending.packet.world_id, pending.packet.key.definition_id);
		if (!CoopNetGame::Instance().SpawnWorldFromTrigger(template_trigger->trigger))
		{
			m_forced_client_spawn_active = false;
			CoopRuntime::Instance().Log(
				"[world-trigger] native trigger rejected host id=%u\r\n",
				pending.packet.world_id);
			return false;
		}
		if (m_forced_client_spawn_active)
		{
			m_forced_client_spawn_active = false;
			CoopRuntime::Instance().Log(
				"[world-trigger] host id=%u did not yield a live local entity\r\n",
				pending.packet.world_id);
		}
		return FindClientEntityById(pending.packet.world_id) != NULL;
	}

	void WorldSync::ResolvePendingSpawns()
	{
		const DWORD now = GetTickCount();
		for (std::vector<PendingSpawn>::iterator it = m_pending_spawns.begin();
			it != m_pending_spawns.end();)
		{
			ClientEntity* const linked = FindClientEntityById(it->packet.world_id);
			if (linked)
			{
				if (!linked->has_latest_snapshot)
				{
					WorldSnapshotPacket initial = {};
					initial.world_id = it->packet.world_id;
					memcpy(initial.position, it->packet.position,
						sizeof(initial.position));
					memcpy(initial.rotation, it->packet.rotation,
						sizeof(initial.rotation));
					AcceptSnapshot(*linked, initial, now);
				}
				it = m_pending_spawns.erase(it);
				continue;
			}
			ClientEntity* const candidate = FindUnlinkedClientEntity(it->packet.key);
			if (candidate)
			{
				candidate->world_id = it->packet.world_id;
				WorldSnapshotPacket initial = {};
				initial.world_id = it->packet.world_id;
				memcpy(initial.position, it->packet.position, sizeof(initial.position));
				memcpy(initial.rotation, it->packet.rotation, sizeof(initial.rotation));
				AcceptSnapshot(*candidate, initial, now);
				CoopRuntime::Instance().Log(
					"[world-link] host id=%u -> local=%p def=%d occ=%u\r\n",
					candidate->world_id, candidate->entity, candidate->key.definition_id,
					candidate->key.occurrence);
				it = m_pending_spawns.erase(it);
				continue;
			}
			// Dynamic spawns (definition_id == -1) cannot be matched by key: both
			// machines build independent trigger objects.  Match the closest local
			// entity of the same subtype near the host-reported spawn position.
			if (it->packet.key.definition_id < 0)
			{
				ClientEntity* best = NULL;
				float best_distance_sq = 4.0f * 4.0f;
				float existing_position[4] = {};
				float existing_rotation[4] = {};
				for (ClientEntity& existing : m_client_entities)
				{
					if (existing.world_id != 0 ||
						existing.key.subtype != it->packet.key.subtype)
					{
						continue;
					}
					if (!ReadEntityTransform(existing.entity, existing_position,
						existing_rotation))
					{
						continue;
					}
					const float dx = existing_position[0] - it->packet.position[0];
					const float dy = existing_position[1] - it->packet.position[1];
					const float dz = existing_position[2] - it->packet.position[2];
					const float distance_sq = dx * dx + dy * dy + dz * dz;
					if (distance_sq < best_distance_sq)
					{
						best_distance_sq = distance_sq;
						best = &existing;
					}
				}
				if (best)
				{
					best->world_id = it->packet.world_id;
					WorldSnapshotPacket initial = {};
					initial.world_id = it->packet.world_id;
					memcpy(initial.position, it->packet.position, sizeof(initial.position));
					memcpy(initial.rotation, it->packet.rotation, sizeof(initial.rotation));
					AcceptSnapshot(*best, initial, now);
					CoopRuntime::Instance().Log(
						"[world-link] dynamic host id=%u -> local=%p subtype=%08X\r\n",
						best->world_id, best->entity, best->key.subtype);
					it = m_pending_spawns.erase(it);
					continue;
				}
			}
			if (it->last_attempt_tick == 0 ||
				static_cast<DWORD>(now - it->last_attempt_tick) >=
				kMissingSpawnRetryMs)
			{
				it->last_attempt_tick = now;
				if (TrySpawnClientEntity(*it))
				{
					ClientEntity* const spawned =
						FindClientEntityById(it->packet.world_id);
					if (spawned)
					{
						WorldSnapshotPacket initial = {};
						initial.world_id = it->packet.world_id;
						memcpy(initial.position, it->packet.position,
							sizeof(initial.position));
						memcpy(initial.rotation, it->packet.rotation,
							sizeof(initial.rotation));
						AcceptSnapshot(*spawned, initial, now);
						it = m_pending_spawns.erase(it);
						continue;
					}
				}
			}
			++it;
		}
	}

	void WorldSync::ApplyPendingSnapshots()
	{
		for (std::vector<WorldSnapshotPacket>::iterator it =
			m_pending_snapshots.begin(); it != m_pending_snapshots.end();)
		{
			ClientEntity* const entity = FindClientEntityById(it->world_id);
			if (!entity)
			{
				++it;
				continue;
			}
			if (!IsLiveEntity(entity->entity))
			{
				CoopRuntime::Instance().Log(
					"[world-snapshot] local id=%u is no longer live; update ignored\r\n",
					it->world_id);
				it = m_pending_snapshots.erase(it);
				continue;
			}
			AcceptSnapshot(*entity, *it, GetTickCount());
			it = m_pending_snapshots.erase(it);
		}
	}

	void WorldSync::AcceptSnapshot(ClientEntity& entity,
		const WorldSnapshotPacket& snapshot, DWORD received_tick)
	{
		if (entity.has_latest_snapshot && snapshot.sequence != 0 &&
			entity.latest_snapshot.sequence != 0 &&
			static_cast<std::int32_t>(snapshot.sequence -
				entity.latest_snapshot.sequence) <= 0)
		{
			return;
		}

		if (entity.has_presentation)
			AdvancePresentation(entity, received_tick);

		const DWORD received_interval = entity.has_latest_snapshot ?
			static_cast<DWORD>(received_tick - entity.latest_received_tick) : 0;
		entity.latest_snapshot = snapshot;
		entity.latest_received_tick = received_tick;
		entity.has_latest_snapshot = true;
		if (!entity.has_presentation)
		{
			memcpy(entity.presentation_position, snapshot.position,
				sizeof(entity.presentation_position));
			memcpy(entity.presentation_rotation, snapshot.rotation,
				sizeof(entity.presentation_rotation));
			entity.has_presentation = true;
		}
		memcpy(entity.blend_start_position, entity.presentation_position,
			sizeof(entity.blend_start_position));
		memcpy(entity.blend_start_rotation, entity.presentation_rotation,
			sizeof(entity.blend_start_rotation));
		entity.blend_started_tick = received_tick;
		entity.blend_duration_ms = received_interval;
		if (entity.blend_duration_ms < 80)
			entity.blend_duration_ms = 80;
		if (entity.blend_duration_ms > 450)
			entity.blend_duration_ms = 450;
		if (!entity.logged_snapshot)
		{
			entity.logged_snapshot = true;
			CoopRuntime::Instance().Log(
				"[world-snapshot] host sample buffered for id=%u local=%p\r\n",
				entity.world_id, entity.entity);
		}
	}

	void WorldSync::AdvancePresentation(ClientEntity& entity, DWORD now)
	{
		if (!entity.has_latest_snapshot || !entity.has_presentation)
			return;
		const DWORD duration = entity.blend_duration_ms ?
			entity.blend_duration_ms : 1;
		float progress = static_cast<float>(now - entity.blend_started_tick) /
			static_cast<float>(duration);
		if (progress > 1.0f)
			progress = 1.0f;
		for (size_t index = 0; index != 4; ++index)
		{
			entity.presentation_position[index] = entity.blend_start_position[index] +
				(entity.latest_snapshot.position[index] -
					entity.blend_start_position[index]) * progress;
			entity.presentation_rotation[index] = entity.blend_start_rotation[index] +
				(entity.latest_snapshot.rotation[index] -
					entity.blend_start_rotation[index]) * progress;
		}
	}

	void WorldSync::ApplyPresentation(ClientEntity& entity)
	{
		__try
		{
			BYTE* const bytes = static_cast<BYTE*>(entity.entity);
			memcpy(bytes + gforce::kEntityPositionOffset, entity.presentation_position,
				sizeof(entity.presentation_position));
			memcpy(bytes + gforce::kEntityRotationOffset, entity.presentation_rotation,
				sizeof(entity.presentation_rotation));
			if (!entity.logged_render_apply)
			{
				entity.logged_render_apply = true;
				CoopRuntime::Instance().Log(
					"[world-authority] render-frame host transform active for id=%u local=%p\r\n",
					entity.world_id, entity.entity);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log(
				"[world-snapshot] transform write fault for id=%u\r\n",
				entity.world_id);
		}
	}

	void WorldSync::ProcessClientPackets()
	{
		std::vector<WorldSpawnPacket> spawns;
		std::vector<WorldSnapshotPacket> snapshots;
		std::vector<WorldTriggerEventPacket> trigger_events;
		AcquireSRWLockExclusive(&m_packet_lock);
		spawns.swap(m_incoming_spawns);
		snapshots.swap(m_incoming_snapshots);
		trigger_events.swap(m_incoming_trigger_events);
		ReleaseSRWLockExclusive(&m_packet_lock);

		for (const WorldSpawnPacket& packet : spawns)
		{
			if (!FindClientEntityById(packet.world_id))
			{
				bool already_pending = false;
				for (const PendingSpawn& pending : m_pending_spawns)
				{
					if (pending.packet.world_id == packet.world_id)
					{
						already_pending = true;
						break;
					}
				}
				if (!already_pending && m_pending_spawns.size() < kMaxPendingWorldPackets)
				{
					PendingSpawn pending = {};
					pending.packet = packet;
					m_pending_spawns.push_back(pending);
				}
			}
		}
		for (const WorldSnapshotPacket& packet : snapshots)
		{
			if (m_pending_snapshots.size() < kMaxPendingWorldPackets)
				m_pending_snapshots.push_back(packet);
		}
		for (const WorldTriggerEventPacket& packet : trigger_events)
		{
			if (!CoopNetGame::Instance().ReplayTriggerEvent(packet.key.family,
				packet.key.subtype, packet.key.definition_id,
				packet.key.occurrence,
				packet.event_code))
			{
				CoopRuntime::Instance().Log(
					"[world-trigger-event] client could not replay def=%d occ=%u event=%d\r\n",
					packet.key.definition_id, packet.key.occurrence,
					packet.event_code);
			}

		}

		std::vector<WorldDespawnPacket> despawns;
		AcquireSRWLockExclusive(&m_packet_lock);
		despawns.swap(m_incoming_despawns);
		ReleaseSRWLockExclusive(&m_packet_lock);
		for (const WorldDespawnPacket& packet : despawns)
			HandleIncomingDespawn(packet);

		ResolvePendingSpawns();
		ApplyPendingSnapshots();
	}

	bool WorldSync::ReadP1Transform(float position[4], float rotation[4]) const
	{
		if (!position || !rotation)
			return false;
		__try
		{
			void* const p1 = reinterpret_cast<void**>(gforce::kGPigEntityArray)[1];
			if (!p1)
				return false;
			const BYTE* const bytes = static_cast<const BYTE*>(p1);
			memcpy(position, bytes + gforce::kEntityPositionOffset, sizeof(float) * 4);
			memcpy(rotation, bytes + gforce::kEntityRotationOffset, sizeof(float) * 4);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool WorldSync::WriteP1Transform(const float position[4],
		const float rotation[4]) const
	{
		if (!position || !rotation)
			return false;
		__try
		{
			void* const p1 = reinterpret_cast<void**>(gforce::kGPigEntityArray)[1];
			if (!p1)
				return false;
			BYTE* const bytes = static_cast<BYTE*>(p1);
			memcpy(bytes + gforce::kEntityPositionOffset, position, sizeof(float) * 4);
			memcpy(bytes + gforce::kEntityRotationOffset, rotation, sizeof(float) * 4);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void WorldSync::ApplyPendingP1Teleports()
	{
		std::vector<TriggerP1TeleportPacket> arrived;
		AcquireSRWLockExclusive(&m_packet_lock);
		arrived.swap(m_incoming_p1_teleports);
		ReleaseSRWLockExclusive(&m_packet_lock);

		for (const TriggerP1TeleportPacket& packet : arrived)
		{
			if (packet.sequence == 0 ||
				(m_last_trigger_p1_teleport_sequence != 0 &&
					static_cast<std::int32_t>(packet.sequence -
						m_last_trigger_p1_teleport_sequence) <= 0))
			{
				continue;
			}
			m_pending_p1_teleports.push_back(packet);
		}

		const DWORD now = GetTickCount();
		if (m_remote_p1_teleport_active &&
			static_cast<LONG>(now - m_remote_p1_teleport_restore_tick) < 0)
		{
			return;
		}

		if (!m_pending_p1_teleports.empty())
		{
			const TriggerP1TeleportPacket packet = m_pending_p1_teleports.front();
			m_pending_p1_teleports.erase(m_pending_p1_teleports.begin());
			if (!m_remote_p1_teleport_active &&
				!ReadP1Transform(m_remote_p1_saved_position, m_remote_p1_saved_rotation))
			{
				return;
			}
			if (WriteP1Transform(packet.position, m_remote_p1_saved_rotation))
			{
				m_remote_p1_teleport_active = true;
				m_last_trigger_p1_teleport_sequence = packet.sequence;
				memcpy(m_remote_p1_target_position, packet.position,
					sizeof(m_remote_p1_target_position));
				m_remote_p1_teleport_restore_tick = now + 250;
				CoopRuntime::Instance().Log(
					"[trigger-p1-teleport] applied seq=%u pending=%u pos=(%.2f,%.2f,%.2f)\r\n",
					packet.sequence, static_cast<unsigned>(m_pending_p1_teleports.size()),
					packet.position[0], packet.position[1], packet.position[2]);
			}
			return;
		}

		if (m_remote_p1_teleport_active)
		{
			if (WriteP1Transform(m_remote_p1_saved_position, m_remote_p1_saved_rotation))
			{
				CoopRuntime::Instance().Log("[trigger-p1-teleport] restored local P1\r\n");
			}
			m_remote_p1_teleport_active = false;
			m_remote_p1_teleport_restore_tick = 0;
		}
	}

	void WorldSync::GameTick()
	{
		if (!CoopNetGame::Instance().HasRemotePeer())
			return;
		if (CoopNetGame::Instance().IsHost())
			EnumerateHostEntities();
		if (CoopNetGame::Instance().IsClient())
			EnumerateClientEntities();
		// Inbound trigger-position teleports are valid in both directions, unlike
		// the earlier host-only spawn and trigger-event paths.
		ProcessClientPackets();
		ApplyPendingP1Teleports();
		// Capture the post-hit local HP before applying incoming peer state.  The
		// receive path refreshes this baseline, so remote HP never bounces back.
		DetectLocalHealthChanges();
		ApplyIncomingDamage();
	}

	void WorldSync::SetTrackedHealth(void* entity, float health)
	{
		if (!entity)
			return;
		if (CoopNetGame::Instance().IsHost())
		{
			for (HostEntity& tracked : m_host_entities)
			{
				if (tracked.entity == entity)
				{
					tracked.last_health = health;
					tracked.have_health = true;
					return;
				}
			}
		}
		else if (CoopNetGame::Instance().IsClient())
		{
			for (ClientEntity& tracked : m_client_entities)
			{
				if (tracked.entity == entity)
				{
					tracked.last_health = health;
					tracked.have_health = true;
					return;
				}
			}
		}
	}

	void WorldSync::DetectLocalHealthChanges()
	{
		if (CoopNetGame::Instance().IsHost())
		{
			for (HostEntity& tracked : m_host_entities)
			{
				float health = 0.0f;
				if (!tracked.entity || !tracked.world_id || !IsLiveEntity(tracked.entity) ||
					!ReadEntityHealth(tracked.entity, health))
				{
					continue;
				}
				if (!tracked.have_health)
				{
					tracked.last_health = health;
					tracked.have_health = true;
					continue;
				}
				if (fabsf(tracked.last_health - health) > 0.01f)
				{
					tracked.last_health = health;
					ReportLocalDamage(tracked.entity, 1);
				}
			}
		}
		else if (CoopNetGame::Instance().IsClient())
		{
			for (ClientEntity& tracked : m_client_entities)
			{
				float health = 0.0f;
				if (!tracked.entity || !tracked.world_id || !IsLiveEntity(tracked.entity) ||
					!ReadEntityHealth(tracked.entity, health))
				{
					continue;
				}
				if (!tracked.have_health)
				{
					tracked.last_health = health;
					tracked.have_health = true;
					continue;
				}
				if (fabsf(tracked.last_health - health) > 0.01f)
				{
					tracked.last_health = health;
					ReportLocalDamage(tracked.entity, 1);
				}
			}
		}
	}

	bool WorldSync::ApplyHealthToEntity(void* entity, std::uint32_t hp_bits) const
	{
		if (!entity)
			return false;
		float health = 0.0f;
		memcpy(&health, &hp_bits, sizeof(health));
		if (health != health || health < 0.0f || health > 100000.0f)
			return false;
		__try
		{
			BYTE* const entity_bytes = static_cast<BYTE*>(entity);
			BYTE* const handler = *reinterpret_cast<BYTE**>(entity_bytes +
				gforce::kEntityHandlerOffset);
			if (!handler)
				return false;
			*reinterpret_cast<float*>(handler + gforce::kHandlerHealthOffset) = health;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void WorldSync::ApplyIncomingDamage()
	{
		std::vector<WorldDamagePacket> damage;
		AcquireSRWLockExclusive(&m_damage_lock);
		damage.swap(m_incoming_damage);
		ReleaseSRWLockExclusive(&m_damage_lock);
		if (damage.empty() && m_pending_damage.empty())
			return;

		for (const WorldDamagePacket& packet : damage)
		{
			void* entity = NULL;
			if (CoopNetGame::Instance().IsHost())
			{
				for (HostEntity& tracked : m_host_entities)
				{
					if (tracked.world_id == packet.world_id)
					{
						entity = tracked.entity;
						break;
					}
				}
			}
			else if (CoopNetGame::Instance().IsClient())
			{
				ClientEntity* const tracked = FindClientEntityById(packet.world_id);
				if (tracked)
					entity = tracked->entity;
			}
			if (!entity)
			{
				PendingDamage pending = {};
					pending.world_id = packet.world_id;
					pending.hp_bits = packet.hp_bits;
					pending.event_code = packet.event_code;
				m_pending_damage.push_back(pending);
				continue;
			}
			// A dead/despawned entity may still sit in the tracked list with a
			// dangling pointer; never dereference it for a remote hit.
			if (!IsLiveEntity(entity))
				continue;

					const bool applied = ApplyHealthToEntity(entity, packet.hp_bits);
					if (applied)
					{
						float received_health = 0.0f;
						memcpy(&received_health, &packet.hp_bits, sizeof(received_health));
						SetTrackedHealth(entity, received_health);
					}
					if (packet.event_code == 0)
					{
						CoopRuntime::Instance().Log(
							"[combat] F9 sync: %s\r\n",
							applied ? "applied" : "target unavailable");
					}

		}
		// Hits that arrived before the local twin was linked are replayed as soon
		// as the link exists, in order, so a kill cannot be lost to streaming.
		for (std::vector<PendingDamage>::iterator it = m_pending_damage.begin();
			it != m_pending_damage.end();)
		{
			void* entity = NULL;
			if (CoopNetGame::Instance().IsHost())
			{
				for (HostEntity& tracked : m_host_entities)
				{
					if (tracked.world_id == it->world_id)
					{
						entity = tracked.entity;
						break;
					}
				}
			}
			else if (CoopNetGame::Instance().IsClient())
			{
				ClientEntity* const tracked = FindClientEntityById(it->world_id);
				if (tracked)
					entity = tracked->entity;
			}
			if (!entity)
			{
				++it;
				continue;
			}
			if (!IsLiveEntity(entity))
			{
				it = m_pending_damage.erase(it);
				continue;
			}
				const bool applied = ApplyHealthToEntity(entity, it->hp_bits);
				if (applied)
				{
					float received_health = 0.0f;
					memcpy(&received_health, &it->hp_bits, sizeof(received_health));
					SetTrackedHealth(entity, received_health);
				}

			it = m_pending_damage.erase(it);
		}
	}

	void WorldSync::ApplyOneHit(void* trigger, std::uint32_t amount,
		std::uint32_t world_id, int event_code)
	{
		__try
		{
			CoopNetGame::Instance().ApplyRemoteDamage(trigger, amount,
				world_id, event_code);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log(
				"[world-damage] hit fault id=%u; entity skipped\r\n", world_id);
		}
	}

	void WorldSync::DebugKillNearest()
	{
		constexpr float kDebugKillRange = 6.0f;
		constexpr float kDebugKillRangeSq = kDebugKillRange * kDebugKillRange;

		static DWORD last_kill_tick = 0;
		const DWORD now = GetTickCount();
		if (last_kill_tick != 0 && now - last_kill_tick < 500)
			return;
		last_kill_tick = now;

		// The native NPC/monster registry also contains player-controlled objects
		// in some modes.  P1 is necessarily at zero distance from itself; Darwin
		// may be co-located as well.  Neither is a valid F9 target.
		void* const p1 = reinterpret_cast<void**>(gforce::kGPigEntityArray)[1];
		void* const p2 = reinterpret_cast<void**>(gforce::kGPigEntityArray)[2];
		void* const p3 = reinterpret_cast<void**>(gforce::kGPigEntityArray)[3];
		void* const darwin = *reinterpret_cast<void**>(gforce::kFlyEntity);
		if (!p1)
		{
			CoopRuntime::Instance().Log("[debug-kill] P1 not found\r\n");
			return;
		}
		float p1_pos[4] = {};
		memcpy(p1_pos, static_cast<const BYTE*>(p1) +
			gforce::kEntityPositionOffset, sizeof(p1_pos));

		// Stage one of the new F9 route is deliberately read-only.  Static buttons,
		// doors and switches can use different dispatchers, so identify the closest
		// map trigger first instead of guessing an event code and opening something
		// unrelated.  The next phase will invoke only the confirmed native route.
		constexpr float kInteractorProbeRange = 6.0f;
		constexpr float kInteractorProbeRangeSq =
			kInteractorProbeRange * kInteractorProbeRange;
		constexpr size_t kInteractorProbeCount = 5;
		TriggerTemplate* nearest_interactors[kInteractorProbeCount] = {};
		float nearest_interactor_dist_sq[kInteractorProbeCount] = {
			kInteractorProbeRangeSq, kInteractorProbeRangeSq, kInteractorProbeRangeSq,
			kInteractorProbeRangeSq, kInteractorProbeRangeSq
		};
		__try
		{
			for (TriggerTemplate& candidate : m_trigger_templates)
			{
				if (!candidate.trigger)
					continue;
				const float* const position = reinterpret_cast<const float*>(
					static_cast<const BYTE*>(candidate.trigger) +
					gforce::kTriggerPositionOffset);
				const float dx = position[0] - p1_pos[0];
				const float dy = position[1] - p1_pos[1];
				const float dz = position[2] - p1_pos[2];
				const float dist_sq = dx * dx + dy * dy + dz * dz;
				for (size_t slot = 0; slot < kInteractorProbeCount; ++slot)
				{
					if (dist_sq > nearest_interactor_dist_sq[slot])
						continue;
					for (size_t move = kInteractorProbeCount - 1; move > slot; --move)
					{
						nearest_interactors[move] = nearest_interactors[move - 1];
						nearest_interactor_dist_sq[move] =
							nearest_interactor_dist_sq[move - 1];
					}
					nearest_interactors[slot] = &candidate;
					nearest_interactor_dist_sq[slot] = dist_sq;
					break;
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log("[interactor-probe] F9 trigger scan fault\r\n");
			return;
		}
		if (nearest_interactors[0])
		{
			for (size_t slot = 0; slot < kInteractorProbeCount &&
				nearest_interactors[slot]; ++slot)
			{
				const TriggerTemplate* const candidate = nearest_interactors[slot];
				CoopRuntime::Instance().Log(
					"[interactor-probe] F9 rank=%u family=%08X subtype=%08X def=%d dist=%.2f trigger=%p\r\n",
					static_cast<unsigned>(slot + 1), candidate->family, candidate->subtype,
					candidate->definition_id, sqrtf(nearest_interactor_dist_sq[slot]),
					candidate->trigger);
			}
			ObserveLocalP1Trigger(nearest_interactors[0]->trigger, false);
			return;
		}

		// The computing-centre source box is a map trigger, not a live combat
		// entity.  Dispatching its verified event lets the stock chain destroy the
		// box, create the computer and later create its dynamic spiders.
		constexpr float kComputerBoxRange = 10.0f;
		constexpr float kComputerBoxRangeSq = kComputerBoxRange * kComputerBoxRange;
		TriggerTemplate* nearest_box = NULL;
		float nearest_box_dist_sq = kComputerBoxRangeSq;
		__try
		{
			for (TriggerTemplate& candidate : m_trigger_templates)
			{
				if (!candidate.trigger ||
					candidate.family != gforce::kMonsterTriggerFamily ||
					candidate.subtype != gforce::kComputerBoxTriggerSubtype ||
					candidate.definition_id != gforce::kComputerBoxTriggerDefinition)
				{
					continue;
				}
				const float* const position = reinterpret_cast<const float*>(
					static_cast<const BYTE*>(candidate.trigger) +
					gforce::kTriggerPositionOffset);
				const float dx = position[0] - p1_pos[0];
				const float dy = position[1] - p1_pos[1];
				const float dz = position[2] - p1_pos[2];
				const float dist_sq = dx * dx + dy * dy + dz * dz;
				if (dist_sq <= nearest_box_dist_sq)
				{
					nearest_box = &candidate;
					nearest_box_dist_sq = dist_sq;
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log("[world-trigger] F9: spawner scan fault\r\n");
			return;
		}
		if (nearest_box)
		{
			const bool dispatched = CoopNetGame::Instance().ReplayTriggerEvent(
				nearest_box->family, nearest_box->subtype,
				nearest_box->definition_id, 0,
				gforce::kComputerBoxActivateEvent);
			CoopRuntime::Instance().Log(
				"[world-trigger] F9: source box %.1f units away, event=0x%08X %s\r\n",
				sqrtf(nearest_box_dist_sq),
				static_cast<unsigned>(gforce::kComputerBoxActivateEvent),
				dispatched ? "dispatched" : "unavailable");
			return;
		}

		void* best_entity = NULL;
		const BYTE* best_trigger = NULL;
		float best_dist_sq = kDebugKillRangeSq;
		__try
		{
			BYTE* const registry = *reinterpret_cast<BYTE**>(gforce::kEntityRegistry);
			const size_t list_offsets[] = {
				gforce::kEntityRegistryMonsterListOffset,
				gforce::kEntityRegistryNpcListOffset
			};
			for (size_t list_index = 0; registry &&
				list_index != _countof(list_offsets); ++list_index)
			{
				BYTE* node = *reinterpret_cast<BYTE**>(
					registry + list_offsets[list_index]);
				for (size_t visited = 0; node && visited != kWorldRegistryWalkLimit;
					++visited)
				{
					void* const entity = *reinterpret_cast<void**>(
						node + gforce::kIntrusiveListValueOffset);
					node = *reinterpret_cast<BYTE**>(
						node + gforce::kIntrusiveListNextOffset);
					if (!entity || entity == p1 || entity == p2 || entity == p3 ||
						entity == darwin || !IsLiveEntity(entity))
					{
						continue;
					}

					const BYTE* const bytes = static_cast<const BYTE*>(entity);
					const BYTE* const trigger = *reinterpret_cast<const BYTE* const*>(
						bytes + gforce::kEntityTriggerOffset);
					// Do not rely only on the registry list: it can contain helper/player
					// objects.  A genuine target must own a supported enemy trigger.
					if (!trigger || !IsSupportedFamily(*reinterpret_cast<const std::uint32_t*>(
						trigger + gforce::kTriggerFamilyOffset)))
					{
						continue;
					}

					float pos[4] = {};
					memcpy(pos, bytes + gforce::kEntityPositionOffset, sizeof(pos));
					const float dx = pos[0] - p1_pos[0];
					const float dy = pos[1] - p1_pos[1];
					const float dz = pos[2] - p1_pos[2];
					const float dist_sq = dx * dx + dy * dy + dz * dz;
					if (dist_sq <= best_dist_sq)
					{
						best_dist_sq = dist_sq;
						best_entity = entity;
						best_trigger = trigger;
					}
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log("[debug-kill] scan faulted\r\n");
			return;
		}

		if (!best_entity || !best_trigger)
		{
			CoopRuntime::Instance().Log(
				"[combat] F9: no enemy within %.1f units\r\n", kDebugKillRange);
			return;
		}

		BYTE* const handler = *reinterpret_cast<BYTE**>(
			static_cast<BYTE*>(best_entity) + gforce::kEntityHandlerOffset);
		if (!handler)
		{
			CoopRuntime::Instance().Log("[combat] F9: target has no combat state\r\n");
			return;
		}
		__try
		{
			const float hp_before = *reinterpret_cast<const float*>(handler +
				gforce::kHandlerHealthOffset);
			*reinterpret_cast<float*>(handler + gforce::kHandlerHealthOffset) = 0.0f;
			SetTrackedHealth(best_entity, 0.0f);
			const bool queued_for_peer = ReportLocalDamage(best_entity, 0);
			CoopRuntime::Instance().Log(
				"[combat] F9: enemy %.1f units away, HP %.0f -> 0, killed; sync=%s\r\n",
				sqrtf(best_dist_sq), hp_before,
				queued_for_peer ? "queued" : "not-linked");
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log("[combat] F9: could not damage target\r\n");
		}
	}

	// Client-side: processes a despawn packet by removing the entity from tracking.
	void WorldSync::HandleIncomingDespawn(const WorldDespawnPacket& packet)
	{
		ClientEntity* const tracked = FindClientEntityById(packet.world_id);
		if (!tracked)
			return;

		CoopRuntime::Instance().Log(
			"[world-despawn] client removing id=%u entity=%p\n",
			tracked->world_id, tracked->entity);

		// Remove from tracking.  The entity will no longer receive snapshots.
		m_client_entities.erase(
			std::remove_if(m_client_entities.begin(), m_client_entities.end(),
				[&](const ClientEntity& e) { return e.world_id == packet.world_id; }),
			m_client_entities.end());
	}

	void WorldSync::OnRenderFrame()
	{
		if (!CoopNetGame::Instance().IsClient() ||
			!CoopNetGame::Instance().HasRemotePeer())
		{
			return;
		}

		for (ClientEntity& entity : m_client_entities)
		{
			if (!entity.world_id || !entity.has_latest_snapshot ||
				!IsLiveEntity(entity.entity))
			{
				continue;
			}
			AdvancePresentation(entity, GetTickCount());
			ApplyPresentation(entity);
		}
	}

	bool WorldSync::OnRemotePacket(const void* data, std::uint32_t size)
	{
		if (!data || size < sizeof(PacketHeader))
			return false;
		const PacketHeader* const header = static_cast<const PacketHeader*>(data);
		if (header->m_PacketID != kCoopPacketWorldSpawn &&
			header->m_PacketID != kCoopPacketWorldSnapshot &&
			header->m_PacketID != kCoopPacketWorldReady &&
						header->m_PacketID != kCoopPacketWorldTriggerEvent &&
			header->m_PacketID != kCoopPacketWorldDamage &&
			header->m_PacketID != kCoopPacketWorldDespawn &&
			header->m_PacketID != kCoopPacketTriggerP1Teleport)

		{
			return false;
		}

		if (header->m_PacketID == kCoopPacketWorldReady)
		{
			if (size != sizeof(WorldSync::WorldReadyPacket) || header->m_CompressSize != 0 ||
				header->Size() != size)
			{
				return true;
			}
			const WorldReadyPacket* const packet =
				static_cast<const WorldReadyPacket*>(data);
			if (CoopNetGame::Instance().IsHost() &&
				!CoopNetGame::Instance().IsClient() && packet->sequence != 0)
			{
				InterlockedExchange(&m_host_resync_requested, 1);
				CoopRuntime::Instance().Log(
					"[world-sync] client world ready; host baseline queued\r\n");
			}
			return true;
		}

		// Damage flows in BOTH directions: the client's hits must kill host-side
		// mobs too.  Handle it before the client-only filter below.
		if (header->m_PacketID == kCoopPacketWorldDamage)
		{
			if (size != sizeof(WorldSync::WorldDamagePacket) || header->m_CompressSize != 0 ||
				header->Size() != size)
			{
				return true;
			}
			const WorldDamagePacket* const packet =
				static_cast<const WorldDamagePacket*>(data);
			if (!packet->world_id)
				return true;

			AcquireSRWLockExclusive(&m_damage_lock);
			if (m_incoming_damage.size() < kMaxPendingWorldPackets)
				m_incoming_damage.push_back(*packet);
			ReleaseSRWLockExclusive(&m_damage_lock);
			return true;
		}

		if (header->m_PacketID == kCoopPacketTriggerP1Teleport)
		{
			if (size != sizeof(WorldSync::TriggerP1TeleportPacket) ||
				header->m_CompressSize != 0 || header->Size() != size)
			{
				return true;
			}
			const TriggerP1TeleportPacket* const packet =
				static_cast<const TriggerP1TeleportPacket*>(data);
			if (!packet->sequence)
				return true;
			AcquireSRWLockExclusive(&m_packet_lock);
			if (m_incoming_p1_teleports.size() < kMaxPendingWorldPackets)
				m_incoming_p1_teleports.push_back(*packet);
			ReleaseSRWLockExclusive(&m_packet_lock);
			return true;
		}

		// Legacy world-despawn never destroyed the native client object; it only
		// removed its mapping and could race the final HP=0 packet.  Ignore it until
		// a verified native removal entry point is mapped.
		if (header->m_PacketID == kCoopPacketWorldDespawn)
			return true;

		if (!CoopNetGame::Instance().IsClient() || CoopNetGame::Instance().IsHost())
		{
			return true;
		}
		if (header->m_CompressSize != 0 || header->Size() != size)
		{
			CoopRuntime::Instance().Log("[world-sync] rejected malformed packet\r\n");
			return true;
		}

		if (header->m_PacketID == kCoopPacketWorldSpawn)
		{
			if (size != sizeof(WorldSync::WorldSpawnPacket))
				return true;
			const WorldSpawnPacket* const packet =
				static_cast<const WorldSpawnPacket*>(data);
			// Dynamic spawns (definition_id == -1) have no occurrence counter;
			// their identity is world_id alone.  Map spawns keep the old rules.
			const bool dynamic = packet->key.definition_id < 0;
			if (!packet->world_id || !IsSupportedFamily(packet->key.family) ||
				(!dynamic && !packet->key.occurrence))
			{
				CoopRuntime::Instance().Log("[world-sync] rejected invalid spawn event\r\n");
				return true;
			}
			AcquireSRWLockExclusive(&m_packet_lock);
			if (m_incoming_spawns.size() < kMaxPendingWorldPackets)
				m_incoming_spawns.push_back(*packet);
			ReleaseSRWLockExclusive(&m_packet_lock);
			return true;
		}

		if (header->m_PacketID == kCoopPacketWorldTriggerEvent)
		{
			if (size != sizeof(WorldSync::WorldTriggerEventPacket))
				return true;
			const WorldTriggerEventPacket* const packet =
				static_cast<const WorldTriggerEventPacket*>(data);
			if (!IsSupportedFamily(packet->key.family) || !packet->key.occurrence)
				return true;
			CoopRuntime::Instance().Log(
				"[world-trigger-event] client received key=%08X/%08X/%d occ=%u event=%d\r\n",
				packet->key.family, packet->key.subtype, packet->key.definition_id,
				packet->key.occurrence, packet->event_code);
			AcquireSRWLockExclusive(&m_packet_lock);
			m_incoming_trigger_events.push_back(*packet);
			ReleaseSRWLockExclusive(&m_packet_lock);
			return true;
		}

		if (size != sizeof(WorldSync::WorldSnapshotPacket))
			return true;
		const WorldSnapshotPacket* const packet =
			static_cast<const WorldSnapshotPacket*>(data);
		if (!packet->world_id || !packet->sequence)
			return true;
		AcquireSRWLockExclusive(&m_packet_lock);
		if (m_incoming_snapshots.size() < kMaxPendingWorldPackets)
			m_incoming_snapshots.push_back(*packet);
		ReleaseSRWLockExclusive(&m_packet_lock);
		return true;
	}

	void WorldSync::SendToRemote(const void* data, std::uint32_t size, int flags) const
	{
		if (!data || !size)
			return;
		if (CoopNetGame::Instance().IsClient())
		{
			if (SteamOClient && SteamOClient->IsConnected())
				SteamOClient->SendRaw(data, size, flags);
			return;
		}
		if (!CoopNetGame::Instance().IsHost())
			return;

		CSteamOfflineSocketServer* servers[] = { SteamOServer, SteamSServer };
		for (CSteamOfflineSocketServer* server : servers)
		{
			if (!server || !server->IsSteamSocketOpen())
				continue;
			for (const HSteamNetConnection connection : server->GetPlayers())
				server->SendRaw(connection, data, size, flags);
		}
	}

	void WorldSync::NetworkTick()
	{
		if (!CoopNetGame::Instance().HasRemotePeer())
			return;
		if (CoopNetGame::Instance().IsClient() &&
			InterlockedExchange(&m_client_ready_pending, 0) != 0)
		{
			WorldSync::WorldReadyPacket ready = {};
			ready.m_PacketID = kCoopPacketWorldReady;
			ready.m_RealSize = sizeof(ready) - sizeof(PacketHeader);
			ready.m_SizeOne = ready.m_RealSize;
			ready.sequence = ++m_client_ready_sequence;
			SendToRemote(&ready, sizeof(ready), k_nSteamNetworkingSend_Reliable);
			InterlockedExchange(&m_client_ready_sent, 1);
			CoopRuntime::Instance().Log(
				"[world-sync] client world ready sent (seq=%u)\r\n",
				ready.sequence);
		}
		std::vector<WorldSpawnPacket> spawns;
				std::vector<WorldSnapshotPacket> snapshots;
		std::vector<WorldTriggerEventPacket> trigger_events;
		std::vector<TriggerP1TeleportPacket> p1_teleports;
		AcquireSRWLockExclusive(&m_packet_lock);
		spawns.swap(m_outgoing_spawns);
		snapshots.swap(m_outgoing_snapshots);
		trigger_events.swap(m_outgoing_trigger_events);
		p1_teleports.swap(m_outgoing_p1_teleports);

		ReleaseSRWLockExclusive(&m_packet_lock);

		for (const WorldSpawnPacket& packet : spawns)
			SendToRemote(&packet, sizeof(packet), k_nSteamNetworkingSend_Reliable);
		for (const WorldSnapshotPacket& packet : snapshots)
			SendToRemote(&packet, sizeof(packet), k_nSteamNetworkingSend_Unreliable);
				for (const WorldTriggerEventPacket& packet : trigger_events)
			SendToRemote(&packet, sizeof(packet), k_nSteamNetworkingSend_Reliable);
		for (const TriggerP1TeleportPacket& packet : p1_teleports)
			SendToRemote(&packet, sizeof(packet), k_nSteamNetworkingSend_Reliable);

		std::vector<WorldDamagePacket> damage;

		AcquireSRWLockExclusive(&m_damage_lock);
		damage.swap(m_outgoing_damage);
		ReleaseSRWLockExclusive(&m_damage_lock);
		for (const WorldDamagePacket& packet : damage)
			SendToRemote(&packet, sizeof(packet), k_nSteamNetworkingSend_Reliable);

		std::vector<WorldDespawnPacket> despawns;
		AcquireSRWLockExclusive(&m_packet_lock);
		despawns.swap(m_outgoing_despawns);
		ReleaseSRWLockExclusive(&m_packet_lock);
		for (const WorldDespawnPacket& packet : despawns)
			SendToRemote(&packet, sizeof(packet), k_nSteamNetworkingSend_Reliable);
	}
} // namespace coop
