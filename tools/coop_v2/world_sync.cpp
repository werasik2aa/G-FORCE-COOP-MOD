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
		m_forced_client_spawn_active(false)
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
		m_outgoing_despawns.clear();
		m_incoming_spawns.clear();
		m_incoming_snapshots.clear();
		m_incoming_trigger_events.clear();
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
		m_next_world_id = 1;
		m_snapshot_sequence = 0;
		m_last_snapshot_tick = 0;
		m_client_ready_sequence = 0;
		InterlockedExchange(&m_client_ready_pending, 0);
		InterlockedExchange(&m_client_ready_sent, 0);
		m_forced_client_spawn_active = false;
		ZeroMemory(&m_forced_client_spawn, sizeof(m_forced_client_spawn));
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
		m_incoming_spawns.clear();
		m_incoming_snapshots.clear();
		m_incoming_trigger_events.clear();
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
		if (!trigger || !IsSupportedFamily(family))
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
		if (definition_id < 0)
			return;

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

		CoopRuntime::Instance().Log(
			"[world-trigger-event] host queued key=%08X/%08X/%d occ=%u event=%d result=%d\r\n",
			key.family, key.subtype, key.definition_id, key.occurrence,
			event_code, result);

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

	void WorldSync::ReportLocalDamage(void* entity, std::uint32_t amount,
		int event_code)
	{
		if (!entity || !amount || !CoopNetGame::Instance().HasRemotePeer())
			return;
		const std::uint32_t world_id = WorldIdOfEntity(entity);
		if (!world_id)
			return;

		WorldDamagePacket packet = {};
		packet.m_PacketID = kCoopPacketWorldDamage;
		packet.m_RealSize = sizeof(packet) - sizeof(PacketHeader);
		packet.m_SizeOne = packet.m_RealSize;
		packet.world_id = world_id;
		packet.amount = amount;
		packet.event_code = event_code;
		AcquireSRWLockExclusive(&m_damage_lock);
		if (m_outgoing_damage.size() < kMaxPendingWorldPackets)
			m_outgoing_damage.push_back(packet);
		ReleaseSRWLockExclusive(&m_damage_lock);
		CoopRuntime::Instance().Log(
			"[world-damage] sent id=%u event=%d/0x%04X\r\n",
			world_id, event_code, static_cast<unsigned>(event_code) & 0xFFFFu);
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

		// Detect dead/despawned entities: any tracked entity no longer in the
		// registry is considered dead.  Queue a despawn packet for it.
		for (auto it = m_host_entities.begin(); it != m_host_entities.end();)
		{
			if (!IsLiveEntity(it->entity))
			{
				if (it->announced)
				{
					QueueHostDespawn(it->world_id);
					CoopRuntime::Instance().Log(
						"[world-despawn] host queued id=%u\n", it->world_id);
				}
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
			else
			{
				CoopRuntime::Instance().Log(
					"[world-trigger-event] client replayed def=%d occ=%u event=%d\r\n",
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

	void WorldSync::GameTick()
	{
		static bool logged_version = false;
		if (!logged_version)
		{
			logged_version = true;
			CoopRuntime::Instance().Log("[world-sync] GameTick active\r\n");
		}

		if (!CoopNetGame::Instance().HasRemotePeer())
			return;
		if (CoopNetGame::Instance().IsHost())
			EnumerateHostEntities();
		if (CoopNetGame::Instance().IsClient())
		{
			EnumerateClientEntities();
			ProcessClientPackets();
		}
		ApplyIncomingDamage();
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
				pending.amount = packet.amount;
				pending.event_code = packet.event_code;
				m_pending_damage.push_back(pending);
				continue;
			}
			// A dead/despawned entity may still sit in the tracked list with a
			// dangling pointer; never dereference it for a remote hit.
			if (!IsLiveEntity(entity))
				continue;

			// Directly set the death flag on the enemy controller instead of
			// replaying the trigger event (0x42FAB0), which is a notification
			// dispatcher that does NOT reduce HP.
			BYTE* const entityBytes = static_cast<BYTE*>(entity);
			// Diagnostic: dump entity bytes around handler+0x144
			CoopRuntime::Instance().Log(
				"[world-damage] ENT_DIAG id=%u entity=%p "
				"ent+140=%02X%02X%02X%02X ent+144=%02X%02X%02X%02X ent+148=%02X%02X%02X%02X\r\n",
				packet.world_id, entity,
				entityBytes[0x140], entityBytes[0x141], entityBytes[0x142], entityBytes[0x143],
				entityBytes[0x144], entityBytes[0x145], entityBytes[0x146], entityBytes[0x147],
				entityBytes[0x148], entityBytes[0x149], entityBytes[0x14A], entityBytes[0x14B]);
			BYTE* const handler = *reinterpret_cast<BYTE**>(entityBytes + gforce::kEntityHandlerOffset);
			if (!handler)
				continue;
			BYTE* const controller = *reinterpret_cast<BYTE**>(handler + gforce::kHandlerControllerOffset);
			if (!controller)
				continue;
			// Diagnostic: dump controller bytes around the expected offsets
			CoopRuntime::Instance().Log(
				"[world-damage] DIAG id=%u entity=%p handler=%p controller=%p "
				"ctrl+A8=%02X%02X%02X%02X ctrl+AC=%02X ctrl+C0=%02X%02X%02X%02X\r\n",
				packet.world_id, entity, handler, controller,
				controller[0xA8], controller[0xA9], controller[0xAA], controller[0xAB],
				controller[0xAC],
				controller[0xC0], controller[0xC1], controller[0xC2], controller[0xC3]);
			// Set accumulated hits high enough to exceed any threshold
			*reinterpret_cast<std::uint32_t*>(controller + gforce::kControllerAccumulatedHitsOffset) = 999;
			// Set death flag
			*(controller + gforce::kControllerDeathFlagOffset) = 1;
			// Clear invulnerability timer
			*reinterpret_cast<float*>(controller + gforce::kControllerInvulnTimerOffset) = 0.0f;
			CoopRuntime::Instance().Log(
				"[world-damage] killed entity id=%u entity=%p controller=%p\r\n",
				packet.world_id, entity, controller);
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
			{
				BYTE* const entityBytes = static_cast<BYTE*>(entity);
				BYTE* const handler = *reinterpret_cast<BYTE**>(entityBytes + gforce::kEntityHandlerOffset);
				BYTE* const controller = handler ? *reinterpret_cast<BYTE**>(handler + gforce::kHandlerControllerOffset) : NULL;
				if (controller)
				{
					*reinterpret_cast<std::uint32_t*>(controller + gforce::kControllerAccumulatedHitsOffset) = 999;
					*(controller + gforce::kControllerDeathFlagOffset) = 1;
					*reinterpret_cast<float*>(controller + gforce::kControllerInvulnTimerOffset) = 0.0f;
					CoopRuntime::Instance().Log(
						"[world-damage] killed deferred entity id=%u entity=%p\r\n",
						it->world_id, entity);
				}
			}
			it = m_pending_damage.erase(it);
		}
	}

	void WorldSync::DebugKillNearest()
	{
		void* p1 = NULL;
		__try
		{
			p1 = reinterpret_cast<void**>(gforce::kGPigEntityArray)[1];
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		if (!p1)
		{
			CoopRuntime::Instance().Log("[debug-kill] no P1 entity\r\n");
			return;
		}

		float p1_pos[3] = {};
		__try
		{
			const float* pos = reinterpret_cast<const float*>(
				static_cast<const BYTE*>(p1) + gforce::kEntityPositionOffset);
			p1_pos[0] = pos[0];
			p1_pos[1] = pos[1];
			p1_pos[2] = pos[2];
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log("[debug-kill] cannot read P1 position\r\n");
			return;
		}

		BYTE* const registry = *reinterpret_cast<BYTE**>(gforce::kEntityRegistry);
		if (!registry)
		{
			CoopRuntime::Instance().Log("[debug-kill] no entity registry\r\n");
			return;
		}

		const size_t list_offsets[] = {
			gforce::kEntityRegistryMonsterListOffset,
			gforce::kEntityRegistryNpcListOffset
		};

		void* best_entity = NULL;
		float best_dist_sq = 1e30f;
		BYTE* best_controller = NULL;
		BYTE* best_handler = NULL;
		BYTE* best_trigger = NULL;
		std::uint32_t best_family = 0;
		std::uint32_t best_subtype = 0;
		std::int32_t best_def = -1;

		for (size_t li = 0; li < _countof(list_offsets); ++li)
		{
			BYTE* node = *reinterpret_cast<BYTE**>(registry + list_offsets[li]);
			for (size_t v = 0; node && v < kWorldRegistryWalkLimit; ++v)
			{
				void* const entity = *reinterpret_cast<void**>(
					node + gforce::kIntrusiveListValueOffset);
				node = *reinterpret_cast<BYTE**>(
					node + gforce::kIntrusiveListNextOffset);
				if (!entity)
					continue;
				__try
				{
					BYTE* const e = static_cast<BYTE*>(entity);
					BYTE* const trigger = *reinterpret_cast<BYTE**>(
						e + gforce::kEntityTriggerOffset);
					if (!trigger)
						continue;
					const std::uint32_t family = *reinterpret_cast<std::uint32_t*>(
						trigger + gforce::kTriggerFamilyOffset);
					const std::uint32_t subtype = *reinterpret_cast<std::uint32_t*>(
						trigger + gforce::kTriggerSubtypeOffset);
					const std::int32_t def = *reinterpret_cast<std::int32_t*>(
						trigger + gforce::kTriggerSpawnIdOffset);

					const float* pos = reinterpret_cast<const float*>(
						e + gforce::kEntityPositionOffset);
					const float dx = pos[0] - p1_pos[0];
					const float dy = pos[1] - p1_pos[1];
					const float dz = pos[2] - p1_pos[2];
					const float dist_sq = dx*dx + dy*dy + dz*dz;
					if (dist_sq >= best_dist_sq)
						continue;
					BYTE* const handler = *reinterpret_cast<BYTE**>(
						e + gforce::kEntityHandlerOffset);
					if (!handler)
						continue;
					BYTE* const controller = *reinterpret_cast<BYTE**>(
						handler + gforce::kHandlerControllerOffset);
					if (!controller)
						continue;

					const DWORD vtbl = *reinterpret_cast<DWORD*>(controller);
					const DWORD slot28 = *reinterpret_cast<DWORD*>(vtbl + 0x28);

					if (slot28 != 0x005B25D0)
						continue;

					best_dist_sq = dist_sq;
					best_entity = entity;
					best_controller = controller;
					best_handler = handler;
					best_trigger = trigger;
					best_family = family;
					best_subtype = subtype;
					best_def = def;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {}
			}
		}

		if (!best_entity || !best_controller)
		{
			CoopRuntime::Instance().Log("[debug-kill] no enemy entities found\r\n");
			return;
		}

		CoopRuntime::Instance().Log(
			"[debug-kill] nearest entity=%p fam=0x%08X sub=0x%08X def=%d "
			"controller=%p handler=%p trigger=%p dist=%.1f\r\n",
			best_entity, best_family, best_subtype, best_def,
			best_controller, best_handler, best_trigger, sqrtf(best_dist_sq));

		// Dump key fields for diagnosis.
		__try
		{
			BYTE* const be = static_cast<BYTE*>(best_entity);
			CoopRuntime::Instance().Log(
				"[debug-kill] entity dump: pos=(%.2f,%.2f,%.2f) "
				"handler=%p trigger=%p resolver_slot=%08X\r\n",
				*reinterpret_cast<float*>(be + gforce::kEntityPositionOffset),
				*reinterpret_cast<float*>(be + gforce::kEntityPositionOffset + 4),
				*reinterpret_cast<float*>(be + gforce::kEntityPositionOffset + 8),
				*reinterpret_cast<void**>(be + gforce::kEntityHandlerOffset),
				*reinterpret_cast<void**>(be + gforce::kEntityTriggerOffset),
				*reinterpret_cast<DWORD*>(be + gforce::kEntityResolverSlotOffset));
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}

		__try
		{
			CoopRuntime::Instance().Log(
				"[debug-kill] trigger dump: spawned_obj=%p "
				"trigger_resolver_slot=%08X\r\n",
				*reinterpret_cast<void**>(best_trigger + gforce::kTriggerSpawnedObjectOffset),
				*reinterpret_cast<DWORD*>(best_trigger + gforce::kTriggerResolverSlotOffset));
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}

		// ===== APPROACH 1: trigger event 280 (kill notification) =====
		if (best_trigger)
		{
			CoopRuntime::Instance().Log(
				"[debug-kill] approach 1: trigger_event(280) trigger=%p\r\n",
				best_trigger);
			__try
			{
				const int result = CoopNetGame::Instance().CallNativeTriggerEvent(
					best_trigger, 0x0118);
				CoopRuntime::Instance().Log(
					"[debug-kill] trigger_event(280) returned %d\r\n", result);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				CoopRuntime::Instance().Log(
					"[debug-kill] trigger_event CRASHED (code=0x%08X)\r\n",
					GetExceptionCode());
			}
		}

		// ===== APPROACH 2: vtable+0x28 damage handler with many event+4 variants =====
		{
			void* p1_trigger = NULL;
			__try
			{
				p1_trigger = *reinterpret_cast<void**>(
					static_cast<BYTE*>(p1) + gforce::kEntityTriggerOffset);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}

			// Read P1's resolver slot (entity+0x658) from ammo trace pattern.
			DWORD p1_resolver_slot = 0;
			__try
			{
				p1_resolver_slot = *reinterpret_cast<DWORD*>(
					static_cast<BYTE*>(p1) + gforce::kEntityResolverSlotOffset);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}

			// Read P1 trigger's resolver slot (trigger+0x168) from NPC trace.
			DWORD p1_trigger_resolver = 0;
			if (p1_trigger)
			{
				__try
				{
					p1_trigger_resolver = *reinterpret_cast<DWORD*>(
						static_cast<BYTE*>(p1_trigger) + gforce::kTriggerResolverSlotOffset);
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {}
			}

			// Read enemy trigger's resolver slot.
			DWORD enemy_trigger_resolver = 0;
			if (best_trigger)
			{
				__try
				{
					enemy_trigger_resolver = *reinterpret_cast<DWORD*>(
						static_cast<BYTE*>(best_trigger) + gforce::kTriggerResolverSlotOffset);
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {}
			}

			// Read P1 trigger's spawned object (trigger+0x98).
			void* p1_spawned_obj = NULL;
			if (p1_trigger)
			{
				__try
				{
					p1_spawned_obj = *reinterpret_cast<void**>(
						static_cast<BYTE*>(p1_trigger) + gforce::kTriggerSpawnedObjectOffset);
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {}
			}

			CoopRuntime::Instance().Log(
				"[debug-kill] P1=%p p1_trigger=%p p1_resolver_slot=%08X "
				"p1_trigger_resolver=%08X p1_spawned_obj=%p "
				"enemy_trigger_resolver=%08X\r\n",
				p1, p1_trigger, p1_resolver_slot,
				p1_trigger_resolver, p1_spawned_obj,
				enemy_trigger_resolver);

			const DWORD vtable = *reinterpret_cast<DWORD*>(best_controller);
			typedef int (__thiscall *DamageHandlerFn)(void* ctrl, void* evt);
			const DamageHandlerFn damage_fn =
				*reinterpret_cast<DamageHandlerFn*>(vtable + 0x28);

			// Build a damage event for each candidate event+4 value.
			struct { const char* label; DWORD val; } candidates[] = {
				{ "p1_ptr",        reinterpret_cast<DWORD>(p1) },
				{ "p1_id",         0x79130001 },
				{ "p1_trig",       reinterpret_cast<DWORD>(p1_trigger) },
				{ "p1_resolver",   p1_resolver_slot },
				{ "p1_trig_res",   p1_trigger_resolver },
				{ "p1_spawned",    reinterpret_cast<DWORD>(p1_spawned_obj) },
				{ "enemy_ptr",     reinterpret_cast<DWORD>(best_entity) },
				{ "enemy_trig",    reinterpret_cast<DWORD>(best_trigger) },
				{ "enemy_trig_res",enemy_trigger_resolver },
				{ "enemy_handler", reinterpret_cast<DWORD>(best_handler) },
				{ "enemy_ctrl",    reinterpret_cast<DWORD>(best_controller) },
				{ "zero",          0 },
				{ "one",           1 },
				{ "p1_handler",    0 },
				{ "p1_ctrl",       0 },
				// Weapon item IDs — ResolveItemById resolves items, not entities
				{ "whip_50",       0x50000000 },
				{ "holster_40",    0x40050001 },
				{ "ammo_60",       0x60050008 },
				// Enemy definition ID from trigger+0x130
				{ "enemy_defid",   0 },
			};

			// Fill in P1 handler, controller, and enemy definition_id.
			__try
			{
				candidates[13].val = reinterpret_cast<DWORD>(
					*reinterpret_cast<void**>(
						static_cast<BYTE*>(p1) + gforce::kEntityHandlerOffset));
				candidates[14].val = reinterpret_cast<DWORD>(
					*reinterpret_cast<void**>(
						*reinterpret_cast<BYTE**>(
							static_cast<BYTE*>(p1) + gforce::kEntityHandlerOffset)
						+ gforce::kHandlerControllerOffset));
				// enemy_defid = trigger+0x130 (definition_id from spawn)
				if (best_trigger)
				{
					candidates[18].val = *reinterpret_cast<DWORD*>(
						static_cast<BYTE*>(best_trigger) +
						gforce::kTriggerSpawnIdOffset);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}

			for (auto& c : candidates)
			{
				__declspec(align(16)) BYTE evt[256] = {};
				*reinterpret_cast<DWORD*>(evt + 4) = c.val;
				*reinterpret_cast<DWORD*>(evt + 80) = 999;

				__try
				{
					const int r = damage_fn(best_controller, evt);
					CoopRuntime::Instance().Log(
						"[debug-kill] vtable+0x28(%s=%08X) => %d\r\n",
						c.label, c.val, r);
					if (r != 0)
					{
						CoopRuntime::Instance().Log(
							"[debug-kill] *** DAMAGE HANDLER ACCEPTED event+4=%s=%08X ***\r\n",
							c.label, c.val);
					}
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					CoopRuntime::Instance().Log(
						"[debug-kill] vtable+0x28(%s) CRASHED (0x%08X)\r\n",
						c.label, GetExceptionCode());
				}
			}
		}

		// ===== APPROACH 3: SelectMode — try every known enemy mode ID =====
		{
			typedef bool (__thiscall *SelectModeFn)(void*, uint32_t, bool);
			const SelectModeFn select_mode =
				reinterpret_cast<SelectModeFn>(gforce::kSelectMode);

			const DWORD enemy_modes[] = {
				0x61000000, // Inactive
				0x6100003B, // Default (GPig)
				0x6100000C, // Scan
				0x61000033, // Fly_Idle
				0x61000034, // Fly_Active
				0x61000065, // Mooch
				0x61000052, // Carried
				0x6100006E, // RDV
				0x61000075, // unknown
				0x61000081, // MiniGame
				0x6100009B, // Pause
			};

			// Also scan the controller's actual mode list.
			__try
			{
				const DWORD mode_count = *reinterpret_cast<DWORD*>(
					best_controller + 0x10 * sizeof(DWORD));
				const DWORD modes_array = *reinterpret_cast<DWORD*>(
					best_controller + 0x12 * sizeof(DWORD));
				CoopRuntime::Instance().Log(
					"[debug-kill] controller mode_count=%u modes_array=%08X\r\n",
					mode_count, modes_array);

				if (modes_array && mode_count > 0 && mode_count < 32)
				{
					for (DWORD m = 0; m < mode_count; ++m)
					{
						const DWORD mode_ptr = *reinterpret_cast<DWORD*>(
							modes_array + m * 4);
						if (!mode_ptr)
							continue;
						const DWORD mode_id = *reinterpret_cast<DWORD*>(
							mode_ptr + 2 * sizeof(DWORD));
						CoopRuntime::Instance().Log(
							"[debug-kill]   mode[%u]=%08X id=%08X\r\n",
							m, mode_ptr, mode_id);
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				CoopRuntime::Instance().Log(
					"[debug-kill] controller mode scan CRASHED\r\n");
			}

			for (DWORD mode_id : enemy_modes)
			{
				__try
				{
					const bool r = select_mode(best_controller, mode_id, true);
					CoopRuntime::Instance().Log(
						"[debug-kill] SelectMode(0x%08X) => %d\r\n",
						mode_id, static_cast<int>(r));
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					CoopRuntime::Instance().Log(
						"[debug-kill] SelectMode(0x%08X) CRASHED (0x%08X)\r\n",
						mode_id, GetExceptionCode());
				}
			}
		}

		// ===== APPROACH 4: Read sub_472B00's input from the entity's own
		//     weapon/projectile code pattern (entity+0x658) and pass directly.
		//     The ammo trace at 0x5B8A29 does: push [edi+658h]; call sub_472B00.
		//     We try ALL sub_472B00 candidates as event+4 for the damage handler.
		// ===== Already covered in Approach 2 above. =====

		// ===== APPROACH 5: sub_472B00 byte dump =====
		{
			const DWORD sub472b00_addr = 0x00472B00;
			__try
			{
				const BYTE* code = reinterpret_cast<const BYTE*>(sub472b00_addr);
				CoopRuntime::Instance().Log(
					"[debug-kill] sub_472B00 bytes: "
					"%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
					code[0], code[1], code[2], code[3],
					code[4], code[5], code[6], code[7],
					code[8], code[9], code[10], code[11]);
				// Compute the real resolver address:
				// bytes 0-3: mov eax,[esp+4]  (4 bytes)
				// byte 4: push eax (1 byte)
				// bytes 5-9: call rel32 (5 bytes)
				// rel32 target = (sub472b00 + 10) + *(int32_t*)(code+6)
				const int32_t rel32 = *reinterpret_cast<const int32_t*>(code + 6);
				const DWORD real_target = sub472b00_addr + 10 + rel32;
				CoopRuntime::Instance().Log(
					"[debug-kill] sub_472B00 calls real resolver at 0x%08X\r\n",
					real_target);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		CoopRuntime::Instance().Log("[debug-kill] done\r\n");
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
			header->m_PacketID != kCoopPacketWorldDespawn)
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
			if (!packet->world_id || !packet->amount)
				return true;
			CoopRuntime::Instance().Log(
				"[world-damage] received id=%u event=%d\r\n",
				packet->world_id, packet->event_code);
			AcquireSRWLockExclusive(&m_damage_lock);
			if (m_incoming_damage.size() < kMaxPendingWorldPackets)
				m_incoming_damage.push_back(*packet);
			ReleaseSRWLockExclusive(&m_damage_lock);
			return true;
		}

		// Host -> client despawn notification (client-only reception).
		if (header->m_PacketID == kCoopPacketWorldDespawn)
		{
			if (size != sizeof(WorldSync::WorldDespawnPacket) || header->m_CompressSize != 0 ||
				header->Size() != size)
			{
				return true;
			}
			const WorldDespawnPacket* const packet =
				static_cast<const WorldDespawnPacket*>(data);
			if (!packet->world_id)
				return true;
			CoopRuntime::Instance().Log(
				"[world-despawn] received id=%u\r\n",
				packet->world_id);
			AcquireSRWLockExclusive(&m_packet_lock);
			if (m_incoming_despawns.size() < kMaxPendingWorldPackets)
				m_incoming_despawns.push_back(*packet);
			ReleaseSRWLockExclusive(&m_packet_lock);
			return true;
		}

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
		AcquireSRWLockExclusive(&m_packet_lock);
		spawns.swap(m_outgoing_spawns);
		snapshots.swap(m_outgoing_snapshots);
		trigger_events.swap(m_outgoing_trigger_events);
		ReleaseSRWLockExclusive(&m_packet_lock);

		for (const WorldSpawnPacket& packet : spawns)
			SendToRemote(&packet, sizeof(packet), k_nSteamNetworkingSend_Reliable);
		for (const WorldSnapshotPacket& packet : snapshots)
			SendToRemote(&packet, sizeof(packet), k_nSteamNetworkingSend_Unreliable);
		for (const WorldTriggerEventPacket& packet : trigger_events)
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
