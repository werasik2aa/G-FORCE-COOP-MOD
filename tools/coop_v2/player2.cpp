#include "player2.h"

#include "coop_netgame.h"
#include "coop_runtime.h"
#include "debug_actions.h"
#include "gforce_constants.h"
#include "retail/retail_types.h"
#include "retail/retail_views.h"
#include "save_sync.h"
#include "world_sync.h"
#include "ServerClient/SteamManager.h"

#include <string.h>

namespace coop
{
	using namespace gforce;

	namespace
	{
		// Both the remote Darwin and the remote-owned shared Fly must consume one
		// coherent packet snapshot while their native controller is ticking.  The
		// scope also restores DirectInput before any local controller can run.
		class RemoteSnapshotInputScope final
		{
		public:
			explicit RemoteSnapshotInputScope(CoopNetGame& netgame) : netgame_(netgame)
			{
				netgame_.BeginRemoteInput();
			}

			~RemoteSnapshotInputScope()
			{
				netgame_.EndRemoteInput();
			}

		private:
			RemoteSnapshotInputScope(const RemoteSnapshotInputScope&);
			RemoteSnapshotInputScope& operator=(const RemoteSnapshotInputScope&);

			CoopNetGame& netgame_;
		};

		class PrimaryGamePadScope final
		{

		public:
			explicit PrimaryGamePadScope(CoopNetGame& netgame, bool activate) :
				netgame_(netgame), original_(nullptr), active_(false)
			{
				if (activate)
					active_ = netgame_.BeginRemoteGamePadScope(original_);
			}

			~PrimaryGamePadScope()
			{
				if (active_)
					netgame_.EndRemoteGamePadScope(original_);
			}

		private:
			PrimaryGamePadScope(const PrimaryGamePadScope&);
			PrimaryGamePadScope& operator=(const PrimaryGamePadScope&);

			CoopNetGame& netgame_;
			void* original_;
			bool active_;
		};

		bool IsNewerLiveSnapshotSequence(std::uint32_t candidate,
			std::uint32_t baseline)
		{
			// `transform_sequence` is nonzero for every published P1 transform
			// and uses the same signed-difference wrap rule as packet acceptance.
			return candidate != 0 && (baseline == 0 ||
				static_cast<std::int32_t>(candidate - baseline) > 0);
		}

	}

	Player2Module& Player2Module::Instance()
	{
		static Player2Module instance;
		return instance;
	}

	Player2Module::Player2Module() :
		m_player2_ready(0),
		m_spawn_snapshot_ready(0),
		m_spawn_in_progress(0),
		m_player2_default_mode_initialized(false),
		m_player2_default_mode_setup_failure_logged(false),
		m_logged_blocked_active_publish(false),
		m_debug_player2_enabled(false),
		m_remote_p2_death_mode_observed(false),
		m_remote_p2_death_mode_entry_sequence(0),

		m_last_player1_mode(0),
		m_abr_native_task_configured_player2(),

		m_last_weapon_type(0xFFFFFFFFu),
		m_spawn_context(),
		m_default_mode_active_stores_patched(false),
		m_original_update(
			reinterpret_cast<ControllerUpdateFn>(kOriginalControllerUpdate))

	{
		ZeroMemory(&m_spawn_position, sizeof(m_spawn_position));
		ZeroMemory(&m_spawn_rotation, sizeof(m_spawn_rotation));
		ZeroMemory(m_original_spawn_call1, sizeof(m_original_spawn_call1));
		ZeroMemory(m_original_spawn_call2, sizeof(m_original_spawn_call2));
		ZeroMemory(m_original_default_mode_active_stores,
			sizeof(m_original_default_mode_active_stores));

	}

	void* Player2Module::GetFlyEntity()
	{
		retail::EntitySlotRepository players;
		retail::EntityRef fly = {};
		return players.Get(retail::EntitySlot::Mooch, fly) ?
			retail::ToPointer(fly.value) : nullptr;
	}

	void Player2Module::PublishDefaultModeActiveEntity(void* entity)
	{
		const retail::EntityRef published = { retail::ToAddress(entity) };
		retail::EntitySlotRepository players;
		retail::EntityRef player2 = {};
		if (players.Get(retail::EntitySlot::RemoteP2, player2) && published == player2)
		{
			if (!m_logged_blocked_active_publish)
			{
				retail::EntityRef active_a = {};
				retail::EntityRef active_b = {};
				retail::ActiveEntityStore().Read(active_a, active_b);
				CoopRuntime::Instance().Log(
					"[active-guard] blocked Default-mode publish for P2 entity=%p; active remains (%p,%p)\r\n",
					entity, retail::ToPointer(active_a.value),
					retail::ToPointer(active_b.value));
				m_logged_blocked_active_publish = true;
			}
			return;
		}

		retail::ActiveEntityStore().Set(published);
	}

	extern "C" void __cdecl Player2PublishDefaultModeActiveEntity(void* entity)
	{
		Player2Module::Instance().PublishDefaultModeActiveEntity(entity);
	}

	extern "C" __declspec(naked) void HookDefaultModeActiveStores()
	{
		__asm
		{
			pushfd
			pushad
			push eax
			call Player2PublishDefaultModeActiveEntity
			add esp, 4
			popad
			popfd
			ret
		}
	}

	void* Player2Module::GetController(void* entity)
	{
		if (!entity)
			return nullptr;

		const retail::EntityRef entity_ref = { retail::ToAddress(entity) };
		retail::HandlerRef handler = {};
		retail::ControllerRef controller = {};
		return retail::EntityView(entity_ref).Handler(handler) &&
			retail::HandlerView(handler).Controller(controller) ?
			retail::ToPointer(controller.value) : nullptr;
	}

	std::uint32_t Player2Module::GetModeId(void* controller)
	{
		if (!controller)
			return 0;

		const retail::ControllerRef controller_ref = {
			retail::ToAddress(controller)
		};
		retail::ModeId mode_id = 0;
		return retail::ControllerView(controller_ref).CurrentMode(mode_id) ?
			mode_id : 0;
	}

	bool Player2Module::IsGPigDeathMode(void* controller) const
	{
		if (!controller)
			return false;
		const retail::ControllerRef controller_ref = {
			retail::ToAddress(controller)
		};
		retail::ModeRef mode = {};
		retail::Address vtable = 0;
		if (!retail::ControllerView(controller_ref).Mode(mode) ||
			!retail::ModeView(mode).VTable(vtable))
		{
			return false;
		}
		return vtable == kGPigDeathModeVtable ||
			vtable == kGPigDeathModeInactiveVtable ||
			vtable == kGPigDeathModeActiveVtable ||
			vtable == kGPigDeathModeFallVtable ||
			vtable == kGPigDeathModeDeathVtable ||
			vtable == kGPigDeathModeRespawnVtable;
	}

	bool Player2Module::TryRecoverRemoteP2Death(void* controller)
	{
		CoopNetGame& netgame = CoopNetGame::Instance();
		std::uint32_t remote_transform_sequence = 0;
		std::uint32_t remote_player_mode = 0;
		// P2 is a presentation of the peer, not an independently respawning
		// single-player actor. Do not revive it from a stale packet or while the
		// owner itself is outside ordinary Darwin Default mode.
		if (!netgame.GetRemotePlayerModeSnapshot(remote_transform_sequence,
			remote_player_mode) || remote_transform_sequence == 0)
		{
			return false;
		}
		if (m_remote_p2_death_mode_entry_sequence == 0)
		{
			// Do not escape a local DeathMode from the packet that happened to be
			// cached when it was entered. Require one later owner snapshot.
			m_remote_p2_death_mode_entry_sequence = remote_transform_sequence;
			return false;
		}
		if (!IsNewerLiveSnapshotSequence(remote_transform_sequence,
			m_remote_p2_death_mode_entry_sequence) ||
			remote_player_mode != kDefaultModeId)
		{
			return false;
		}

		const retail::ControllerRef controller_ref = {
			retail::ToAddress(controller)
		};
		if (!retail::ControllerView(controller_ref).SelectMode(kDefaultModeId) ||
			GetModeId(controller) != kDefaultModeId)
		{
			return false;
		}

		// The new per-controller Default object needs the same conflict-mask setup
		// as a freshly spawned P2 before its next remote tick.
		m_player2_default_mode_initialized = false;
		m_player2_default_mode_setup_failure_logged = false;
		CoopRuntime::Instance().Log(
			"[p2-death-recovery] exited native DeathMode via newer peer Default snapshot seq=%u entry_seq=%u\r\n",
			remote_transform_sequence, m_remote_p2_death_mode_entry_sequence);
		return true;
	}

	bool Player2Module::SyncPlayer2WeaponSelection(void* player2)

	{
		if (!player2)
			return false;

		retail::EntitySlotRepository players;
		retail::EntityRef player1 = {};
		retail::HandlerRef player1_handler = {};
		std::uint32_t player1_weapon_type = 0xFFFFFFFFu;
		if (!players.Get(retail::EntitySlot::LocalP1, player1) ||
			!retail::EntityView(player1).Handler(player1_handler) ||
			!retail::HandlerView(player1_handler).SelectedWeaponType(
				player1_weapon_type))
		{
			return false;
		}
		return ApplyPlayer2WeaponSelection(player2, player1_weapon_type, "local P1");
	}

	bool Player2Module::ApplyPlayer2WeaponSelection(void* player2,
		std::uint32_t weapon_type, const char* source)
	{
		if (!player2 || weapon_type == 0xFFFFFFFFu)
			return false;

		const retail::EntityRef player2_ref = { retail::ToAddress(player2) };
		retail::HandlerRef player2_handler = {};
		std::uint32_t player2_weapon_type = 0xFFFFFFFFu;
		if (!retail::EntityView(player2_ref).Handler(player2_handler) ||
			!retail::HandlerView(player2_handler).SelectedWeaponType(player2_weapon_type))
		{
			return false;
		}

		// The handler temporarily reports melee/holster while a draw, jump or attack
		// animation is in progress.  A source sequence, not a transient local handler
		// value, is the authority for a real player selection.
		if (m_last_weapon_type == weapon_type)
			return false;

		m_last_weapon_type = weapon_type;
		std::uint32_t item_id = 0xFFFFFFFFu;
		if (!retail::NativeGameApi::WeaponTypeToItemId(weapon_type, item_id))
		{
			CoopRuntime::Instance().Log(
				"[weapon-selection] native type-to-item call failed\r\n");
			return false;
		}
		if (item_id == kDefaultMeleeItemId && weapon_type != 0x40050001u)
			return false;
		if (!retail::NativeGameApi::SetSelectedWeaponType(player2_handler,
			weapon_type))
		{
			CoopRuntime::Instance().Log(
				"[weapon-selection] native P2 selection call failed\r\n");
			return false;
		}
		std::uint32_t player2_current = 0xFFFFFFFFu;
		if (!retail::NativeGameApi::CurrentWeaponId(player2_handler,
			player2_current))
		{
			CoopRuntime::Instance().Log(
				"[weapon-selection] native P2 current-item call failed\r\n");
			return false;
		}
		CoopRuntime::Instance().Log("[weapon-selection] source=%s type=0x%08X item=0x%08X P2.old_type=0x%08X P2.current_before=0x%08X action=%s\r\n",
			source ? source : "unknown", weapon_type, item_id,
			player2_weapon_type, player2_current,
			item_id == kDefaultMeleeItemId ? "holster" : "select");
		return true;
	}

	bool Player2Module::EnsureNetworkPlayer2()
	{
		retail::EntitySlotRepository players;
		retail::EntitySlotBinding player2 = {};
		if (players.GetBinding(retail::EntitySlot::RemoteP2, player2))
		{
			InterlockedExchange(&m_player2_ready, 1);
			return true;
		}
		if (InterlockedCompareExchange(&m_spawn_snapshot_ready, 0, 0) == 0 ||
			!m_spawn_context)
			return false;
		retail::EntitySlotBinding player1 = {};
		if (!players.GetBinding(retail::EntitySlot::LocalP1, player1))
			return false;

		// Network creation runs from GameTick only after P1's own stock update.  The
		// original Default-only gate strands a peer that connects after the ABR
		// cutscene, although the proven native factory path is safe with P1 already in
		// RDV. Allow precisely those two lifecycle modes, not arbitrary transitions.
		const std::uint32_t player1_mode = GetModeId(
			retail::ToPointer(player1.controller.value));
		if (player1_mode != kDefaultModeId && player1_mode != kAbrModeId)
			return false;
		return SpawnPlayer2FromSnapshot("network", false) &&
			InterlockedCompareExchange(&m_player2_ready, 0, 0) != 0;
	}

	bool Player2Module::SpawnPlayer2FromSnapshot(const char* trigger,
		bool allow_when_coop_disabled)
	{
		if (!trigger)
			trigger = "unknown";
		if (InterlockedCompareExchange(&m_spawn_in_progress, 1, 0) != 0)
			return false;
		if (!allow_when_coop_disabled && !CoopRuntime::Instance().Config().enabled)
		{
			CoopRuntime::Instance().Log("[spawn-%s] co-op is disabled in coop.ini\r\n",
				trigger);
			InterlockedExchange(&m_spawn_in_progress, 0);
			return false;
		}
		retail::EntitySlotRepository players;
		const retail::EntityRef existing_player2_ref = players.GetSelectable(
			retail::EntitySlot::RemoteP2);
		void* const existing_player2 =
			retail::ToPointer(existing_player2_ref.value);
		if (existing_player2)
		{
			CoopRuntime::Instance().Log("[spawn-%s] player 2 already exists\r\n",
				trigger);
			InterlockedExchange(&m_spawn_in_progress, 0);
			return GetController(existing_player2) != nullptr;
		}
		if (InterlockedCompareExchange(&m_spawn_snapshot_ready, 0, 0) == 0 ||
			!m_spawn_context)
		{
			CoopRuntime::Instance().Log(
				"[spawn-%s] no stock player spawn snapshot yet\r\n", trigger);
			InterlockedExchange(&m_spawn_in_progress, 0);
			return false;
		}

		const retail::EntityRef player1_ref = players.GetSelectable(
			retail::EntitySlot::LocalP1);
		void* player1 = retail::ToPointer(player1_ref.value);
		Vec4 player2_position = m_spawn_position;
		Vec4 player2_rotation = m_spawn_rotation;
		bool used_live_transform = false;
		if (player1)
		{
			retail::Transform player1_transform = {};
			if (retail::EntityView(player1_ref).ReadTransform(player1_transform))
			{
				memcpy(&player2_position, &player1_transform.position,
					sizeof(player2_position));
				memcpy(&player2_rotation, &player1_transform.rotation,
					sizeof(player2_rotation));
				// Keep the pigs out of each other during cutscenes and shared spawns.
				player2_position.x += 0.5f;
				used_live_transform = true;
			}
		}

		retail::EntityRef player2_ref = {};
		CoopRuntime::Instance().Log("[spawn-%s] creating P2 id=0x%08X context=%p transform=%s position=(%.3f, %.3f, %.3f)\r\n",
			trigger, kGPig2Id, retail::ToPointer(m_spawn_context.value),
			used_live_transform ? "live-P1" : "stock-snapshot",
			player2_position.x, player2_position.y, player2_position.z);
		if (!retail::NativeGameApi::SpawnGPig(&player2_position,
			&player2_rotation, kGPig2Id, m_spawn_context, player2_ref))
		{
			CoopRuntime::Instance().Log(
				"[spawn-%s] native P2 factory fault\r\n", trigger);
		}

		retail::HandlerRef player2_handler_ref = {};
		if (player2_ref)
		{
			retail::EntityView(player2_ref).Handler(player2_handler_ref);
		}
		void* const player2 = retail::ToPointer(player2_ref.value);
		void* const player2_handler = retail::ToPointer(player2_handler_ref.value);
		void* player2_controller = GetController(player2);
		if (player2 && player2_controller)
		{
			m_last_weapon_type = 0xFFFFFFFFu;

			m_player2_default_mode_initialized = false;
			m_player2_default_mode_setup_failure_logged = false;
			m_abr_native_task_configured_player2 = {};

			m_logged_blocked_active_publish = false;

			InterlockedExchange(&m_player2_ready, 1);

		}
		CoopRuntime::Instance().Log("[spawn-%s-result] P1=%p P2=%p P2.handler=%p P2.controller=%p ready=%ld\r\n",
			trigger, player1, player2, player2_handler, player2_controller,
			m_player2_ready);

		InterlockedExchange(&m_spawn_in_progress, 0);
		return player2 && player2_controller;
	}

	bool Player2Module::TryEnsurePlayer2RdvTask(const char* source)
	{
		if (!source)
			source = "unknown";

		CoopNetGame& netgame = CoopNetGame::Instance();
		if (!netgame.HasRemotePeer())
		{
			CoopRuntime::Instance().Log(
				"[abr-task] source=%s ignored: this native task experiment is network-only\r\n",
				source);
			return false;
		}

		retail::EntitySlotRepository players;
		const retail::EntityRef player1 = players.GetSelectable(
			retail::EntitySlot::LocalP1);
		const retail::EntityRef player2 = players.GetSelectable(
			retail::EntitySlot::RemoteP2);
		if (!player1 || !player2 ||
			GetModeId(GetController(retail::ToPointer(player1.value))) != kAbrModeId)
			return false;

		// The factory may succeed before the native spawn-context configurator is
		// ready.  Only its postcondition (`task+0x30 == 1`) is a valid cache hit;
		// otherwise the next controller tick must retry the stock configurator.
		if (m_abr_native_task_configured_player2 == player2)
			return true;

		retail::HandlerRef player2_handler = {};
		retail::MotorSystemRef motor_system = {};
		retail::Address state_table = 0;
		retail::Address resource_table = 0;
		retail::MotorResourceRef rdv_resource = {};
		retail::MotorTaskRef existing_task = {};
		std::uint32_t resource_count = 0;
		std::uint32_t state_index = 0;
		retail::Address resource_vtable = 0;
		retail::Address task_vtable = 0;
		bool contract_valid = retail::EntityView(player2).Handler(player2_handler) &&
			retail::HandlerView(player2_handler).MotorSystem(motor_system);
		if (contract_valid)
		{
			retail::MotorSystemView motor_system_view(motor_system);
			contract_valid = motor_system_view.TaskStateTable(state_table) &&
				motor_system_view.ResourceCount(resource_count) &&
				motor_system_view.ResourceTable(resource_table) &&
				retail::ReadGPigRdvTaskStateIndex(state_index) &&
				state_index < kMotorSystemTaskStateSafetyLimit &&
				resource_count > kGPigRdvMotorResourceIndex &&
				motor_system_view.ResourceAt(kGPigRdvMotorResourceIndex,
					rdv_resource) &&
				retail::MotorResourceView(rdv_resource).VTable(resource_vtable) &&
				resource_vtable == kGPigRdvMotorFunctionVtable &&
				motor_system_view.TaskAt(state_index, existing_task);
			if (contract_valid && existing_task)
			{
				contract_valid = retail::MotorTaskView(existing_task).VTable(task_vtable) &&
					task_vtable == kGPigRdvTaskVtable;
			}
		}
		if (!contract_valid)
		{
			CoopRuntime::Instance().Log(
				"[abr-task] source=%s rejected: task factory contract unavailable P2=%p handler=%p state=%p index=%u resources=%p count=%u existing=%p\r\n",
				source, retail::ToPointer(player2.value),
				retail::ToPointer(player2_handler.value),
				retail::ToPointer(state_table), state_index,
				retail::ToPointer(resource_table), resource_count,
				retail::ToPointer(existing_task.value));
			return false;
		}
		if (existing_task)
		{
			CoopRuntime::Instance().Log(
				"[abr-task] source=%s P2=%p already has native XMotorTask_RDV=%p; no factory call made\r\n",
				source, retail::ToPointer(player2.value),
				retail::ToPointer(existing_task.value));
			return ConfigurePlayer2RdvTask(source, player2, player2_handler,
				existing_task);
		}

		// This is the engine's own lazy factory: it allocates, constructs and inserts
		// XMotorTask_RDV in handler+0x4EC. The test intentionally does not select RDV
		// mode and does not configure task fields; it only proves whether this missing
		// stock object is sufficient for the next controller-mode experiment.
		CoopRuntime::Instance().Log(
			"[abr-task] source=%s native factory begin P2=%p handler=%p motor=%p state=%p index=%u\r\n",
			source, retail::ToPointer(player2.value),
			retail::ToPointer(player2_handler.value),
			retail::ToPointer(motor_system.value),
			retail::ToPointer(state_table), state_index);
		retail::MotorTaskRef created_task = {};
		if (!retail::NativeGameApi::EnsureGPigRdvTask(motor_system, true,
			created_task))
		{
			CoopRuntime::Instance().Log(
				"[abr-task] source=%s native factory fault\r\n", source);
			return false;
		}

		retail::MotorTaskRef table_task = {};
		task_vtable = 0;
		const bool creation_valid = created_task &&
			retail::MotorSystemView(motor_system).TaskAt(state_index, table_task) &&
			created_task == table_task &&
			retail::MotorTaskView(created_task).VTable(task_vtable) &&
			task_vtable == kGPigRdvTaskVtable;
		if (!creation_valid)
		{
			CoopRuntime::Instance().Log(
				"[abr-task] source=%s native factory postcondition failed P2=%p task=%p vtbl=%p\r\n",
				source, retail::ToPointer(player2.value),
				retail::ToPointer(created_task.value),
				retail::ToPointer(task_vtable));
			return false;
		}

		CoopRuntime::Instance().Log(
			"[abr-task] source=%s native factory complete P2=%p XMotorTask_RDV=%p vtbl=%p\r\n",
			source, retail::ToPointer(player2.value),
			retail::ToPointer(created_task.value),
			retail::ToPointer(task_vtable));

		return ConfigurePlayer2RdvTask(source, player2, player2_handler,
			created_task);
	}

	bool Player2Module::ConfigurePlayer2RdvTask(const char* source,
		retail::EntityRef player2, retail::HandlerRef player2_handler,
		retail::MotorTaskRef task)
	{
		if (!source)
			source = "unknown";
		if (m_abr_native_task_configured_player2 == player2)
			return true;

		const retail::SpawnContextRef context = m_spawn_context;
		retail::EntitySlotRepository players;
		const retail::EntityRef player1 = players.GetSelectable(
			retail::EntitySlot::LocalP1);
		retail::EntityRef context_entity = {};
		retail::EntityRef active_a = {};
		retail::EntityRef active_b = {};
		std::uint32_t context_flags = 0;
		retail::Address task_vtable = 0;
		const bool contract_valid = context && player1 && player2 &&
			player2_handler && task &&
			retail::SpawnContextView(context).Flags(context_flags) &&
			retail::SpawnContextView(context).ActiveEntity(context_entity) &&
			retail::ActiveEntityStore().Read(active_a, active_b) &&
			context_entity == player1 && active_a == player1 && active_b == player1 &&
			(context_flags & kGPigSpawnContextRdvFlag) != 0 &&
			retail::MotorTaskView(task).VTable(task_vtable) &&
			task_vtable == kGPigRdvTaskVtable;
		if (!contract_valid)
		{
			CoopRuntime::Instance().Log(
				"[abr-task-config] source=%s rejected P1=%p P2=%p handler=%p task=%p context=%p flags=0x%08X context_entity=%p active=(%p,%p)\r\n",
				source, retail::ToPointer(player1.value),
				retail::ToPointer(player2.value),
				retail::ToPointer(player2_handler.value),
				retail::ToPointer(task.value), retail::ToPointer(context.value),
				context_flags, retail::ToPointer(context_entity.value),
				retail::ToPointer(active_a.value), retail::ToPointer(active_b.value));
			return false;
		}

		CoopRuntime::Instance().Log(
			"[abr-task-config] source=%s native begin P2=%p task=%p context=%p flags=0x%08X\r\n",
			source, retail::ToPointer(player2.value),
			retail::ToPointer(task.value), retail::ToPointer(context.value),
			context_flags);
		if (!retail::NativeGameApi::ConfigureGPigRdvTask(context,
			player2_handler))
		{
			CoopRuntime::Instance().Log(
				"[abr-task-config] source=%s native configurator fault\r\n", source);
			return false;
		}

		uint8_t enabled = 0;
		task_vtable = 0;
		const bool configured = retail::MotorTaskView(task).VTable(task_vtable) &&
			task_vtable == kGPigRdvTaskVtable &&
			retail::MotorTaskView(task).RdvEnabled(enabled) &&
			enabled == kGPigRdvTaskEnabledValue;
		if (!configured)
		{
			CoopRuntime::Instance().Log(
				"[abr-task-config] source=%s postcondition failed P2=%p task=%p enabled=%u\r\n",
				source, retail::ToPointer(player2.value),
				retail::ToPointer(task.value), static_cast<unsigned>(enabled));
			return false;
		}

		m_abr_native_task_configured_player2 = player2;
		CoopRuntime::Instance().Log(
			"[abr-task-config] source=%s native configured P2=%p task=%p enabled=%u\r\n",
			source, retail::ToPointer(player2.value),
			retail::ToPointer(task.value), static_cast<unsigned>(enabled));
		return true;
	}

	void __fastcall Player2Module::HookControllerUpdate(
		void* controller, void*)
	{
		Instance().UpdateController(controller);
	}

	void Player2Module::TickPlayer1(void* player1_controller)

	{
		// A remote host load is initiated from this guaranteed game-thread hook.
		// The old controller may be destroyed by the loader, so do not touch it or
		// continue the stock update after a load was started.
		if (SaveSync::Instance().OnMainFrame())
			return;

		// The first real P1 controller tick cannot happen in the main menu.  It is
		// therefore the safe boundary for turning a normal loaded save into an IP
		// host, without advertising or probing Steam during startup.
		SaveSync::Instance().CaptureLoadedHostSlot();
		WorldSync::Instance().NotifyLocalWorldReady();
		if (SteamManager)
			SteamManager->NotifyGameWorldReady();

		// The single shared GPig camera belongs to whoever the player is actually
		// driving.  Its mode goes back to Default immediately after the native Mooch
		// hand-off, so use the confirmed network owner state rather than a mode or
		// process-global pointer that lasts only for that transition frame.
		CoopNetGame& netgame = CoopNetGame::Instance();
		if (!netgame.IsLocalFlyControlled() &&
			GetModeId(player1_controller) == kDefaultModeId)
			m_camera.RefreshForController(player1_controller);
		netgame.BeginLocalInputCapture();
		if (!RunStockControllerUpdate(player1_controller, "local-player1"))
			return;
		netgame.PublishLocalPlayerMode(
			GetModeId(player1_controller));
		retail::EntitySlotRepository players;
		const retail::EntityRef player1 = players.GetSelectable(
			retail::EntitySlot::LocalP1);
		netgame.PublishLocalPlayerTransform(retail::ToPointer(player1.value));
		// During local Mooch control P1's Default update runs before Mooch's own
		// controller, so its shared-camera value is stale.  Leave the last yaw in
		// place; UpdateFlyController publishes the current Mooch yaw after its
		// native tick, and that is what the next network send must carry.
		if (!netgame.IsLocalFlyControlled())
		{
			float local_camera_yaw = 0.0f;
			netgame.PublishLocalCameraYaw(local_camera_yaw,
				m_camera.ReadLocalYaw(local_camera_yaw));
		}

		// Network spawning must happen after P1's normal update.  The factory then
		// receives the same settled transform/physics context as the proven-safe
		// earlier native-factory path. GameTick itself returns immediately once P2 exists.
		netgame.GameTick();
		DebugActions::Instance().Tick();

		HandlePlayer1ModeTransition(player1_controller);

		// A peer-owned Mooch is not guaranteed to receive a native controller tick
		// while the local player stays in ordinary Darwin mode. Its root and Aim
		// motor target must therefore be presented from this guaranteed game-thread
		// seam as well. If Mooch receives its own later Idle tick, UpdateFlyController
		// reapplies the same target after that tick clears its transient state.
		if (!netgame.IsLocalFlyControlled())
		{
			void* const fly = GetFlyEntity();
			netgame.ApplyRemoteFlyTransform(fly);
			netgame.ApplyRemoteFlyAimMotor(fly);
		}

	}

	void Player2Module::HandlePlayer1ModeTransition(void* player1_controller)
	{
		const std::uint32_t mode_now = GetModeId(player1_controller);
		if (mode_now == m_last_player1_mode)
			return;

		if (mode_now == kMoochSwitchModeId)
		{
			CoopNetGame::Instance().ConfirmLocalFlyControl();
		}

		m_last_player1_mode = mode_now;

	}

	void Player2Module::ResetForWorldLoad()
	{
		// P2 entity pointers become invalid during native save load.
		// Clearing these flags prevents UpdateController from dereferencing
		// stale pointers to freed GPig/Fly entities, which caused crashes
		// after cutscenes and location transitions.
		InterlockedExchange(&m_player2_ready, 0);
		InterlockedExchange(&m_spawn_snapshot_ready, 0);
		InterlockedExchange(&m_spawn_in_progress, 0);
		m_player2_default_mode_initialized = false;
		m_player2_default_mode_setup_failure_logged = false;
		m_debug_player2_enabled = false;
		m_remote_p2_death_mode_observed = false;
		m_remote_p2_death_mode_entry_sequence = 0;
		m_last_player1_mode = 0;
		m_last_weapon_type = 0xFFFFFFFFu;
		m_spawn_context = {};
		m_abr_native_task_configured_player2 = {};
		DebugActions::Instance().ResetForWorldLoad();
		CoopRuntime::Instance().Log("[reset] P2 state cleared for world load\r\n");
	}

	bool Player2Module::ConfigurePlayer2DefaultMode(void* controller)
	{
		if (!controller || GetModeId(controller) != kDefaultModeId)
			return false;

		const retail::ControllerRef controller_ref = {
			retail::ToAddress(controller)
		};
		retail::ModeRef mode = {};
		std::uint32_t original_mask = 0;
		if (!retail::ControllerView(controller_ref).Mode(mode) ||
			!retail::ModeView(mode).ConflictMask(original_mask))
		{
			if (!m_player2_default_mode_setup_failure_logged)
			{
				CoopRuntime::Instance().Log(
					"[arbiter] unable to read P2 Default conflict mask; will retry\r\n");
				m_player2_default_mode_setup_failure_logged = true;
			}
			return false;
		}
		if (original_mask != kDefaultModeConflictMask)
		{
			if (!m_player2_default_mode_setup_failure_logged)
			{
				CoopRuntime::Instance().Log(
					"[arbiter] P2 Default conflict mask unexpected: 0x%08X; will retry\r\n",
					original_mask);
				m_player2_default_mode_setup_failure_logged = true;
			}
			return false;
		}

		// The modes are per-controller objects: the observed P2 Default instance
		// differs from P1's Mooch instance.  0x5BFF80 compares the bit newly
		// required by P1's Mooch -> Default hand-off (bit 0x1) against every
		// other controller.  P2 has no local camera/active-entity ownership, so
		// leaving that bit set falsely makes it a competing single-player owner.
		// Retain the rest of P2's native Default mask and its full stock tick.
		const std::uint32_t revised_mask = original_mask &
			~kP2DefaultExclusiveMask;
		if (!retail::ModeView(mode).SetConflictMask(revised_mask))
		{
			if (!m_player2_default_mode_setup_failure_logged)
			{
				CoopRuntime::Instance().Log(
					"[arbiter] unable to write P2 Default conflict mask; will retry\r\n");
				m_player2_default_mode_setup_failure_logged = true;
			}
			return false;
		}
		m_player2_default_mode_setup_failure_logged = false;
		CoopRuntime::Instance().Log(
			"[arbiter] P2 Default mode=%p conflict mask 0x%08X -> 0x%08X\r\n",
			retail::ToPointer(mode.value), original_mask, revised_mask);
		return true;
	}

	bool Player2Module::RunStockControllerUpdate(void* controller,
		const char* context)
	{
		if (!controller || !m_original_update)
			return false;
		__try
		{
			m_original_update(controller);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log(
				"[controller] stock update fault context=%s controller=%p\r\n",
				context ? context : "unknown", controller);
			return false;
		}
	}

	void Player2Module::UpdateController(void* controller)
	{
		retail::EntitySlotRepository players;
		const retail::ControllerRef controller_ref = {
			retail::ToAddress(controller)
		};
		const retail::EntitySlot slot =
			players.FindSelectableGPigSlotForController(controller_ref);
		if (slot == retail::EntitySlot::LocalP1)
		{
			TickPlayer1(controller);
			return;
		}

		const bool is_remote_p2 = slot == retail::EntitySlot::RemoteP2;
		// F5 can create a local diagnostic P2 in the same retail slot. It has no
		// authoritative peer snapshot, so never leave that opt-in local experiment
		// permanently held in a remote-only DeathMode guard. The guard applies only
		// to the replicated P2 while a peer actually owns its presentation.
		const bool is_networked_remote_p2 = is_remote_p2 &&
			CoopNetGame::Instance().HasRemotePeer();
		if (is_networked_remote_p2 && IsGPigDeathMode(controller))
		{
			if (TryRecoverRemoteP2Death(controller))
			{
				m_remote_p2_death_mode_observed = false;
				m_remote_p2_death_mode_entry_sequence = 0;
			}
			else
			{
				if (!m_remote_p2_death_mode_observed)
				{
					CoopRuntime::Instance().Log(
						"[p2-death-guard] holding native DeathMode until peer sends a newer Default snapshot entry_seq=%u\r\n",
						m_remote_p2_death_mode_entry_sequence);
					m_remote_p2_death_mode_observed = true;
				}
				return;
			}
		}
		else if (is_remote_p2)
		{
			m_remote_p2_death_mode_observed = false;
			m_remote_p2_death_mode_entry_sequence = 0;
		}

		if (UpdateFlyController(controller))
			return;

		const bool is_ready_player2 =
			slot == retail::EntitySlot::RemoteP2 &&
			(CoopRuntime::Instance().Config().enabled || m_debug_player2_enabled) &&
			InterlockedCompareExchange(&m_player2_ready, 0, 0) != 0;
		if (!is_ready_player2)
		{
			RunStockControllerUpdate(controller, "non-player");
			return;
		}

		UpdateRemotePlayer2Controller(controller);
	}

	bool Player2Module::UpdateFlyController(void* controller)
	{
		void* const fly = GetFlyEntity();
		if (!controller || controller != GetController(fly))
			return false;

		CoopNetGame& netgame = CoopNetGame::Instance();
		const bool local_fly_controlled = netgame.IsLocalFlyControlled();
		const bool remote_fly_presentation = !local_fly_controlled &&
			netgame.IsRemoteFlyControlled();
		// A peer-owned Mooch is presentation-only on this process. Do not mirror its
		// +0x53 state flag or run its remote controller/input scope: that would
		// switch this machine's camera/player into Mooch. Clear an older DLL's
		// mirrored state before the stock idle tick; a separate scoped Fly_Active
		// pass later feeds only the native Aim motor and restores the shared camera.
		if (remote_fly_presentation)
			netgame.SetFlyControlActiveState(fly, false);

		std::uint32_t remote_zero_owner_input_sequence = 0;
		if (netgame.ConsumeRemoteFlyZeroOwnerTransition(
			remote_zero_owner_input_sequence))
		{
			const std::uint32_t transition_mode_before = GetModeId(controller);
			const retail::ControllerRef controller_ref = {
				retail::ToAddress(controller)
			};
			const bool transition_accepted =
				retail::ControllerView(controller_ref).SelectMode(
					kFlyDeactivatedModeId);
			const std::uint32_t transition_mode_after = GetModeId(controller);
			// The network worker never calls retail state code.  This single request
			// happens on the exact Mooch game-thread tick after a sequenced peer
			// zero-owner state.  A racing newer peer claim is still protected by the
			// StateMachine_SelectState guard, so "requested" is intentionally not a
			// claim that Deactivated::Enter necessarily ran.
			CoopRuntime::Instance().Log(
				"[fly-lifecycle] consumed ordered remote zero-owner input_seq=%u; requested stock Fly_Deactivated controller=%p mode=0x%08X->0x%08X accepted=%u\r\n",
				remote_zero_owner_input_sequence, controller,
				transition_mode_before, transition_mode_after,
				transition_accepted ? 1u : 0u);
		}

		const std::uint32_t mode_before = GetModeId(controller);
		// A receiver-side laser pulse lasts one Mooch controller tick, matching the
		// stock pressed-edge field write. Clear any previous pulse before stock can
		// run; this also handles a peer-to-local ownership hand-off without touching
		// this process's camera, HUD or controller selection.
		netgame.BeginRemoteFlyDualLaserPresentationTick();
		const bool stock_update_completed = RunStockControllerUpdate(controller, "fly");

		if (!stock_update_completed)
			return true;
		netgame.ObserveLocalFlyMode(mode_before, GetModeId(controller));
		// Mooch reads/writes the same process-global camera handler after P1's
		// Default controller has already run.  Capture its yaw here, not in
		// TickPlayer1, otherwise P1 overwrites the owner yaw just before the packet
		// is sent and the peer steers the remote fly toward P1's old camera.
		if (netgame.IsLocalFlyControlled())
		{
			float local_camera_yaw = 0.0f;
			netgame.PublishLocalCameraYaw(local_camera_yaw,
				m_camera.ReadLocalYaw(local_camera_yaw));
			netgame.PublishLocalFlyAimRay();
		}
		netgame.MaintainLocalFlyActiveEntity(fly);
		// Publish the root transform produced by this native Fly tick. The receiver's
		// EntityView invalidates retail's cached root matrix after applying it.
		netgame.PublishLocalFlyTransform(fly);
		if (!netgame.IsLocalFlyControlled())
		{
			netgame.ApplyRemoteFlyTransform(fly);
			if (remote_fly_presentation)
				netgame.ApplyRemoteFlyAimMotor(fly);
			if (remote_fly_presentation)
				netgame.ApplyRemoteFlyDualLaserPresentation(fly);
		}
		return true;
	}

	void Player2Module::UpdateRemotePlayer2Controller(void* controller)
	{
		CoopNetGame& netgame = CoopNetGame::Instance();

		retail::EntitySlotRepository players;
		const retail::EntityRef player1_ref = players.GetSelectable(
			retail::EntitySlot::LocalP1);
		const retail::EntityRef player2_ref = players.GetSelectable(
			retail::EntitySlot::RemoteP2);
		void* const player1 = retail::ToPointer(player1_ref.value);
		void* const player2 = retail::ToPointer(player2_ref.value);

		const bool local_player_is_abr =
			GetModeId(GetController(player1)) == kAbrModeId;
		const bool remote_player_is_abr = GetModeId(controller) == kAbrModeId;
		if (local_player_is_abr || remote_player_is_abr)
		{
			// ABR is a vehicle-motor domain. Do not even enter the generic P2 input,
			// camera, weapon or root-transform path while either controller is in it:
			// those paths belong to ordinary Darwin locomotion, not the RDV vehicle.
			// The native task setup needs the local P1 ABR spawn-context contract; an
			// isolated remote-P2 ABR transition is still kept out of generic P2 code.
			if (local_player_is_abr && netgame.HasRemotePeer())
				TryEnsurePlayer2RdvTask("network-ABR");
			RunStockControllerUpdate(controller, local_player_is_abr ?
				"remote-player2-ABR-shared" : "remote-player2-ABR-only");

			return;
		}

		RemoteSnapshotInputScope remote_input(netgame);

		const bool preserve_fly_camera = netgame.IsLocalFlyControlled();
		PrimaryGamePadScope remote_gamepad(netgame, preserve_fly_camera);

		if (GetModeId(controller) == kInactiveModeId)
		{
			SharedCameraCoordinator::AimState saved_camera_state = {};
			const bool restore_camera = m_camera.SaveAimState(saved_camera_state);
			const retail::ControllerRef controller_ref = {
				retail::ToAddress(controller)
			};
			retail::ControllerView(controller_ref).SelectMode(kDefaultModeId);
			if (restore_camera)
				m_camera.RestoreAimState(saved_camera_state);
		}
		if (!m_player2_default_mode_initialized && GetModeId(controller) == kDefaultModeId)
		{
			m_player2_default_mode_initialized =
				ConfigurePlayer2DefaultMode(controller);
		}

		std::uint32_t remote_weapon_type = 0xFFFFFFFFu;
		if (netgame.GetActiveRemoteWeaponType(remote_weapon_type))
			ApplyPlayer2WeaponSelection(player2, remote_weapon_type, "remote P1");
		netgame.ArmRemoteP2AmmoOwner(player2);

		SharedCameraCoordinator::AimState saved_fly_camera_state = {};
		const bool restore_fly_camera = preserve_fly_camera &&
			m_camera.SaveAimState(saved_fly_camera_state);
		const bool stock_update_completed =
			RunStockControllerUpdate(controller, "remote-player2");
		if (restore_fly_camera)
			m_camera.RestoreAimState(saved_fly_camera_state);
		if (!stock_update_completed)
			return;

		if (netgame.HasRemotePeer())
			TryEnsurePlayer2RdvTask("network-post-P2-tick");
		netgame.ApplyRemotePlayerTransform(player2);
	}

	void* __cdecl Player2Module::HookSpawnGPig(const Vec4* position, const Vec4* rotation, std::uint32_t gpig_id, void* context)
	{
		return Instance().SpawnGPig(position, rotation, gpig_id, context);
	}

	void* Player2Module::SpawnGPig(
		const Vec4* position, const Vec4* rotation, std::uint32_t gpig_id, void* context)
	{
		CoopRuntime::Instance().Log("[spawn-enter] id=0x%08X position=%p rotation=%p context=%p\r\n",
			gpig_id, position, rotation, context);
		retail::EntityRef player1_ref = {};
		const retail::SpawnContextRef spawn_context = {
			retail::ToAddress(context)
		};
		if (!retail::NativeGameApi::SpawnGPig(position, rotation, gpig_id,
			spawn_context, player1_ref))
		{
			CoopRuntime::Instance().Log(
				"[spawn-stock-fault] id=0x%08X\r\n", gpig_id);
			return nullptr;
		}
		void* const player1 = retail::ToPointer(player1_ref.value);
		CoopRuntime::Instance().Log("[spawn-stock-ok] id=0x%08X entity=%p\r\n", gpig_id, player1);

		if (gpig_id != kGPig1Id || !player1)

			return player1;

		__try
		{
			if (!position || !rotation)
				return player1;
			m_spawn_position = *position;
			m_spawn_rotation = *rotation;
			m_spawn_context = { retail::ToAddress(context) };
			InterlockedExchange(&m_spawn_snapshot_ready, 1);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log("[error] invalid stock spawn arguments; network P2 creation is unavailable\r\n");
			return player1;
		}
		CoopRuntime::Instance().Log("[spawn-snapshot] P1=%p context=%p; network peer creates P2 after connection\r\n",
			player1, context);
		return player1;
	}

	bool Player2Module::PatchSpawnCall(std::uintptr_t address, const BYTE expected[5], BYTE original[5])
	{
		BYTE* call = reinterpret_cast<BYTE*>(address);
		if (memcmp(call, expected, 5) != 0)
		{
			CoopRuntime::Instance().Log("[error] spawn CALL mismatch at 0x%08X\r\n",
				static_cast<unsigned>(address));
			return false;
		}
		memcpy(original, call, 5);
		BYTE replacement[5] = { 0xE8, 0, 0, 0, 0 };
		const intptr_t displacement =
			reinterpret_cast<BYTE*>(&HookSpawnGPig) - (call + 5);
		const std::int32_t relative = static_cast<std::int32_t>(displacement);
		memcpy(replacement + 1, &relative, sizeof(relative));
		return MemoryPatch::Write(call, replacement, sizeof(replacement));
	}

	bool Player2Module::PatchDefaultModeActivePublish()
	{
		BYTE* stores = reinterpret_cast<BYTE*>(kDefaultModeActiveStores);
		if (memcmp(stores, kExpectedDefaultModeActiveStores,
			sizeof(kExpectedDefaultModeActiveStores)) != 0)
		{
			CoopRuntime::Instance().Log("[error] Default-mode active stores mismatch at 0x%08X\r\n",
				static_cast<unsigned>(kDefaultModeActiveStores));
			return false;
		}

		memcpy(m_original_default_mode_active_stores, stores,
			sizeof(m_original_default_mode_active_stores));
		BYTE replacement[10];
		memset(replacement, 0x90, sizeof(replacement));
		replacement[0] = 0xE8;
		const intptr_t displacement =
			reinterpret_cast<BYTE*>(&HookDefaultModeActiveStores) - (stores + 5);
		const std::int32_t relative = static_cast<std::int32_t>(displacement);
		memcpy(replacement + 1, &relative, sizeof(relative));
		if (!MemoryPatch::Write(stores, replacement, sizeof(replacement)))
			return false;
		m_default_mode_active_stores_patched = true;
		return true;
	}

	bool Player2Module::Install()
	{
		void** gpig_update_slot = reinterpret_cast<void**>(kGPigUpdateVtableSlot);
		void** fly_update_slot = reinterpret_cast<void**>(kFlyUpdateVtableSlot);
		if (*gpig_update_slot != reinterpret_cast<void*>(kOriginalControllerUpdate))
		{
			CoopRuntime::Instance().Log("[error] GPig update vtable mismatch: %p\r\n",
				*gpig_update_slot);
			return false;
		}
		if (*fly_update_slot != reinterpret_cast<void*>(kOriginalControllerUpdate))
		{
			CoopRuntime::Instance().Log("[error] Fly update vtable mismatch: %p\r\n",
				*fly_update_slot);
			return false;
		}

		if (!PatchSpawnCall(kSpawnCall1, kExpectedSpawnCall1, m_original_spawn_call1))
			return false;
		if (!PatchSpawnCall(kSpawnCall2, kExpectedSpawnCall2, m_original_spawn_call2))
		{
			MemoryPatch::Write(reinterpret_cast<void*>(kSpawnCall1),
				m_original_spawn_call1, sizeof(m_original_spawn_call1));
			return false;
		}
		if (!PatchDefaultModeActivePublish())
		{
			MemoryPatch::Write(reinterpret_cast<void*>(kSpawnCall1),
				m_original_spawn_call1, sizeof(m_original_spawn_call1));
			MemoryPatch::Write(reinterpret_cast<void*>(kSpawnCall2),
				m_original_spawn_call2, sizeof(m_original_spawn_call2));
			return false;
		}
		void* replacement = reinterpret_cast<void*>(&HookControllerUpdate);

		if (!MemoryPatch::Write(gpig_update_slot, &replacement, sizeof(replacement)))
		{
			MemoryPatch::Write(reinterpret_cast<void*>(kSpawnCall1),
				m_original_spawn_call1, sizeof(m_original_spawn_call1));
			MemoryPatch::Write(reinterpret_cast<void*>(kSpawnCall2),
				m_original_spawn_call2, sizeof(m_original_spawn_call2));
			MemoryPatch::Write(reinterpret_cast<void*>(kDefaultModeActiveStores),
				m_original_default_mode_active_stores,
				sizeof(m_original_default_mode_active_stores));
			m_default_mode_active_stores_patched = false;
			return false;
		}
		if (!MemoryPatch::Write(fly_update_slot, &replacement, sizeof(replacement)))

		{
			void* original = reinterpret_cast<void*>(kOriginalControllerUpdate);
			MemoryPatch::Write(gpig_update_slot, &original, sizeof(original));
			MemoryPatch::Write(reinterpret_cast<void*>(kSpawnCall1),
				m_original_spawn_call1, sizeof(m_original_spawn_call1));
			MemoryPatch::Write(reinterpret_cast<void*>(kSpawnCall2),
				m_original_spawn_call2, sizeof(m_original_spawn_call2));
			MemoryPatch::Write(reinterpret_cast<void*>(kDefaultModeActiveStores),
				m_original_default_mode_active_stores,
				sizeof(m_original_default_mode_active_stores));
			m_default_mode_active_stores_patched = false;
			return false;
		}

		CoopRuntime::Instance().Log("[ok] spawn CALLs patched at 0x%08X and 0x%08X\r\n",

			static_cast<unsigned>(kSpawnCall1), static_cast<unsigned>(kSpawnCall2));
		CoopRuntime::Instance().Log("[ok] GPig/Fly update vtable slots 0x%08X/0x%08X: 0x%08X -> %p\r\n",
			static_cast<unsigned>(kGPigUpdateVtableSlot),
			static_cast<unsigned>(kFlyUpdateVtableSlot),
			static_cast<unsigned>(kOriginalControllerUpdate), replacement);
		CoopRuntime::Instance().Log("[ok] Default-mode active-player stores guarded at 0x%08X\r\n",
			static_cast<unsigned>(kDefaultModeActiveStores));
		return true;

	}

	void Player2Module::Remove()
	{
		void** gpig_update_slot = reinterpret_cast<void**>(kGPigUpdateVtableSlot);

		void** fly_update_slot = reinterpret_cast<void**>(kFlyUpdateVtableSlot);
		void* original = reinterpret_cast<void*>(kOriginalControllerUpdate);
		if (*gpig_update_slot == reinterpret_cast<void*>(&HookControllerUpdate))
		{
			MemoryPatch::Write(gpig_update_slot, &original, sizeof(original));
		}
		if (*fly_update_slot == reinterpret_cast<void*>(&HookControllerUpdate))
			MemoryPatch::Write(fly_update_slot, &original, sizeof(original));
		if (m_original_spawn_call1[0] == 0xE8)
			MemoryPatch::Write(reinterpret_cast<void*>(kSpawnCall1),
				m_original_spawn_call1, sizeof(m_original_spawn_call1));
		if (m_original_spawn_call2[0] == 0xE8)
			MemoryPatch::Write(reinterpret_cast<void*>(kSpawnCall2),
				m_original_spawn_call2, sizeof(m_original_spawn_call2));
		if (m_default_mode_active_stores_patched)
		{
			MemoryPatch::Write(reinterpret_cast<void*>(kDefaultModeActiveStores),
				m_original_default_mode_active_stores,
				sizeof(m_original_default_mode_active_stores));
			m_default_mode_active_stores_patched = false;
		}
	}
}
