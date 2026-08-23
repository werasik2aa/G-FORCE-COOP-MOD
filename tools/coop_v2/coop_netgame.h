#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstddef>
#include <cstdint>

#include "ServerClient/MTypes.h"

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
	// Q starts a process-global hand-off from Darwin to Mooch.  Let that local
	// hand-off settle before P2 enters its next packet-driven stock tick.
	void RequestRemotePlayerTickDeferral();
	bool ConsumeRemotePlayerTickDeferral();
	void BeginLocalInputCapture();
	SHORT HandleGetAsyncKeyState(int virtual_key);
	void PublishLocalPlayerTransform(const void* player);
	void PublishLocalFlyTransform(const void* fly, bool controlled);
	// Publishes the local camera yaw read from 0x52AD20 right after P1's own
	// controller tick, which is the frame point where P1's 0x5BCF30 has just
	// finished driving the shared camera.
	void PublishLocalCameraYaw(float yaw, bool valid);
	bool ApplyRemotePlayerTransform(void* player2);
	bool ApplyRemoteFlyTransform(void* fly);
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
	typedef float (__thiscall* InputAxisQueryFn)(void*, std::uint32_t,
		std::uint32_t, std::uint32_t);
	typedef float (__thiscall* CameraYawFn)(void*);
	typedef void (__thiscall* GPigCameraUpdateFn)(void*);
	typedef void (__thiscall* DefaultModeUpdateFn)(void*, void*, void*);
	typedef void (__thiscall* FireHandlerFn)(void*, void*, void*);
	typedef void* (__thiscall* XGamePadCtorFn)(void*);

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
	void CaptureLocalAimRay(const void* input_manager);
	void* GetRemoteGamePad();
	bool ApplyActiveRemoteAimRay(void* input_manager, float saved_ray[6]) const;
	void RestoreAimRay(void* input_manager, const float saved_ray[6]) const;
	void HandleDefaultModeUpdate(void* mode, void* input_manager,
		void* mode_context);
	void HandleFireHandler(void* mode, void* input_manager,
		void* mode_context);
	bool GetActiveRemoteAction(std::uint32_t action) const;
	bool GetActiveRemoteHold(std::uint32_t action, float threshold) const;
	bool IsRemoteFlyControlled() const;
	// True for actions that must never be mirrored to P2: night vision (TAB) and
	// the fly (Q).  Both consume a resource that belongs to the local player, so
	// replaying the sender's key on the receiver drained one pool twice.
	bool IsMirrorSuppressedAction(std::uint32_t action) const;
	// One-shot, never per-frame: writes P1's live binding table to the log so the
	// semantic action index behind every key is known, instead of matching the
	// physical scan code (which a rebind would break).
	void DumpActionBindingsOnce();
	std::uint32_t GetActionBindingProfile(const void* pad) const;
	const char* DescribeScanCode(std::uint32_t scan_code) const;
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
	bool InstallCameraYawHook();
	void RemoveCameraYawHook();
	bool InstallGPigCameraUpdateHook();
	void RemoveGPigCameraUpdateHook();
	bool InstallDefaultModeUpdateHook();
	void RemoveDefaultModeUpdateHook();
	bool InstallFireHandlerHook();
	void RemoveFireHandlerHook();
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
		void* input_manager, void* mode_context);	static void __fastcall HookFireHandler(void* mode, void*,
		void* input_manager, void* mode_context);

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
	DWORD m_remote_input_thread_id;
	std::uint32_t m_local_transform_sequence;
	std::uint32_t m_local_fly_transform_sequence;
	std::uint32_t m_local_weapon_sequence;
	std::uint32_t m_last_local_weapon_type;
	volatile LONG m_peer_connected_tick;
	volatile LONG m_logged_spawn;
	volatile LONG m_remote_input_active;
	volatile LONG m_defer_remote_player_tick;
	volatile LONG m_logged_action_bindings;
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
	// One bit per action index, never cleared: "this index has already been logged
	// once".  Turns a single key press into one log line naming the semantic index,
	// which is what the mirror-suppression list is pinned to.
	std::uint32_t m_logged_local_press[3];
};
}
