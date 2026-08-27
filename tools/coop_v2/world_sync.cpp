#include "world_sync.h"

#include "coop_netgame.h"
#include "coop_runtime.h"
#include "gforce_constants.h"
#include "retail/retail_views.h"
#include "protocol/packet_view.h"
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
		m_stale_client_world_ids.clear();
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

		std::uint32_t WorldSync::LocalOccurrence(void* trigger)
		{
			return NextOccurrence(CoopNetGame::Instance().IsHost() ?
				m_host_trigger_counters : m_client_trigger_counters, trigger);
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

			retail::Transform transform = {};
			const retail::EntityRef entity_ref = { retail::ToAddress(entity) };
			if (!retail::EntityView(entity_ref).ReadTransform(transform))
				return false;

			memcpy(position, &transform.position, sizeof(transform.position));
			memcpy(rotation, &transform.rotation, sizeof(transform.rotation));
			return true;
		}

		bool WorldSync::ReadEntityHealth(void* entity, float& health) const
		{
			if (!entity)
				return false;
			const retail::EntityRef entity_ref = { retail::ToAddress(entity) };
			return retail::EntityView(entity_ref).ReadHealth(health);
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
			// Keep every map-defined trigger: breakables and doors are not only
			// represented by live NPC/monster spawners.
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
			protocol::InitializeFixedPacket(packet, protocol::PacketKind::WorldSpawn);
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
			protocol::InitializeFixedPacket(packet,
				protocol::PacketKind::WorldDespawn);
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
			protocol::InitializeFixedPacket(packet,
				protocol::PacketKind::WorldSnapshot);
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

		void WorldSync::QueueTriggerEvent(const TriggerKey& key, int event_code,
			int result)
		{
			if (!CoopNetGame::Instance().HasRemotePeer() || !key.occurrence)
				return;

			WorldTriggerEventPacket packet = {};
			protocol::InitializeFixedPacket(packet,
				protocol::PacketKind::WorldTriggerEvent);
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
				protocol::InitializeFixedPacket(packet,
					protocol::PacketKind::WorldDamage);
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
				if (std::find(m_stale_client_world_ids.begin(),
					m_stale_client_world_ids.end(), it->world_id) !=
					m_stale_client_world_ids.end())
				{
					it = m_pending_snapshots.erase(it);
					continue;
				}
				++it;
				continue;
			}
			if (!IsLiveEntity(entity->entity))
			{
				const std::uint32_t stale_world_id = entity->world_id;
				if (std::find(m_stale_client_world_ids.begin(),
					m_stale_client_world_ids.end(), stale_world_id) ==
					m_stale_client_world_ids.end())
				{
					m_stale_client_world_ids.push_back(stale_world_id);
				}
				CoopRuntime::Instance().Log(
					"[world-snapshot] client forgetting stale id=%u entity=%p\r\n",
					stale_world_id, entity->entity);
				m_client_entities.erase(std::remove_if(m_client_entities.begin(),
					m_client_entities.end(), [stale_world_id](const ClientEntity& tracked)
					{
						return tracked.world_id == stale_world_id;
					}), m_client_entities.end());
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

	void WorldSync::GameTick()
	{
		if (!CoopNetGame::Instance().HasRemotePeer())
			return;
		if (CoopNetGame::Instance().IsHost())
			EnumerateHostEntities();
		if (CoopNetGame::Instance().IsClient())
			EnumerateClientEntities();
			// Inbound trigger pulses are valid in both directions, unlike the earlier
			// host-only spawn and trigger-event paths.
		ProcessClientPackets();
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
			const retail::EntityRef entity_ref = { retail::ToAddress(entity) };
			return retail::EntityView(entity_ref).SetHealth(health);
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

			bool WorldSync::HandleWorldReadyPacket(const protocol::PacketView& view)
		{
			WorldReadyPacket packet = {};
			if (!view.CopyUncompressedExact(packet))
				return true;
			if (CoopNetGame::Instance().IsHost() &&
				!CoopNetGame::Instance().IsClient() && packet.sequence != 0)
			{
				InterlockedExchange(&m_host_resync_requested, 1);
				CoopRuntime::Instance().Log(
					"[world-sync] client world ready; host baseline queued\r\n");
			}
			return true;
		}

		bool WorldSync::HandleWorldDamagePacket(const protocol::PacketView& view)
		{
			WorldDamagePacket packet = {};
			if (!view.CopyUncompressedExact(packet) || !packet.world_id)
				return true;
			AcquireSRWLockExclusive(&m_damage_lock);
			if (m_incoming_damage.size() < kMaxPendingWorldPackets)
				m_incoming_damage.push_back(packet);
			ReleaseSRWLockExclusive(&m_damage_lock);
			return true;
		}

		bool WorldSync::HandleWorldSpawnPacket(const protocol::PacketView& view)
		{
			WorldSpawnPacket packet = {};
			if (!view.CopyUncompressedExact(packet) ||
				!CoopNetGame::Instance().IsClient() || CoopNetGame::Instance().IsHost())
			{
				return true;
			}

			const bool dynamic = packet.key.definition_id < 0;
			if (!packet.world_id || !IsSupportedFamily(packet.key.family) ||
				(!dynamic && !packet.key.occurrence))
			{
				CoopRuntime::Instance().Log("[world-sync] rejected invalid spawn event\r\n");
				return true;
			}

			AcquireSRWLockExclusive(&m_packet_lock);
			if (m_incoming_spawns.size() < kMaxPendingWorldPackets)
				m_incoming_spawns.push_back(packet);
			ReleaseSRWLockExclusive(&m_packet_lock);
			return true;
		}

		bool WorldSync::HandleWorldTriggerEventPacket(const protocol::PacketView& view)
		{
			WorldTriggerEventPacket packet = {};
			if (!view.CopyUncompressedExact(packet) || !packet.key.occurrence)
				return true;

			CoopRuntime::Instance().Log(
				"[world-trigger-event] peer received key=%08X/%08X/%d occ=%u event=%d\r\n",
				packet.key.family, packet.key.subtype, packet.key.definition_id,
				packet.key.occurrence, packet.event_code);
			AcquireSRWLockExclusive(&m_packet_lock);
			if (m_incoming_trigger_events.size() < kMaxPendingWorldPackets)
				m_incoming_trigger_events.push_back(packet);
			ReleaseSRWLockExclusive(&m_packet_lock);
			return true;
		}

		bool WorldSync::HandleWorldSnapshotPacket(const protocol::PacketView& view)
		{
			WorldSnapshotPacket packet = {};
			if (!view.CopyUncompressedExact(packet) ||
				!CoopNetGame::Instance().IsClient() || CoopNetGame::Instance().IsHost())
			{
				return true;
			}
			if (!packet.world_id || !packet.sequence)
				return true;

			AcquireSRWLockExclusive(&m_packet_lock);
			if (m_incoming_snapshots.size() < kMaxPendingWorldPackets)
				m_incoming_snapshots.push_back(packet);
			ReleaseSRWLockExclusive(&m_packet_lock);
			return true;
		}

		bool WorldSync::OnRemotePacket(const void* data, std::uint32_t size)
		{
			const protocol::PacketView view(data, size);
			PacketHeader header = {};
			if (!view.ReadHeader(header))
				return false;

			switch (view.Kind())
			{
			case protocol::PacketKind::WorldReady:
				return HandleWorldReadyPacket(view);
			case protocol::PacketKind::WorldSpawn:
				return HandleWorldSpawnPacket(view);
			case protocol::PacketKind::WorldSnapshot:
				return HandleWorldSnapshotPacket(view);
			case protocol::PacketKind::WorldTriggerEvent:
				return HandleWorldTriggerEventPacket(view);
			case protocol::PacketKind::WorldDamage:
				return HandleWorldDamagePacket(view);
			case protocol::PacketKind::WorldDespawn:
				// Legacy despawn has no verified native destruction entry point.  Keep
				// consuming it for wire compatibility, but do not drop a live local object.
				return true;
			default:
				return false;
			}
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
				protocol::InitializeFixedPacket(ready, protocol::PacketKind::WorldReady);
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
