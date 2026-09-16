#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>

#include "retail/retail_types.h"
#include "protocol/rally_packets.h"
#include "shared_camera.h"

namespace coop
{
	class CoopNetGame;

	class Player2Module final
	{
	public:
		static Player2Module& Instance();

		bool Install();
		void Remove();
		void PublishDefaultModeActiveEntity(void* entity);
		bool EnsureNetworkPlayer2();
		// Game-thread-only local debug controls. They use the same recorded native
		// P1 spawn context as network P2 creation, but do not require a second game
		// process or a connected peer.
		bool EnsureLocalPlayer2ForDebug();
		bool EnableLocalAbrForDebug();
		// Called after the stock generic state dispatcher has completed a native
		// transition. It observes only positively identified inner Ledge/Climb
		// machines; it never selects or writes a state.
		void ObserveInnerStateSelection(void* state_machine,
			std::uint32_t selected_mode);
		// Called during native save load / transition to clear P2 state
		// and prevent crashes from stale entity pointers.
		void ResetForWorldLoad();
		// Applies a one-shot progression rally to the local representation. The
		// source process moves its remote P2 copy; the receiving process moves its
		// local P1. Both use the same finite root snapshot and cache invalidation.
		bool ApplyProgressionRallyToRemoteP2(const retail::Transform& transform,
			protocol::ProgressionRallyReason reason, std::uint32_t sequence);
		bool ApplyProgressionRallyToLocalP1(const retail::Transform& transform,
			protocol::ProgressionRallyReason reason, std::uint32_t sequence);

	private:
		// The stock factory ABI is the same four-float value already used by the
		// retail boundary.  Do not maintain a second look-alike Vec4 declaration.
		using Vec4 = retail::Vec4;

		enum class AttachmentFamily : std::uint8_t
		{
			None,
			Ledge,
			Climb
		};

		typedef void(__thiscall* ControllerUpdateFn)(void*);
		typedef int(__thiscall* LedgeStateUpdateFn)(void*);

		Player2Module();
		~Player2Module() = default;
		Player2Module(const Player2Module&) = delete;
		Player2Module& operator=(const Player2Module&) = delete;

		void* GetFlyEntity();
		void* GetController(void* entity);
		std::uint32_t GetModeId(void* controller);
		bool IsGPigDeathMode(void* controller) const;
		// A local P2 death must never run its own checkpoint respawn. Once the
		// authoritative peer is visibly back in Default, use the confirmed native
		// dispatcher to leave the frozen DeathMode instead of leaving its hide pose.
		bool TryRecoverRemoteP2Death(void* controller);

		bool SyncPlayer2WeaponSelection(void* player2);

		bool ApplyPlayer2WeaponSelection(void* player2, std::uint32_t weapon_type, const char* source);

		bool SpawnPlayer2FromSnapshot(const char* trigger);
		// Client-only role hand-off: the spawned Black Pig becomes the true local
		// P1 and the original Darwin becomes the packet-driven remote P2, on
		// ordinary levels and ABR tracks alike. The stripped P2 conflict mask
		// is restored on promotion; without it the native arbiter drops the
		// promoted controller to Inactive.
		bool PromoteClientBlackPigToPlayer1(void*& player1_controller);
		// Shared single-thread role hand-off plumbing. Swaps the two selectable
		// slots and rebinds the process-global active entity to the pre-swap
		// RemoteP2 entity, with rollback when the rebind fails. The spawn
		// context is deliberately untouched. Entity/mode IDs below are
		// identical across the FRE/USA/RUS data builds (only audio archives
		// and UI text differ), so this path has no per-language branch.
		bool RebindClientLocalPlayerTo(retail::EntityRef new_local_entity);
		void ResetClientRoleCaches();
		void RefreshPlayer1ControllerFromSlot(void*& player1_controller);
		void LogClientRoleGateOnce(const char* gate,
			std::uint32_t detail_a, std::uint32_t detail_b);
		bool TryEnsurePlayer2RdvTask(const char* source);
		bool TryEnsureRdvTaskForEntity(const char* source,
			retail::EntityRef entity, retail::EntityRef& configured_entity);
		bool TryEnterPlayer2AbrMode(void* controller);
		bool ConfigureRdvTaskForEntity(const char* source,
			retail::EntityRef entity, retail::HandlerRef handler,
			retail::MotorTaskRef task, retail::EntityRef& configured_entity);
		bool RestoreLocalModeContract(retail::ControllerRef controller,
			std::uint32_t mode_id);
		bool SetAbrDriveGate(void* player, bool active);
		bool SetLocalAbrPropulsion(void* player, float direction);

		void TickPlayer1(void* player1_controller);
		void HandlePlayer1ModeTransition(void* player1_controller);

		// Returns false when the checked per-controller setup could not be applied.
		// The caller retries on later game frames rather than treating a transient
		// read/write failure as a completed initialization.
		bool ConfigurePlayer2DefaultMode(void* controller);
		bool ApplyProgressionRally(retail::EntitySlot slot,
			const retail::Transform& transform,
			protocol::ProgressionRallyReason reason, std::uint32_t sequence,
			const char* recipient);
		void UpdateController(void* controller);
		// Returns true only when controller is the single native Mooch controller.
		// Keeping this route separate prevents the shared Fly lifecycle from being
		// mistaken for a fourth network player.
		bool UpdateFlyController(void* controller);
		void UpdateRemotePlayer2Controller(void* controller);
		bool RunStockControllerUpdate(void* controller, const char* context);
		int RunLedgeStateUpdate(void* ledge_state, LedgeStateUpdateFn original);
		void ObserveRemoteLedgeState(void* ledge_state);
		bool TryQueueRemoteAttachmentRelease(void* player2, void* controller,
			CoopNetGame& netgame, float& distance);
		bool InstallLedgeObserverHooks();
		void RemoveLedgeObserverHooks();
		void* SpawnGPig(const Vec4* position, const Vec4* rotation, std::uint32_t gpig_id, void* context);
		bool PatchSpawnCall(std::uintptr_t address, const BYTE expected[5], BYTE original[5]);
		bool PatchDefaultModeActivePublish();

		static void __fastcall HookControllerUpdate(void* controller, void*);
		static void* __cdecl HookSpawnGPig(const Vec4* position, const Vec4* rotation, std::uint32_t gpig_id, void* context);
		static int __fastcall HookLedgeIdleOrIntoUpdate(void* ledge_state, void*);
		static int __fastcall HookLedgeStrafeEndUpdate(void* ledge_state, void*);
		static int __fastcall HookLedgeJumpUpdate(void* ledge_state, void*);

		volatile LONG m_player2_ready;
		volatile LONG m_spawn_snapshot_ready;
		volatile LONG m_spawn_in_progress;
		bool m_player2_default_mode_initialized;
		bool m_player2_default_mode_setup_failure_logged;
		bool m_logged_blocked_active_publish;
		bool m_debug_player2_enabled;
		bool m_client_black_pig_promoted;
		LONG m_last_role_heartbeat_tick;
		LONG m_last_attachment_release_log_tick;
		bool m_client_role_gate_logged;
		retail::EntityRef m_client_black_pig_entity;
		retail::EntityRef m_client_original_darwin_entity;
		bool m_remote_p2_death_mode_observed;
		std::uint32_t m_remote_p2_death_mode_entry_sequence;
		SharedCameraCoordinator m_camera;

		std::uint32_t m_last_player1_mode;
		std::uint32_t m_remote_p2_attachment_active_tick;
		std::uint32_t m_remote_p2_attachment_release_divergence_begin_tick;
		bool m_remote_p2_attachment_active;
		AttachmentFamily m_remote_p2_attachment_family;
		retail::StateMachineRef m_remote_p2_attachment_state_machine;

		// This cache is written only after ConfigureRdvTaskForEntity verifies that
		// the stock configurator enabled the native task.  A merely allocated task
		// is not ready and must be retried after transient spawn-context failures.
		retail::EntityRef m_abr_native_task_configured_player2;
		bool m_player2_abr_mode_setup_failure_logged;
		// The retail ABR target-speed is preserved while co-op gates propulsion.
		// No entity transform or logical Jump/action is synthesized.
		bool m_local_abr_propulsion_locked;
		float m_local_abr_saved_target_speed;
		int m_local_abr_propulsion_direction;

		std::uint32_t m_last_weapon_type;
		std::uint32_t m_last_remote_p2_mode;
		Vec4 m_spawn_position;
		Vec4 m_spawn_rotation;
		retail::SpawnContextRef m_spawn_context;
		BYTE m_original_spawn_call1[5];
		BYTE m_original_spawn_call2[5];
		BYTE m_original_default_mode_active_stores[10];
		bool m_default_mode_active_stores_patched;
		bool m_ledge_observer_hooks_installed;
		LedgeStateUpdateFn m_original_ledge_idle_or_into_update;
		LedgeStateUpdateFn m_original_ledge_strafe_end_update;
		LedgeStateUpdateFn m_original_ledge_jump_update;
		ControllerUpdateFn m_original_update;
	};
}
