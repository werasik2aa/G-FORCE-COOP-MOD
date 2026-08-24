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

namespace
{
constexpr DWORD kWorldSnapshotIntervalMs = 350;
constexpr DWORD kMissingSpawnRetryMs = 1000;
constexpr size_t kWorldRegistryWalkLimit = 512;
constexpr size_t kMaxPendingWorldPackets = 1024;

bool SameTriggerKey(const coop::WorldSync::TriggerKey& left,
	const coop::WorldSync::TriggerKey& right)
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
}

namespace coop
{
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
	m_incoming_spawns.clear();
	m_incoming_snapshots.clear();
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
}

void WorldSync::ResetForWorldLoad()
{
	ClearGameState();
	AcquireSRWLockExclusive(&m_packet_lock);
	m_outgoing_spawns.clear();
	m_outgoing_snapshots.clear();
	m_incoming_spawns.clear();
	m_incoming_snapshots.clear();
	ReleaseSRWLockExclusive(&m_packet_lock);
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

void WorldSync::RecordNativeSpawn(void* trigger, void* entity,
	std::uint32_t family, std::uint32_t subtype, std::int32_t definition_id)
{
	if (!trigger || !entity || !IsSupportedFamily(family) || definition_id < 0)
		return;
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
				if (definition_id < 0)
					continue;

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
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log(
			"[world-sync] host registry read fault; frame skipped\r\n");
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
				if (definition_id < 0)
					continue;

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
	AcquireSRWLockExclusive(&m_packet_lock);
	spawns.swap(m_incoming_spawns);
	snapshots.swap(m_incoming_snapshots);
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
	{
		EnumerateClientEntities();
		ProcessClientPackets();
	}
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
		header->m_PacketID != kCoopPacketWorldReady)
	{
		return false;
	}

	if (header->m_PacketID == kCoopPacketWorldReady)
	{
		if (size != sizeof(WorldReadyPacket) || header->m_CompressSize != 0 ||
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

	if (!CoopNetGame::Instance().IsClient() ||
		CoopNetGame::Instance().IsHost())
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
		if (size != sizeof(WorldSpawnPacket))
			return true;
		const WorldSpawnPacket* const packet =
			static_cast<const WorldSpawnPacket*>(data);
		if (!packet->world_id || !IsSupportedFamily(packet->key.family) ||
			packet->key.definition_id < 0 || !packet->key.occurrence)
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

	if (size != sizeof(WorldSnapshotPacket))
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
		WorldReadyPacket ready = {};
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
	AcquireSRWLockExclusive(&m_packet_lock);
	spawns.swap(m_outgoing_spawns);
	snapshots.swap(m_outgoing_snapshots);
	ReleaseSRWLockExclusive(&m_packet_lock);

	for (const WorldSpawnPacket& packet : spawns)
		SendToRemote(&packet, sizeof(packet), k_nSteamNetworkingSend_Reliable);
	for (const WorldSnapshotPacket& packet : snapshots)
		SendToRemote(&packet, sizeof(packet), k_nSteamNetworkingSend_Unreliable);
}
}
