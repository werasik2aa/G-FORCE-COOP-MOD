#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstddef>
#include <cstdint>

#include "ServerClient/MTypes.h"

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

	void OnPeerConnected();
	void OnPeerDisconnected();
	void OnRemotePacket(const void* data, std::uint32_t size);
	void NetworkTick();
	void GameTick();

	bool InstallInputHook();
	void RemoveInputHook();
	void BeginRemoteInput();
	void EndRemoteInput();
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
	void RequestRemotePlayerTickDeferral();
	bool ConsumeRemotePlayerTickDeferral();
	void BeginLocalInputCapture();
	SHORT HandleGetAsyncKeyState(int virtual_key);
	void PublishLocalPlayerTransform(const void* player);
	// Mooch ownership is latched only after the EXE has actually selected its
	// one-frame switch mode.  The active-entity globals are transient during
	// that hand-off and must not decide who publishes the shared fly.
	void ConfirmLocalFlyControl();
	// 0x61000034 is a short native entry signal.  Read both sides of the fly's
	// tick so the hand-off latch cannot miss it when the same tick ends in 0x33.
	void ObserveLocalFlyMode(std::uint32_t mode_before,
		std::uint32_t mode_after);
	bool IsLocalFlyControlled() const;
	// Fly_Active::Enter normally publishes Mooch as both active entities, but
	// P1's still-ticking Default controller later overwrites those globals.  Keep
	// the native fly ownership published after its own tick; never manufacture
	// its transform or rotation.
	void MaintainLocalFlyActiveEntity(void* fly);
	void PublishLocalFlyTransform(const void* fly);
	// Publishes the local camera yaw read from 0x52AD20 right after P1's own
	// controller tick, which is the frame point where P1's 0x5BCF30 has just
	// finished driving the shared camera.
	void PublishLocalCameraYaw(float yaw, bool valid);
	bool ApplyRemotePlayerTransform(void* player2);
	bool ApplyRemoteFlyTransform(void* fly);
	// Game-thread-only replay of the exact native trigger spawn route.  WorldSync
	// uses it only after a host event cannot be paired to a client-native entity.
	bool SpawnWorldFromTrigger(void* trigger);
	bool ReplayTriggerEvent(std::uint32_t family, std::uint32_t subtype,
		std::int32_t definition_id, std::uint32_t occurrence, int event_code);
	// Applies a remote player's damage to the local twin of a world entity by
	// driving the stock trigger event dispatcher.
	void ApplyRemoteDamage(void* trigger, std::uint32_t amount,
		std::uint32_t world_id, int event_code);
	bool GetActiveRemoteWeaponType(std::uint32_t& weapon_type) const;
	bool __fastcall HandleInputActionQuery(void* input_manager,
		void*, std::uint32_t device, std::uint32_t action,
		std::uint32_t flags);
	bool __fastcall HandleInputActionUpQuery(void* input_manager,
		void*, std::uint32_t device, std::uint32_t action,
		std::uint32_t flags);
	bool __fastcall HandleInputThresholdQuery(void* input_manager,
		void*, std::uint32_t device, std::uint32_t action,
		float threshold, std::uint32_t flags);
	bool __fastcall HandleInputPressedQuery(void* input_manager,
		void*, std::uint32_t device, std::uint32_t action,
		std::uint32_t flags);
	bool __fastcall HandleInputReleasedQuery(void* input_manager,
		void*, std::uint32_t device, std::uint32_t action,
		std::uint32_t flags);
	bool __fastcall HandleInputHoldDurationQuery(void* input_manager,
		void*, std::uint32_t device, std::uint32_t action,
		float threshold, std::uint32_t flags);
	// 0x488B00 takes flags BEFORE the float, unlike 0x488DC0.
	bool __fastcall HandleInputAimHoldQuery(void* input_manager,
		void*, std::uint32_t device, std::uint32_t action,
		std::uint32_t flags, float threshold);
	bool __fastcall HandleInputRawPressedQuery(void* input_manager, void*,
		void* device, std::uint32_t action, std::uint32_t flags, bool record);
	bool __fastcall HandleInputRawReleasedQuery(void* input_manager, void*,
		void* device, std::uint32_t action, std::uint32_t flags, bool record);
	bool __fastcall HandleInputRawHeldQuery(void* input_manager, void*,
		void* device, std::uint32_t action, std::uint32_t flags, bool record);
	float __fastcall HandleInputAxisQuery(void* input_manager,
		void*, std::uint32_t device, std::uint32_t axis,
		std::uint32_t flags);
	// 0x52AD20, __thiscall(camera handler), float in st(0).
	float __fastcall HandleCameraYawQuery(void* camera_handler, void*);
	// 0x5BCF30, __thiscall(mode), no stack arguments.
	void __fastcall HandleGPigCameraUpdate(void* mode, void*);

private:
	typedef SHORT (WINAPI* GetAsyncKeyStateFn)(int);
	typedef bool (__thiscall* InputActionQueryFn)(void*, std::uint32_t,
		std::uint32_t, std::uint32_t);
	typedef bool (__thiscall* InputThresholdQueryFn)(void*, std::uint32_t,
		std::uint32_t, float, std::uint32_t);
	typedef bool (__thiscall* InputAimHoldQueryFn)(void*, std::uint32_t,
		std::uint32_t, std::uint32_t, float);
	typedef bool (__thiscall* InputRawQueryFn)(void*, void*, std::uint32_t,
		std::uint32_t, bool);
	typedef float (__thiscall* InputAxisQueryFn)(void*, std::uint32_t,
		std::uint32_t, std::uint32_t);
	typedef float (__thiscall* CameraYawFn)(void*);
	typedef void (__thiscall* GPigCameraUpdateFn)(void*);
	typedef void (__thiscall* DefaultModeUpdateFn)(void*, void*, void*);
	typedef void (__thiscall* FireHandlerFn)(void*, void*, void*);
	typedef void (__thiscall* WeaponAmmoConsumeFn)(void*);
	typedef void (__thiscall* TriggerSpawnFromDefinitionFn)(void*);
	typedef void* (__cdecl* TriggerFactoryFn)(std::uint32_t,
		std::uint32_t, void*);
	typedef int (__thiscall* TriggerEventFn)(void*, int);
	typedef void* (__thiscall* XGamePadCtorFn)(void*);
	typedef std::uint32_t (__thiscall* GetCurrentWeaponIdFn)(void*);
	typedef std::uint32_t (__cdecl* WeaponTypeToItemIdFn)(std::uint32_t);
	typedef void* (__thiscall* ResolveWeaponRecordFn)(void*, std::uint32_t);
	typedef void* (__thiscall* ResolveAmmoEntryFn)(void*, std::uint32_t);

	enum Role
	{
		RoleNone,
		RoleHost,
		RoleClient
	};

	CoopNetGame();
	~CoopNetGame() = default;
	CoopNetGame(const CoopNetGame&) = delete;
	CoopNetGame& operator=(const CoopNetGame&) = delete;

	void CaptureLocalInput(CoopInput& input) const;
	void CaptureLocalLookAxis(std::uint32_t axis, float value);
	void CaptureLocalAction(std::uint32_t action, bool is_down);
	void CaptureLocalPress(std::uint32_t action);
	void CaptureLocalRelease(std::uint32_t action);
	void CaptureLocalFlyRaw(std::uint32_t action, bool is_down,
		bool pressed_edge, bool released_edge);
	void CaptureLocalAimRay(const void* input_manager);
	void* GetRemoteGamePad();
	bool ApplyActiveRemoteAimRay(void* input_manager, float saved_ray[6]) const;
	void RestoreAimRay(void* input_manager, const float saved_ray[6]) const;
	void HandleDefaultModeUpdate(void* mode, void* input_manager,
		void* mode_context);
	void HandleFireHandler(void* mode, void* input_manager,
		void* mode_context);
	void HandleWeaponAmmoConsume(void* weapon_record);
	// Both NPC and monster trigger vtables reach 0x41F220.  The hook preserves
	// the native call, then hands its trigger-to-live-entity result to WorldSync.
	void HandleTriggerSpawnFromDefinition(void* trigger);
	void* HandleTriggerFactory(std::uint32_t family, std::uint32_t subtype,
		void* output, std::uintptr_t caller);
	int HandleTriggerEvent(void* trigger, int event_code);
	bool GetActiveRemoteAction(std::uint32_t action) const;
	bool GetActiveRemoteHold(std::uint32_t action, float threshold) const;
	bool GetRemoteFlyRawHeld(std::uint32_t action) const;
	bool ConsumeRemoteFlyRawEdge(std::uint32_t action, bool pressed);
	bool GetRemoteFlyFireAction() const;
	bool IsRemoteFlyControlled() const;
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
	bool InstallTriggerSpawnTraceHook();
	void RemoveTriggerSpawnTraceHook();
	bool InstallTriggerFactoryTraceHook();
	void RemoveTriggerFactoryTraceHook();
	bool InstallTriggerEventTraceHook();
	void RemoveTriggerEventTraceHook();
	// Generic 5-byte E9 detour installer.  relocate_len original bytes are copied
	// into a freshly allocated trampoline which then jumps back to address +
	// relocate_len; relocate_len must be >= 5 and cover whole instructions.  The
	// original entry (callable via the trampoline) is returned in *trampoline_out.
	bool InstallJmpHookRaw(std::uintptr_t address, const std::uint8_t* expected,
		std::size_t relocate_len, void* hook, BYTE* saved_bytes,
		BYTE** trampoline_out, const char* label);
	void RemoveJmpHookRaw(std::uintptr_t address, const BYTE* saved_bytes,
		std::size_t relocate_len, BYTE** trampoline_ptr);
	float GetRemoteLookAxis(std::uint32_t axis) const;
	bool IsGameForeground() const;
	static void __fastcall HookDefaultModeUpdate(void* mode, void*,
		void* input_manager, void* mode_context);
	static void __fastcall HookFireHandler(void* mode, void*,
		void* input_manager, void* mode_context);
	static void __fastcall HookWeaponAmmoConsume(void* weapon_record, void*);
	static void __fastcall HookTriggerSpawnFromDefinition(void* trigger, void*);
	static void* __cdecl HookTriggerFactory(std::uint32_t family,
		std::uint32_t subtype, void* output);
	static int __fastcall HookTriggerEvent(void* trigger, void*, int event_code);

	volatile LONG m_role;
	volatile LONG m_remote_connected;
	mutable SRWLOCK m_input_lock;
	CoopInput m_remote_input;
	CoopInput m_active_remote_input;
	CoopInput m_local_input;
	BYTE* m_keyboard_state_buffer;
	BYTE* m_keyboard_state_secondary_buffer;
	BYTE m_saved_keyboard_state[256];
	BYTE m_saved_keyboard_state_secondary[256];
	BYTE m_active_remote_scan_codes[256];
	DWORD m_last_send_tick;
	DWORD m_last_remote_transform_apply_tick;
	DWORD m_fly_handoff_started_tick;
	DWORD m_remote_input_thread_id;
	std::uint32_t m_local_transform_sequence;
	std::uint32_t m_local_fly_transform_sequence;
	bool m_local_fly_active_seen;
	bool m_local_mooch_exit_key_down;
	bool m_logged_fly_active_entity_repair;
	std::uint32_t m_local_weapon_sequence;
	std::uint32_t m_last_local_weapon_type;
	volatile LONG m_peer_connected_tick;
	volatile LONG m_logged_spawn;
	volatile LONG m_remote_input_active;
	volatile LONG m_defer_remote_player_tick;
	bool m_keyboard_state_swapped;
	bool m_logged_keyboard_state_swap;
	bool m_input_hooked;
	bool m_action_query_hooked;
	bool m_action_up_query_hooked;
	bool m_threshold_query_hooked;
	bool m_axis_query_hooked;
	bool m_pressed_query_hooked;
	bool m_released_query_hooked;
	bool m_hold_duration_query_hooked;
	bool m_aim_hold_query_hooked;
	bool m_camera_yaw_hooked;
	bool m_gpig_camera_update_hooked;
	bool m_logged_camera_yaw_override;
	bool m_logged_camera_update_skip;
	bool m_default_mode_update_hooked;
	bool m_logged_remote_gamepad;
	bool m_remote_gamepad_unavailable;
	void* m_remote_gamepad;
	ULONG_PTR* m_async_key_state_iat_slot;
	GetAsyncKeyStateFn m_original_get_async_key_state;
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
	BYTE m_original_trigger_spawn_bytes[14];
	BYTE* m_trigger_spawn_trampoline;
	TriggerSpawnFromDefinitionFn m_original_trigger_spawn;
	bool m_trigger_spawn_trace_hooked;
	volatile LONG m_trigger_spawn_sequence;
	BYTE m_original_trigger_factory_bytes[14];
	BYTE* m_trigger_factory_trampoline;
	TriggerFactoryFn m_original_trigger_factory;
	bool m_trigger_factory_trace_hooked;
	volatile LONG m_trigger_factory_sequence;
	BYTE m_original_trigger_event_bytes[11];
	BYTE* m_trigger_event_trampoline;
	TriggerEventFn m_original_trigger_event;
	bool m_trigger_event_trace_hooked;
	volatile LONG m_trigger_event_sequence;
	bool m_logged_axis_queries[2];
	bool m_logged_remote_transform;
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
	std::uint8_t m_prev_remote_fly_raw_press_seq[kCoopFlyRawActionCount];
	std::uint8_t m_prev_remote_fly_raw_release_seq[kCoopFlyRawActionCount];
	bool m_logged_remote_p2_ammo_restore;
	void* m_remote_p2_weapon_record;
};
}
