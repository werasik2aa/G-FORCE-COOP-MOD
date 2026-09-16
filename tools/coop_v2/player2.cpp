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

#include <float.h>
#include <math.h>
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

		class RemoteAbrFireInputScope final
		{
		public:
			explicit RemoteAbrFireInputScope(CoopNetGame& netgame) :
				netgame_(netgame)
			{
				netgame_.BeginRemoteAbrFireInput();
			}

			~RemoteAbrFireInputScope()
			{
				netgame_.EndRemoteInput();
			}

		private:
			RemoteAbrFireInputScope(const RemoteAbrFireInputScope&);
			RemoteAbrFireInputScope& operator=(const RemoteAbrFireInputScope&);
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

		constexpr float kP2AttachmentReleaseDistance = 0.2f;
		constexpr DWORD kP2AttachmentReleaseRepeatWindowMs = 250u;

		bool IsFiniteProgressionTransform(const retail::Transform& transform)
		{
			const float values[] = {
				transform.position.x, transform.position.y, transform.position.z,
				transform.position.w, transform.rotation.x, transform.rotation.y,
				transform.rotation.z, transform.rotation.w
			};
			for (const float value : values)
			{
				if (value < -FLT_MAX || value > FLT_MAX)
					return false;
			}
			return true;
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
		m_client_black_pig_promoted(false),
		m_last_role_heartbeat_tick(0),
		m_last_attachment_release_log_tick(0),
		m_client_role_gate_logged(false),
		m_client_black_pig_entity(),
		m_client_original_darwin_entity(),
		m_remote_p2_death_mode_observed(false),
		m_remote_p2_death_mode_entry_sequence(0),

		m_last_player1_mode(0),
		m_remote_p2_attachment_active_tick(0),
		m_remote_p2_attachment_release_divergence_begin_tick(0),
		m_remote_p2_attachment_active(false),
		m_remote_p2_attachment_family(AttachmentFamily::None),
		m_remote_p2_attachment_state_machine(),
		m_abr_native_task_configured_player2(),
		m_player2_abr_mode_setup_failure_logged(false),
		m_local_abr_propulsion_locked(false),
		m_local_abr_saved_target_speed(1.0f),
		m_local_abr_propulsion_direction(1),

		m_last_weapon_type(0xFFFFFFFFu),
		m_last_remote_p2_mode(0),
		m_spawn_context(),
		m_default_mode_active_stores_patched(false),
		m_ledge_observer_hooks_installed(false),
		m_original_ledge_idle_or_into_update(nullptr),
		m_original_ledge_strafe_end_update(nullptr),
		m_original_ledge_jump_update(nullptr),
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
		return SpawnPlayer2FromSnapshot("network") &&
			InterlockedCompareExchange(&m_player2_ready, 0, 0) != 0;
	}

	bool Player2Module::SpawnPlayer2FromSnapshot(const char* trigger)
	{
		if (!trigger)
			trigger = "unknown";
		if (InterlockedCompareExchange(&m_spawn_in_progress, 1, 0) != 0)
			return false;
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
			m_player2_abr_mode_setup_failure_logged = false;

			m_logged_blocked_active_publish = false;

			InterlockedExchange(&m_player2_ready, 1);

		}
		CoopRuntime::Instance().Log("[spawn-%s-result] P1=%p P2=%p P2.handler=%p P2.controller=%p ready=%ld\r\n",
			trigger, player1, player2, player2_handler, player2_controller,
			m_player2_ready);

		InterlockedExchange(&m_spawn_in_progress, 0);
		return player2 && player2_controller;
	}

	void Player2Module::RefreshPlayer1ControllerFromSlot(
		void*& player1_controller)
	{
		retail::EntitySlotRepository players;
		retail::EntitySlotBinding binding = {};
		if (players.GetBinding(retail::EntitySlot::LocalP1, binding) &&
			binding.controller)
		{
			player1_controller = retail::ToPointer(binding.controller.value);
		}
	}

	void Player2Module::LogClientRoleGateOnce(const char* gate,
		std::uint32_t detail_a, std::uint32_t detail_b)
	{
		if (!gate || m_client_role_gate_logged)
			return;
		m_client_role_gate_logged = true;
		CoopRuntime::Instance().Log(
			"[client-role] waiting: gate=%s detail_a=0x%08X detail_b=0x%08X\r\n",
			gate, detail_a, detail_b);
	}

	void Player2Module::ResetClientRoleCaches()
	{
		m_client_black_pig_promoted = false;
		m_client_black_pig_entity = {};
		m_client_original_darwin_entity = {};
		m_abr_native_task_configured_player2 = {};
		m_player2_default_mode_initialized = false;
		m_player2_default_mode_setup_failure_logged = false;
		m_player2_abr_mode_setup_failure_logged = false;
		m_remote_p2_death_mode_observed = false;
		m_remote_p2_death_mode_entry_sequence = 0;
		m_remote_p2_attachment_active = false;
		m_remote_p2_attachment_family = AttachmentFamily::None;
		m_remote_p2_attachment_state_machine = {};
		m_last_weapon_type = 0xFFFFFFFFu;
		m_last_player1_mode = 0;
		m_last_remote_p2_mode = 0;
		InterlockedExchange(&m_player2_ready, 1);
	}

	bool Player2Module::RebindClientLocalPlayerTo(
		retail::EntityRef new_local_entity)
	{
		// Proven minimal hand-off (b64543c): swap the two selectable slots and
		// rebind the process-global active entity. The spawn context is
		// deliberately untouched: rebinding it froze the promoted Black Pig.
		if (!new_local_entity)
			return false;
		retail::EntityRef active_a = {};
		retail::EntityRef active_b = {};
		if (!retail::ActiveEntityStore().Read(active_a, active_b))
		{
			return false;
		}
		retail::EntitySlotRepository players;
		if (!players.SwapSelectable(retail::EntitySlot::LocalP1,
			retail::EntitySlot::RemoteP2))
		{
			return false;
		}
		if (!retail::ActiveEntityStore().Set(new_local_entity))
		{
			retail::ActiveEntityStore().Restore(active_a, active_b);
			players.SwapSelectable(retail::EntitySlot::LocalP1,
				retail::EntitySlot::RemoteP2);
			return false;
		}
		return true;
	}

	bool Player2Module::PromoteClientBlackPigToPlayer1(
		void*& player1_controller)
	{
		CoopNetGame& netgame = CoopNetGame::Instance();
		if (!netgame.IsClient())
			return false;

		retail::EntitySlotRepository players;
		retail::EntitySlotBinding local = {};
		retail::EntitySlotBinding remote = {};
		if (!netgame.HasRemotePeer())
		{
			LogClientRoleGateOnce("no-remote-peer", 0, 0);
			RefreshPlayer1ControllerFromSlot(player1_controller);
			return false;
		}
		if (InterlockedCompareExchange(&m_spawn_snapshot_ready, 0, 0) == 0 ||
			!m_spawn_context)
		{
			// The stock P1 factory has not run since the last world load, so the
			// network P2 cannot be created either. This is the late-join/save-load
			// seam: a native load that deserializes P1 without the factory leaves
			// no spawn context to reuse.
			LogClientRoleGateOnce("no-spawn-snapshot", 0, 0);
			RefreshPlayer1ControllerFromSlot(player1_controller);
			return false;
		}
		if (!players.GetBinding(retail::EntitySlot::LocalP1, local) ||
			!players.GetBinding(retail::EntitySlot::RemoteP2, remote))
		{
			LogClientRoleGateOnce("no-role-bindings",
				local.entity.value, remote.entity.value);
			RefreshPlayer1ControllerFromSlot(player1_controller);
			return false;
		}

		if (m_client_black_pig_promoted)
		{
			if (local.entity == m_client_black_pig_entity &&
				remote.entity == m_client_original_darwin_entity)
			{
				player1_controller = retail::ToPointer(local.controller.value);
				return true;
			}
			// A world rebuild replaced one side without the normal reset seam.
			// Drop the role cache, then evaluate the fresh entities below
			// instead of the stale locals.
			ResetClientRoleCaches();
			m_client_role_gate_logged = false;
			if (!players.GetBinding(retail::EntitySlot::LocalP1, local) ||
				!players.GetBinding(retail::EntitySlot::RemoteP2, remote))
			{
				RefreshPlayer1ControllerFromSlot(player1_controller);
				return false;
			}
		}

		// The hand-off stays active on ABR tracks too: the earlier wrong-way
		// turns were the stripped P2 conflict mask, which is restored below.
		const std::uint32_t local_mode = GetModeId(
			retail::ToPointer(local.controller.value));
		const std::uint32_t black_mode = GetModeId(
			retail::ToPointer(remote.controller.value));
		if (local_mode != kDefaultModeId && local_mode != kAbrModeId)
		{
			LogClientRoleGateOnce("local-not-drivable", local_mode, black_mode);
			RefreshPlayer1ControllerFromSlot(player1_controller);
			return false;
		}
		if (black_mode != kInactiveModeId && black_mode != kDefaultModeId &&
			black_mode != kAbrModeId)
		{
			LogClientRoleGateOnce("black-pig-not-idle", local_mode, black_mode);
			RefreshPlayer1ControllerFromSlot(player1_controller);
			return false;
		}

		// The network factory already spawns the Black Pig at the live P1
		// position (see [spawn-network] log), so no transform copy is needed.
		if (!RebindClientLocalPlayerTo(remote.entity))
		{
			LogClientRoleGateOnce("role-rebind-failed",
				local.entity.value, remote.entity.value);
			RefreshPlayer1ControllerFromSlot(player1_controller);
			return false;
		}

		player1_controller = retail::ToPointer(remote.controller.value);
		if (GetModeId(player1_controller) == kInactiveModeId)
		{
			retail::ControllerView(remote.controller).SelectMode(kDefaultModeId);
		}
		// The Black Pig served as packet-driven P2 before the hand-off, so its
		// Default conflict mask was stripped of the P1-owner bit. Without the
		// restore the native arbiter drops the promoted controller to Inactive
		// and the client freezes (no move/attack/Mooch) with live input.
		{
			const std::uint32_t promoted_mode = GetModeId(player1_controller);
			const std::uint32_t contract_mode =
				promoted_mode == kAbrModeId ? kAbrModeId : kDefaultModeId;
			retail::ModeRef mode = {};
			std::uint32_t mask_before = 0;
			std::uint32_t mask_after = 0;
			const bool read_before =
				retail::ControllerView(remote.controller).RegisteredMode(
					contract_mode, mode) &&
				retail::ModeView(mode).ConflictMask(mask_before);
			const bool restored = RestoreLocalModeContract(remote.controller,
				contract_mode);
			const bool read_after = read_before &&
				retail::ModeView(mode).ConflictMask(mask_after);
			CoopRuntime::Instance().Log(
				"[client-role] mask restore controller=%p mode=0x%08X before=0x%08X after=0x%08X restored=%d read=%d/%d\r\n",
				player1_controller, contract_mode, mask_before, mask_after,
				restored ? 1 : 0, read_before ? 1 : 0,
				read_after ? 1 : 0);
		}

		ResetClientRoleCaches();
		m_client_black_pig_promoted = true;
		m_client_black_pig_entity = remote.entity;
		m_client_original_darwin_entity = local.entity;
		m_client_role_gate_logged = false;

		CoopRuntime::Instance().Log(
			"[client-role] Black Pig promoted to local P1=%p mode=0x%08X; Darwin moved to remote P2=%p\r\n",
			retail::ToPointer(remote.entity.value), GetModeId(player1_controller),
			retail::ToPointer(local.entity.value));
		return true;
	}

	bool Player2Module::RestoreLocalModeContract(
		retail::ControllerRef controller, std::uint32_t mode_id)
	{
		if (!controller || (mode_id != kDefaultModeId && mode_id != kAbrModeId))
			return false;
		retail::ModeRef mode = {};
		std::uint32_t conflict_mask = 0;
		const std::uint32_t expected_mask = mode_id == kAbrModeId ?
			kAbrModeConflictMask : kDefaultModeConflictMask;
		if (!retail::ControllerView(controller).RegisteredMode(mode_id, mode) ||
			!retail::ModeView(mode).ConflictMask(conflict_mask))
		{
			return false;
		}
		if (conflict_mask == expected_mask)
			return true;
		if (conflict_mask != (expected_mask & ~kP2DefaultExclusiveMask))
			return false;
		return retail::ModeView(mode).SetConflictMask(expected_mask);
	}

	bool Player2Module::TryEnsurePlayer2RdvTask(const char* source)
	{
		if (!source)
			source = "unknown";

		if (!CoopNetGame::Instance().HasRemotePeer())
		{
			CoopRuntime::Instance().Log(
				"[abr-task] source=%s ignored: native task setup is network-only\r\n",
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
		return TryEnsureRdvTaskForEntity(source, player2,
			m_abr_native_task_configured_player2);
	}

	bool Player2Module::TryEnsureRdvTaskForEntity(const char* source,
		retail::EntityRef entity, retail::EntityRef& configured_entity)
	{
		if (!source)
			source = "unknown";
		if (!entity || !m_spawn_context)
			return false;

		// The stock configurator is valid only while the spawn context and both
		// process-global active pointers name the current local P1. Keep the target
		// entity explicit so a Black Pig returned from local ownership can safely
		// reuse its already-created native task as the remote presentation.
		retail::EntitySlotRepository players;
		const retail::EntityRef local_player = players.GetSelectable(
			retail::EntitySlot::LocalP1);
		retail::EntityRef context_entity = {};
		retail::EntityRef active_a = {};
		retail::EntityRef active_b = {};
		std::uint32_t context_flags = 0;
		const bool context_ready = local_player &&
			retail::SpawnContextView(m_spawn_context).Flags(context_flags) &&
			retail::SpawnContextView(m_spawn_context).ActiveEntity(context_entity) &&
			retail::ActiveEntityStore().Read(active_a, active_b) &&
			(context_flags & kGPigSpawnContextRdvFlag) != 0 &&
			context_entity == local_player && active_a == local_player &&
			active_b == local_player;
		if (!context_ready)
			return false;

		// The factory may succeed before the native spawn-context configurator is
		// ready.  Only its postcondition (`task+0x30 == 1`) is a valid cache hit;
		// otherwise the next controller tick must retry the stock configurator.
		if (configured_entity == entity)
			return true;

		retail::HandlerRef handler = {};
		retail::MotorSystemRef motor_system = {};
		retail::Address state_table = 0;
		retail::Address resource_table = 0;
		retail::MotorResourceRef rdv_resource = {};
		retail::MotorTaskRef existing_task = {};
		std::uint32_t resource_count = 0;
		std::uint32_t state_index = 0;
		retail::Address resource_vtable = 0;
		retail::Address task_vtable = 0;
		bool contract_valid = retail::EntityView(entity).Handler(handler) &&
			retail::HandlerView(handler).MotorSystem(motor_system);
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
				"[abr-task] source=%s rejected: task factory contract unavailable entity=%p handler=%p state=%p index=%u resources=%p count=%u existing=%p\r\n",
				source, retail::ToPointer(entity.value),
				retail::ToPointer(handler.value),
				retail::ToPointer(state_table), state_index,
				retail::ToPointer(resource_table), resource_count,
				retail::ToPointer(existing_task.value));
			return false;
		}
		if (existing_task)
		{
			std::uint8_t enabled = 0;
			if (retail::MotorTaskView(existing_task).RdvEnabled(enabled) &&
				enabled == kGPigRdvTaskEnabledValue)
			{
				configured_entity = entity;
				CoopRuntime::Instance().Log(
					"[abr-task] source=%s entity=%p reused configured XMotorTask_RDV=%p\r\n",
					source, retail::ToPointer(entity.value),
					retail::ToPointer(existing_task.value));
				return true;
			}
			CoopRuntime::Instance().Log(
				"[abr-task] source=%s entity=%p already has native XMotorTask_RDV=%p; no factory call made\r\n",
				source, retail::ToPointer(entity.value),
				retail::ToPointer(existing_task.value));
			return ConfigureRdvTaskForEntity(source, entity, handler,
				existing_task, configured_entity);
		}

		// This is the engine's own lazy factory: it allocates, constructs and inserts
		// XMotorTask_RDV in handler+0x4EC. The test intentionally does not select RDV
		// mode and does not configure task fields; it only proves whether this missing
		// stock object is sufficient for the next controller-mode experiment.
		CoopRuntime::Instance().Log(
			"[abr-task] source=%s native factory begin entity=%p handler=%p motor=%p state=%p index=%u\r\n",
			source, retail::ToPointer(entity.value),
			retail::ToPointer(handler.value),
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
				"[abr-task] source=%s native factory postcondition failed entity=%p task=%p vtbl=%p\r\n",
				source, retail::ToPointer(entity.value),
				retail::ToPointer(created_task.value),
				retail::ToPointer(task_vtable));
			return false;
		}

		CoopRuntime::Instance().Log(
			"[abr-task] source=%s native factory complete entity=%p XMotorTask_RDV=%p vtbl=%p\r\n",
			source, retail::ToPointer(entity.value),
			retail::ToPointer(created_task.value),
			retail::ToPointer(task_vtable));

		return ConfigureRdvTaskForEntity(source, entity, handler,
			created_task, configured_entity);
	}

	bool Player2Module::TryEnterPlayer2AbrMode(void* controller)
	{
		if (!controller)
			return false;
		const std::uint32_t current_mode = GetModeId(controller);
		if (current_mode == kAbrModeId)
			return true;
		// Never override Death, Ledge, cutscene or other native transitions.
		if (current_mode != kDefaultModeId)
			return false;

		const retail::ControllerRef controller_ref = {
			retail::ToAddress(controller)
		};
		const retail::ControllerView controller_view(controller_ref);
		retail::ModeRef abr_mode = {};
		std::uint32_t conflict_mask = 0;
		const bool contract_valid =
			controller_view.RegisteredMode(kAbrModeId, abr_mode) &&
			retail::ModeView(abr_mode).ConflictMask(conflict_mask) &&
			(conflict_mask == kAbrModeConflictMask ||
				conflict_mask == (kAbrModeConflictMask &
					~kP2DefaultExclusiveMask));
		if (!contract_valid ||
			(conflict_mask == kAbrModeConflictMask &&
				!retail::ModeView(abr_mode).SetConflictMask(
					kAbrModeConflictMask & ~kP2DefaultExclusiveMask)))
		{
			if (!m_player2_abr_mode_setup_failure_logged)
			{
				CoopRuntime::Instance().Log(
					"[abr-mode] P2 registered ABR contract rejected mode=%p mask=0x%08X\r\n",
					retail::ToPointer(abr_mode.value), conflict_mask);
				m_player2_abr_mode_setup_failure_logged = true;
			}
			return false;
		}

		const bool accepted = controller_view.SelectMode(kAbrModeId);
		const std::uint32_t selected_mode = GetModeId(controller);
		const bool entered = selected_mode == kAbrModeId;
		if (entered || !m_player2_abr_mode_setup_failure_logged)
		{
			CoopRuntime::Instance().Log(
				"[abr-mode] P2 native enter requested current=0x%08X selected=0x%08X accepted=%u mode=%p mask=0x%08X\r\n",
				current_mode, selected_mode, accepted ? 1u : 0u,
				retail::ToPointer(abr_mode.value),
				kAbrModeConflictMask & ~kP2DefaultExclusiveMask);
		}
		if (entered)
			m_player2_abr_mode_setup_failure_logged = false;
		else
		{
			// A rejected transition must leave the unused registered mode in its
			// stock configuration; retry only on a later authoritative ABR tick.
			if (conflict_mask == kAbrModeConflictMask)
				retail::ModeView(abr_mode).SetConflictMask(conflict_mask);
			m_player2_abr_mode_setup_failure_logged = true;
		}
		return entered;
	}

	bool Player2Module::ConfigureRdvTaskForEntity(const char* source,
		retail::EntityRef entity, retail::HandlerRef handler,
		retail::MotorTaskRef task, retail::EntityRef& configured_entity)
	{
		if (!source)
			source = "unknown";
		if (configured_entity == entity)
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
		const bool contract_valid = context && player1 && entity &&
			handler && task &&
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
				"[abr-task-config] source=%s rejected local_P1=%p entity=%p handler=%p task=%p context=%p flags=0x%08X context_entity=%p active=(%p,%p)\r\n",
				source, retail::ToPointer(player1.value),
				retail::ToPointer(entity.value),
				retail::ToPointer(handler.value),
				retail::ToPointer(task.value), retail::ToPointer(context.value),
				context_flags, retail::ToPointer(context_entity.value),
				retail::ToPointer(active_a.value), retail::ToPointer(active_b.value));
			return false;
		}

		CoopRuntime::Instance().Log(
			"[abr-task-config] source=%s native begin entity=%p task=%p context=%p flags=0x%08X\r\n",
			source, retail::ToPointer(entity.value),
			retail::ToPointer(task.value), retail::ToPointer(context.value),
			context_flags);
		if (!retail::NativeGameApi::ConfigureGPigRdvTask(context,
			handler))
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
				"[abr-task-config] source=%s postcondition failed entity=%p task=%p enabled=%u\r\n",
				source, retail::ToPointer(entity.value),
				retail::ToPointer(task.value), static_cast<unsigned>(enabled));
			return false;
		}

		configured_entity = entity;
		CoopRuntime::Instance().Log(
			"[abr-task-config] source=%s native configured entity=%p task=%p enabled=%u\r\n",
			source, retail::ToPointer(entity.value),
			retail::ToPointer(task.value), static_cast<unsigned>(enabled));
		return true;
	}

	bool Player2Module::SetAbrDriveGate(void* player, bool active)
	{
		if (!player)
			return false;
		const retail::EntityRef player_ref = { retail::ToAddress(player) };
		retail::HandlerRef handler = {};
		retail::MotorTaskRef drive_task = {};
		retail::Address vtable = 0;
		return retail::EntityView(player_ref).Handler(handler) &&
			retail::NativeGameApi::GetGPigRdvDriveTask(handler,
				drive_task) && drive_task &&
			retail::MotorTaskView(drive_task).VTable(vtable) &&
			vtable == kGPigRdvDriveTaskVtable &&
			retail::MotorTaskView(drive_task).SetRdvDriveGate(active);
	}

	bool Player2Module::SetLocalAbrPropulsion(void* player, float direction)
	{
		if (!player)
			return false;
		const retail::EntityRef player_ref = { retail::ToAddress(player) };
		retail::HandlerRef handler = {};
		retail::MotorSystemRef motor_system = {};
		retail::MotorTaskRef rdv_task = {};
		retail::Address vtable = 0;
		std::uint32_t state_index = 0;
		if (!retail::EntityView(player_ref).Handler(handler) ||
			!retail::HandlerView(handler).MotorSystem(motor_system) ||
			!retail::ReadGPigRdvTaskStateIndex(state_index) ||
			state_index >= kMotorSystemTaskStateSafetyLimit ||
			!retail::MotorSystemView(motor_system).TaskAt(state_index, rdv_task) ||
			!rdv_task || !retail::MotorTaskView(rdv_task).VTable(vtable) ||
			vtable != kGPigRdvTaskVtable)
		{
			return false;
		}

		retail::MotorTaskView task(rdv_task);
		float current_speed = 0.0f;
		float target_speed = 0.0f;
		if (!task.RdvSpeed(current_speed, target_speed))
			return false;

		const int requested_direction = direction > 0.05f ? 1 :
			(direction < -0.05f ? -1 : 0);
		const float target_magnitude = fabsf(target_speed);
		const float current_magnitude = fabsf(current_speed);
		if (!m_local_abr_propulsion_locked)
		{
			if (_finite(target_magnitude) && target_magnitude > 0.001f)
				m_local_abr_saved_target_speed = target_magnitude;
			else if (_finite(current_magnitude) && current_magnitude > 0.001f)
				m_local_abr_saved_target_speed = current_magnitude;
		}
		if (!_finite(m_local_abr_saved_target_speed) ||
			m_local_abr_saved_target_speed <= 0.001f)
		{
			m_local_abr_saved_target_speed = 1.0f;
		}

		const bool direction_changed =
			m_local_abr_propulsion_direction != requested_direction;
		if (requested_direction == 0)
		{
			if (!task.SetRdvSpeed(0.0f, 0.0f))
				return false;
			m_local_abr_propulsion_locked = true;
			m_local_abr_propulsion_direction = 0;
			if (direction_changed)
			{
				CoopRuntime::Instance().Log(
					"[abr-drive] propulsion stopped saved_target=%.3f\r\n",
					m_local_abr_saved_target_speed);
			}
			return true;
		}

		const float signed_speed = m_local_abr_saved_target_speed *
			static_cast<float>(requested_direction);
		if (m_local_abr_propulsion_locked || direction_changed)
		{
			// Set current and target together on a direction edge. This avoids making
			// the retail acceleration integrator cross zero with a stale sign.
			if (!task.SetRdvSpeed(signed_speed, signed_speed))
				return false;
		}
		m_local_abr_propulsion_locked = false;
		m_local_abr_propulsion_direction = requested_direction;
		if (direction_changed)
		{
			CoopRuntime::Instance().Log(
				"[abr-drive] propulsion direction=%d target=%.3f\r\n",
				requested_direction, signed_speed);
		}
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
		CoopNetGame& netgame = CoopNetGame::Instance();

		// On a client the stock Darwin first reaches this hook as LocalP1. Once the
		// network Black Pig exists, swap the two complete entities and continue this
		// same guaranteed local tick with the promoted Black Pig controller.
		PromoteClientBlackPigToPlayer1(player1_controller);
		if (m_client_black_pig_promoted && player1_controller &&
			GetModeId(player1_controller) == kInactiveModeId)
		{
			// The native arbiter can still deactivate the promoted controller
			// later (cutscene edges). Re-enter Default exactly like b64543c.
			const retail::ControllerRef controller_ref = {
				retail::ToAddress(player1_controller)
			};
			if (retail::ControllerView(controller_ref).SelectMode(
				kDefaultModeId))
			{
				CoopRuntime::Instance().Log(
					"[client-role] re-entered Default from Inactive local=%p\r\n",
					player1_controller);
			}
		}
		if (m_client_black_pig_promoted)
		{
			const LONG now = static_cast<LONG>(GetTickCount());
			if (now - m_last_role_heartbeat_tick > 5000)
			{
				m_last_role_heartbeat_tick = now;
				CoopRuntime::Instance().Log(
					"[client-role] tick alive local=%p mode=0x%08X\r\n",
					player1_controller, GetModeId(player1_controller));
			}
		}


		// The single shared GPig camera belongs to whoever the player is actually
		// driving.  Its mode goes back to Default immediately after the native Mooch
		// hand-off, so use the confirmed network owner state rather than a mode or
		// process-global pointer that lasts only for that transition frame.
		retail::EntitySlotRepository players;
		const retail::EntityRef player1 = players.GetSelectable(
			retail::EntitySlot::LocalP1);
		const std::uint32_t mode_before = GetModeId(player1_controller);
		if (!netgame.IsLocalFlyControlled() &&
			mode_before == kDefaultModeId)
			m_camera.RefreshForController(player1_controller);
		netgame.BeginLocalInputCapture();
		// ABR is auto-propelled by XMotorTask_RDV. Axes 0/1 are only the native
		// direction input; use their bound values as a propulsion gate without
		// inventing a Jump/action or rewriting the entity transform.
		bool abr_movement_known = false;
		float abr_propulsion_direction = 0.0f;
		if (mode_before == kAbrModeId)
		{
			abr_movement_known = netgame.PollLocalAbrPropulsionDirection(
				abr_propulsion_direction);
			if (player1 && abr_movement_known)
				SetLocalAbrPropulsion(retail::ToPointer(player1.value),
					abr_propulsion_direction);
		}
		if (!RunStockControllerUpdate(player1_controller, "local-player1"))
			return;
		const std::uint32_t mode_after = GetModeId(player1_controller);
		if (mode_after == kAbrModeId)
		{
			// Re-read after the native RDV tick because its own axis queries have now
			// refreshed XGamePad. Reapply the gate after the native speed ramp.
			float post_propulsion_direction = 0.0f;
			const bool post_movement_known =
				netgame.PollLocalAbrPropulsionDirection(
					post_propulsion_direction);
			if (post_movement_known)
			{
				abr_movement_known = true;
				abr_propulsion_direction = post_propulsion_direction;
			}
			if (player1 && abr_movement_known)
				SetLocalAbrPropulsion(retail::ToPointer(player1.value),
					abr_propulsion_direction);
			netgame.PublishLocalAbrAimRay();
		}
		else if (mode_before == kAbrModeId && player1)
		{
			// Restore the game's target speed before the task leaves ABR mode.
			SetLocalAbrPropulsion(retail::ToPointer(player1.value), 1.0f);
		}
		netgame.PublishLocalPlayerMode(
			mode_after);
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
		ResetClientRoleCaches();
		InterlockedExchange(&m_player2_ready, 0);
		InterlockedExchange(&m_spawn_snapshot_ready, 0);
		InterlockedExchange(&m_spawn_in_progress, 0);
		m_debug_player2_enabled = false;
		m_client_role_gate_logged = false;
		m_remote_p2_attachment_active_tick = 0;
		m_remote_p2_attachment_release_divergence_begin_tick = 0;
		m_spawn_context = {};
		m_local_abr_propulsion_locked = false;
		m_local_abr_saved_target_speed = 1.0f;
		m_local_abr_propulsion_direction = 1;
		DebugActions::Instance().ResetForWorldLoad();
		CoopRuntime::Instance().Log("[reset] P2 state cleared for world load\r\n");
	}

	bool Player2Module::ApplyProgressionRallyToRemoteP2(
		const retail::Transform& transform,
		protocol::ProgressionRallyReason reason, std::uint32_t sequence)
	{
		return ApplyProgressionRally(retail::EntitySlot::RemoteP2, transform,
			reason, sequence, "remote P2");
	}

	bool Player2Module::ApplyProgressionRallyToLocalP1(
		const retail::Transform& transform,
		protocol::ProgressionRallyReason reason, std::uint32_t sequence)
	{
		return ApplyProgressionRally(retail::EntitySlot::LocalP1, transform,
			reason, sequence, "local P1");
	}

	bool Player2Module::ApplyProgressionRally(retail::EntitySlot slot,
		const retail::Transform& transform,
		protocol::ProgressionRallyReason reason, std::uint32_t sequence,
		const char* recipient)
	{
		if (sequence == 0 ||
			(reason != protocol::ProgressionRallyReason::Cutscene &&
				reason != protocol::ProgressionRallyReason::Checkpoint) ||
			!IsFiniteProgressionTransform(transform))
		{
			return false;
		}

		retail::EntitySlotRepository players;
		retail::EntitySlotBinding binding = {};
		if (!players.GetBinding(slot, binding) || !binding.entity)
			return false;

		void* const controller = retail::ToPointer(binding.controller.value);
		// A vehicle owns its own root and attached presentation.  Keep the rally
		// pending rather than writing an on-foot correction into ABR.
		if (controller && GetModeId(controller) == kAbrModeId)
			return false;

		const retail::EntityView entity(binding.entity);
		if (!entity.WriteTransform(transform))
			return false;

		CoopRuntime::Instance().Log(
			"[progression-rally] moved %s seq=%u target=(%.2f,%.2f,%.2f)\r\n",
			recipient ? recipient : "player", sequence,
			transform.position.x, transform.position.y, transform.position.z);
		return true;
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

	void Player2Module::ObserveRemoteLedgeState(void* ledge_state)
	{
		if (!ledge_state)
			return;

		const retail::GPigAttachmentStateRef attachment_state_ref = {
			retail::ToAddress(ledge_state)
		};
		retail::EntityRef owner = {};
		retail::EntitySlotRepository players;
		retail::EntityRef remote_p2 = {};
		if (retail::GPigAttachmentStateView(attachment_state_ref).OwnerEntity(owner) &&
			players.Get(retail::EntitySlot::RemoteP2, remote_p2) &&
			owner == remote_p2)
		{
			// This is an observation only. The concrete native state still owns the
			// attachment, animation and contact result.
			m_remote_p2_attachment_active_tick = GetTickCount();
		}
	}

	void Player2Module::ObserveInnerStateSelection(void* state_machine,
		std::uint32_t selected_mode)
	{
		if (!state_machine)
			return;

		const retail::StateMachineRef state_machine_ref = {
			retail::ToAddress(state_machine)
		};
		const retail::StateMachineView state_machine_view(state_machine_ref);
		// Numeric state IDs are shared by many nested motors. Identify the exact
		// Ledge and Climb families by registered native classes instead.
		const bool is_ledge_machine =
			state_machine_view.ContainsRegisteredVTable(kGPigLedgeIdleVtable) ||
			state_machine_view.ContainsRegisteredVTable(kGPigLedgeIntoVtable) ||
			state_machine_view.ContainsRegisteredVTable(kGPigLedgeStrafeEndVtable) ||
			state_machine_view.ContainsRegisteredVTable(kGPigLedgeJumpVtable);
		const bool is_climb_machine = !is_ledge_machine &&
			(state_machine_view.ContainsRegisteredVTable(kGPigClimbDropVtable) ||
				state_machine_view.ContainsRegisteredVTable(kGPigClimbJumpVtable));
		if (!is_ledge_machine && !is_climb_machine)
		{
			return;
		}

		retail::ModeRef current_state = {};
		retail::ModeId current_mode = 0;
		retail::EntityRef owner = {};
		retail::EntitySlotRepository players;
		retail::EntityRef remote_p2 = {};
		if (!state_machine_view.CurrentState(current_state) ||
			!retail::ModeView(current_state).Id(current_mode))
		{
			return;
		}
		const retail::GPigAttachmentStateRef attachment_state_ref = {
			current_state.value
		};
		if (!retail::GPigAttachmentStateView(attachment_state_ref).OwnerEntity(owner) ||
			!players.Get(retail::EntitySlot::RemoteP2, remote_p2) ||
			owner != remote_p2)
		{
			return;
		}

		const AttachmentFamily family = is_ledge_machine ?
			AttachmentFamily::Ledge : AttachmentFamily::Climb;
		const AttachmentFamily previous_family = m_remote_p2_attachment_family;
		const bool was_active = m_remote_p2_attachment_active &&
			m_remote_p2_attachment_state_machine == state_machine_ref;
		const bool is_active = current_mode != kInactiveModeId;
		m_remote_p2_attachment_active_tick = GetTickCount();
		m_remote_p2_attachment_active = is_active;
		m_remote_p2_attachment_family = is_active ? family : AttachmentFamily::None;
		m_remote_p2_attachment_state_machine = state_machine_ref;
		if (!is_active)
		{
			m_remote_p2_attachment_release_divergence_begin_tick = 0;
		}
		if (was_active != is_active ||
			(is_active && previous_family != family))
		{
			CoopRuntime::Instance().Log(
				"[p2-attachment-observer] family=%s attachment=%u machine=%p mode=0x%08X selected=0x%08X\r\n",
				family == AttachmentFamily::Ledge ? "ledge" : "climb",
				is_active ? 1u : 0u, state_machine, current_mode, selected_mode);
		}
	}

	int Player2Module::RunLedgeStateUpdate(void* ledge_state,
		LedgeStateUpdateFn original)
	{
		if (!ledge_state || !original)
			return 0;
		ObserveRemoteLedgeState(ledge_state);
		__try
		{
			return original(ledge_state);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log(
				"[p2-ledge-recovery] native Ledge update fault state=%p\r\n",
				ledge_state);
			return 0;
		}
	}

	bool Player2Module::TryQueueRemoteAttachmentRelease(void* player2,
		void* controller, CoopNetGame& netgame, float& distance)
	{
		distance = 0.0f;
		const DWORD now = GetTickCount();
		// Do not require a particular inner Ledge/Climb class here. The exact
		// observer remains diagnostic, but a stale native attachment is precisely
		// what can prevent that observer from advancing. Persistent P2/owner
		// separation is the recovery signal; stock input decides whether the
		// current state can consume the ordinary release action.
		if (!player2 || !controller ||
			GetModeId(controller) != kDefaultModeId ||
			netgame.IsLocalFlyControlled() || netgame.IsRemoteFlyControlled())
		{
			m_remote_p2_attachment_release_divergence_begin_tick = 0;
			return false;
		}

		retail::Transform target = {};
		std::uint32_t target_sequence = 0;
		std::uint32_t target_mode = 0;
		if (!netgame.GetActiveRemotePlayerTransform(target, target_sequence,
			target_mode) || target_mode != kDefaultModeId)
		{
			m_remote_p2_attachment_release_divergence_begin_tick = 0;
			return false;
		}

		const retail::EntityRef player2_ref = { retail::ToAddress(player2) };
		retail::Transform current = {};
		if (!retail::EntityView(player2_ref).ReadTransform(current))
		{
			m_remote_p2_attachment_release_divergence_begin_tick = 0;
			return false;
		}

		const float dx = target.position.x - current.position.x;
		const float dy = target.position.y - current.position.y;
		const float dz = target.position.z - current.position.z;
		distance = sqrtf(dx * dx + dy * dy + dz * dz);
		if (!(distance >= 0.0f) || distance > FLT_MAX)
		{
			m_remote_p2_attachment_release_divergence_begin_tick = 0;
			return false;
		}
		if (distance <= kP2AttachmentReleaseDistance ||
			netgame.HasActiveRemotePressedEdge(kLedgeReleaseActionId))
		{
			m_remote_p2_attachment_release_divergence_begin_tick = 0;
			return false;
		}

		if (m_remote_p2_attachment_release_divergence_begin_tick == 0)
		{
			m_remote_p2_attachment_release_divergence_begin_tick = now;
			return false;
		}
		if (static_cast<DWORD>(now -
			m_remote_p2_attachment_release_divergence_begin_tick) <
			kP2AttachmentReleaseRepeatWindowMs)
		{
			return false;
		}
		if (!netgame.ArmRemoteLedgeReleaseEdge())
			return false;

		// No one-shot latch: if stock input does not detach a stale attachment,
		// record a fresh divergence window and offer the same native release edge
		// again after 250 ms while P2 remains more than one metre away.
		m_remote_p2_attachment_release_divergence_begin_tick = 0;
		const LONG now_tick = static_cast<LONG>(GetTickCount());
		if (now_tick - m_last_attachment_release_log_tick > 5000)
		{
			m_last_attachment_release_log_tick = now_tick;
			CoopRuntime::Instance().Log(
				"[p2-attachment-release] queued distance=%.2f observer=%s target_seq=%u\r\n",
				distance, m_remote_p2_attachment_family == AttachmentFamily::Ledge ?
					"ledge" : (m_remote_p2_attachment_family == AttachmentFamily::Climb ?
						"climb" : "none"),
				target_sequence);
		}
		return true;
	}

	int __fastcall Player2Module::HookLedgeIdleOrIntoUpdate(void* ledge_state,
		void*)
	{
		return Instance().RunLedgeStateUpdate(ledge_state,
			Instance().m_original_ledge_idle_or_into_update);
	}

	int __fastcall Player2Module::HookLedgeStrafeEndUpdate(void* ledge_state,
		void*)
	{
		return Instance().RunLedgeStateUpdate(ledge_state,
			Instance().m_original_ledge_strafe_end_update);
	}

	int __fastcall Player2Module::HookLedgeJumpUpdate(void* ledge_state, void*)
	{
		return Instance().RunLedgeStateUpdate(ledge_state,
			Instance().m_original_ledge_jump_update);
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

		const std::uint32_t remote_mode_now = GetModeId(controller);
		if (remote_mode_now != m_last_remote_p2_mode)
		{
			CoopRuntime::Instance().Log(
				"[p2-mode] remote P2 controller=%p mode=0x%08X -> 0x%08X\r\n",
				controller, m_last_remote_p2_mode, remote_mode_now);
			m_last_remote_p2_mode = remote_mode_now;
		}

		const bool local_player_is_abr =
			GetModeId(GetController(player1)) == kAbrModeId;
		std::uint32_t remote_transform_sequence = 0;
		std::uint32_t remote_peer_mode = 0;
		const bool remote_mode_known = netgame.GetRemotePlayerModeSnapshot(
			remote_transform_sequence, remote_peer_mode) &&
			remote_transform_sequence != 0;
		if (!local_player_is_abr && remote_mode_known &&
			remote_peer_mode == kDefaultModeId &&
			GetModeId(controller) == kAbrModeId &&
			!netgame.IsLocalFlyControlled())
		{
			// Both owners have left the vehicle. Use the stock dispatcher to
			// return P2 to its registered on-foot mode before generic P2 input.
			const retail::ControllerRef controller_ref = {
				retail::ToAddress(controller)
			};
			const bool accepted = retail::ControllerView(controller_ref).
				SelectMode(kDefaultModeId);
			const std::uint32_t mode_after = GetModeId(controller);
			CoopRuntime::Instance().Log(
				"[abr-mode] P2 native exit requested selected=0x%08X accepted=%u peer_seq=%u\r\n",
				mode_after, accepted ? 1u : 0u,
				remote_transform_sequence);
			if (accepted && mode_after == kDefaultModeId)
				m_player2_default_mode_initialized = false;
		}
		const bool remote_player_is_abr = GetModeId(controller) == kAbrModeId;
		if (local_player_is_abr || remote_player_is_abr)
		{
			// ABR is a vehicle-motor domain. Do not even enter the generic P2 input,
			// camera, weapon or root-transform path while either controller is in it:
			// those paths belong to ordinary Darwin locomotion, not the RDV vehicle.
			// The native task setup needs the local P1 ABR spawn-context contract; an
			// isolated remote-P2 ABR transition is still kept out of generic P2 code.
			const bool abr_task_ready = (local_player_is_abr || remote_player_is_abr) &&
				remote_mode_known && remote_peer_mode == kAbrModeId &&
				netgame.HasRemotePeer() &&
				TryEnsurePlayer2RdvTask("network-ABR");
			SharedCameraCoordinator::AimState saved_abr_camera_state = {};
			const bool restore_abr_camera =
				m_camera.SaveAimState(saved_abr_camera_state);
			retail::EntityRef active_a = {};
			retail::EntityRef active_b = {};
			const bool restore_local_active =
				retail::ActiveEntityStore().Read(active_a, active_b);
			if (abr_task_ready && !remote_player_is_abr)
				TryEnterPlayer2AbrMode(controller);
			// P2 never owns the local physical movement input. Explicitly clear
			// the retail drive latch every tick, including after the one-shot
			// remote Fire fallback below.
			SetAbrDriveGate(player2, false);
			// The native RDV task positions attached vehicle parts from the owner
			// root it reads at the start of this tick. Seed it from the peer's
			// settled transform, then restore that root after the stock motor step.
			netgame.ApplyRemoteAbrTransform(player2);
			bool stock_update_completed = false;
			{
				RemoteAbrFireInputScope remote_fire_input(netgame);
				stock_update_completed = RunStockControllerUpdate(controller,
					local_player_is_abr ? "remote-player2-ABR-shared" :
						"remote-player2-ABR-only");
				netgame.RunRemoteAbrFireFallback(controller);
			}
			// P1 has already published its post-vehicle-tick root into CoopInput.
			// Apply only that settled root to the remote ABR copy: the native RDV
			// task keeps its own motor and attached-part state.
			if (stock_update_completed)
				netgame.ApplyRemoteAbrTransform(player2);
			SetAbrDriveGate(player2, false);
			if (restore_abr_camera)
				m_camera.RestoreAimState(saved_abr_camera_state);
			if (restore_local_active)
				retail::ActiveEntityStore().Restore(active_a, active_b);

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
		float attachment_release_distance = 0.0f;
		const bool attachment_release_queued = TryQueueRemoteAttachmentRelease(player2,
			controller, netgame, attachment_release_distance);
		// Compute one interpolated root for this frame before the native update.
		// Reusing this same root after the update keeps interpolation smooth while
		// preventing local physics/attachments from pulling the model back below
		// the map between packet corrections.
		netgame.ApplyRemotePlayerTransform(player2);
		const bool stock_update_completed =
			RunStockControllerUpdate(controller, "remote-player2");
		if (restore_fly_camera)
			m_camera.RestoreAimState(saved_fly_camera_state);
		if (attachment_release_queued)
		{
			bool attachment_release_consumed = false;
			netgame.FinishRemoteLedgeReleaseEdge(attachment_release_consumed);
			const LONG now_tick = static_cast<LONG>(GetTickCount());
			if (now_tick - m_last_attachment_release_log_tick > 5000)
			{
				m_last_attachment_release_log_tick = now_tick;
				CoopRuntime::Instance().Log(
					"[p2-attachment-release] result consumed=%u distance=%.2f\r\n",
					attachment_release_consumed ? 1u : 0u,
					attachment_release_distance);
			}
		}
		netgame.ReapplyRemotePlayerFrameTransform(player2);
		if (!stock_update_completed)
			return;

		if (netgame.HasRemotePeer())
			TryEnsurePlayer2RdvTask("network-post-P2-tick");
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

	bool Player2Module::InstallLedgeObserverHooks()
	{
		if (m_ledge_observer_hooks_installed)
			return true;

		void** const idle_slot = reinterpret_cast<void**>(
			kGPigLedgeIdleUpdateVtableSlot);
		void** const into_slot = reinterpret_cast<void**>(
			kGPigLedgeIntoUpdateVtableSlot);
		void** const strafe_end_slot = reinterpret_cast<void**>(
			kGPigLedgeStrafeEndUpdateVtableSlot);
		void** const jump_slot = reinterpret_cast<void**>(
			kGPigLedgeJumpUpdateVtableSlot);
		if (*idle_slot != reinterpret_cast<void*>(kGPigLedgeIdleUpdate) ||
			*into_slot != reinterpret_cast<void*>(kGPigLedgeIdleUpdate) ||
			*strafe_end_slot != reinterpret_cast<void*>(kGPigLedgeStrafeEndUpdate) ||
			*jump_slot != reinterpret_cast<void*>(kGPigLedgeJumpUpdate))
		{
			CoopRuntime::Instance().Log(
				"[error] Ledge observer vtable mismatch idle=%p into=%p strafe-end=%p jump=%p\r\n",
				*idle_slot, *into_slot, *strafe_end_slot, *jump_slot);
			return false;
		}

		m_original_ledge_idle_or_into_update =
			reinterpret_cast<LedgeStateUpdateFn>(*idle_slot);
		m_original_ledge_strafe_end_update =
			reinterpret_cast<LedgeStateUpdateFn>(*strafe_end_slot);
		m_original_ledge_jump_update =
			reinterpret_cast<LedgeStateUpdateFn>(*jump_slot);
		void* const idle_or_into_hook = reinterpret_cast<void*>(
			&HookLedgeIdleOrIntoUpdate);
		void* const strafe_end_hook = reinterpret_cast<void*>(
			&HookLedgeStrafeEndUpdate);
		void* const jump_hook = reinterpret_cast<void*>(&HookLedgeJumpUpdate);
		void* const idle_or_into_original = reinterpret_cast<void*>(
			m_original_ledge_idle_or_into_update);
		void* const strafe_end_original = reinterpret_cast<void*>(
			m_original_ledge_strafe_end_update);

		if (!MemoryPatch::Write(idle_slot, &idle_or_into_hook,
			sizeof(idle_or_into_hook)))
			goto failed;
		if (!MemoryPatch::Write(into_slot, &idle_or_into_hook,
			sizeof(idle_or_into_hook)))
		{
			MemoryPatch::Write(idle_slot, &idle_or_into_original,
				sizeof(idle_or_into_original));
			goto failed;
		}
		if (!MemoryPatch::Write(strafe_end_slot, &strafe_end_hook,
			sizeof(strafe_end_hook)))
		{
			MemoryPatch::Write(into_slot, &idle_or_into_original,
				sizeof(idle_or_into_original));
			MemoryPatch::Write(idle_slot, &idle_or_into_original,
				sizeof(idle_or_into_original));
			goto failed;
		}
		if (!MemoryPatch::Write(jump_slot, &jump_hook, sizeof(jump_hook)))
		{
			MemoryPatch::Write(strafe_end_slot, &strafe_end_original,
				sizeof(strafe_end_original));
			MemoryPatch::Write(into_slot, &idle_or_into_original,
				sizeof(idle_or_into_original));
			MemoryPatch::Write(idle_slot, &idle_or_into_original,
				sizeof(idle_or_into_original));
			goto failed;
		}

		m_ledge_observer_hooks_installed = true;
		CoopRuntime::Instance().Log(
			"[ok] P2 active-Ledge observer vtable slots installed\r\n");
		return true;

	failed:
		m_original_ledge_idle_or_into_update = nullptr;
		m_original_ledge_strafe_end_update = nullptr;
		m_original_ledge_jump_update = nullptr;
		return false;
	}

	void Player2Module::RemoveLedgeObserverHooks()
	{
		if (!m_ledge_observer_hooks_installed)
			return;

		void** const idle_slot = reinterpret_cast<void**>(
			kGPigLedgeIdleUpdateVtableSlot);
		void** const into_slot = reinterpret_cast<void**>(
			kGPigLedgeIntoUpdateVtableSlot);
		void** const strafe_end_slot = reinterpret_cast<void**>(
			kGPigLedgeStrafeEndUpdateVtableSlot);
		void** const jump_slot = reinterpret_cast<void**>(
			kGPigLedgeJumpUpdateVtableSlot);
		void* const idle_or_into_original = reinterpret_cast<void*>(
			m_original_ledge_idle_or_into_update);
		void* const strafe_end_original = reinterpret_cast<void*>(
			m_original_ledge_strafe_end_update);
		void* const jump_original = reinterpret_cast<void*>(
			m_original_ledge_jump_update);
		if (*idle_slot == reinterpret_cast<void*>(&HookLedgeIdleOrIntoUpdate))
			MemoryPatch::Write(idle_slot, &idle_or_into_original,
				sizeof(idle_or_into_original));
		if (*into_slot == reinterpret_cast<void*>(&HookLedgeIdleOrIntoUpdate))
			MemoryPatch::Write(into_slot, &idle_or_into_original,
				sizeof(idle_or_into_original));
		if (*strafe_end_slot == reinterpret_cast<void*>(&HookLedgeStrafeEndUpdate))
			MemoryPatch::Write(strafe_end_slot, &strafe_end_original,
				sizeof(strafe_end_original));
		if (*jump_slot == reinterpret_cast<void*>(&HookLedgeJumpUpdate))
			MemoryPatch::Write(jump_slot, &jump_original, sizeof(jump_original));
		m_ledge_observer_hooks_installed = false;
		m_original_ledge_idle_or_into_update = nullptr;
		m_original_ledge_strafe_end_update = nullptr;
		m_original_ledge_jump_update = nullptr;
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
		if (!InstallLedgeObserverHooks())
		{
			void* original = reinterpret_cast<void*>(kOriginalControllerUpdate);
			MemoryPatch::Write(fly_update_slot, &original, sizeof(original));
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
		RemoveLedgeObserverHooks();
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
