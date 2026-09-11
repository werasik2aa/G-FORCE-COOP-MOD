#include "world_sync.h"

#include "coop_netgame.h"
#include "coop_runtime.h"
#include "gforce_constants.h"
#include "player2.h"
#include "retail/retail_types.h"
#include "retail/retail_views.h"
#include "protocol/packet_view.h"
#include "ServerClient/MClient.h"
#include "ServerClient/MServer.h"
#include "ServerClient/MServerONLINE.h"
#include "ServerClient/SteamManager.h"

#include <float.h>
#include <math.h>
#include <string.h>

namespace coop
{
	constexpr DWORD kWorldSnapshotIntervalMs = 350;
	constexpr DWORD kMissingSpawnRetryMs = 1000;
	constexpr DWORD kForcedClientSpawnRegistrationTimeoutMs = 3000;
	constexpr size_t kMaxPendingWorldPackets = 1024;

	namespace
	{
	bool IsFiniteFloat(float value)
	{
		// Comparisons reject NaN, while both infinities are outside FLT_MAX.
		// Keep this x86 wire path independent from CRT-specific finite helpers.
		return value >= -FLT_MAX && value <= FLT_MAX;
	}

	bool IsFiniteFloatArray(const float* values, size_t count)
	{
		if (!values)
			return false;
		for (size_t index = 0; index < count; ++index)
		{
			if (!IsFiniteFloat(values[index]))
				return false;
		}
		return true;
	}

	bool IsFiniteWireTransform(const float position[4], const float rotation[4])
	{
		return IsFiniteFloatArray(position, 4) &&
			IsFiniteFloatArray(rotation, 4);
	}

	bool IsFiniteRetailTransform(const retail::Transform& transform)
	{
		return IsFiniteFloat(transform.position.x) &&
			IsFiniteFloat(transform.position.y) &&
			IsFiniteFloat(transform.position.z) &&
			IsFiniteFloat(transform.position.w) &&
			IsFiniteFloat(transform.rotation.x) &&
			IsFiniteFloat(transform.rotation.y) &&
			IsFiniteFloat(transform.rotation.z) &&
			IsFiniteFloat(transform.rotation.w);
	}

	std::uint32_t HashMapObjectTransform(const retail::Transform& transform)
	{
		// Map-trigger transforms are fixed load data. Hash their exact IEEE fields
		// rather than a process pointer; a zero result is reserved as invalid.
		const float values[] = {
			transform.position.x, transform.position.y, transform.position.z,
			transform.position.w, transform.rotation.x, transform.rotation.y,
			transform.rotation.z, transform.rotation.w
		};
		std::uint32_t hash = 2166136261u;
		for (const float value : values)
		{
			std::uint32_t bits = 0;
			memcpy(&bits, &value, sizeof(bits));
			hash ^= bits;
			hash *= 16777619u;
		}
		return hash != 0 ? hash : 1u;
	}

	std::uint32_t TriggerTransformSignature(void* trigger)
	{
		if (!trigger)
			return 0;
		const retail::TriggerRef trigger_ref = { retail::ToAddress(trigger) };
		retail::Transform transform = {};
		if (!retail::TriggerView(trigger_ref).ReadTransform(transform) ||
			!IsFiniteRetailTransform(transform))
		{
			return 0;
		}
		return HashMapObjectTransform(transform);
	}

	bool IsStrictlyNewerSequence(std::uint32_t candidate,
		std::uint32_t previous)
	{
		return previous == 0 || static_cast<std::int32_t>(candidate - previous) > 0;
	}

	bool IsObjectEventRoute(std::uint32_t route)
	{
		return route == protocol::kWorldObjectEventRouteRelay ||
			route == protocol::kWorldObjectEventRouteForwarder ||
			route == protocol::kWorldObjectEventRouteEntityTriggerActivation ||
			route == protocol::kWorldObjectEventRouteEntityTriggerRequest;
	}

	bool IsEntityTriggerRoute(std::uint32_t route)
	{
		return route == protocol::kWorldObjectEventRouteEntityTriggerActivation ||
			route == protocol::kWorldObjectEventRouteEntityTriggerRequest;
	}
	}

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
		m_object_event_sequence(0),
		m_last_received_object_event_sequence(0),
		m_last_snapshot_tick(0),
		m_host_resync_requested(1),
		m_client_ready_pending(0),
		m_client_ready_sent(0),
		m_client_ready_sequence(0),
		m_forced_client_spawn_active(false),
		m_forced_client_spawn_trigger(nullptr),
		m_forced_client_spawn_started_tick(0),
		m_forced_client_spawn_native_invoked(false)
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

	void WorldSync::ClearForcedClientSpawn()
	{
		m_forced_client_spawn_active = false;
		m_forced_client_spawn_trigger = nullptr;
		m_forced_client_spawn_started_tick = 0;
		m_forced_client_spawn_native_invoked = false;
		ZeroMemory(&m_forced_client_spawn, sizeof(m_forced_client_spawn));
	}

	bool WorldSync::IsExpectedClientReplicaSpawn(void* trigger,
		std::uint32_t family, std::uint32_t subtype) const
	{
		return CoopNetGame::Instance().IsClient() && m_forced_client_spawn_active &&
			trigger && trigger == m_forced_client_spawn_trigger &&
			m_forced_client_spawn.key.family == family &&
			m_forced_client_spawn.key.subtype == subtype;
	}

	bool WorldSync::BeginExpectedClientReplicaSpawn(void* trigger,
		std::uint32_t family, std::uint32_t subtype)
	{
		if (!IsExpectedClientReplicaSpawn(trigger, family, subtype) ||
			m_forced_client_spawn_native_invoked)
		{
			return false;
		}
		m_forced_client_spawn_native_invoked = true;
		return true;
	}

	bool WorldSync::ClaimForcedClientSpawn(void* trigger, std::uint32_t family,
		std::uint32_t subtype, TriggerKey& out_key,
		std::uint32_t& out_world_id)
	{
		out_key = {};
		out_world_id = 0;
		if (!IsExpectedClientReplicaSpawn(trigger, family, subtype))
			return false;
		out_key = m_forced_client_spawn.key;
		out_world_id = m_forced_client_spawn.world_id;
		ClearForcedClientSpawn();
		return out_world_id != 0;
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
		m_outgoing_object_events.clear();
		m_outgoing_despawns.clear();

		m_incoming_spawns.clear();
		m_incoming_snapshots.clear();
		m_incoming_trigger_events.clear();
		m_incoming_object_events.clear();
		m_object_event_sequence = 0;
		m_last_received_object_event_sequence = 0;
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
		m_object_event_sequence = 0;
		m_last_received_object_event_sequence = 0;
		m_last_snapshot_tick = 0;
		m_client_ready_sequence = 0;
		InterlockedExchange(&m_client_ready_pending, 0);
		InterlockedExchange(&m_client_ready_sent, 0);
		ClearForcedClientSpawn();
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
		// Reset P2 state so stale entity pointers don't crash on update.
		Player2Module::Instance().ResetForWorldLoad();
		AcquireSRWLockExclusive(&m_packet_lock);
		m_outgoing_spawns.clear();
		m_outgoing_snapshots.clear();
		m_outgoing_trigger_events.clear();
		m_outgoing_object_events.clear();
		m_incoming_spawns.clear();

		m_incoming_snapshots.clear();
		m_incoming_trigger_events.clear();
		m_incoming_object_events.clear();
		m_object_event_sequence = 0;
		m_last_received_object_event_sequence = 0;
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
					CoopRuntime::Instance().Log(
						"[world-trigger] loose template match family=%08X subtype=%08X host_def=%d local_def=%d\r\n",
						family, subtype, definition_id, existing.definition_id);
					return existing.trigger;
				}
			}
		}
		return nullptr;
	}

	bool WorldSync::ReadEntityTransform(void* entity, float position[4],
		float rotation[4]) const
	{
		if (!entity || !position || !rotation)
			return false;

		retail::Transform transform = {};
		const retail::EntityRef entity_ref = { retail::ToAddress(entity) };
		if (!retail::EntityView(entity_ref).ReadTransform(transform) ||
			!IsFiniteRetailTransform(transform))
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
		const retail::EntityRef expected = { retail::ToAddress(entity) };
		bool found = false;
		const retail::EntityRegistryView registry;
		const bool walked = registry.VisitLiveEntities(
			gforce::kEntityRegistryWalkSafetyLimit,
			[&expected, &found](retail::EntityRef candidate)
			{
				if (candidate != expected)
					return true;
				found = true;
				return false;
			});
		return walked && found;
	}

	void WorldSync::RecordTriggerTemplate(void* trigger, std::uint32_t family,
		std::uint32_t subtype)
	{
		// Keep every map-defined trigger: breakables and doors are not only
		// represented by live NPC/monster spawners.
		if (!trigger)
			return;

		const retail::TriggerRef trigger_ref = {
			retail::ToAddress(trigger)
		};
		retail::TriggerIdentity identity = {};
		if (!retail::TriggerView(trigger_ref).Identity(identity))
			return;
		family = identity.family;
		subtype = identity.subtype;
		const std::int32_t definition_id = identity.definition_id;
		const std::uint32_t transform_signature = TriggerTransformSignature(trigger);
		for (TriggerTemplate& existing : m_trigger_templates)
		{
			if (existing.trigger == trigger)
			{
				existing.family = family;
				existing.subtype = subtype;
				existing.definition_id = definition_id;
				existing.transform_signature = transform_signature;
				return;
			}
		}
		TriggerTemplate entry = {};
		entry.trigger = trigger;
		entry.family = family;
		entry.subtype = subtype;
		entry.definition_id = definition_id;
		entry.transform_signature = transform_signature;
		entry.last_event_code = 0;
		entry.has_last_event = false;
		m_trigger_templates.push_back(entry);
	}

	void WorldSync::RecordTriggerEvent(void* trigger, int event_code)
	{
		if (!trigger)
			return;

		for (TriggerTemplate& existing : m_trigger_templates)
		{
			if (existing.trigger == trigger)
			{
				existing.last_event_code = event_code;
				existing.has_last_event = true;
				return;
			}
		}

		const retail::TriggerRef trigger_ref = {
			retail::ToAddress(trigger)
		};
		retail::TriggerIdentity identity = {};
		if (!retail::TriggerView(trigger_ref).Identity(identity))
			return;
		RecordTriggerTemplate(trigger, identity.family, identity.subtype);
		for (TriggerTemplate& existing : m_trigger_templates)
		{
			if (existing.trigger == trigger)
			{
				existing.last_event_code = event_code;
				existing.has_last_event = true;
				return;
			}
		}
	}

	bool WorldSync::ReadDebugPlayerPosition(float position[4]) const
	{
		if (!position)
			return false;
		retail::EntitySlotRepository players;
		retail::EntityRef player1 = {};
		retail::Transform transform = {};
		if (!players.Get(retail::EntitySlot::LocalP1, player1) ||
			!retail::EntityView(player1).ReadTransform(transform) ||
			!IsFiniteRetailTransform(transform))
		{
			return false;
		}
		memcpy(position, &transform.position, sizeof(transform.position));
		return true;
	}

	WorldSync::TriggerTemplate* WorldSync::FindNearestDebugTrigger(
		DebugTriggerFilter filter)
	{
		float player_position[4] = {};
		const bool have_player_position = ReadDebugPlayerPosition(player_position);
		TriggerTemplate* nearest = nullptr;
		float nearest_distance_squared = 0.0f;

		for (TriggerTemplate& candidate : m_trigger_templates)
		{
			if (!candidate.trigger)
				continue;

			const retail::TriggerRef trigger_ref = {
				retail::ToAddress(candidate.trigger)
			};
			const retail::TriggerView trigger(trigger_ref);
			retail::TriggerIdentity identity = {};
			if (!trigger.Identity(identity) ||
				identity.family != candidate.family ||
				identity.subtype != candidate.subtype ||
				identity.definition_id != candidate.definition_id)
			{
				continue;
			}

			bool matches = false;
			switch (filter)
			{
			case DebugTriggerFilter::SpawnDefinition:
			{
				std::uint32_t flags = 0;
				matches = trigger.Flags(flags) &&
					(flags & gforce::kTriggerHasSpawnDefinition) != 0;
				break;
			}
			case DebugTriggerFilter::RecordedEvent:
				matches = candidate.has_last_event;
				break;
			case DebugTriggerFilter::KnownInteractive:
				matches = identity.subtype == gforce::kComputerBoxTriggerSubtype &&
					identity.definition_id == gforce::kComputerBoxTriggerDefinition;
				break;
			}
			if (!matches)
				continue;

			if (!have_player_position)
			{
				if (!nearest)
					nearest = &candidate;
				continue;
			}

			retail::Transform transform = {};
			if (!trigger.ReadTransform(transform) ||
				!IsFiniteRetailTransform(transform))
				continue;
			const float dx = transform.position.x - player_position[0];
			const float dy = transform.position.y - player_position[1];
			const float dz = transform.position.z - player_position[2];
			const float distance_squared = dx * dx + dy * dy + dz * dz;
			if (!nearest || distance_squared < nearest_distance_squared)
			{
				nearest = &candidate;
				nearest_distance_squared = distance_squared;
			}
		}
		return nearest;
	}

	bool WorldSync::DebugLogInteractiveCandidates()
	{
		float player_position[4] = {};
		const bool have_player_position = ReadDebugPlayerPosition(player_position);
		std::uint32_t approved_count = 0;
		std::uint32_t observed_count = 0;
		std::uint32_t guess_count = 0;
		std::uint32_t unknown_count = 0;
		std::uint32_t stale_count = 0;

		CoopRuntime::Instance().Log(
			"[debug-F9] trigger catalog begin templates=%u; approved=verified ComputerBox, observed=recorded native event, guess=spawn template, unknown=unclassified\r\n",
			static_cast<unsigned int>(m_trigger_templates.size()));

		for (std::size_t index = 0; index < m_trigger_templates.size(); ++index)
		{
			const TriggerTemplate& candidate = m_trigger_templates[index];
			const retail::TriggerRef trigger_ref = {
				retail::ToAddress(candidate.trigger)
			};
			const retail::TriggerView trigger(trigger_ref);
			retail::TriggerIdentity identity = {};
			if (!candidate.trigger || !trigger.Identity(identity) ||
				identity.family != candidate.family ||
				identity.subtype != candidate.subtype ||
				identity.definition_id != candidate.definition_id)
			{
				++stale_count;
				CoopRuntime::Instance().Log(
					"[interactive-point] index=%u confidence=stale reason=unreadable-or-reused trigger=%p family=%08X subtype=%08X definition=%d\r\n",
					static_cast<unsigned int>(index), candidate.trigger,
					candidate.family, candidate.subtype, candidate.definition_id);
				continue;
			}

			std::uint32_t flags = 0;
			const bool have_flags = trigger.Flags(flags);
			retail::TriggerCounterState counter = {};
			if (trigger.ReadCounterState(counter))
			{
				CoopRuntime::Instance().Log(
					"[world-counter-catalog] trigger=%p value=%u threshold=%d "
					"state=%08X\r\n", candidate.trigger,
					static_cast<unsigned>(counter.value), counter.threshold,
					counter.state_flags);
			}
			retail::Transform transform = {};
			const bool have_transform = trigger.ReadTransform(transform) &&
				IsFiniteRetailTransform(transform);
			const bool is_computer_box =
				identity.subtype == gforce::kComputerBoxTriggerSubtype &&
				identity.definition_id == gforce::kComputerBoxTriggerDefinition;
			const bool has_spawn_definition = have_flags &&
				(flags & gforce::kTriggerHasSpawnDefinition) != 0;
			const char* confidence = "unknown";
			const char* reason = "unclassified";
			if (is_computer_box)
			{
				confidence = "approved";
				reason = "ComputerBox";
				++approved_count;
			}
			else if (candidate.has_last_event)
			{
				confidence = "observed";
				reason = "recorded-native-event";
				++observed_count;
			}
			else if (has_spawn_definition)
			{
				confidence = "guess";
				reason = "spawn-template";
				++guess_count;
			}
			else
			{
				++unknown_count;
			}

			if (have_transform && have_player_position)
			{
				const float dx = transform.position.x - player_position[0];
				const float dy = transform.position.y - player_position[1];
				const float dz = transform.position.z - player_position[2];
				const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
				CoopRuntime::Instance().Log(
					"[interactive-point] index=%u confidence=%s reason=%s trigger=%p family=%08X subtype=%08X definition=%d flags=%08X flags_ok=%u event=%08X event_seen=%u pos=(%.2f,%.2f,%.2f) distance=%.2f\r\n",
					static_cast<unsigned int>(index), confidence, reason,
					candidate.trigger, identity.family, identity.subtype,
					identity.definition_id, flags, have_flags ? 1u : 0u,
					static_cast<unsigned int>(candidate.last_event_code),
					candidate.has_last_event ? 1u : 0u, transform.position.x,
					transform.position.y, transform.position.z, distance);
			}
			else
			{
				CoopRuntime::Instance().Log(
					"[interactive-point] index=%u confidence=%s reason=%s trigger=%p family=%08X subtype=%08X definition=%d flags=%08X flags_ok=%u event=%08X event_seen=%u pos=unavailable distance=unavailable\r\n",
					static_cast<unsigned int>(index), confidence, reason,
					candidate.trigger, identity.family, identity.subtype,
					identity.definition_id, flags, have_flags ? 1u : 0u,
					static_cast<unsigned int>(candidate.last_event_code),
					candidate.has_last_event ? 1u : 0u);
			}
		}

		const std::uint32_t live_count = approved_count + observed_count +
			guess_count + unknown_count;
		CoopRuntime::Instance().Log(
			"[debug-F9] trigger catalog end live=%u approved=%u observed=%u guess=%u unknown=%u stale=%u\r\n",
			live_count, approved_count, observed_count, guess_count, unknown_count,
			stale_count);
		return live_count != 0;
	}

	bool WorldSync::DebugSpawnNearestTrigger()
	{
		TriggerTemplate* selected = FindNearestDebugTrigger(
			DebugTriggerFilter::SpawnDefinition);
		if (!selected)
		{
			CoopRuntime::Instance().Log(
				"[debug-F2] no live spawn-definition trigger is registered\r\n");
			return false;
		}
		const TriggerTemplate target = *selected;
		const bool result = CoopNetGame::Instance().SpawnWorldFromTrigger(
			target.trigger);
		CoopRuntime::Instance().Log(
			"[debug-F2] spawn trigger=%p family=%08X subtype=%08X definition=%d submitted=%u\r\n",
			target.trigger, target.family, target.subtype, target.definition_id,
			result ? 1u : 0u);
		return result;
	}

	bool WorldSync::DebugDispatchNearestRecordedEvent()
	{
		TriggerTemplate* selected = FindNearestDebugTrigger(
			DebugTriggerFilter::RecordedEvent);
		if (!selected)
		{
			CoopRuntime::Instance().Log(
				"[debug-F3] no previously observed native trigger event is registered\r\n");
			return false;
		}
		const TriggerTemplate target = *selected;
		const bool result = CoopNetGame::Instance().DispatchWorldTriggerEvent(
			target.trigger, target.last_event_code);
		CoopRuntime::Instance().Log(
			"[debug-F3] replay trigger=%p family=%08X subtype=%08X definition=%d event=%d submitted=%u\r\n",
			target.trigger, target.family, target.subtype, target.definition_id,
			target.last_event_code, result ? 1u : 0u);
		return result;
	}

	bool WorldSync::DebugActivateNearestKnownInteractive()
	{
		TriggerTemplate* selected = FindNearestDebugTrigger(
			DebugTriggerFilter::KnownInteractive);
		if (!selected)
		{
			CoopRuntime::Instance().Log(
				"[debug-F4] no verified ComputerBox interactive trigger is registered\r\n");
			return false;
		}
		const TriggerTemplate target = *selected;
		const bool result = CoopNetGame::Instance().DispatchWorldTriggerEvent(
			target.trigger, gforce::kComputerBoxActivateEvent);
		CoopRuntime::Instance().Log(
			"[debug-F4] ComputerBox activation trigger=%p definition=%d submitted=%u\r\n",
			target.trigger, target.definition_id, result ? 1u : 0u);
		return result;
	}

	WorldSync::HostEntity* WorldSync::FindHostEntity(void* entity)
	{
		for (HostEntity& existing : m_host_entities)
		{
			if (existing.entity == entity)
				return &existing;
		}
		return nullptr;
	}

	WorldSync::ClientEntity* WorldSync::FindClientEntityById(
		std::uint32_t world_id)
	{
		if (!world_id)
			return nullptr;
		for (ClientEntity& existing : m_client_entities)
		{
			if (existing.world_id == world_id)
				return &existing;
		}
		return nullptr;
	}

	WorldSync::ClientEntity* WorldSync::FindClientEntity(void* entity)
	{
		if (!entity)
			return nullptr;
		for (ClientEntity& existing : m_client_entities)
		{
			if (existing.entity == entity)
				return &existing;
		}
		return nullptr;
	}

	WorldSync::ClientEntity* WorldSync::FindUnlinkedClientEntity(
		const WorldSpawnPacket& packet, const char*& match_kind)
	{
		match_kind = "none";
		ClientEntity* signature_match = nullptr;
		ClientEntity* key_match = nullptr;
		std::uint32_t signature_count = 0;
		std::uint32_t key_count = 0;
		for (ClientEntity& existing : m_client_entities)
		{
			if (existing.world_id != 0)
				continue;
			if (packet.trigger_signature != 0 &&
				existing.key.family == packet.key.family &&
				existing.key.subtype == packet.key.subtype &&
				existing.trigger_signature == packet.trigger_signature)
			{
				signature_match = &existing;
				++signature_count;
			}
			if (SameTriggerKey(existing.key, packet.key))
			{
				key_match = &existing;
				++key_count;
			}
		}
		// A non-zero signature is a map identity, not merely a hint.  Falling
		// through to key/occurrence here would reintroduce the exact failure this
		// path prevents: several MO_Mouse triggers can all be def=0/occ=1.
		if (packet.trigger_signature != 0)
		{
			if (signature_count == 1)
			{
				match_kind = "signature";
				return signature_match;
			}
			return nullptr;
		}
		if (key_count == 1)
		{
			match_kind = "key";
			return key_match;
		}
		return nullptr;
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
		return nullptr;
	}

	WorldSync::TriggerTemplate* WorldSync::FindSpawnTemplate(
		const WorldSpawnPacket& packet, const char*& match_kind)
	{
		match_kind = "none";
		TriggerTemplate* exact = nullptr;
		TriggerTemplate* signature = nullptr;
		TriggerTemplate* definition = nullptr;
		std::uint32_t exact_count = 0;
		std::uint32_t signature_count = 0;
		std::uint32_t definition_count = 0;
		for (TriggerTemplate& candidate : m_trigger_templates)
		{
			if (!candidate.trigger || candidate.family != packet.key.family ||
				candidate.subtype != packet.key.subtype)
			{
				continue;
			}
			// Trigger transforms are not immutable in this retail map.  The object
			// event resolver already proved that a current read matches the host
			// signature, while the cached factory-time value can be stale.  Re-read
			// it here rather than rejecting a valid replica (or trusting stale data).
			const std::uint32_t current_signature =
				TriggerTransformSignature(candidate.trigger);
			if (current_signature != 0)
				candidate.transform_signature = current_signature;
			const bool same_signature = packet.trigger_signature != 0 &&
				current_signature == packet.trigger_signature;
			const bool same_definition =
				candidate.definition_id == packet.key.definition_id;
			if (same_signature && same_definition)
			{
				exact = &candidate;
				++exact_count;
			}
			if (same_signature)
			{
				signature = &candidate;
				++signature_count;
			}
			if (same_definition)
			{
				definition = &candidate;
				++definition_count;
			}
		}
		// When the host could read the static transform, it is the authoritative
		// trigger identity.  Do not silently select a same-definition template if
		// the signature is absent or ambiguous on this process.
		if (packet.trigger_signature != 0)
		{
			if (exact_count == 1)
			{
				match_kind = "definition+signature";
				return exact;
			}
			if (signature_count == 1)
			{
				match_kind = "signature";
				return signature;
			}
			return nullptr;
		}
		if (definition_count == 1)
		{
			match_kind = "unique-definition";
			return definition;
		}
		return nullptr;
	}

	void WorldSync::QueueHostSpawn(HostEntity& entity)
	{
		if (!CoopNetGame::Instance().HasRemotePeer() ||
			!IsFiniteWireTransform(entity.last_position, entity.last_rotation))
			return;

		if (!entity.have_health)
		{
			float health = 0.0f;
			if (ReadEntityHealth(entity.entity, health))
			{
				entity.last_health = health;
				entity.have_health = true;
			}
		}

		WorldSpawnPacket packet = {};
		protocol::InitializeFixedPacket(packet, protocol::PacketKind::WorldSpawn);
		packet.world_id = entity.world_id;
		packet.key = entity.key;
		packet.trigger_signature = entity.trigger_signature;
		memcpy(packet.position, entity.last_position, sizeof(packet.position));
		memcpy(packet.rotation, entity.last_rotation, sizeof(packet.rotation));
		AcquireSRWLockExclusive(&m_packet_lock);
		m_outgoing_spawns.push_back(packet);
		ReleaseSRWLockExclusive(&m_packet_lock);
		entity.announced = true;
		if (entity.have_health)
			ReportLocalDamage(entity.entity, 0);
		CoopRuntime::Instance().Log(
			"[world-id] host id=%u def=%d occ=%u sig=%08X entity=%p\r\n",
			entity.world_id, entity.key.definition_id, entity.key.occurrence,
			entity.trigger_signature,
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
		if (!entity.announced || !IsFiniteWireTransform(position, rotation))
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

	bool WorldSync::BuildObjectEventPacket(void* source,
		std::uint32_t source_vtable, int event_code, std::uint32_t route,
		WorldObjectEventPacket& out) const
	{
		out = {};
		if (!source || !source_vtable ||
			(static_cast<std::uint32_t>(event_code) & 0xFF000000u) !=
				0x41000000u || !IsObjectEventRoute(route))
		{
			return false;
		}

		// A relay source must first prove that it is one of the native map
		// templates the factory saw in this process. This excludes arbitrary
		// pointers carried by the generic relay chain.
		bool is_registered_template = false;
		for (const TriggerTemplate& existing : m_trigger_templates)
		{
			if (existing.trigger == source)
			{
				is_registered_template = true;
				break;
			}
		}
		if (!is_registered_template)
			return false;

		const retail::TriggerRef source_ref = { retail::ToAddress(source) };
		const retail::TriggerView source_view(source_ref);
		retail::TriggerIdentity identity = {};
		retail::Transform transform = {};
		if (!source_view.Identity(identity) ||
			!source_view.ReadTransform(transform) ||
			!IsFiniteRetailTransform(transform) || identity.family == 0 ||
			identity.subtype == 0 || identity.definition_id < 0)
		{
			return false;
		}
		// Relay/forwarder packets are map-object actions. Entity trigger packets
		// deliberately carry only NPC/monster dispatches, which keeps two distinct
		// native mechanisms from being replayed through each other.
		if (IsEntityTriggerRoute(route) != IsSupportedFamily(identity.family))
			return false;

		protocol::InitializeFixedPacket(out,
			protocol::PacketKind::WorldObjectEvent);
		out.source_vtable = source_vtable;
		out.family = identity.family;
		out.subtype = identity.subtype;
		out.definition_id = identity.definition_id;
		out.transform_signature = HashMapObjectTransform(transform);
		out.event_code = event_code;
		out.route = route;
		return true;
	}

	bool WorldSync::QueueObjectEvent(void* source, std::uint32_t source_vtable,
		int event_code, std::uint32_t route)
	{
		if (!CoopNetGame::Instance().HasRemotePeer())
			return false;

		WorldObjectEventPacket packet = {};
		if (!BuildObjectEventPacket(source, source_vtable, event_code, route,
			packet))
			return false;

		bool queued = false;
		AcquireSRWLockExclusive(&m_packet_lock);
		if (m_outgoing_object_events.size() < kMaxPendingWorldPackets)
		{
			packet.sequence = ++m_object_event_sequence;
			if (packet.sequence == 0)
				packet.sequence = ++m_object_event_sequence;
			m_outgoing_object_events.push_back(packet);
			queued = true;
		}
		ReleaseSRWLockExclusive(&m_packet_lock);

		if (queued)
		{
			CoopRuntime::Instance().Log(
				"[world-object] queued seq=%u route=%u vtable=%08X family=%08X subtype=%08X def=%d sig=%08X event=%08X\r\n",
				packet.sequence, packet.route, packet.source_vtable, packet.family, packet.subtype,
				packet.definition_id, packet.transform_signature,
				static_cast<unsigned>(packet.event_code));
		}
		return queued;
	}

	void* WorldSync::FindObjectEventTrigger(
		const WorldObjectEventPacket& packet, const char*& match_kind) const
	{
		match_kind = "none";
		void* exact = nullptr;
		void* signature = nullptr;
		void* definition = nullptr;
		std::uint32_t exact_count = 0;
		std::uint32_t signature_count = 0;
		std::uint32_t definition_count = 0;

		for (const TriggerTemplate& candidate : m_trigger_templates)
		{
			if (!candidate.trigger || candidate.family != packet.family ||
				candidate.subtype != packet.subtype)
			{
				continue;
			}
			retail::Address candidate_vtable = 0;
			if (!retail::TryRead(retail::ToAddress(candidate.trigger),
				candidate_vtable) || candidate_vtable != packet.source_vtable)
			{
				continue;
			}

			retail::Transform transform = {};
			const retail::TriggerRef candidate_ref = {
				retail::ToAddress(candidate.trigger)
			};
			const bool same_signature =
				retail::TriggerView(candidate_ref).ReadTransform(transform) &&
				IsFiniteRetailTransform(transform) &&
				HashMapObjectTransform(transform) == packet.transform_signature;
			const bool same_definition =
				candidate.definition_id == packet.definition_id;
			if (same_definition && same_signature)
			{
				exact = candidate.trigger;
				++exact_count;
			}
			if (same_signature)
			{
				signature = candidate.trigger;
				++signature_count;
			}
			if (same_definition)
			{
				definition = candidate.trigger;
				++definition_count;
			}
		}

		if (exact_count == 1)
		{
			match_kind = "definition+signature";
			return exact;
		}
		if (signature_count == 1)
		{
			match_kind = "signature";
			return signature;
		}
		if (definition_count == 1)
		{
			match_kind = "definition";
			return definition;
		}
		return nullptr;
	}

	void WorldSync::ReplayRemoteObjectEvent(
		const WorldObjectEventPacket& packet)
	{
		const bool entity_request = packet.route ==
			protocol::kWorldObjectEventRouteEntityTriggerRequest;
		const bool entity_activation = packet.route ==
			protocol::kWorldObjectEventRouteEntityTriggerActivation;
		if ((entity_request && !CoopNetGame::Instance().IsHost()) ||
			(entity_activation && !CoopNetGame::Instance().IsClient()))
		{
			CoopRuntime::Instance().Log(
				"[world-object] route ignored seq=%u route=%u role=host:%u client:%u\r\n",
				packet.sequence, packet.route,
				CoopNetGame::Instance().IsHost() ? 1u : 0u,
				CoopNetGame::Instance().IsClient() ? 1u : 0u);
			return;
		}
		const char* match_kind = "none";
		void* const target = FindObjectEventTrigger(packet, match_kind);
		if (!target)
		{
			CoopRuntime::Instance().Log(
				"[world-object] unresolved seq=%u route=%u vtable=%08X family=%08X subtype=%08X def=%d sig=%08X event=%08X\r\n",
				packet.sequence, packet.route, packet.source_vtable, packet.family, packet.subtype,
				packet.definition_id, packet.transform_signature,
				static_cast<unsigned>(packet.event_code));
			return;
		}
		// This target has just passed the stricter object-event match: family,
		// subtype, vtable and its live transform signature.  Keep that exact map
		// trigger available to the later WorldSpawn resolver; factory-time cached
		// transforms can be stale by the time a ventilation chain fires.
		if (entity_request || entity_activation)
			RecordTriggerTemplate(target, packet.family, packet.subtype);
		bool replayed = false;
		if (entity_request)
		{
			replayed = CoopNetGame::Instance().DispatchWorldTriggerEvent(target,
				packet.event_code);
		}
		else if (entity_activation)
		{
			// Do not execute the NPC/monster dispatcher on the client. The host will
			// announce each actual entity through WorldSpawn; that packet drives the
			// one permitted local retail spawn with its final world_id attached.
			CoopRuntime::Instance().Log(
				"[world-entity-trigger] host activation deferred seq=%u target=%p match=%s event=%08X; awaiting WorldSpawn\r\n",
				packet.sequence, target, match_kind,
				static_cast<unsigned>(packet.event_code));
			replayed = true;
		}
		else
		{
			replayed = CoopNetGame::Instance().ReplayObjectEvent(target,
				packet.event_code, packet.route);
		}
		if (!replayed)
		{
			CoopRuntime::Instance().Log(
				"[world-object] native route rejected seq=%u route=%u target=%p match=%s event=%08X\r\n",
				packet.sequence, packet.route, target, match_kind,
				static_cast<unsigned>(packet.event_code));
			return;
		}
		CoopRuntime::Instance().Log(
			"[world-object] applied seq=%u route=%u target=%p match=%s event=%08X\r\n",
			packet.sequence, packet.route, target, match_kind,
			static_cast<unsigned>(packet.event_code));
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

	bool WorldSync::DescribeTrackedHealthComponent(void* component,
		std::uint32_t& world_id, void*& entity) const
	{
		world_id = 0;
		entity = nullptr;
		if (!component)
			return false;

		auto matches_component = [this, component, &world_id, &entity](
			void* candidate, std::uint32_t candidate_world_id)
		{
			if (!candidate || !candidate_world_id || !IsLiveEntity(candidate))
				return false;
			const retail::EntityRef entity_ref = { retail::ToAddress(candidate) };
			retail::HandlerRef handler_ref = {};
			retail::HealthComponentRef health_component = {};
			if (!retail::EntityView(entity_ref).Handler(handler_ref) ||
				!retail::HandlerView(handler_ref).HealthComponent(health_component) ||
				retail::ToPointer(health_component.value) != component)
			{
				return false;
			}
			world_id = candidate_world_id;
			entity = candidate;
			return true;
		};

		if (CoopNetGame::Instance().IsHost())
		{
			for (const HostEntity& tracked : m_host_entities)
			{
				if (matches_component(tracked.entity, tracked.world_id))
					return true;
			}
		}
		else if (CoopNetGame::Instance().IsClient())
		{
			for (const ClientEntity& tracked : m_client_entities)
			{
				if (matches_component(tracked.entity, tracked.world_id))
					return true;
			}
		}
		return false;
	}

	bool WorldSync::ReportLocalDamage(void* entity, int event_code)
	{
		// Each side first resolves its own native hit and then mirrors that exact
		// HP value to the linked replica.  This preserves immediate local combat
		// feedback while the world-id mapping makes the peer update the matching
		// object rather than an arbitrary nearby NPC.
		if (!entity ||
			!CoopNetGame::Instance().HasRemotePeer())
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
		if (queued)
		{
			CoopRuntime::Instance().Log(
				"[world-hp] role=%s id=%u hp=%.2f event=%d\r\n",
				CoopNetGame::Instance().IsHost() ? "host" : "client",
				world_id, health, event_code);
		}
		return queued;
	}

	void* WorldSync::EntityOfTrigger(void* trigger) const
	{
		if (!trigger)
			return nullptr;
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
			const retail::TriggerRef expected = { retail::ToAddress(trigger) };
			for (const ClientEntity& tracked : m_client_entities)
			{
				if (!tracked.entity || !tracked.world_id)
					continue;
				const retail::EntityRef entity_ref = {
					retail::ToAddress(tracked.entity)
				};
				retail::TriggerRef entity_trigger = {};
				if (retail::EntityView(entity_ref).Trigger(entity_trigger) &&
					entity_trigger == expected)
					return tracked.entity;
			}
		}
		return nullptr;
	}

	void WorldSync::RecordNativeSpawn(void* trigger, void* entity,

		std::uint32_t family, std::uint32_t subtype, std::int32_t definition_id)
	{
		if (!trigger || !entity || !IsSupportedFamily(family))
			return;
		const std::uint32_t trigger_signature = TriggerTransformSignature(trigger);
		// Dynamic triggers can carry definition_id == -1.  Their transform still
		// identifies the map template, so retain it for a later host replica.
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
				existing->trigger_signature = trigger_signature;
				existing->announced = false;
				existing->have_transform = false;
				existing->world_id = m_next_world_id++;
				existing->key.family = family;
				existing->key.subtype = subtype;
				existing->key.definition_id = definition_id;
				existing->key.occurrence = NextOccurrence(m_host_trigger_counters,
					trigger);
				existing->have_transform = ReadEntityTransform(entity,
					existing->last_position, existing->last_rotation);
				if (existing->have_transform)
				{
					QueueHostSpawn(*existing);
				}
				else
				{
					// Do not publish zero/stale coordinates if a just-rebound native
					// entity is still between its constructor and first valid transform.
					// EnumerateHostEntities retries the normal reliable spawn next frame.
					CoopRuntime::Instance().Log(
						"[world-sync] deferred spawn id=%u: transform is unavailable\r\n",
						existing->world_id);
				}
				return;
			}

			HostEntity added = {};
			added.entity = entity;
			added.trigger = trigger;
			added.trigger_signature = trigger_signature;
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
		std::uint32_t world_id = 0;
		if (!ClaimForcedClientSpawn(trigger, family, subtype, key,
			world_id))
		{
			key.family = family;
			key.subtype = subtype;
			key.definition_id = definition_id;
			key.occurrence = NextOccurrence(m_client_trigger_counters, trigger);
		}
		AddClientEntity(entity, key, trigger_signature, world_id);
	}

	void WorldSync::AddClientEntity(void* entity, const TriggerKey& key,
		std::uint32_t trigger_signature, std::uint32_t world_id)
	{
		for (ClientEntity& existing : m_client_entities)
		{
			if (existing.entity != entity)
				continue;
			existing.key = key;
			if (trigger_signature)
				existing.trigger_signature = trigger_signature;
			if (world_id)
				existing.world_id = world_id;
			return;
		}
		ClientEntity added = {};
		added.entity = entity;
		added.key = key;
		added.trigger_signature = trigger_signature;
		added.world_id = world_id;
		m_client_entities.push_back(added);
		if (world_id)
		{
			CoopRuntime::Instance().Log(
				"[world-link] client id=%u def=%d occ=%u sig=%08X entity=%p\r\n",
				world_id, key.definition_id, key.occurrence, trigger_signature, entity);
		}
		else
		{
			CoopRuntime::Instance().Log(
				"[world-id] client candidate def=%d occ=%u sig=%08X entity=%p\r\n",
				key.definition_id, key.occurrence, trigger_signature, entity);
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

		const retail::EntityRegistryView registry;
		const bool walked = registry.VisitLiveEntities(
			gforce::kEntityRegistryWalkSafetyLimit,
			[this, send_snapshots](retail::EntityRef entity_ref)
			{
				void* const entity = retail::ToPointer(entity_ref.value);
				retail::TriggerRef trigger_ref = {};
				if (!retail::EntityView(entity_ref).Trigger(trigger_ref))
					return true;
				retail::TriggerIdentity identity = {};
				if (!retail::TriggerView(trigger_ref).Identity(identity) ||
					!IsSupportedFamily(identity.family))
				{
					return true;
				}
				void* const trigger = retail::ToPointer(trigger_ref.value);
				HostEntity* tracked = FindHostEntity(entity);
				if (!tracked || tracked->trigger != trigger)
				{
					RecordNativeSpawn(trigger, entity, identity.family, identity.subtype,
						identity.definition_id);
					tracked = FindHostEntity(entity);
				}
				if (!tracked)
					return true;

				float position[4] = {};
				float rotation[4] = {};
				if (!ReadEntityTransform(entity, position, rotation))
					return true;
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
				return true;
			});
		if (!walked)
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
		const retail::EntityRegistryView registry;
		const bool walked = registry.VisitLiveEntities(
			gforce::kEntityRegistryWalkSafetyLimit,
			[this](retail::EntityRef entity_ref)
			{
				void* const entity = retail::ToPointer(entity_ref.value);
				if (FindClientEntity(entity))
					return true;

				retail::TriggerRef trigger_ref = {};
				if (!retail::EntityView(entity_ref).Trigger(trigger_ref))
					return true;
				retail::TriggerIdentity identity = {};
				if (!retail::TriggerView(trigger_ref).Identity(identity) ||
					!IsSupportedFamily(identity.family))
				{
					return true;
				}
				void* const trigger = retail::ToPointer(trigger_ref.value);
				RecordTriggerTemplate(trigger, identity.family, identity.subtype);
				TriggerKey key = {};
				std::uint32_t world_id = 0;
				if (!ClaimForcedClientSpawn(trigger, identity.family, identity.subtype,
					key, world_id))
				{
					key.family = identity.family;
					key.subtype = identity.subtype;
					key.definition_id = identity.definition_id;
					key.occurrence = NextOccurrence(m_client_trigger_counters, trigger);
				}
				AddClientEntity(entity, key, TriggerTransformSignature(trigger), world_id);
				return true;
			});
		if (!walked)
		{
			CoopRuntime::Instance().Log(
				"[world-sync] client registry read fault; frame skipped\r\n");
		}
	}

	bool WorldSync::TrySpawnClientEntity(PendingSpawn& pending)
	{
		// After connection the host owns NPC/monster creation. The client uses this
		// exact retail spawn boundary only after a received WorldSpawn has named the
		// new replica's world_id.
		if (m_forced_client_spawn_active)
			return false;

		const char* match_kind = "none";
		TriggerTemplate* const template_trigger = FindSpawnTemplate(pending.packet,
			match_kind);
		if (!template_trigger)
		{
			if (!pending.logged_missing_template)
			{
				pending.logged_missing_template = true;
				CoopRuntime::Instance().Log(
					"[world-trigger] client missing id=%u def=%d sig=%08X: no unique local template\r\n",
					pending.packet.world_id, pending.packet.key.definition_id,
					pending.packet.trigger_signature);
			}
			return false;
		}

		m_forced_client_spawn = pending.packet;
		m_forced_client_spawn_active = true;
		m_forced_client_spawn_trigger = template_trigger->trigger;
		m_forced_client_spawn_started_tick = GetTickCount();
		m_forced_client_spawn_native_invoked = false;
		CoopRuntime::Instance().Log(
			"[world-trigger] client invoking native trigger for host id=%u host_def=%d local_def=%d sig=%08X match=%s\r\n",
			pending.packet.world_id, pending.packet.key.definition_id,
			template_trigger->definition_id, pending.packet.trigger_signature,
			match_kind);
		if (!CoopNetGame::Instance().SpawnWorldFromTrigger(template_trigger->trigger))
		{
			ClearForcedClientSpawn();
			CoopRuntime::Instance().Log(
				"[world-trigger] native trigger rejected host id=%u\r\n",
				pending.packet.world_id);
			return false;
		}
		if (m_forced_client_spawn_active)
		{
			CoopRuntime::Instance().Log(
				"[world-trigger] host id=%u awaiting delayed local registry entry\r\n",
				pending.packet.world_id);
		}
		return FindClientEntityById(pending.packet.world_id) != nullptr;
	}

	void WorldSync::ResolvePendingSpawns()
	{
		const DWORD now = GetTickCount();
		if (m_forced_client_spawn_active)
		{
			if (static_cast<DWORD>(now - m_forced_client_spawn_started_tick) <
				kForcedClientSpawnRegistrationTimeoutMs)
			{
				return;
			}
			CoopRuntime::Instance().Log(
				"[world-trigger] host id=%u timed out awaiting local registry entry\r\n",
				m_forced_client_spawn.world_id);
			ClearForcedClientSpawn();
		}
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
			const char* candidate_match = "none";
			ClientEntity* const candidate = FindUnlinkedClientEntity(it->packet,
				candidate_match);
			if (candidate)
			{
				candidate->world_id = it->packet.world_id;
				WorldSnapshotPacket initial = {};
				initial.world_id = it->packet.world_id;
				memcpy(initial.position, it->packet.position, sizeof(initial.position));
				memcpy(initial.rotation, it->packet.rotation, sizeof(initial.rotation));
				AcceptSnapshot(*candidate, initial, now);
				CoopRuntime::Instance().Log(
					"[world-link] host id=%u -> local=%p def=%d occ=%u sig=%08X match=%s\r\n",
					candidate->world_id, candidate->entity, candidate->key.definition_id,
					candidate->key.occurrence, candidate->trigger_signature,
					candidate_match);
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
		if (!IsFiniteWireTransform(snapshot.position, snapshot.rotation))
		{
			CoopRuntime::Instance().Log(
				"[world-sync] rejected non-finite buffered snapshot id=%u seq=%u\r\n",
				snapshot.world_id, snapshot.sequence);
			return;
		}
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
		retail::Transform transform = {};
		memcpy(&transform.position, entity.presentation_position,
			sizeof(entity.presentation_position));
		memcpy(&transform.rotation, entity.presentation_rotation,
			sizeof(entity.presentation_rotation));
		if (!IsFiniteRetailTransform(transform))
		{
			CoopRuntime::Instance().Log(
				"[world-sync] rejected non-finite presentation id=%u\r\n",
				entity.world_id);
			return;
		}
		const retail::EntityRef entity_ref = { retail::ToAddress(entity.entity) };
		if (!retail::EntityView(entity_ref).WriteTransform(transform))
		{
			CoopRuntime::Instance().Log(
				"[world-snapshot] transform write fault for id=%u\r\n",
				entity.world_id);
			return;
		}
		if (!entity.logged_render_apply)
		{
			entity.logged_render_apply = true;
			CoopRuntime::Instance().Log(
				"[world-authority] render-frame host transform active for id=%u local=%p\r\n",
				entity.world_id, entity.entity);
		}
	}

	void WorldSync::ProcessClientPackets()
	{
		std::vector<WorldSpawnPacket> spawns;
		std::vector<WorldSnapshotPacket> snapshots;
		std::vector<WorldTriggerEventPacket> trigger_events;
		std::vector<WorldObjectEventPacket> object_events;
		AcquireSRWLockExclusive(&m_packet_lock);
		spawns.swap(m_incoming_spawns);
		snapshots.swap(m_incoming_snapshots);
		trigger_events.swap(m_incoming_trigger_events);
		object_events.swap(m_incoming_object_events);
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
					CoopRuntime::Instance().Log(
						"[world-spawn] client received id=%u family=%08X subtype=%08X def=%d occ=%u sig=%08X\r\n",
						packet.world_id, packet.key.family, packet.key.subtype,
						packet.key.definition_id, packet.key.occurrence,
						packet.trigger_signature);
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
		for (const WorldObjectEventPacket& packet : object_events)
			ReplayRemoteObjectEvent(packet);

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
					// Preserve the local native result and mirror it to the linked peer.
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
					// Keep the client's native local death/hit result, then mirror it
					// to the matching host replica through the established world-id.
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
			void* entity = nullptr;
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
				if (m_pending_damage.size() < kMaxPendingWorldPackets)
				{
					PendingDamage pending = {};
					pending.world_id = packet.world_id;
					pending.hp_bits = packet.hp_bits;
					pending.event_code = packet.event_code;
					m_pending_damage.push_back(pending);
				}
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
			void* entity = nullptr;
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
		if (!view.CopyUncompressedExact(packet))
		{
			PacketHeader header = {};
			if (view.ReadHeader(header))
			{
				CoopRuntime::Instance().Log(
					"[world-sync] rejected WorldSpawn wire size=%u header=%u expected=%u; peer DLL mismatch\r\n",
					view.size(), header.Size(), static_cast<unsigned>(sizeof(packet)));
			}
			return true;
		}
		if (!CoopNetGame::Instance().IsClient() || CoopNetGame::Instance().IsHost())
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
		if (!IsFiniteWireTransform(packet.position, packet.rotation))
		{
			CoopRuntime::Instance().Log(
				"[world-sync] rejected non-finite spawn transform id=%u\r\n",
				packet.world_id);
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
		// An old peer can still send a generic NPC/monster dispatcher packet. Do
		// not replay it: the client already has its stock spawn route and a second
		// dispatcher call can create a duplicate live entity.
		if (IsSupportedFamily(packet.key.family))
		{
			CoopRuntime::Instance().Log(
				"[world-trigger-event] entity dispatcher replay suppressed family=%08X subtype=%08X def=%d event=%d\r\n",
				packet.key.family, packet.key.subtype, packet.key.definition_id,
				packet.event_code);
			return true;
		}

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

	bool WorldSync::HandleWorldObjectEventPacket(
		const protocol::PacketView& view)
	{
		WorldObjectEventPacket packet = {};
		if (!view.CopyUncompressedExact(packet) || packet.sequence == 0 ||
			packet.source_vtable == 0 || packet.family == 0 ||
			packet.subtype == 0 || packet.definition_id < 0 ||
			packet.transform_signature == 0 ||
			(static_cast<std::uint32_t>(packet.event_code) & 0xFF000000u) !=
				0x41000000u || !IsObjectEventRoute(packet.route) ||
			(IsEntityTriggerRoute(packet.route) != IsSupportedFamily(packet.family)))
		{
			return true;
		}
		if ((packet.route == protocol::kWorldObjectEventRouteEntityTriggerRequest &&
				!CoopNetGame::Instance().IsHost()) ||
			(packet.route == protocol::kWorldObjectEventRouteEntityTriggerActivation &&
				!CoopNetGame::Instance().IsClient()))
		{
			return true;
		}

		bool accepted = false;
		AcquireSRWLockExclusive(&m_packet_lock);
		if (IsStrictlyNewerSequence(packet.sequence,
			m_last_received_object_event_sequence) &&
			m_incoming_object_events.size() < kMaxPendingWorldPackets)
		{
			m_incoming_object_events.push_back(packet);
			m_last_received_object_event_sequence = packet.sequence;
			accepted = true;
		}
		ReleaseSRWLockExclusive(&m_packet_lock);

		if (accepted)
		{
			CoopRuntime::Instance().Log(
				"[world-object] peer received seq=%u route=%u vtable=%08X family=%08X subtype=%08X def=%d sig=%08X event=%08X\r\n",
				packet.sequence, packet.route, packet.source_vtable, packet.family, packet.subtype,
				packet.definition_id, packet.transform_signature,
				static_cast<unsigned>(packet.event_code));
		}
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
		if (!IsFiniteWireTransform(packet.position, packet.rotation))
		{
			CoopRuntime::Instance().Log(
				"[world-sync] rejected non-finite snapshot id=%u seq=%u\r\n",
				packet.world_id, packet.sequence);
			return true;
		}

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
		case protocol::PacketKind::WorldObjectEvent:
			return HandleWorldObjectEventPacket(view);
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
		std::vector<WorldObjectEventPacket> object_events;
		AcquireSRWLockExclusive(&m_packet_lock);
		spawns.swap(m_outgoing_spawns);
		snapshots.swap(m_outgoing_snapshots);
		trigger_events.swap(m_outgoing_trigger_events);
		object_events.swap(m_outgoing_object_events);

		ReleaseSRWLockExclusive(&m_packet_lock);

		for (const WorldSpawnPacket& packet : spawns)
			SendToRemote(&packet, sizeof(packet), k_nSteamNetworkingSend_Reliable);
		for (const WorldSnapshotPacket& packet : snapshots)
			SendToRemote(&packet, sizeof(packet), k_nSteamNetworkingSend_Unreliable);
		for (const WorldTriggerEventPacket& packet : trigger_events)
			SendToRemote(&packet, sizeof(packet), k_nSteamNetworkingSend_Reliable);
		for (const WorldObjectEventPacket& packet : object_events)
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
