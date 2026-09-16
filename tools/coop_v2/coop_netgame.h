#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstddef>
#include <cstdint>

#include "ServerClient/MTypes.h"
#include "protocol/fly_packets.h"
#include "retail/retail_types.h"

class WorldSync;
namespace coop
{
	class CoopNetGame final
	{
	public:
		static CoopNetGame& Instance();

		void SetModeHost();
		void SetModeClient();
		bool IsHost() const;
		bool IsClient() const;
		bool HasRemotePeer() const;
		bool HasNativeLoadHook() const { return m_load_game_hooked; }

		void OnPeerConnected();
		void OnPeerDisconnected();
		void OnRemotePacket(const void* data, std::uint32_t size);
		// Socket-thread ingress validates and queues this reliable event. Native
		// Fly code is reached later on the exact game-thread tick.
		void OnRemoteFlyAbilityPacket(const void* data, std::uint32_t size);
		void NetworkTick();
		void GameTick();

		bool InstallInputHook();
		void RemoveInputHook();
		// Mirrors the retail audio language choice onto the hardcoded voice blob.
		// The EXE selects File_XXX.bin by its own language mechanic (0x458A40)
		// but always asks for \data\File_RUS.000 (0x6F858C). Whichever language
		// index the game actually opened is reused for the blob, so every player
		// hears their own language (FRE/USA/RUS/...) with zero configuration.
		// Forces the game's own language selector (0x4596A0) to the coop.ini
		// [language] language when a 3-letter code is configured. Text, audio
		// requests and the File_XXX.bin choice then follow natively.
		bool InstallLanguageSelectHook();
		void RemoveLanguageSelectHook();

		void BeginRemoteInput();
		void EndRemoteInput();
		// ABR keeps its own vehicle motor. This scope exposes only the replicated
		// logical Fire level/edges to its native combat machine.
		void BeginRemoteAbrFireInput();
		// If the P2 dispatcher skipped its registered outer ABR update, run that
		// exact native mode once for a fresh remote Fire edge.
		bool RunRemoteAbrFireFallback(void* controller);
		// Poll the retail movement axes through the current binding. The signed
		// result is +1 for forward propulsion, -1 for reverse and 0 for stop.
		bool PollLocalAbrPropulsionDirection(float& direction);
		void ResetForWorldLoad();

		// The stock ammo-consume callback may run after P2's controller scope has
		// returned.  Record P2's concrete WeaponAmmoItem pointer while that scope is
		// active, so P1 can never be protected merely because it fired nearby in time.
		void ArmRemoteP2AmmoOwner(void* player2);

		// The stock P2 mode reloads [0x9905CC] instead of using its explicit pad
		// argument. Scope that global to P2's private XGamePad for one synchronous
		// controller tick, then restore the physical P1 pad before Fly_Active runs.
		bool BeginRemoteGamePadScope(void*& primary_gamepad);
		void EndRemoteGamePadScope(void* primary_gamepad);

		// The confirmed Darwin-to-Mooch hand-off must settle before P2 enters its
		// next packet-driven stock tick.
		void BeginLocalInputCapture();
		SHORT HandleGetAsyncKeyState(int virtual_key);

		// Local transform>
		void PublishLocalPlayerTransform(const void* player);
		void PublishLocalPlayerMode(std::uint32_t mode);

		// Mooch ownership is latched only after the EXE has actually selected its
		// one-frame switch mode.  The active-entity globals are transient during
		// that hand-off and must not decide who publishes the shared fly.
		void ConfirmLocalFlyControl();

		// 0x61000034/0x61000033 are short Respawn/Orbit signals. Read both sides
		// of the fly's tick so the hand-off latch cannot miss the transition.
		// Fly_Deactivated is a local native transition, not proof that a peer's
		// shared Mooch died: only a local owner may publish the zero-owner exit.
		void ObserveLocalFlyMode(std::uint32_t mode_before, std::uint32_t mode_after);
		bool IsLocalFlyControlled() const;

		// The peer's Mooch ownership is still used for presentation-transform
		// selection, but it must not switch the receiver's controller or camera.
		bool IsRemoteFlyControlled() const;
		// Game-thread-only replay for a peer-owned Mooch dual-laser event. The
		// primary path runs the registered Fly_Active update with a synthetic raw
		// button edge, but never selects/enters the mode or assigns the camera/HUD.
		// A direct route-item pulse remains only as an explicitly logged fallback
		// for a profile/runtime mismatch.
		void BeginRemoteFlyDualLaserPresentationTick();
		// Writes the owner direction into FlyFly's native body state, then invokes
		// its terminal LookAt submitter after the remote root transform. It never
		// calls Fly_Active or touches camera/HUD/controller ownership on receiver.
		bool ApplyRemoteFlyAimMotor(void* fly);
		bool ApplyRemoteFlyDualLaserPresentation(void* fly);
		// Network ingress records a real, ordered remote-owner -> zero-owner edge
		// under m_input_lock.  The exact Mooch game-thread tick consumes it once,
		// after both sides are non-owners, before asking the retail dispatcher for
		// the normal Fly_Deactivated transition.
		bool ConsumeRemoteFlyZeroOwnerTransition(std::uint32_t& input_sequence);
		bool SetFlyControlActiveState(void* fly, bool active) const;
		// Fly_Active::Enter normally publishes Mooch as both active entities, but
		// P1's still-ticking Default controller later overwrites those globals.  Keep
		// the native fly ownership published after its own tick; never manufacture
		// its transform or rotation.
		void MaintainLocalFlyActiveEntity(void* fly);
		void PublishLocalFlyTransform(const void* fly);

		// Publishes the yaw read from 0x52AD20 after whichever local native
		// controller currently owns the shared camera: P1 normally, Mooch after its
		// own Fly tick. The caller must not publish P1's stale yaw during local Fly.
		void PublishLocalCameraYaw(float yaw, bool valid);
		// Fly_Active maintains its own XGamePad ray instead of passing through the
		// normal Default-controller hook. Publish it every locally owned Fly tick so
		// a receiver can reconstruct the native body aim target continuously.
		void PublishLocalFlyAimRay();
		// RDV reads the same process-global XGamePad ray but does not pass through
		// Default-mode's capture hook. Publish it after every local ABR tick.
		void PublishLocalAbrAimRay();
		// Darwin's normal on-foot controller owns this correction. Vehicle/RDV uses
		// a separate native motor path and must never be fed through this helper.
		bool ApplyRemotePlayerTransform(void* player2);
		// Reapplies the exact interpolated root prepared before this P2 update.
		// This prevents physics from undoing the correction without performing a
		// second interpolation step in the same rendered frame.
		bool ReapplyRemotePlayerFrameTransform(void* player2);
		// ABR/RDV still publishes P1's settled root transform through the ordinary
		// input snapshot. The receiver applies that complete root after its own
		// stock vehicle tick, without borrowing on-foot input, camera or weapon code.
		bool ApplyRemoteAbrTransform(void* player2);
		// Valid only during a scoped remote P2 controller tick. This exposes the
		// finite peer target to the Ledge recovery gate without copying its private
		// input packet into Player2Module.
		bool GetActiveRemotePlayerTransform(retail::Transform& transform,
			std::uint32_t& transform_sequence, std::uint32_t& player_mode) const;
		bool HasActiveRemotePressedEdge(std::uint32_t action) const;
		// Supplies one synthetic instance of the existing logical Ledge-release
		// edge to the stock P2 update. It is scoped to one P2 tick and never sends
		// a fake physical key or invokes the Ledge machine directly.
		bool ArmRemoteLedgeReleaseEdge();
		bool FinishRemoteLedgeReleaseEdge(bool& consumed);

		// The shared Mooch is an entity, not a noclip camera. Apply its complete
		// networked transform after the native fly controller tick.
		bool ApplyRemoteFlyTransform(void* fly);

		// Game-thread-only replay of the exact native trigger spawn route.  WorldSync
		// uses it only after a host event cannot be paired to a client-native entity.
		bool SpawnWorldFromTrigger(void* trigger);
		// Runs the original trigger dispatcher through the same bookkeeping and
		// network replication path used by a real map event.
		bool DispatchWorldTriggerEvent(void* trigger, int event_code);
		bool ReplayTriggerEvent(std::uint32_t family, std::uint32_t subtype, std::int32_t definition_id, std::uint32_t occurrence, int event_code);
		// Game-thread-only replay of a matched native route. `route` retains relay,
		// direct-forwarder, or host entity-dispatcher activation; replay is scoped so
		// any nested local callback cannot echo a packet to the sender.
		bool ReplayObjectEvent(void* source, int event_code,
			std::uint32_t route);

		// Applies a remote player's damage to the local twin of a world entity by
		// driving the stock trigger event dispatcher.
		void ApplyRemoteDamage(void* trigger, std::uint32_t amount, std::uint32_t world_id, int event_code);
		bool GetActiveRemoteWeaponType(std::uint32_t& weapon_type) const;

		// Copies the last received packet ray under the existing input lock. It is
		// diagnostic-only and never applies, normalizes, or transmits any value.
		bool GetRemoteAimRaySnapshot(float origin[3], float direction[3], std::uint32_t& transform_sequence) const;
		// Narrow read-only ownership/lifecycle view for Player2Module. Feature code
		// must not copy the whole private input packet merely to decide whether a
		// locally divergent P2 may leave a native death state.
		bool GetRemotePlayerModeSnapshot(std::uint32_t& transform_sequence,
			std::uint32_t& player_mode) const;
		bool __fastcall HandleInputActionQuery(void* input_manager, void*, std::uint32_t device, std::uint32_t action, std::uint32_t flags, std::uintptr_t caller_return_address);
		bool __fastcall HandleInputActionUpQuery(void* input_manager, void*, std::uint32_t device, std::uint32_t action, std::uint32_t flags);
		bool __fastcall HandleInputThresholdQuery(void* input_manager, void*, std::uint32_t device, std::uint32_t action, float threshold, std::uint32_t flags);
		bool __fastcall HandleInputPressedQuery(void* input_manager, void*, std::uint32_t device, std::uint32_t action, std::uint32_t flags, std::uintptr_t caller_return_address);
		bool __fastcall HandleInputReleasedQuery(void* input_manager, void*, std::uint32_t device, std::uint32_t action, std::uint32_t flags);
		bool __fastcall HandleInputHoldDurationQuery(void* input_manager, void*, std::uint32_t device, std::uint32_t action, float threshold, std::uint32_t flags);
		
		// 0x488B00 takes flags BEFORE the float, unlike 0x488DC0.
		bool __fastcall HandleInputAimHoldQuery(void* input_manager, void*, std::uint32_t device, std::uint32_t action, std::uint32_t flags, float threshold);
		bool __fastcall HandleInputRawPressedQuery(void* input_manager, void*, void* device, std::uint32_t action, std::uint32_t flags, bool record, std::uintptr_t caller_return_address);
		bool __fastcall HandleInputRawReleasedQuery(void* input_manager, void*, void* device, std::uint32_t action, std::uint32_t flags, bool record, std::uintptr_t caller_return_address);
		bool __fastcall HandleInputRawHeldQuery(void* input_manager, void*, void* device, std::uint32_t action, std::uint32_t flags, bool record, std::uintptr_t caller_return_address);
		float __fastcall HandleInputAxisQuery(void* input_manager, void*, std::uint32_t device, std::uint32_t axis, std::uint32_t flags);
		
		// 0x52AD20, __thiscall(camera handler), float in st(0).
		float __fastcall HandleCameraYawQuery(void* camera_handler, void*);
		
		// 0x5BCF30, __thiscall(mode), no stack arguments.
		void __fastcall HandleGPigCameraUpdate(void* mode, void*);
	private:
		typedef SHORT(WINAPI* GetAsyncKeyStateFn)(int);
		typedef bool(__thiscall* InputActionQueryFn)(void*, std::uint32_t, std::uint32_t, std::uint32_t);
		typedef bool(__thiscall* InputThresholdQueryFn)(void*, std::uint32_t, std::uint32_t, float, std::uint32_t);
		typedef bool(__thiscall* InputAimHoldQueryFn)(void*, std::uint32_t, std::uint32_t, std::uint32_t, float);
		typedef bool(__thiscall* InputRawQueryFn)(void*, void*, std::uint32_t, std::uint32_t, bool);
		typedef float(__thiscall* InputAxisQueryFn)(void*, std::uint32_t, std::uint32_t, std::uint32_t);
		typedef bool(__thiscall* StateMachineSelectStateFn)(void*, std::uint32_t, bool);
		typedef bool(__thiscall* AbrAttackPredicateFn)(void*);
		typedef bool(__thiscall* NativeSaveLoadFn)(void*, std::uint32_t);
		typedef float(__thiscall* CameraYawFn)(void*);
		typedef void(__thiscall* GPigCameraUpdateFn)(void*);
		typedef void(__thiscall* DefaultModeUpdateFn)(void*, void*, void*);
		typedef void(__thiscall* FireHandlerFn)(void*, void*, void*);
		typedef void(__thiscall* WeaponAmmoConsumeFn)(void*);
		typedef void(__thiscall* TriggerSpawnFromDefinitionFn)(void*);
		typedef void* (__cdecl* TriggerFactoryFn)(std::uint32_t, std::uint32_t, void*);
		typedef int(__thiscall* TriggerEventFn)(void*, int);
		typedef int(__thiscall* GlobalEventForwarderFn)(void*, void*, int);
		typedef int(__cdecl* ObjectEventRelayFn)(void*, int);
		// `sub_46D6F0` takes the global event receiver in ECX, then source/event on
		// the stack. The former stdcall declaration accidentally relied on ECX
		// surviving the hook; preserve the actual __thiscall ABI instead.
		typedef int(__thiscall* ObjectEventForwarderFn)(void*, void*, int);
		typedef void(__thiscall* LanguageSelectFn)(void*, std::uint32_t,
			std::uint32_t);
		typedef void(__thiscall* HealthComponentSetFn)(void*, float, std::uint32_t, bool);
		typedef void(__thiscall* HealthComponentAddFn)(void*, float, std::uint32_t);
		typedef void(__thiscall* HealthComponentSubtractFn)(void*, float, std::uint32_t);
		enum Role
		{
			RoleNone,
			RoleHost,
			RoleClient
		};
		enum
		{
			kFlyAbilityQueueCapacity = 8
		};
		struct FlyAbilityQueueEntry final
		{
			protocol::FlyAbilityPacket packet;
			DWORD received_tick;
		};
		enum
		{
			kInputEdgeTraceSlotCount = 16
		};
		struct InputEdgeTraceSlot final
		{
			std::uint32_t action;
			std::uintptr_t caller_return_address;
			DWORD tick;
			bool raw;
		};
		enum
		{
			kObjectDiagnosticTraceSlotCount = 16
		};
		struct ObjectDiagnosticTraceSlot final
		{
			std::uint32_t route;
			std::uint32_t event_code;
			std::uintptr_t caller_return_address;
			std::uintptr_t object_vtable;
			DWORD tick;
		};

		CoopNetGame();
		~CoopNetGame() = default;
		CoopNetGame(const CoopNetGame&) = delete;
		CoopNetGame& operator=(const CoopNetGame&) = delete;

		void CaptureLocalInput(CoopInput& input) const;
		void CaptureLocalAnalogAxis(std::uint32_t axis, float value);
		void CaptureLocalAction(std::uint32_t action, bool is_down);
		void CaptureLocalPress(std::uint32_t action);
		void CaptureLocalRelease(std::uint32_t action);
		bool ClaimLocalInputEdgeTrace(std::uint32_t action,
			std::uintptr_t caller_return_address, bool raw);
		bool ClaimObjectDiagnosticTrace(std::uint32_t route,
			std::uint32_t event_code,
			std::uintptr_t caller_return_address,
			std::uintptr_t object_vtable);
		void CaptureLocalFlyRaw(std::uint32_t action, bool is_down, bool pressed_edge, bool released_edge);
		void CaptureLocalAimRay(const void* input_manager);
		void* GetRemoteGamePad();
		bool ApplyActiveRemoteAimRay(void* input_manager, retail::AimRay& saved_ray) const;
		void RestoreAimRay(void* input_manager, const retail::AimRay& saved_ray) const;
		void HandleDefaultModeUpdate(void* mode, void* input_manager, void* mode_context);
		bool HandleStateMachineSelectState(void* controller, std::uint32_t mode_id,
			bool force_reselect);
		bool HandleAbrAttackPredicate(void* mode);
		void HandleFireHandler(void* mode, void* input_manager, void* mode_context);
		void HandleWeaponAmmoConsume(void* weapon_record);
		
		// Read-only trace for the two native Handler+0x5A0 health-component
		// mutators.  It logs only components that are proven to belong to P1/P2.
		void HandleHealthComponentSet(void* component, float requested_value, std::uint32_t slot, bool notify, void* caller);
		void HandleHealthComponentAdd(void* component, float delta, std::uint32_t slot, void* caller);
		void HandleHealthComponentSubtract(void* component, float amount, std::uint32_t slot, void* caller);

		// Both NPC and monster trigger vtables reach 0x41F220.  The hook preserves
		// the native call, then hands its trigger-to-live-entity result to WorldSync.
		void HandleTriggerSpawnFromDefinition(void* trigger);
		void* HandleTriggerFactory(std::uint32_t family, std::uint32_t subtype, void* output);
		int HandleTriggerEvent(void* trigger, int event_code);
		int HandleGlobalEventForwarder(void* receiver, void* source, int event_code);
		int HandleObjectEventRelay(void* source, int event_code,
			std::uintptr_t caller_return_address);
		int HandleObjectEventForwarder(void* receiver, void* object, int event_code,
			std::uintptr_t caller_return_address);
		bool GetActiveRemoteAction(std::uint32_t action) const;
		bool GetActiveRemoteHold(std::uint32_t action, float threshold) const;
		// During a scoped native tick this must answer from m_active_remote_input,
		// never from a packet the network worker received half-way through the tick.
		bool IsRemoteFlyControlledForInputQuery() const;
		// Only the shared Mooch can be presentation-only. A local owner must retain
		// its real death/respawn route even if an old peer packet still says Fly.
		bool IsRemotePresentationMoochController(void* controller) const;
		bool GetRemoteFlyRawHeld(std::uint32_t action) const;
		bool ConsumeRemoteFlyRawEdge(std::uint32_t action, bool pressed);
		bool ReadFlyControlActiveState(void* fly, bool& active) const;
		bool RunFlyNativeDualLaserPass(void* fly, const float target[3],
			bool remote_owner);
		bool ApplyDirectFlyDualLaserPulse(void* fly, const float target[3],
			bool remote_owner);
		bool RememberFlyDualLaserPulse(void* fly, bool remote_owner);
		bool IsFlyNativeAbilityPassActiveOnThisThread() const;
		bool IsFlyNativeAbilityPassActiveForController(void* controller) const;
		void QueueLocalFlyDualLaserEvent(const void* input_manager);
		bool ConsumeReadyRemoteFlyDualLaserEvent(
			std::uint32_t remote_fly_transform_sequence,
			protocol::FlyAbilityPacket& event);
		void ClearRemoteFlyDualLaserPulse();
		void SendQueuedFlyAbilityPackets();
		void ClearFlyAbilityQueues();
		void ClearIncomingFlyAbilityEvents();
		static void ClearFlyInputLocked(CoopInput& input);
		// Caller holds m_input_lock. Bump the ordinary snapshot sequence together
		// with a local Fly exit, so an already-sent live Fly packet cannot win over
		// the zero-owner state before P1's next controller tick.
		void ClearLocalFlyOwnershipLocked();
		bool IsMoochAction(std::uint32_t action) const;

		// The same logical Mooch action that enters the fly returns from it.  Once a
		// native entry is confirmed, the next local edge releases only our ownership
		// latch; the stock fly controller still performs its own normal exit.
		
		bool ConsumeLocalMoochFlyExit();
		// True for actions that P2 must never replay.  Mooch changes process-global
		// ownership; the map is a local UI action.  Neither belongs in a remote tick.
		bool IsMirrorSuppressedAction(std::uint32_t action) const;
		bool GetActiveRemoteCameraYaw(float& yaw) const;
		void SendLocalInput();
		bool GetRemoteInput(CoopInput& input) const;
		void ApplyRemoteKeyboardState();
		void RestoreKeyboardState();
		void BuildRemoteScanCodeState();
		bool IsRemoteInputActiveOnThisThread() const;
		bool InstallStateMachineSelectStateHook();
		void RemoveStateMachineSelectStateHook();
		bool InstallActionQueryHook();
		void RemoveActionQueryHook();
		bool InstallActionUpQueryHook();
		void RemoveActionUpQueryHook();
		bool InstallThresholdQueryHook();
		void RemoveThresholdQueryHook();
		bool InstallAxisQueryHook();
		void RemoveAxisQueryHook();
		bool InstallPressedQueryHook();
		void RemovePressedQueryHook();
		bool InstallAbrAttackPredicateHook();
		void RemoveAbrAttackPredicateHook();
		bool InstallReleasedQueryHook();
		void RemoveReleasedQueryHook();
		bool InstallHoldDurationQueryHook();
		void RemoveHoldDurationQueryHook();
		bool InstallAimHoldQueryHook();
		void RemoveAimHoldQueryHook();
		bool InstallRawPressedQueryHook();
		void RemoveRawPressedQueryHook();
		bool InstallRawReleasedQueryHook();
		void RemoveRawReleasedQueryHook();
		bool InstallRawHeldQueryHook();
		void RemoveRawHeldQueryHook();
		bool InstallCameraYawHook();
		void RemoveCameraYawHook();
		bool InstallGPigCameraUpdateHook();
		void RemoveGPigCameraUpdateHook();
		bool InstallDefaultModeUpdateHook();
		void RemoveDefaultModeUpdateHook();
		bool InstallFireHandlerHook();
		void RemoveFireHandlerHook();
		bool InstallWeaponAmmoConsumeHook();
		void RemoveWeaponAmmoConsumeHook();
		bool InstallHealthComponentSetHook();
		void RemoveHealthComponentSetHook();
		bool InstallHealthComponentAddHook();
		void RemoveHealthComponentAddHook();
		bool InstallHealthComponentSubtractHook();
		void RemoveHealthComponentSubtractHook();
		bool IsVehicleMotorActiveForRemoteP2(const CoopInput& remote) const;

		bool InstallTriggerSpawnHook();
		void RemoveTriggerSpawnHook();
		bool InstallTriggerFactoryHook();
		void RemoveTriggerFactoryHook();
		bool InstallTriggerEventHook();
		void RemoveTriggerEventHook();
		bool InstallGlobalEventForwarderHook();
		void RemoveGlobalEventForwarderHook();
		bool InstallObjectEventRelayHook();
		void RemoveObjectEventRelayHook();
		bool InstallObjectEventForwarderHook();
		void RemoveObjectEventForwarderHook();
		bool InstallLoadGameHook();
		void RemoveLoadGameHook();

		// Generic 5-byte E9 detour installer.  relocate_len original bytes are copied
		// into a freshly allocated trampoline which then jumps back to address +
		// relocate_len; relocate_len must be >= 5 and cover whole instructions.  The
		// original entry (callable via the trampoline) is returned in *trampoline_out.
		bool InstallJmpHookRaw(std::uintptr_t address, const std::uint8_t* expected, std::size_t relocate_len, void* hook, BYTE* saved_bytes, BYTE** trampoline_out, const char* label);
		void RemoveJmpHookRaw(std::uintptr_t address, const BYTE* saved_bytes, std::size_t relocate_len, BYTE** trampoline_ptr);
		float GetRemoteAnalogAxis(std::uint32_t axis) const;
		bool IsGameForeground() const;
		static void __fastcall HookDefaultModeUpdate(void* mode, void*, void* input_manager, void* mode_context);
		static bool __fastcall HookStateMachineSelectState(void* controller, void*,
			std::uint32_t mode_id, bool force_reselect);
		static bool __fastcall HookAbrAttackPredicate(void* mode, void*);
		static void __fastcall HookFireHandler(void* mode, void*, void* input_manager, void* mode_context);
		static void __fastcall HookWeaponAmmoConsume(void* weapon_record, void*);
		static void __fastcall HookHealthComponentSet(void* component, void*, float requested_value, std::uint32_t slot, bool notify);
		static void __fastcall HookHealthComponentAdd(void* component, void*, float delta, std::uint32_t slot);
		static void __fastcall HookHealthComponentSubtract(void* component, void*, float amount, std::uint32_t slot);
		static void __fastcall HookTriggerSpawnFromDefinition(void* trigger, void*);
		static bool __fastcall HookNativeSaveLoad(void* manager, void*, std::uint32_t slot);

		static void __fastcall HookLanguageSelect(void* manager, void*,
			std::uint32_t language, std::uint32_t arg);
		// Maps a forced 3-letter code to the game language byte (USA 0, FRE 6,
		// GER 7, ITA 8, SPA 0xB, RUS 0xF, DUT 4, CZE 0x12, POL 0x13, BRA 0x17).
		// Returns the original byte to keep the game's own choice.
		static int MapForcedGameLanguage(int original);

		static void* __cdecl HookTriggerFactory(std::uint32_t family, std::uint32_t subtype, void* output);
		static int __fastcall HookTriggerEvent(void* trigger, void*, int event_code);
		static int __fastcall HookGlobalEventForwarder(void* receiver, void*,
			void* source, int event_code);
		static int __cdecl HookObjectEventRelay(void* source, int event_code);
		static int __fastcall HookObjectEventForwarder(void* receiver, void*,
			void* object, int event_code);

		volatile LONG m_role;
		volatile LONG m_remote_connected;
		mutable SRWLOCK m_input_lock;
		mutable SRWLOCK m_fly_ability_lock;
		CoopInput m_remote_input;
		CoopInput m_active_remote_input;
		CoopInput m_local_input;
		retail::KeyboardStateBuffers m_keyboard_state_buffers;
		retail::KeyboardStateSnapshot m_saved_keyboard_state;
		retail::KeyboardStateSnapshot m_saved_keyboard_state_secondary;
		retail::KeyboardStateSnapshot m_active_remote_scan_codes;
		DWORD m_last_send_tick;
		DWORD m_last_remote_transform_apply_tick;
		retail::Transform m_remote_player_frame_transform;
		void* m_remote_player_frame_entity;
		std::uint32_t m_remote_player_frame_sequence;
		bool m_remote_player_frame_transform_valid;
		// Throttles malformed snapshot diagnostics on the network worker. A rejected
		// snapshot never replaces the last finite P2/Fly state.
		DWORD m_last_invalid_input_trace_tick;
		// Game-thread-only throttle for the narrow receiver-side Fly_Deactivated
		// guard. A visible log is evidence that a local death transition was blocked.
		DWORD m_last_remote_fly_deactivation_suppression_tick;
		DWORD m_remote_input_thread_id;

		std::uint32_t m_local_transform_sequence;
		std::uint32_t m_local_fly_transform_sequence;
		// Input packets use Steam's unreliable lane. This is the latest accepted
		// sender snapshot sequence; stale packets must not undo newer P2/Mooch
		// transforms, rotations, or a later zero-owner Fly exit.
		std::uint32_t m_last_accepted_remote_input_sequence;
		// An accepted raw fly_controlled 1 -> 0 edge.  This is deliberately the
		// whole-input sequence rather than a Fly transform sequence: a valid owner
		// exit clears fly_transform_sequence to zero.  It is consumed only by the
		// exact Mooch controller on the game thread.
		std::uint32_t m_pending_remote_fly_zero_owner_input_sequence;
		FlyAbilityQueueEntry m_outgoing_fly_abilities[kFlyAbilityQueueCapacity];
		FlyAbilityQueueEntry m_incoming_fly_abilities[kFlyAbilityQueueCapacity];
		std::uint32_t m_outgoing_fly_ability_head;
		std::uint32_t m_outgoing_fly_ability_count;
		std::uint32_t m_incoming_fly_ability_head;
		std::uint32_t m_incoming_fly_ability_count;
		std::uint32_t m_local_fly_ability_sequence;
		std::uint32_t m_last_remote_fly_ability_sequence;
		retail::FlyDualLaserRouteItemRef m_remote_fly_laser_route_items[2];
		std::uint32_t m_remote_fly_laser_item_ids[2];
		bool m_remote_fly_laser_pulse_active;
		bool m_remote_ledge_release_edge_armed;
		bool m_remote_ledge_release_edge_consumed;
		volatile LONG m_fly_native_pass_active;
		volatile LONG m_fly_native_synthetic_press_mask;
		DWORD m_fly_native_pass_thread_id;
		void* m_fly_native_pass_controller;
		retail::FlyDualLaserRouteItemRef m_debug_fly_laser_route_items[2];
		std::uint32_t m_debug_fly_laser_item_ids[2];
		bool m_debug_fly_laser_pulse_active;
		bool m_local_fly_active_seen;
		bool m_local_mooch_exit_key_down;
		bool m_local_fly_deactivation_seen;

		bool m_logged_fly_active_entity_repair;
		std::uint32_t m_local_weapon_sequence;
		std::uint32_t m_last_local_weapon_type;
		volatile LONG m_peer_connected_tick;
		volatile LONG m_logged_spawn;
		volatile LONG m_remote_input_active;
		bool m_keyboard_state_swapped;
		bool m_logged_keyboard_state_swap;
		bool m_input_hooked;
		bool m_state_machine_select_state_hooked;
		bool m_action_query_hooked;
		bool m_action_up_query_hooked;
		bool m_threshold_query_hooked;
		bool m_axis_query_hooked;
		bool m_pressed_query_hooked;
		bool m_abr_attack_predicate_hooked;
		bool m_abr_attack_predicate_seen_in_scope;
		bool m_remote_abr_aim_ray_applied;
		retail::GamePadRef m_remote_abr_aim_input_manager;
		retail::AimRay m_remote_abr_saved_aim_ray;
		bool m_released_query_hooked;
		bool m_hold_duration_query_hooked;
		bool m_aim_hold_query_hooked;
		bool m_camera_yaw_hooked;
		bool m_gpig_camera_update_hooked;
		bool m_default_mode_update_hooked;
		bool m_logged_remote_gamepad;
		bool m_remote_gamepad_unavailable;
		void* m_remote_gamepad;
		ULONG_PTR* m_async_key_state_iat_slot;
		GetAsyncKeyStateFn m_original_get_async_key_state;
		BYTE m_original_language_select_bytes[8];
		BYTE* m_language_select_trampoline;
		LanguageSelectFn m_original_language_select;
		bool m_language_select_hooked;
		bool m_logged_lang_override;
		BYTE m_original_state_machine_select_state_bytes[5];
		BYTE* m_state_machine_select_state_trampoline;
		StateMachineSelectStateFn m_original_state_machine_select_state;
		BYTE m_original_input_action_query_bytes[5];
		BYTE* m_input_action_trampoline;
		InputActionQueryFn m_original_input_action_query;
		BYTE m_original_input_action_up_query_bytes[5];
		BYTE* m_input_action_up_trampoline;
		InputActionQueryFn m_original_input_action_up_query;
		BYTE m_original_input_threshold_query_bytes[5];
		BYTE* m_input_threshold_trampoline;
		InputThresholdQueryFn m_original_input_threshold_query;
		BYTE m_original_input_axis_query_bytes[5];
		BYTE* m_input_axis_trampoline;
		InputAxisQueryFn m_original_input_axis_query;
		
		// Pressed-edge query (0x488CE0) and released-edge query (0x488C00) both have a
		// 6-byte relocation prologue; hold-duration (0x488E50) relocates 5.
		BYTE m_original_input_pressed_query_bytes[6];
		BYTE* m_input_pressed_trampoline;
		InputActionQueryFn m_original_input_pressed_query;
		BYTE m_original_abr_attack_predicate_bytes[9];
		BYTE* m_abr_attack_predicate_trampoline;
		AbrAttackPredicateFn m_original_abr_attack_predicate;
		BYTE m_original_input_released_query_bytes[6];
		BYTE* m_input_released_trampoline;
		InputActionQueryFn m_original_input_released_query;
		BYTE m_original_input_hold_duration_query_bytes[5];
		BYTE* m_input_hold_duration_trampoline;
		InputThresholdQueryFn m_original_input_hold_duration_query;
		BYTE m_original_input_aim_hold_query_bytes[5];
		BYTE* m_input_aim_hold_trampoline;
		InputAimHoldQueryFn m_original_input_aim_hold_query;
		BYTE m_original_input_raw_pressed_query_bytes[5];
		BYTE* m_input_raw_pressed_trampoline;
		InputRawQueryFn m_original_input_raw_pressed_query;
		BYTE m_original_input_raw_released_query_bytes[5];
		BYTE* m_input_raw_released_trampoline;
		InputRawQueryFn m_original_input_raw_released_query;
		BYTE m_original_input_raw_held_query_bytes[5];
		BYTE* m_input_raw_held_trampoline;
		InputRawQueryFn m_original_input_raw_held_query;
		bool m_raw_pressed_query_hooked;
		bool m_raw_released_query_hooked;
		bool m_raw_held_query_hooked;
		
		// 0x52AD20 relocates nine bytes, 0x5BCF30 five.
		BYTE m_original_camera_yaw_bytes[9];
		BYTE* m_camera_yaw_trampoline;
		CameraYawFn m_original_camera_yaw;
		BYTE m_original_gpig_camera_update_bytes[5];
		BYTE* m_gpig_camera_update_trampoline;
		GPigCameraUpdateFn m_original_gpig_camera_update;
		BYTE m_original_default_mode_update_bytes[7];
		BYTE* m_default_mode_update_trampoline;
		DefaultModeUpdateFn m_original_default_mode_update;
		BYTE m_original_fire_handler_bytes[7];
		BYTE* m_fire_handler_trampoline;
		FireHandlerFn m_original_fire_handler;
		bool m_fire_handler_hooked;
		BYTE m_original_weapon_ammo_consume_bytes[10];
		BYTE* m_weapon_ammo_consume_trampoline;
		WeaponAmmoConsumeFn m_original_weapon_ammo_consume;
		bool m_weapon_ammo_consume_hooked;
		BYTE m_original_health_component_set_bytes[6];
		BYTE* m_health_component_set_trampoline;
		HealthComponentSetFn m_original_health_component_set;
		bool m_health_component_set_hooked;
		BYTE m_original_health_component_add_bytes[6];
		BYTE* m_health_component_add_trampoline;
		HealthComponentAddFn m_original_health_component_add;
		bool m_health_component_add_hooked;
		BYTE m_original_health_component_subtract_bytes[6];
		BYTE* m_health_component_subtract_trampoline;
		HealthComponentSubtractFn m_original_health_component_subtract;
		bool m_health_component_subtract_hooked;
		BYTE m_original_trigger_spawn_bytes[14];

		BYTE* m_trigger_spawn_trampoline;
		TriggerSpawnFromDefinitionFn m_original_trigger_spawn;
		bool m_trigger_spawn_hooked;
		BYTE m_original_trigger_factory_bytes[14];
		BYTE* m_trigger_factory_trampoline;
		TriggerFactoryFn m_original_trigger_factory;
		bool m_trigger_factory_hooked;
		BYTE m_original_trigger_event_bytes[11];
		BYTE* m_trigger_event_trampoline;
		TriggerEventFn m_original_trigger_event;
		bool m_trigger_event_hooked;
		BYTE m_original_global_event_forwarder_bytes[8];
		BYTE* m_global_event_forwarder_trampoline;
		GlobalEventForwarderFn m_original_global_event_forwarder;
		bool m_global_event_forwarder_hooked;
		BYTE m_original_object_event_relay_bytes[8];
		BYTE* m_object_event_relay_trampoline;
		ObjectEventRelayFn m_original_object_event_relay;
		bool m_object_event_relay_hooked;
		BYTE m_original_object_event_forwarder_bytes[5];
		BYTE* m_object_event_forwarder_trampoline;
		ObjectEventForwarderFn m_original_object_event_forwarder;
		bool m_object_event_forwarder_hooked;
		bool m_load_game_hooked;
		bool m_logged_remote_transform;
		bool m_logged_remote_abr_transform;
		BYTE m_original_native_save_load_bytes[7];
		BYTE* m_native_save_load_trampoline;
		NativeSaveLoadFn m_original_native_save_load;

		std::uint32_t m_prev_local_action_down[3];
		std::uint32_t m_prev_remote_action_down[3];
		
		// Per-action press/release edge tracking.  m_prev_remote_*_seq holds the last
		// consumed sender counter; comparing it to the active packet's counter yields
		// a rising/falling edge that is latched into m_remote_*_edge for exactly one
		// P2 frame (recomputed in BeginRemoteInput).  m_local_*_recorded prevents a
		// single physical edge from being counted twice when the engine polls the
		// same edge query more than once inside one P1 frame.
		std::uint8_t m_prev_remote_press_seq[kCoopActionCount];
		std::uint8_t m_prev_remote_release_seq[kCoopActionCount];
		bool m_remote_press_edge[kCoopActionCount];
		bool m_remote_release_edge[kCoopActionCount];
		
		// Reconstructed hold timing for the threshold (0x488DC0) and hold-duration
		// (0x488E50) queries, which answer "held for >= N seconds".  P2 has no local
		// hold timer for the remote player, so it derives one from the held-level
		// stream: the wall-clock tick when action_down last transitioned 0->1, plus a
		// currently-held flag.  This stops charge/hold weapons (the melee whip's
		// special, flamethrower windup) from reading as instantly and continuously
		// charged on P2 the moment the remote fire button goes down.
		DWORD m_remote_hold_start_tick[kCoopActionCount];
		bool m_remote_action_held[kCoopActionCount];
		std::uint32_t m_local_press_recorded[3];
		std::uint32_t m_local_release_recorded[3];
		InputEdgeTraceSlot m_input_edge_trace_slots[kInputEdgeTraceSlotCount];
		ObjectDiagnosticTraceSlot m_object_diagnostic_trace_slots[
			kObjectDiagnosticTraceSlotCount];
		std::uint8_t m_prev_remote_fly_raw_press_seq[kCoopFlyRawActionCount];
		std::uint8_t m_prev_remote_fly_raw_release_seq[kCoopFlyRawActionCount];
		bool m_logged_remote_p2_ammo_restore;
		void* m_remote_p2_weapon_record;
	};
}
