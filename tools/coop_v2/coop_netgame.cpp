#include "coop_netgame.h"

#include "coop_runtime.h"
#include "gforce_constants.h"
#include "player2.h"
#include "world_sync.h"
#include "ServerClient/MClient.h"
#include "ServerClient/MServer.h"
#include "ServerClient/MServerONLINE.h"
#include "ServerClient/SteamManager.h"

#include <math.h>
#include <string.h>
#include <intrin.h>

namespace
{
constexpr DWORD kInputSendIntervalMs = 16;
constexpr DWORD kNetworkSpawnDelayMs = 1000;
constexpr LONG kDeferRemotePlayerOneTick = 1;
constexpr LONG kDeferRemotePlayerUntilFlyActive = 2;
constexpr DWORD kFlyHandoffTimeoutMs = 1500;

const std::uint32_t kFlyRawActionIds[kCoopFlyRawActionCount] = {
	0x40080046u,
	0x40080047u,
	0x40080029u,
	0x40080034u,
	0x40080036u,
	0x4008000Bu
};

int FindFlyRawActionIndex(std::uint32_t action)
{
	for (std::uint32_t index = 0; index < kCoopFlyRawActionCount; ++index)
	{
		if (kFlyRawActionIds[index] == action)
			return static_cast<int>(index);
	}
	return -1;
}

SHORT WINAPI HookGetAsyncKeyState(int virtual_key)
{
	return coop::CoopNetGame::Instance().HandleGetAsyncKeyState(virtual_key);
}

bool __fastcall HookInputActionQuery(void* input_manager, void* edx,
	std::uint32_t device, std::uint32_t action, std::uint32_t flags)
{
	return coop::CoopNetGame::Instance().HandleInputActionQuery(
		input_manager, edx, device, action, flags);
}

bool __fastcall HookInputActionUpQuery(void* input_manager, void* edx,
	std::uint32_t device, std::uint32_t action, std::uint32_t flags)
{
	return coop::CoopNetGame::Instance().HandleInputActionUpQuery(
		input_manager, edx, device, action, flags);
}

bool __fastcall HookInputThresholdQuery(void* input_manager, void* edx,
	std::uint32_t device, std::uint32_t action, float threshold,
	std::uint32_t flags)
{
	return coop::CoopNetGame::Instance().HandleInputThresholdQuery(
		input_manager, edx, device, action, threshold, flags);
}

float __fastcall HookInputAxisQuery(void* input_manager, void* edx,
	std::uint32_t device, std::uint32_t axis, std::uint32_t flags)
{
	return coop::CoopNetGame::Instance().HandleInputAxisQuery(
		input_manager, edx, device, axis, flags);
}

bool __fastcall HookInputPressedQuery(void* input_manager, void* edx,
	std::uint32_t device, std::uint32_t action, std::uint32_t flags)
{
	return coop::CoopNetGame::Instance().HandleInputPressedQuery(
		input_manager, edx, device, action, flags);
}

bool __fastcall HookInputReleasedQuery(void* input_manager, void* edx,
	std::uint32_t device, std::uint32_t action, std::uint32_t flags)
{
	return coop::CoopNetGame::Instance().HandleInputReleasedQuery(
		input_manager, edx, device, action, flags);
}

bool __fastcall HookInputHoldDurationQuery(void* input_manager, void* edx,
	std::uint32_t device, std::uint32_t action, float threshold,
	std::uint32_t flags)
{
	return coop::CoopNetGame::Instance().HandleInputHoldDurationQuery(
		input_manager, edx, device, action, threshold, flags);
}

bool __fastcall HookInputAimHoldQuery(void* input_manager, void* edx,
	std::uint32_t device, std::uint32_t action, std::uint32_t flags,
	float threshold)
{
	return coop::CoopNetGame::Instance().HandleInputAimHoldQuery(
		input_manager, edx, device, action, flags, threshold);
}

bool __fastcall HookInputRawPressedQuery(void* input_manager, void* edx,
	void* device, std::uint32_t action, std::uint32_t flags, bool record)
{
	return coop::CoopNetGame::Instance().HandleInputRawPressedQuery(
		input_manager, edx, device, action, flags, record);
}

bool __fastcall HookInputRawReleasedQuery(void* input_manager, void* edx,
	void* device, std::uint32_t action, std::uint32_t flags, bool record)
{
	return coop::CoopNetGame::Instance().HandleInputRawReleasedQuery(
		input_manager, edx, device, action, flags, record);
}

bool __fastcall HookInputRawHeldQuery(void* input_manager, void* edx,
	void* device, std::uint32_t action, std::uint32_t flags, bool record)
{
	return coop::CoopNetGame::Instance().HandleInputRawHeldQuery(
		input_manager, edx, device, action, flags, record);
}

float __fastcall HookCameraYaw(void* camera_handler, void* edx)
{
	return coop::CoopNetGame::Instance().HandleCameraYawQuery(camera_handler,
		edx);
}

void __fastcall HookGPigCameraUpdate(void* mode, void* edx)
{
	coop::CoopNetGame::Instance().HandleGPigCameraUpdate(mode, edx);
}

void SetVirtualKey(CoopInput& input, unsigned virtual_key)
{
	if (virtual_key < 256)
		input.virtual_keys[virtual_key / 32] |=
			1u << (virtual_key % 32);
}

bool IsVirtualKeyDown(const CoopInput& input, unsigned virtual_key)
{
	return virtual_key < 256 &&
		(input.virtual_keys[virtual_key / 32] &
			(1u << (virtual_key % 32))) != 0;
}
}

namespace coop
{
using namespace gforce;

CoopNetGame& CoopNetGame::Instance()
{
	static CoopNetGame instance;
	return instance;
}

CoopNetGame::CoopNetGame() :
	m_role(RoleNone),
	m_remote_connected(0),
	m_last_send_tick(0),
	m_last_remote_transform_apply_tick(0),
	m_fly_handoff_started_tick(0),
	m_peer_connected_tick(0),
	m_logged_spawn(0),
	m_remote_input_active(0),
	m_defer_remote_player_tick(0),
	m_keyboard_state_buffer(NULL),
	m_keyboard_state_secondary_buffer(NULL),
	m_keyboard_state_swapped(false),
	m_logged_keyboard_state_swap(false),
	m_input_hooked(false),
	m_action_query_hooked(false),
	m_action_up_query_hooked(false),
	m_threshold_query_hooked(false),
	m_axis_query_hooked(false),
	m_async_key_state_iat_slot(NULL),
	m_original_get_async_key_state(NULL),
	m_input_action_trampoline(NULL),
	m_original_input_action_query(NULL),
	m_input_action_up_trampoline(NULL),
	m_original_input_action_up_query(NULL),
	m_input_threshold_trampoline(NULL),
	m_original_input_threshold_query(NULL),
	m_input_axis_trampoline(NULL),
	m_original_input_axis_query(NULL),
	m_logged_remote_gamepad(false),
	m_remote_gamepad_unavailable(false),
	m_remote_gamepad(NULL),
	m_default_mode_update_trampoline(NULL),
	m_original_default_mode_update(NULL),
	m_fire_handler_trampoline(NULL),
	m_original_fire_handler(NULL),
	m_fire_handler_hooked(false),
	m_weapon_ammo_consume_trampoline(NULL),
	m_original_weapon_ammo_consume(NULL),
	m_weapon_ammo_consume_hooked(false),
	m_trigger_spawn_trampoline(NULL),
	m_original_trigger_spawn(NULL),
	m_trigger_spawn_trace_hooked(false),
	m_trigger_spawn_sequence(0),
	m_trigger_factory_trampoline(NULL),
	m_original_trigger_factory(NULL),
	m_trigger_factory_trace_hooked(false),
	m_trigger_factory_sequence(0),
	m_trigger_event_trampoline(NULL),
	m_original_trigger_event(NULL),
	m_trigger_event_trace_hooked(false),
	m_trigger_event_sequence(0),
	m_remote_input_thread_id(0),
	m_local_transform_sequence(0),
	m_local_fly_transform_sequence(0),
	m_local_fly_active_seen(false),
	m_local_mooch_exit_key_down(false),
	m_logged_fly_active_entity_repair(false),
	m_local_weapon_sequence(0),
	m_last_local_weapon_type(0xFFFFFFFFu),
	m_logged_remote_transform(false)
{
	InitializeSRWLock(&m_input_lock);
	ZeroMemory(&m_remote_input, sizeof(m_remote_input));
	ZeroMemory(&m_active_remote_input, sizeof(m_active_remote_input));
	ZeroMemory(&m_local_input, sizeof(m_local_input));
	ZeroMemory(m_saved_keyboard_state, sizeof(m_saved_keyboard_state));
	ZeroMemory(m_saved_keyboard_state_secondary,
		sizeof(m_saved_keyboard_state_secondary));
	ZeroMemory(m_active_remote_scan_codes,
		sizeof(m_active_remote_scan_codes));
	ZeroMemory(m_original_input_action_query_bytes,
		sizeof(m_original_input_action_query_bytes));
	ZeroMemory(m_original_input_action_up_query_bytes,
		sizeof(m_original_input_action_up_query_bytes));
	ZeroMemory(m_original_input_threshold_query_bytes,
		sizeof(m_original_input_threshold_query_bytes));
	ZeroMemory(m_original_input_axis_query_bytes,
		sizeof(m_original_input_axis_query_bytes));
	ZeroMemory(m_logged_axis_queries, sizeof(m_logged_axis_queries));
	ZeroMemory(m_prev_local_action_down, sizeof(m_prev_local_action_down));
	ZeroMemory(m_prev_remote_action_down, sizeof(m_prev_remote_action_down));
	ZeroMemory(m_prev_remote_press_seq, sizeof(m_prev_remote_press_seq));
	m_pressed_query_hooked = false;
	m_released_query_hooked = false;
	m_hold_duration_query_hooked = false;
	m_aim_hold_query_hooked = false;
	m_camera_yaw_hooked = false;
	m_gpig_camera_update_hooked = false;
	m_logged_camera_yaw_override = false;
	m_logged_camera_update_skip = false;
	m_default_mode_update_hooked = false;
	m_input_pressed_trampoline = NULL;
	m_original_input_pressed_query = NULL;
	m_input_released_trampoline = NULL;
	m_original_input_released_query = NULL;
	m_input_hold_duration_trampoline = NULL;
	m_original_input_hold_duration_query = NULL;
	m_input_aim_hold_trampoline = NULL;
	m_original_input_aim_hold_query = NULL;
	m_camera_yaw_trampoline = NULL;
	m_original_camera_yaw = NULL;
	m_gpig_camera_update_trampoline = NULL;
	m_original_gpig_camera_update = NULL;
	ZeroMemory(m_original_default_mode_update_bytes,
		sizeof(m_original_default_mode_update_bytes));
	ZeroMemory(m_original_fire_handler_bytes,
		sizeof(m_original_fire_handler_bytes));
	ZeroMemory(m_original_weapon_ammo_consume_bytes,
		sizeof(m_original_weapon_ammo_consume_bytes));
	ZeroMemory(m_original_trigger_spawn_bytes,
		sizeof(m_original_trigger_spawn_bytes));
	ZeroMemory(m_original_trigger_factory_bytes,
		sizeof(m_original_trigger_factory_bytes));
	ZeroMemory(m_original_trigger_event_bytes,
		sizeof(m_original_trigger_event_bytes));
	ZeroMemory(m_original_input_pressed_query_bytes,
		sizeof(m_original_input_pressed_query_bytes));
	ZeroMemory(m_original_input_released_query_bytes,
		sizeof(m_original_input_released_query_bytes));
	ZeroMemory(m_original_input_hold_duration_query_bytes,
		sizeof(m_original_input_hold_duration_query_bytes));
	ZeroMemory(m_original_input_aim_hold_query_bytes,
		sizeof(m_original_input_aim_hold_query_bytes));
	ZeroMemory(m_original_input_raw_pressed_query_bytes,
		sizeof(m_original_input_raw_pressed_query_bytes));
	ZeroMemory(m_original_input_raw_released_query_bytes,
		sizeof(m_original_input_raw_released_query_bytes));
	ZeroMemory(m_original_input_raw_held_query_bytes,
		sizeof(m_original_input_raw_held_query_bytes));
	ZeroMemory(m_original_camera_yaw_bytes,
		sizeof(m_original_camera_yaw_bytes));
	ZeroMemory(m_original_gpig_camera_update_bytes,
		sizeof(m_original_gpig_camera_update_bytes));
	ZeroMemory(m_prev_remote_release_seq, sizeof(m_prev_remote_release_seq));
	ZeroMemory(m_prev_remote_fly_raw_press_seq,
		sizeof(m_prev_remote_fly_raw_press_seq));
	ZeroMemory(m_prev_remote_fly_raw_release_seq,
		sizeof(m_prev_remote_fly_raw_release_seq));
	ZeroMemory(m_remote_press_edge, sizeof(m_remote_press_edge));
	ZeroMemory(m_remote_release_edge, sizeof(m_remote_release_edge));
	ZeroMemory(m_remote_hold_start_tick, sizeof(m_remote_hold_start_tick));
	ZeroMemory(m_remote_action_held, sizeof(m_remote_action_held));
	ZeroMemory(m_local_press_recorded, sizeof(m_local_press_recorded));
	ZeroMemory(m_local_release_recorded, sizeof(m_local_release_recorded));
	m_logged_remote_p2_ammo_restore = false;
	m_remote_p2_weapon_record = NULL;
	m_input_raw_pressed_trampoline = NULL;
	m_original_input_raw_pressed_query = NULL;
	m_input_raw_released_trampoline = NULL;
	m_original_input_raw_released_query = NULL;
	m_input_raw_held_trampoline = NULL;
	m_original_input_raw_held_query = NULL;
	m_raw_pressed_query_hooked = false;
	m_raw_released_query_hooked = false;
	m_raw_held_query_hooked = false;
}

void CoopNetGame::SetModeHost()
{
	InterlockedExchange(&m_role, RoleHost);
	CoopRuntime::Instance().Log("[netgame] role=HOST\r\n");
}

void CoopNetGame::SetModeClient()
{
	InterlockedExchange(&m_role, RoleClient);
	CoopRuntime::Instance().Log("[netgame] role=CLIENT\r\n");
}

bool CoopNetGame::IsHost() const
{
	return InterlockedCompareExchange(
		const_cast<volatile LONG*>(&m_role), RoleNone, RoleNone) == RoleHost;
}

bool CoopNetGame::IsClient() const
{
	return InterlockedCompareExchange(
		const_cast<volatile LONG*>(&m_role), RoleNone, RoleNone) == RoleClient;
}

bool CoopNetGame::HasRemotePeer() const
{
	return InterlockedCompareExchange(
		const_cast<volatile LONG*>(&m_remote_connected), 0, 0) != 0;
}

void CoopNetGame::OnPeerConnected()
{
	InterlockedExchange(&m_remote_connected, 1);
	InterlockedExchange(&m_peer_connected_tick,
		static_cast<LONG>(GetTickCount()));
	InterlockedExchange(&m_logged_spawn, 0);
	m_last_remote_transform_apply_tick = 0;
	WorldSync::Instance().OnPeerConnected();
	CoopRuntime::Instance().Log(
		"[netgame] peer connected; P2 spawn queued for game thread\r\n");
}

void CoopNetGame::OnPeerDisconnected()
{
	InterlockedExchange(&m_remote_connected, 0);
	InterlockedExchange(&m_peer_connected_tick, 0);
	AcquireSRWLockExclusive(&m_input_lock);
	ZeroMemory(&m_remote_input, sizeof(m_remote_input));
	ReleaseSRWLockExclusive(&m_input_lock);
	m_last_remote_transform_apply_tick = 0;
	WorldSync::Instance().OnPeerDisconnected();
	CoopRuntime::Instance().Log("[netgame] remote peer disconnected\r\n");
}

void CoopNetGame::OnRemotePacket(
	const void* data, std::uint32_t size)
{
	if (!data || size < sizeof(CoopInputPacket))
		return;
	const CoopInputPacket* packet =
		static_cast<const CoopInputPacket*>(data);
	if (packet->m_PacketID != kCoopPacketInput ||
		packet->Size() != sizeof(CoopInputPacket))
		return;

	// This is a state buffer, not a one-frame input event.  It deliberately
	// remains valid until a later packet replaces it, so held keys survive
	// packet pacing and the remote controller sees a stable input state.
	const bool remote_fly_controlled = packet->input.fly_controlled != 0 &&
		packet->input.fly_transform_sequence != 0;
	bool client_yielded_fly = false;
	AcquireSRWLockExclusive(&m_input_lock);
	m_remote_input = packet->input;
	// Mooch is a single world object.  If both peers obtain the local hand-off
	// in the same network window, the client yields to the host.  In all normal
	// cases only one side has fly_controlled set, so this branch is untouched.
	if (remote_fly_controlled && m_local_input.fly_controlled != 0 &&
		IsClient())
	{
		m_local_input.fly_controlled = 0;
		m_local_input.fly_transform_sequence = 0;
		m_local_fly_active_seen = false;
		client_yielded_fly = true;
	}
	ReleaseSRWLockExclusive(&m_input_lock);
	if (client_yielded_fly)
	{
		CoopRuntime::Instance().Log(
			"[fly] simultaneous claim: client yielded Mooch to host\r\n");
	}
}

bool CoopNetGame::IsGameForeground() const
{
	const HWND foreground = GetForegroundWindow();
	if (!foreground)
		return false;
	DWORD process_id = 0;
	GetWindowThreadProcessId(foreground, &process_id);
	return process_id == GetCurrentProcessId();
}

void CoopNetGame::CaptureLocalInput(CoopInput& input) const
{
	ZeroMemory(input.virtual_keys, sizeof(input.virtual_keys));
	if (!IsGameForeground())
		return;

	for (unsigned virtual_key = 0; virtual_key < 256; ++virtual_key)
	{
		if (GetAsyncKeyState(static_cast<int>(virtual_key)) & 0x8000)
			SetVirtualKey(input, virtual_key);
	}
}

void CoopNetGame::CaptureLocalLookAxis(std::uint32_t axis, float value)
{
	if (axis > 1)
		return;
	AcquireSRWLockExclusive(&m_input_lock);
	m_local_input.look_axis[axis] = value;
	ReleaseSRWLockExclusive(&m_input_lock);
}

void CoopNetGame::CaptureLocalAimRay(const void* input_manager)
{
	if (!input_manager)
		return;

	float origin[3] = {};
	float direction[3] = {};
	__try
	{
		const BYTE* bytes = static_cast<const BYTE*>(input_manager);
		memcpy(origin, bytes + kInputAimOriginOffset, sizeof(origin));
		memcpy(direction, bytes + kInputAimDirectionOffset,
			sizeof(direction));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return;
	}

	// Ignore the constructor's empty cache and any malformed values. The ray is
	// later used verbatim by the stock fire command, so it must never become a
	// NaN projectile direction on the receiving machine.
	const float length_squared = direction[0] * direction[0] +
		direction[1] * direction[1] + direction[2] * direction[2];
	if (!(length_squared > 0.25f && length_squared < 4.0f))
		return;

	AcquireSRWLockExclusive(&m_input_lock);
	memcpy(m_local_input.aim_origin, origin, sizeof(origin));
	memcpy(m_local_input.aim_direction, direction, sizeof(direction));
	ReleaseSRWLockExclusive(&m_input_lock);
}

void* CoopNetGame::GetRemoteGamePad()
{
	if (m_remote_gamepad)
		return m_remote_gamepad;
	if (m_remote_gamepad_unavailable)
		return NULL;

	// 0x487F10 is deliberately not called. It registers its argument by writing
	// 0x9905CC, the process-global P1 camera/input bridge. Default mode already
	// accepts an XGamePad argument, so P2 needs a constructed object only.
	void* const memory = VirtualAlloc(NULL, kXGamePadSize,
		MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!memory)
	{
		m_remote_gamepad_unavailable = true;
		CoopRuntime::Instance().Log(
			"[p2-gamepad] allocation failed; using P1 pad fallback\r\n");
		return NULL;
	}

	void* gamepad = NULL;
	__try
	{
		XGamePadCtorFn ctor = reinterpret_cast<XGamePadCtorFn>(kXGamePadCtor);
		gamepad = ctor(memory);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		gamepad = NULL;
	}
	if (!gamepad)
	{
		VirtualFree(memory, 0, MEM_RELEASE);
		m_remote_gamepad_unavailable = true;
		CoopRuntime::Instance().Log(
			"[p2-gamepad] constructor fault; using P1 pad fallback\r\n");
		return NULL;
	}

	m_remote_gamepad = gamepad;
	if (!m_logged_remote_gamepad)
	{
		CoopRuntime::Instance().Log(
			"[p2-gamepad] private XGamePad=%p created; P1 global pad remains=%p\r\n",
			m_remote_gamepad,
			*reinterpret_cast<void**>(kPrimaryGamePad));
		m_logged_remote_gamepad = true;
	}
	return m_remote_gamepad;
}

bool CoopNetGame::BeginRemoteGamePadScope(void*& primary_gamepad)
{
	primary_gamepad = NULL;
	void* const remote_gamepad = GetRemoteGamePad();
	if (!remote_gamepad)
		return false;

	__try
	{
		void** const primary_gamepad_slot =
			reinterpret_cast<void**>(kPrimaryGamePad);
		primary_gamepad = *primary_gamepad_slot;
		*primary_gamepad_slot = remote_gamepad;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		primary_gamepad = NULL;
		CoopRuntime::Instance().Log(
			"[p2-gamepad] unable to scope the primary XGamePad\r\n");
		return false;
	}
}

void CoopNetGame::EndRemoteGamePadScope(void* primary_gamepad)
{
	__try
	{
		*reinterpret_cast<void**>(kPrimaryGamePad) = primary_gamepad;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log(
			"[p2-gamepad] unable to restore the primary XGamePad\r\n");
	}
}

bool CoopNetGame::ApplyActiveRemoteAimRay(void* input_manager,
	float saved_ray[6]) const
{
	if (!input_manager || !saved_ray ||
		m_active_remote_input.transform_sequence == 0)
	{
		return false;
	}

	const float* const direction = m_active_remote_input.aim_direction;
	const float length_squared = direction[0] * direction[0] +
		direction[1] * direction[1] + direction[2] * direction[2];
	if (!(length_squared > 0.25f && length_squared < 4.0f))
		return false;

	__try
	{
		BYTE* const bytes = static_cast<BYTE*>(input_manager);
		memcpy(saved_ray, bytes + kInputAimOriginOffset, 3 * sizeof(float));
		memcpy(saved_ray + 3, bytes + kInputAimDirectionOffset,
			3 * sizeof(float));
		memcpy(bytes + kInputAimOriginOffset,
			m_active_remote_input.aim_origin, 3 * sizeof(float));
		memcpy(bytes + kInputAimDirectionOffset,
			m_active_remote_input.aim_direction, 3 * sizeof(float));
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

void CoopNetGame::RestoreAimRay(void* input_manager,
	const float saved_ray[6]) const
{
	if (!input_manager || !saved_ray)
		return;
	__try
	{
		BYTE* const bytes = static_cast<BYTE*>(input_manager);
		memcpy(bytes + kInputAimOriginOffset, saved_ray, 3 * sizeof(float));
		memcpy(bytes + kInputAimDirectionOffset, saved_ray + 3,
			3 * sizeof(float));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}

void __fastcall CoopNetGame::HookDefaultModeUpdate(void* mode, void*,
	void* input_manager, void* mode_context)
{
	Instance().HandleDefaultModeUpdate(mode, input_manager, mode_context);
}

void CoopNetGame::HandleDefaultModeUpdate(void* mode, void* input_manager,
	void* mode_context)
{
	if (!m_original_default_mode_update)
		return;

	// The normal Default-mode caller takes the globally registered P1 pad and
	// passes it here.  P2's networked keyboard actions are supplied by the
	// packet-backed query hooks, but its stock motor also makes raw XGamePad
	// reads that a freshly constructed private pad cannot answer.  Keep the
	// original pad for normal P2 play so those raw reads retain the stock path.
	// Only while this machine owns Mooch do we substitute P2's private pad: that
	// isolates P2's reset/update work from the physical pad which Fly_Active
	// uses for its local movable crosshair.
	void* mode_input = input_manager;
	if (IsRemoteInputActiveOnThisThread() && IsLocalFlyControlled())
	{
		void* const remote_gamepad = GetRemoteGamePad();
		if (remote_gamepad)
			mode_input = remote_gamepad;
	}
	m_original_default_mode_update(mode, mode_input, mode_context);

	// Capture P1's stock ray for the packet.  P2's ray is substituted only at
	// the exact fire-handler call, then immediately restored.
	if (!IsRemoteInputActiveOnThisThread())
		CaptureLocalAimRay(mode_input);
}

void __fastcall CoopNetGame::HookFireHandler(void* mode, void*,
	void* input_manager, void* mode_context)
{
	Instance().HandleFireHandler(mode, input_manager, mode_context);
}

void CoopNetGame::HandleFireHandler(void* mode, void* input_manager,
	void* mode_context)
{
	if (!m_original_fire_handler)
		return;

	float saved_ray[6] = {};
	const bool remote_ray_applied = IsRemoteInputActiveOnThisThread() &&
		ApplyActiveRemoteAimRay(input_manager, saved_ray);
	// 0x5B8760 copies the cached ray to the projectile command synchronously.
	// Restricting the swap to this call avoids leaving a P2 ray in P1's shared
	// XGamePad during Default-mode weapon transitions.
	m_original_fire_handler(mode, input_manager, mode_context);
	if (remote_ray_applied)
		RestoreAimRay(input_manager, saved_ray);
}

void CoopNetGame::BeginLocalInputCapture()
{
	AcquireSRWLockExclusive(&m_input_lock);
	// Keep held levels across frames.  Some action modes (the whip charge is one)
	// poll only the press/release or threshold path while a key remains held, so
	// rebuilding action_down here loses the whole duration after the first frame.
	// Level queries still refresh their actions in CaptureLocalAction; edge paths
	// maintain actions that do not use a level query every frame.
	ZeroMemory(m_local_press_recorded, sizeof(m_local_press_recorded));
	ZeroMemory(m_local_release_recorded, sizeof(m_local_release_recorded));
	ReleaseSRWLockExclusive(&m_input_lock);
}

void CoopNetGame::CaptureLocalAction(std::uint32_t action, bool is_down)
{
	if (action < kFirstKeyboardActionId ||
		action >= kFirstKeyboardActionId + kKeyboardActionCount)
	{
		return;
	}
	const uint32_t action_index = action - kFirstKeyboardActionId;
	const uint32_t word = action_index / 32;
	const uint32_t bit = action_index % 32;
	const bool was_down = (m_prev_local_action_down[word] & (1u << bit)) != 0;
	if (action_index == kMoochActionIndex && is_down && !was_down &&
		!ConsumeLocalMoochFlyExit())
	{
		RequestRemotePlayerTickDeferral();
	}
	if (action_index == kMoochActionIndex && !is_down)
	{
		// The level path can observe the key-up before the released-edge hook.
		// Clear the duplicate-edge guard here as well so the next physical press
		// is allowed to perform the normal fly exit.
		AcquireSRWLockExclusive(&m_input_lock);
		m_local_mooch_exit_key_down = false;
		ReleaseSRWLockExclusive(&m_input_lock);
	}
	if (is_down)
		m_prev_local_action_down[word] |= 1u << bit;
	else
		m_prev_local_action_down[word] &= ~(1u << bit);
	AcquireSRWLockExclusive(&m_input_lock);
	// Held level only.  Rising/falling edges are captured separately from the
	// engine's own edge queries (CaptureLocalPress / CaptureLocalRelease) so the
	// edge attribution matches exactly how the native weapon state machine polls.
	if (is_down)
		m_local_input.action_down[word] |= 1u << bit;
	else
		m_local_input.action_down[word] &= ~(1u << bit);
	ReleaseSRWLockExclusive(&m_input_lock);
}

void CoopNetGame::CaptureLocalPress(std::uint32_t action)
{
	if (action < kFirstKeyboardActionId ||
		action >= kFirstKeyboardActionId + kKeyboardActionCount)
	{
		return;
	}
	const uint32_t action_index = action - kFirstKeyboardActionId;
	const uint32_t word = action_index / 32;
	const uint32_t bit = action_index % 32;
	// Mooch can be entered through the pressed-edge path without a preceding
	// level query. Request the one-frame P2 hand-off gap here as well; otherwise
	// BeginRemoteInput is still active when the EXE transfers the active entity
	// from Darwin to the fly and the first Mooch press is consumed by P2's context.
	if (action_index == kMoochActionIndex &&
		!ConsumeLocalMoochFlyExit())
	{
		RequestRemotePlayerTickDeferral();
	}
	AcquireSRWLockExclusive(&m_input_lock);
	if ((m_local_press_recorded[word] & (1u << bit)) == 0)
	{
		m_local_press_recorded[word] |= 1u << bit;
		m_local_input.action_press_seq[action_index]++;
		m_local_input.action_down[word] |= 1u << bit;
	}
	ReleaseSRWLockExclusive(&m_input_lock);
}

void CoopNetGame::CaptureLocalRelease(std::uint32_t action)
{
	if (action < kFirstKeyboardActionId ||
		action >= kFirstKeyboardActionId + kKeyboardActionCount)
	{
		return;
	}
	const uint32_t action_index = action - kFirstKeyboardActionId;
	const uint32_t word = action_index / 32;
	const uint32_t bit = action_index % 32;
	AcquireSRWLockExclusive(&m_input_lock);
	if (action_index == kMoochActionIndex)
		m_local_mooch_exit_key_down = false;
	if ((m_local_release_recorded[word] & (1u << bit)) == 0)
	{
		m_local_release_recorded[word] |= 1u << bit;
		m_local_input.action_release_seq[action_index]++;
		m_local_input.action_down[word] &= ~(1u << bit);
	}
	ReleaseSRWLockExclusive(&m_input_lock);
}

void CoopNetGame::CaptureLocalFlyRaw(std::uint32_t action, bool is_down,
	bool pressed_edge, bool released_edge)
{
	const int raw_index = FindFlyRawActionIndex(action);
	if (raw_index < 0 || !IsLocalFlyControlled())
		return;

	const std::uint32_t bit = 1u << static_cast<std::uint32_t>(raw_index);
	AcquireSRWLockExclusive(&m_input_lock);
	if (is_down)
		m_local_input.fly_raw_down |= bit;
	else
		m_local_input.fly_raw_down &= ~bit;
	if (pressed_edge)
	{
		m_local_input.fly_raw_press_seq[raw_index]++;
		m_local_input.fly_raw_down |= bit;
	}
	if (released_edge)
	{
		m_local_input.fly_raw_release_seq[raw_index]++;
		m_local_input.fly_raw_down &= ~bit;
	}
	ReleaseSRWLockExclusive(&m_input_lock);
}

bool CoopNetGame::GetActiveRemoteAction(std::uint32_t action) const
{
	if (action < kFirstKeyboardActionId ||
		action >= kFirstKeyboardActionId + kKeyboardActionCount)
	{
		return false;
	}
	if (IsMirrorSuppressedAction(action))
		return false;
	const uint32_t action_index = action - kFirstKeyboardActionId;
	return (m_active_remote_input.action_down[action_index / 32] &
		(1u << (action_index % 32))) != 0;
}

bool CoopNetGame::IsMoochAction(std::uint32_t action) const
{
	return action == kFirstKeyboardActionId + kMoochActionIndex;
}

bool CoopNetGame::ConsumeLocalMoochFlyExit()
{
	bool released = false;
	AcquireSRWLockExclusive(&m_input_lock);
	if (m_local_mooch_exit_key_down)
	{
		// The level and pressed-edge hooks can both observe one physical press.
		// Treat the second observation as the same exit rather than arming a new
		// Darwin-to-Mooch hand-off.
		ReleaseSRWLockExclusive(&m_input_lock);
		return true;
	}
	if (m_local_input.fly_controlled != 0)
	{
		m_local_input.fly_controlled = 0;
		m_local_input.fly_transform_sequence = 0;
		m_local_input.fly_raw_down = 0;
		m_local_fly_active_seen = false;
		m_local_mooch_exit_key_down = true;
		m_logged_fly_active_entity_repair = false;
		released = true;
	}
	ReleaseSRWLockExclusive(&m_input_lock);
	if (released)
	{
		CoopRuntime::Instance().Log(
			"[fly] local Mooch exit action observed; local authority released\r\n");
	}
	return released;
}

// Mooch changes process-global ownership and the map is UI local to one game
// window, so neither action can be replayed through P2.  This is indexed by the
// logical action rather than a physical key, and remains correct after a rebind.
bool CoopNetGame::IsMirrorSuppressedAction(std::uint32_t action) const
{
	if (action < kFirstKeyboardActionId ||
		action >= kFirstKeyboardActionId + kKeyboardActionCount)
	{
		return false;
	}
	const uint32_t action_index = action - kFirstKeyboardActionId;
	return action_index == kMoochActionIndex ||
		action_index == kMapActionIndex;
}

bool CoopNetGame::IsRemoteFlyControlled() const
{
	CoopInput remote = {};
	return GetRemoteInput(remote) && remote.fly_controlled != 0 &&
		remote.fly_transform_sequence != 0;
}

bool CoopNetGame::GetRemoteFlyRawHeld(std::uint32_t action) const
{
	const int raw_index = FindFlyRawActionIndex(action);
	if (raw_index < 0)
		return false;
	CoopInput remote = {};
	return GetRemoteInput(remote) && remote.fly_controlled != 0 &&
		remote.fly_transform_sequence != 0 &&
		(remote.fly_raw_down & (1u << static_cast<std::uint32_t>(raw_index))) != 0;
}

bool CoopNetGame::ConsumeRemoteFlyRawEdge(std::uint32_t action, bool pressed)
{
	const int raw_index = FindFlyRawActionIndex(action);
	if (raw_index < 0)
		return false;

	AcquireSRWLockExclusive(&m_input_lock);
	const bool remote_controls_fly = m_remote_input.fly_controlled != 0 &&
		m_remote_input.fly_transform_sequence != 0;
	std::uint8_t* previous = pressed ? m_prev_remote_fly_raw_press_seq :
		m_prev_remote_fly_raw_release_seq;
	const std::uint8_t current = pressed ?
		m_remote_input.fly_raw_press_seq[raw_index] :
		m_remote_input.fly_raw_release_seq[raw_index];
	const bool edge = remote_controls_fly && previous[raw_index] != current;
	previous[raw_index] = current;
	ReleaseSRWLockExclusive(&m_input_lock);
	return edge;
}

bool CoopNetGame::GetRemoteFlyFireAction() const
{
	CoopInput remote = {};
	if (!GetRemoteInput(remote) || remote.fly_controlled == 0 ||
		remote.fly_transform_sequence == 0)
	{
		return false;
	}
	const std::uint32_t action_index = kFireActionId - kFirstKeyboardActionId;
	return (remote.action_down[action_index / 32] &
		(1u << (action_index % 32))) != 0;
}

bool CoopNetGame::IsLocalFlyControlled() const
{
	AcquireSRWLockShared(const_cast<SRWLOCK*>(&m_input_lock));
	const bool controlled = m_local_input.fly_controlled != 0;
	ReleaseSRWLockShared(const_cast<SRWLOCK*>(&m_input_lock));
	return controlled;
}

void CoopNetGame::MaintainLocalFlyActiveEntity(void* fly)
{
	if (!fly || !IsLocalFlyControlled())
		return;

	__try
	{
		void** const active_a = reinterpret_cast<void**>(kActiveEntityA);
		void** const active_b = reinterpret_cast<void**>(kActiveEntityB);
		const bool repaired = *active_a != fly || *active_b != fly;
		*active_a = fly;
		*active_b = fly;
		if (repaired && !m_logged_fly_active_entity_repair)
		{
			m_logged_fly_active_entity_repair = true;
			CoopRuntime::Instance().Log(
				"[fly] restored native active entity to Mooch after fly tick\r\n");
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log(
			"[fly] unable to restore active Mooch entity\r\n");
	}
}

void CoopNetGame::ConfirmLocalFlyControl()
{
	bool became_owner = false;
	AcquireSRWLockExclusive(&m_input_lock);
	if (m_local_input.fly_controlled == 0)
	{
		// A sequence of zero prevents a receiver from applying the previous
		// flight's last coordinate before the fly controller publishes a fresh
		// post-motor transform below.
		m_local_input.fly_controlled = 1;
		m_local_input.fly_transform_sequence = 0;
		m_local_input.fly_raw_down = 0;
		m_local_fly_active_seen = false;
		// Confirm happens after the same physical Mooch press has selected
		// 0x61000065.  The pressed-edge path can still observe that entry press
		// on the following tick, so keep it consumed until the key is released.
		m_local_mooch_exit_key_down = true;
		m_logged_fly_active_entity_repair = false;
		became_owner = true;
	}
	ReleaseSRWLockExclusive(&m_input_lock);
	if (became_owner)
	{
		// P2's Default update is safe once the fly controller is genuinely active,
		// but not during Darwin's one-frame hand-off or the immediately following
		// Mooch update.  A one-frame gap left P2 running before Fly_Active::Enter
		// had published the fly as the active entity.
		m_fly_handoff_started_tick = GetTickCount();
		InterlockedExchange(&m_defer_remote_player_tick,
			kDeferRemotePlayerUntilFlyActive);
		CoopRuntime::Instance().Log(
			"[fly] native Darwin-to-Mooch hand-off confirmed; P2 paused until Fly_Active\r\n");
	}
}

void CoopNetGame::ObserveLocalFlyMode(std::uint32_t mode_before,
	std::uint32_t mode_after)
{
	bool active_seen = false;
	AcquireSRWLockExclusive(&m_input_lock);
	if (m_local_input.fly_controlled != 0)
	{
		// The mode can be 0x34 at the beginning of its first tick and 0x33 at
		// the end of that same tick.  Observing either side catches the real
		// entry without treating the subsequent idle/follow state as an exit.
		if (mode_before == kFlyControlledModeId ||
			mode_after == kFlyControlledModeId)
		{
			active_seen = !m_local_fly_active_seen;
			m_local_fly_active_seen = true;
		}
	}
	ReleaseSRWLockExclusive(&m_input_lock);
	if (active_seen)
	{
		CoopRuntime::Instance().Log(
			"[fly] Fly_Active observed; local ownership latched\r\n");
	}
}

// Answer the native "held for >= threshold seconds" queries (0x488DC0 threshold,
// 0x488E50 hold-duration) for the remote player.  m_remote_hold_start_tick /
// m_remote_action_held are refreshed once per P2 frame in BeginRemoteInput from
// the held-level stream; here we just compare the elapsed hold against the exact
// float threshold the engine passed, so a remote tap no longer reads as an
// instantly-completed charge and a genuine long hold still crosses the gate.
bool CoopNetGame::GetActiveRemoteHold(std::uint32_t action, float threshold) const
{
	if (action < kFirstKeyboardActionId ||
		action >= kFirstKeyboardActionId + kKeyboardActionCount)
	{
		return false;
	}
	if (IsMirrorSuppressedAction(action))
		return false;
	const uint32_t action_index = action - kFirstKeyboardActionId;
	if (!m_remote_action_held[action_index])
		return false;
	if (threshold <= 0.0f)
		return true;
	const DWORD elapsed = GetTickCount() - m_remote_hold_start_tick[action_index];
	return elapsed >= static_cast<DWORD>(threshold * 1000.0f);
}

void CoopNetGame::SendLocalInput()
{
	CoopInputPacket packet = {};
	packet.m_PacketID = kCoopPacketInput;
	packet.m_RealSize = sizeof(CoopInputPacket) - sizeof(PacketHeader);
	packet.m_SizeOne = packet.m_RealSize;
	AcquireSRWLockShared(&m_input_lock);
	packet.input = m_local_input;
	ReleaseSRWLockShared(&m_input_lock);
	CaptureLocalInput(packet.input);

	if (IsClient() && SteamOClient && SteamOClient->IsConnected())
	{
		SteamOClient->SendRaw(&packet, sizeof(packet),
			k_nSteamNetworkingSend_Unreliable);
	}
	if (IsHost())
	{
		CSteamOfflineSocketServer* servers[2] = {
			SteamOServer, SteamSServer
		};
		for (CSteamOfflineSocketServer* server : servers)
		{
			if (!server || !server->IsSteamSocketOpen())
				continue;
			for (const HSteamNetConnection connection : server->GetPlayers())
			{
				server->SendRaw(connection, &packet, sizeof(packet),
					k_nSteamNetworkingSend_Unreliable);
			}
		}
	}
}

void CoopNetGame::NetworkTick()
{
	if (!HasRemotePeer())
		return;
	// World events are reliable and should leave the worker immediately; they
	// must not wait for the next 60 Hz input pacing slot.
	WorldSync::Instance().NetworkTick();
	const DWORD now = GetTickCount();
	if (static_cast<DWORD>(now - m_last_send_tick) < kInputSendIntervalMs)
		return;
	m_last_send_tick = now;
	SendLocalInput();
}

void CoopNetGame::GameTick()
{
	if (!HasRemotePeer())
		return;
	// Runs after P1's native controller tick.  It is the only place WorldSync
	// follows game pointers or applies a received transform.
	WorldSync::Instance().GameTick();
	const DWORD connected_tick = static_cast<DWORD>(
		InterlockedCompareExchange(&m_peer_connected_tick, 0, 0));
	if (connected_tick == 0 ||
		static_cast<DWORD>(GetTickCount() - connected_tick) <
		kNetworkSpawnDelayMs)
		return;
	if (Player2Module::Instance().EnsureNetworkPlayer2() &&
		InterlockedCompareExchange(&m_logged_spawn, 1, 0) == 0)
	{
		CoopRuntime::Instance().Log(
			"[netgame] network P2 is ready on this process\r\n");
	}
}

bool CoopNetGame::GetRemoteInput(CoopInput& input) const
{
	if ((!IsHost() && !IsClient()) || !HasRemotePeer())
		return false;
	AcquireSRWLockShared(&m_input_lock);
	input = m_remote_input;
	ReleaseSRWLockShared(&m_input_lock);
	return true;
}

void CoopNetGame::PublishLocalPlayerTransform(const void* player)
{
	if (!player)
		return;
	CoopInput snapshot = {};
	uint32_t selected_weapon_type = 0xFFFFFFFFu;
	__try
	{
		const BYTE* bytes = static_cast<const BYTE*>(player);
		memcpy(snapshot.position, bytes + kEntityPositionOffset,
			sizeof(snapshot.position));
		memcpy(snapshot.rotation, bytes + kEntityRotationOffset,
			sizeof(snapshot.rotation));
		BYTE* handler = *reinterpret_cast<BYTE* const*>(
			bytes + kEntityHandlerOffset);
		if (handler)
		{
			selected_weapon_type = *reinterpret_cast<uint32_t*>(
				handler + kHandlerSelectedWeaponTypeOffset);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return;
	}

	AcquireSRWLockExclusive(&m_input_lock);
	memcpy(m_local_input.position, snapshot.position,
		sizeof(snapshot.position));
	memcpy(m_local_input.rotation, snapshot.rotation,
		sizeof(snapshot.rotation));
	m_local_input.transform_sequence = ++m_local_transform_sequence;
	m_local_input.selected_weapon_type = selected_weapon_type;
	if (selected_weapon_type != m_last_local_weapon_type)
	{
		m_last_local_weapon_type = selected_weapon_type;
		++m_local_weapon_sequence;
	}
	m_local_input.weapon_sequence = m_local_weapon_sequence;
	ReleaseSRWLockExclusive(&m_input_lock);
}

void CoopNetGame::PublishLocalFlyTransform(const void* fly)
{
	bool controlled = IsLocalFlyControlled();
	float position[4] = {};
	if (controlled)
	{
		if (!fly)
			controlled = false;
		else
		{
			__try
			{
				memcpy(position, static_cast<const BYTE*>(fly) +
					kEntityPositionOffset, sizeof(position));
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				controlled = false;
			}
		}
	}

	AcquireSRWLockExclusive(&m_input_lock);
	// The owner can be released while the native controller is ticking.  Never
	// resurrect it from this late transform write.
	if (controlled && m_local_input.fly_controlled != 0)
	{
		memcpy(m_local_input.fly_position, position, sizeof(position));
		m_local_input.fly_transform_sequence = ++m_local_fly_transform_sequence;
	}
	else if (m_local_input.fly_controlled == 0)
	{
		m_local_input.fly_transform_sequence = 0;
	}
	ReleaseSRWLockExclusive(&m_input_lock);
}

void CoopNetGame::PublishLocalCameraYaw(float yaw, bool valid)
{
	if (valid && !(yaw > -1000.0f && yaw < 1000.0f))
		valid = false;
	AcquireSRWLockExclusive(&m_input_lock);
	m_local_input.camera_yaw = valid ? yaw : 0.0f;
	m_local_input.camera_yaw_valid = valid ? 1u : 0u;
	ReleaseSRWLockExclusive(&m_input_lock);
}

bool CoopNetGame::ApplyRemotePlayerTransform(void* player2)
{
	if (!player2 || !IsRemoteInputActiveOnThisThread() ||
		m_active_remote_input.transform_sequence == 0)
		return false;
	__try
	{
		BYTE* bytes = static_cast<BYTE*>(player2);
		float* const position = reinterpret_cast<float*>(
			bytes + kEntityPositionOffset);
		float* const rotation = reinterpret_cast<float*>(
			bytes + kEntityRotationOffset);

		// P2's stock controller has just processed the buffered remote input.
		// Keep that motor result: it owns locomotion, turns and the associated
		// animation transitions.  The network transform is a correction target,
		// rather than a full replacement of position every frame.
		const DWORD now = GetTickCount();
		DWORD elapsed_ms = m_last_remote_transform_apply_tick == 0 ? 16 :
			now - m_last_remote_transform_apply_tick;
		m_last_remote_transform_apply_tick = now;
		if (elapsed_ms > 50)
			elapsed_ms = 50;
		const float delta_seconds = static_cast<float>(elapsed_ms) * 0.001f;
		const float dx = m_active_remote_input.position[0] - position[0];
		const float dy = m_active_remote_input.position[1] - position[1];
		const float dz = m_active_remote_input.position[2] - position[2];
		const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
		float speed = distance;
		if (speed < 9.0f)
			speed = 9.0f;
		else if (speed > 13.0f)
			speed = 13.0f;
		float factor = speed * 0.7f * delta_seconds;
		if (factor > 1.0f)
			factor = 1.0f;
		for (unsigned index = 0; index != 3; ++index)
		{
			position[index] += (m_active_remote_input.position[index] -
				position[index]) * factor;
			rotation[index] += (m_active_remote_input.rotation[index] -
				rotation[index]) * factor;
		}
		position[3] = m_active_remote_input.position[3];
		rotation[3] = m_active_remote_input.rotation[3];
		if (!m_logged_remote_transform)
		{
			CoopRuntime::Instance().Log(
				"[net-transform pid=%lu] remote target correction active for P2\r\n",
				GetCurrentProcessId());
			m_logged_remote_transform = true;
		}
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log(
			"[net-transform-error] could not apply remote P1 transform to P2\r\n");
		return false;
	}
}

bool CoopNetGame::ApplyRemoteFlyTransform(void* fly)
{
	if (!fly)
		return false;
	CoopInput remote = {};
	if (!GetRemoteInput(remote) || remote.fly_controlled == 0 ||
		remote.fly_transform_sequence == 0)
	{
		return false;
	}
	__try
	{
		memcpy(static_cast<BYTE*>(fly) + kEntityPositionOffset,
			remote.fly_position, sizeof(remote.fly_position));
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log(
			"[fly-sync-error] could not apply remote Mooch position\r\n");
		return false;
	}
}

void __fastcall CoopNetGame::HookWeaponAmmoConsume(void* weapon_record,
	void*)
{
	Instance().HandleWeaponAmmoConsume(weapon_record);
}

void __fastcall CoopNetGame::HookTriggerSpawnFromDefinition(void* trigger,
	void*)
{
	Instance().HandleTriggerSpawnFromDefinition(trigger);
}

void* __cdecl CoopNetGame::HookTriggerFactory(std::uint32_t family,
	std::uint32_t subtype, void* output)
{
	return Instance().HandleTriggerFactory(family, subtype, output,
		reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}

int __fastcall CoopNetGame::HookTriggerEvent(void* trigger, void*,
	int event_code)
{
	return Instance().HandleTriggerEvent(trigger, event_code);
}

void CoopNetGame::HandleTriggerSpawnFromDefinition(void* trigger)
{
	if (!m_original_trigger_spawn)
		return;

	m_original_trigger_spawn(trigger);
	if (!trigger)
		return;

	// Do not emit noise for every scripted object in the level.  The two
	// families below are the exact shared path for live NPCs and monster/
	// appliance enemies, which is the population the co-op layer needs to own.
	__try
	{
		const BYTE* const bytes = static_cast<const BYTE*>(trigger);
		const std::uint32_t family = *reinterpret_cast<const std::uint32_t*>(
			bytes + kTriggerFamilyOffset);
		if (family != kMonsterTriggerFamily && family != kNpcTriggerFamily)
			return;

		const char* role = IsHost() ? "host" :
			(IsClient() ? "client" : "none");
		const unsigned sequence = static_cast<unsigned>(
			InterlockedIncrement(&m_trigger_spawn_sequence));
		const std::uint32_t subtype = *reinterpret_cast<const std::uint32_t*>(
			bytes + kTriggerSubtypeOffset);
		const std::int32_t definition_id = *reinterpret_cast<const std::int32_t*>(
			bytes + kTriggerSpawnIdOffset);
		// Factory construction precedes assignment of definition_id.  At this later
		// point the trigger can be retained as a client-side native spawn template
		// even though its entity is not in the live registry quite yet.
		WorldSync::Instance().RecordTriggerTemplate(trigger, family, subtype);
		const void* const spawned_object = *reinterpret_cast<void* const*>(
			bytes + kTriggerSpawnedObjectOffset);
		const float* const position = reinterpret_cast<const float*>(
			bytes + kTriggerPositionOffset);
		const void* live_entity = NULL;
		const float* live_position = NULL;
		BYTE* const registry = *reinterpret_cast<BYTE**>(kEntityRegistry);
		const size_t list_offsets[] = {
			kEntityRegistryMonsterListOffset, kEntityRegistryNpcListOffset
		};
		for (size_t list_index = 0; registry &&
			list_index < _countof(list_offsets) && !live_entity; ++list_index)
		{
			BYTE* node = *reinterpret_cast<BYTE**>(registry +
				list_offsets[list_index]);
			for (size_t visited = 0; node && visited != 512; ++visited)
			{
				BYTE* const entity = *reinterpret_cast<BYTE**>(node +
					kIntrusiveListValueOffset);
				if (entity && *reinterpret_cast<void**>(entity +
					kEntityTriggerOffset) == trigger)
				{
					live_entity = entity;
					live_position = reinterpret_cast<const float*>(entity +
						kEntityPositionOffset);
					break;
				}
				node = *reinterpret_cast<BYTE**>(node +
					kIntrusiveListNextOffset);
			}
		}
		CoopRuntime::Instance().Log(
			"[world-spawn-trace] seq=%u role=%s peer=%d trigger=%p family=0x%08X subtype=0x%08X def=%d object=%p live=%p trigger_pos=(%.2f,%.2f,%.2f) entity_pos=(%.2f,%.2f,%.2f)\r\n",
			sequence, role, HasRemotePeer() ? 1 : 0, trigger, family, subtype,
			definition_id, spawned_object, live_entity, position[0], position[1],
			position[2], live_position ? live_position[0] : 0.0f,
			live_position ? live_position[1] : 0.0f,
			live_position ? live_position[2] : 0.0f);
		if (live_entity)
		{
			WorldSync::Instance().RecordNativeSpawn(trigger,
				const_cast<void*>(live_entity), family,
				subtype, definition_id);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log(
			"[world-spawn-trace] trigger read fault after native spawn\r\n");
	}
}

void* CoopNetGame::HandleTriggerFactory(std::uint32_t family,
	std::uint32_t subtype, void* output, std::uintptr_t caller)
{
	if (!m_original_trigger_factory)
		return NULL;

	void* const trigger = m_original_trigger_factory(family, subtype, output);
	if (family != kMonsterTriggerFamily && family != kNpcTriggerFamily)
		return trigger;

	const char* role = IsHost() ? "host" :
		(IsClient() ? "client" : "none");
	const unsigned sequence = static_cast<unsigned>(
		InterlockedIncrement(&m_trigger_factory_sequence));
	WorldSync::Instance().RecordTriggerTemplate(trigger, family, subtype);
	CoopRuntime::Instance().Log(
		"[world-trigger-trace] seq=%u role=%s peer=%d caller=0x%08X family=0x%08X subtype=0x%08X trigger=%p output=%p\r\n",
		sequence, role, HasRemotePeer() ? 1 : 0,
		static_cast<unsigned>(caller), family, subtype, trigger, output);
	return trigger;
}

int CoopNetGame::HandleTriggerEvent(void* trigger, int event_code)
{
	if (!m_original_trigger_event)
		return 0;
	const int result = m_original_trigger_event(trigger, event_code);
	if (!trigger || !HasRemotePeer() || (!IsHost() && !IsClient()))
		return result;

	// This is diagnostic only.  The event must be observed before it is ever
	// replayed remotely: some ids are state updates rather than an activation.
	__try
	{
		const BYTE* const bytes = static_cast<const BYTE*>(trigger);
		const std::uint32_t family = *reinterpret_cast<const std::uint32_t*>(
			bytes + kTriggerFamilyOffset);
		if (family != kMonsterTriggerFamily && family != kNpcTriggerFamily)
			return result;
		const std::uint32_t subtype = *reinterpret_cast<const std::uint32_t*>(
			bytes + kTriggerSubtypeOffset);
		const std::int32_t definition_id = *reinterpret_cast<const std::int32_t*>(
			bytes + kTriggerSpawnIdOffset);
		const unsigned sequence = static_cast<unsigned>(
			InterlockedIncrement(&m_trigger_event_sequence));
		CoopRuntime::Instance().Log(
			"[world-trigger-event] seq=%u role=%s trigger=%p family=0x%08X subtype=0x%08X def=%d event=%d/0x%04X result=%d\r\n",
			sequence, IsHost() ? "host" : "client", trigger, family, subtype,
			definition_id, event_code, static_cast<unsigned>(event_code) & 0xFFFFu,
			result);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log(
			"[world-trigger-event] trigger read fault after event=%d\r\n",
			event_code);
	}
	return result;
}

bool CoopNetGame::SpawnWorldFromTrigger(void* trigger)
{
	if (!trigger || !m_original_trigger_spawn)
		return false;
	HandleTriggerSpawnFromDefinition(trigger);
	return true;
}

void CoopNetGame::HandleWeaponAmmoConsume(void* weapon_record)
{
	if (!m_original_weapon_ammo_consume)
		return;

	// 0x59F650 is the stock write that first reduces a weapon record's local
	// count (+0x67C), then mirrors that new value into the shared HUD pool.  The
	// P2 inventory record is armed during P2's tick, so P1's record remains a
	// normal stock write even when both players use the same weapon type.
	if (!weapon_record || weapon_record != m_remote_p2_weapon_record)
	{
		m_original_weapon_ammo_consume(weapon_record);
		return;
	}

	std::uint32_t p2_rounds_before = 0;
	std::uint32_t p1_pool_before = 0;
	std::uint32_t ammo_id = 0xFFFFFFFFu;
	void* ammo_entry = NULL;
	__try
	{
		BYTE* const record = static_cast<BYTE*>(weapon_record);
		p2_rounds_before = *reinterpret_cast<std::uint32_t*>(
			record + kWeaponRecordRoundCountOffset);
		ammo_id = *reinterpret_cast<std::uint32_t*>(
			record + kWeaponRecordAmmoIdOffset);
		if (ammo_id != 0xFFFFFFFFu)
		{
			ResolveAmmoEntryFn resolve_ammo_entry =
				reinterpret_cast<ResolveAmmoEntryFn>(kResolveAmmoEntry);
			ammo_entry = resolve_ammo_entry(
				reinterpret_cast<void*>(kAmmoPool), ammo_id);
			if (ammo_entry)
			{
				p1_pool_before = *reinterpret_cast<std::uint32_t*>(
					static_cast<BYTE*>(ammo_entry) + kAmmoEntryCurrentOffset);
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		ammo_entry = NULL;
	}

	m_original_weapon_ammo_consume(weapon_record);

	__try
	{
		// P2 is deliberately infinite: restore its own stock counter so its next
		// shot does not depend on the number visible to P1.  If this call had no
		// recognised ammo-pool entry (for example an energy-only weapon), this is
		// still the correct local P2 behaviour.
		*reinterpret_cast<std::uint32_t*>(static_cast<BYTE*>(weapon_record) +
			kWeaponRecordRoundCountOffset) = p2_rounds_before;
		if (ammo_entry)
		{
			*reinterpret_cast<std::uint32_t*>(static_cast<BYTE*>(ammo_entry) +
				kAmmoEntryCurrentOffset) = p1_pool_before;
			if (!m_logged_remote_p2_ammo_restore)
			{
				CoopRuntime::Instance().Log(
					"[p2-ammo] stock consume restored P2=%u; P1 pool ammo=0x%X current=%u\r\n",
					p2_rounds_before, ammo_id, p1_pool_before);
				m_logged_remote_p2_ammo_restore = true;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log(
			"[p2-ammo-error] unable to restore post-shot ammunition\r\n");
	}
}

bool CoopNetGame::GetActiveRemoteWeaponType(uint32_t& weapon_type) const
{
	weapon_type = 0xFFFFFFFFu;
	if (!IsRemoteInputActiveOnThisThread() ||
		m_active_remote_input.transform_sequence == 0 ||
		m_active_remote_input.selected_weapon_type == 0xFFFFFFFFu)
	{
		return false;
	}
	weapon_type = m_active_remote_input.selected_weapon_type;
	return true;
}

void CoopNetGame::BeginRemoteInput()
{
	ZeroMemory(&m_active_remote_input, sizeof(m_active_remote_input));
	bool have_remote = GetRemoteInput(m_active_remote_input);
	BuildRemoteScanCodeState();
	ApplyRemoteKeyboardState();
	// Latch press/release edges for exactly this P2 frame.  An edge exists when
	// the sender's monotonic counter differs from the last one we consumed; we
	// then advance prev so the edge is reported for one frame only, no matter how
	// many times the weapon state machine polls the edge query this frame.  This
	// is what makes a single-press weapon fire exactly once and never repeat
	// while the button is held.  m_prev_remote_*_seq must NOT be reset here.
	//
	// Only evaluate edges when a real packet is present; otherwise the zeroed
	// snapshot would look like a falling counter versus a stale prev and forge a
	// phantom press (e.g. right after a disconnect).
	ZeroMemory(m_remote_press_edge, sizeof(m_remote_press_edge));
	ZeroMemory(m_remote_release_edge, sizeof(m_remote_release_edge));
	for (uint32_t i = 0; i < kCoopActionCount; ++i)
	{
		// Reconstruct per-action hold timing from the held-level bitset.  This runs
		// unconditionally and is safe with no packet: action_down was zeroed above,
		// so a disconnect clears any stale hold instead of leaving it stuck "held".
		const bool held = (m_active_remote_input.action_down[i / 32] &
			(1u << (i % 32))) != 0;
		if (held)
		{
			if (!m_remote_action_held[i])
			{
				m_remote_action_held[i] = true;
				m_remote_hold_start_tick[i] = GetTickCount();
			}
		}
		else
		{
			m_remote_action_held[i] = false;
			m_remote_hold_start_tick[i] = 0;
		}

		// Rising/falling edges only when a real packet is present; otherwise the
		// zeroed snapshot would look like a falling counter versus a stale prev and
		// forge a phantom press right after a disconnect.
		if (have_remote)
		{
			const uint8_t press = m_active_remote_input.action_press_seq[i];
			m_remote_press_edge[i] = (press != m_prev_remote_press_seq[i]);
			m_prev_remote_press_seq[i] = press;
			const uint8_t release = m_active_remote_input.action_release_seq[i];
			m_remote_release_edge[i] = (release != m_prev_remote_release_seq[i]);
			m_prev_remote_release_seq[i] = release;
		}
	}

	m_remote_input_thread_id = GetCurrentThreadId();
	InterlockedExchange(&m_remote_input_active, 1);
}

void CoopNetGame::EndRemoteInput()
{
	InterlockedExchange(&m_remote_input_active, 0);
	m_remote_input_thread_id = 0;
	RestoreKeyboardState();
	ZeroMemory(&m_active_remote_input, sizeof(m_active_remote_input));
	ZeroMemory(m_active_remote_scan_codes, sizeof(m_active_remote_scan_codes));
}

void CoopNetGame::ArmRemoteP2AmmoOwner(void* player2)
{
	m_remote_p2_weapon_record = NULL;
	if (!player2 || !IsRemoteInputActiveOnThisThread())
		return;

	__try
	{
		BYTE* const handler = *reinterpret_cast<BYTE**>(
			reinterpret_cast<BYTE*>(player2) + kEntityHandlerOffset);
		if (!handler)
			return;
		// The inventory is a pointer stored in the handler.  0x5D60EF performs
		// `mov edi, [handler+0x514]` before calling 0x5933E0; passing the address
		// of that field makes the resolver see an empty, unrelated structure.
		void* const inventory = *reinterpret_cast<void**>(
			handler + kHandlerInventoryOffset);
		if (!inventory)
			return;
		GetCurrentWeaponIdFn get_current_weapon_id =
			reinterpret_cast<GetCurrentWeaponIdFn>(kGetCurrentWeaponId);
		ResolveWeaponRecordFn resolve_weapon_record =
			reinterpret_cast<ResolveWeaponRecordFn>(kResolveWeaponRecord);
		std::uint32_t item_id = get_current_weapon_id(handler);
		void* weapon_record = item_id == 0xFFFFFFFFu ? NULL :
			resolve_weapon_record(inventory, item_id);
		if (!weapon_record)
		{
			// During draw the native current-item getter still reports melee, while
			// handler+0x26E0 already names the selected gun.  Use the same type ->
			// item mapping as the stock ammo HUD, then resolve it in P2's inventory.
			const std::uint32_t weapon_type = *reinterpret_cast<std::uint32_t*>(
				handler + kHandlerSelectedWeaponTypeOffset);
			WeaponTypeToItemIdFn weapon_type_to_item_id =
				reinterpret_cast<WeaponTypeToItemIdFn>(kWeaponTypeToItemId);
			item_id = weapon_type_to_item_id(weapon_type);
			if (item_id != 0xFFFFFFFFu)
				weapon_record = resolve_weapon_record(inventory, item_id);
		}
		if (weapon_record)
			m_remote_p2_weapon_record = weapon_record;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}


void CoopNetGame::RequestRemotePlayerTickDeferral()
{
	// A subsequent level/edge poll of the same local key must not downgrade the
	// confirmed "wait for Fly_Active" hand-off to a single frame.
	if (InterlockedCompareExchange(&m_defer_remote_player_tick,
		kDeferRemotePlayerOneTick, 0) == 0)
	{
		CoopRuntime::Instance().Log(
			"[fly] Darwin-to-Mooch transition: deferring one P2 remote tick\r\n");
	}
}

bool CoopNetGame::ConsumeRemotePlayerTickDeferral()
{
	const LONG state = InterlockedCompareExchange(&m_defer_remote_player_tick,
		0, 0);
	if (state == 0)
		return false;
	if (state == kDeferRemotePlayerOneTick)
	{
		InterlockedExchange(&m_defer_remote_player_tick, 0);
		return true;
	}

	bool fly_active = false;
	AcquireSRWLockShared(&m_input_lock);
	fly_active = m_local_input.fly_controlled != 0 && m_local_fly_active_seen;
	ReleaseSRWLockShared(&m_input_lock);
	if (fly_active)
	{
		InterlockedExchange(&m_defer_remote_player_tick, 0);
		CoopRuntime::Instance().Log(
			"[fly] Fly_Active observed; P2 remote tick resumed\r\n");
		return false;
	}
	if (GetTickCount() - m_fly_handoff_started_tick >= kFlyHandoffTimeoutMs)
	{
		InterlockedExchange(&m_defer_remote_player_tick, 0);
		CoopRuntime::Instance().Log(
			"[fly] Fly_Active was not observed in time; P2 remote tick resumed\r\n");
		return false;
	}
	return true;
}

void CoopNetGame::BuildRemoteScanCodeState()
{
	ZeroMemory(m_active_remote_scan_codes,
		sizeof(m_active_remote_scan_codes));
	for (unsigned virtual_key = 0; virtual_key < 256; ++virtual_key)
	{
		// Night vision and the map stay with the player who pressed the key.  This
		// also closes the raw paths that read the DirectInput array directly
		// (0x488A70's sign-bit branch, and the still unhooked 0x4008xxxx family),
		// not just the hooked logical-action queries.
		if (virtual_key == VK_TAB || virtual_key == 'Q')
			continue;
		if (!IsVirtualKeyDown(m_active_remote_input, virtual_key))
			continue;
		UINT scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC_EX);
		if ((scan_code & 0xFF00u) == 0xE000u)
			scan_code = (scan_code & 0xFFu) | 0x80u;
		else
			scan_code &= 0xFFu;
		if (scan_code != 0)
			m_active_remote_scan_codes[scan_code] = 0x80;
	}
}

bool CoopNetGame::IsRemoteInputActiveOnThisThread() const
{
	return InterlockedCompareExchange(
		const_cast<volatile LONG*>(&m_remote_input_active), 0, 0) != 0 &&
		m_remote_input_thread_id == GetCurrentThreadId();
}

void CoopNetGame::ApplyRemoteKeyboardState()
{
	if (m_keyboard_state_swapped)
		return;
	BYTE* input_owner = NULL;
	__try
	{
		input_owner = *reinterpret_cast<BYTE**>(kKeyboardStateOwner);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return;
	}
	if (!input_owner)
		return;

	BYTE* keyboard_state = input_owner + kKeyboardStateBytesOffset;
	BYTE* secondary_keyboard_state = input_owner +
		kKeyboardStateSecondaryBytesOffset;
	__try
	{
		memcpy(m_saved_keyboard_state, keyboard_state,
			kKeyboardStateBytes);
		memcpy(m_saved_keyboard_state_secondary, secondary_keyboard_state,
			kKeyboardStateBytes);
		m_keyboard_state_buffer = keyboard_state;
		m_keyboard_state_secondary_buffer = secondary_keyboard_state;
		m_keyboard_state_swapped = true;
		memcpy(keyboard_state, m_active_remote_scan_codes,
			kKeyboardStateBytes);
		// The action edge/hold path (0x488DC0) reads this second DirectInput
		// array.  Replacing only +0x04 lets some actions through but leaves
		// GPig locomotion in idle.
		memcpy(secondary_keyboard_state, m_active_remote_scan_codes,
			kKeyboardStateBytes);
		if (!m_logged_keyboard_state_swap)
		{
			CoopRuntime::Instance().Log(
				"[netgame] DirectInput keyboard snapshot override active for P2\r\n");
			m_logged_keyboard_state_swap = true;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		RestoreKeyboardState();
	}
}

void CoopNetGame::RestoreKeyboardState()
{
	if (!m_keyboard_state_swapped || !m_keyboard_state_buffer ||
		!m_keyboard_state_secondary_buffer)
		return;
	__try
	{
		memcpy(m_keyboard_state_buffer, m_saved_keyboard_state,
			kKeyboardStateBytes);
		memcpy(m_keyboard_state_secondary_buffer,
			m_saved_keyboard_state_secondary, kKeyboardStateBytes);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
	m_keyboard_state_buffer = NULL;
	m_keyboard_state_secondary_buffer = NULL;
	m_keyboard_state_swapped = false;
}

SHORT CoopNetGame::HandleGetAsyncKeyState(int virtual_key)
{
	if (IsRemoteInputActiveOnThisThread() &&
		virtual_key >= 0 && virtual_key < 256)
	{
		// Same exclusion as BuildRemoteScanCodeState: never hand P2 the sender's
		// TAB or Q, or the receiver toggles its own night vision or map UI.
		if (virtual_key == VK_TAB || virtual_key == 'Q')
			return 0;
		if (IsVirtualKeyDown(m_active_remote_input,
			static_cast<unsigned>(virtual_key)))
			return static_cast<SHORT>(0x8000);
		return 0;
	}
	return m_original_get_async_key_state ?
		m_original_get_async_key_state(virtual_key) : 0;
}

bool __fastcall CoopNetGame::HandleInputActionQuery(void* input_manager,
	void*, std::uint32_t device, std::uint32_t action, std::uint32_t flags)
{
	if (!m_original_input_action_query)
		return false;

	const bool is_remote_thread = IsRemoteInputActiveOnThisThread();
	const bool is_keyboard_action = (action >= kFirstKeyboardActionId &&
		action < kFirstKeyboardActionId + kKeyboardActionCount);

	// 0x488A70 is a level ("is-down now") query.  On P2 return the held state
	// straight from the packet; rising/falling transitions are served by the
	// dedicated edge hooks (0x488CE0 / 0x488C00) instead.
	if (is_remote_thread && is_keyboard_action)
	{
		// The same Fire action drives both Darwin weapons and Mooch.  While the
		// sender owns Mooch, reserve Fire for Fly_Scan's explicit call-site below;
		// feeding it into P2 here makes the remote Darwin attack at the same time.
		if (action == kFireActionId && IsRemoteFlyControlled())
			return false;
		return GetActiveRemoteAction(action);
	}
	if (!is_remote_thread && action == kFireActionId &&
		reinterpret_cast<std::uintptr_t>(_ReturnAddress()) ==
			kFlyScanFireActionQueryReturn && IsRemoteFlyControlled())
	{
		// Only XControllerMode_Fly_Scan gets the remote Fire state.  Darwin's
		// weapon paths use the same logical action and must keep physical input.
		return GetRemoteFlyFireAction();
	}
	if (is_keyboard_action && IsMoochAction(action) &&
		IsRemoteFlyControlled())
	{
		return false;
	}

	const bool result = m_original_input_action_query(input_manager, device,
		action, flags);
	if (is_keyboard_action && !is_remote_thread)
		CaptureLocalAction(action, result);
	return result;
}

bool __fastcall CoopNetGame::HandleInputActionUpQuery(void* input_manager,
	void*, std::uint32_t device, std::uint32_t action, std::uint32_t flags)
{
	if (!m_original_input_action_up_query)
		return false;

	const bool is_keyboard_action = (action >= kFirstKeyboardActionId &&
		action < kFirstKeyboardActionId + kKeyboardActionCount);
	if (IsRemoteInputActiveOnThisThread() && is_keyboard_action)
	{
		// 0x488B70 is the inverse level query: returning P1's physical key
		// state here made P2 simultaneously see remote "move down" and local
		// "move up", leaving the motor in idle while transform correction slid it.
		return action == kFireActionId && IsRemoteFlyControlled() ? true :
			!GetActiveRemoteAction(action);
	}
	if (is_keyboard_action && IsMoochAction(action) &&
		IsRemoteFlyControlled())
	{
		return true;
	}

	return m_original_input_action_up_query(input_manager, device, action,
		flags);
}

bool __fastcall CoopNetGame::HandleInputPressedQuery(void* input_manager,
	void*, std::uint32_t device, std::uint32_t action, std::uint32_t flags)
{
	if (!m_original_input_pressed_query)
		return false;

	const bool is_remote_thread = IsRemoteInputActiveOnThisThread();
	const bool is_keyboard_action = (action >= kFirstKeyboardActionId &&
		action < kFirstKeyboardActionId + kKeyboardActionCount);

	// 0x488CE0 is the "pressed this frame" rising-edge query the single-press /
	// single-click weapons poll.  On P2 answer from the latched remote edge so a
	// remote press fires exactly once.
	if (is_remote_thread && is_keyboard_action)
	{
		if (action == kFireActionId && IsRemoteFlyControlled())
			return false;
		if (IsMirrorSuppressedAction(action))
			return false;
		const uint32_t action_index = action - kFirstKeyboardActionId;
		const bool edge = m_remote_press_edge[action_index];
		if (edge && action == kFireActionId)
		{
			static volatile LONG s_logged_fire_press = 0;
			if (InterlockedCompareExchange(&s_logged_fire_press, 1, 0) == 0)
			{
				uint32_t weapon_type = 0xFFFFFFFFu;
				GetActiveRemoteWeaponType(weapon_type);
				CoopRuntime::Instance().Log(
					"[netgame-diag] remote FIRE press-edge reached P2 "
					"(action=0x%08X weapon_type=%d)\r\n",
					action, static_cast<int>(weapon_type));
			}
		}
		return edge;
	}
	if (is_keyboard_action && IsMoochAction(action) &&
		IsRemoteFlyControlled())
	{
		return false;
	}

	const bool result = m_original_input_pressed_query(input_manager, device,
		action, flags);
	if (is_keyboard_action && !is_remote_thread && result)
		CaptureLocalPress(action);
	return result;
}

bool __fastcall CoopNetGame::HandleInputReleasedQuery(void* input_manager,
	void*, std::uint32_t device, std::uint32_t action, std::uint32_t flags)
{
	if (!m_original_input_released_query)
		return false;

	const bool is_remote_thread = IsRemoteInputActiveOnThisThread();
	const bool is_keyboard_action = (action >= kFirstKeyboardActionId &&
		action < kFirstKeyboardActionId + kKeyboardActionCount);

	// 0x488C00 is the "released this frame" falling-edge query.  Semi-auto weapons
	// use it to re-arm the trigger, so P2 must see the remote release exactly once.
	if (is_remote_thread && is_keyboard_action)
	{
		if (action == kFireActionId && IsRemoteFlyControlled())
			return false;
		if (IsMirrorSuppressedAction(action))
			return false;
		const uint32_t action_index = action - kFirstKeyboardActionId;
		return m_remote_release_edge[action_index];
	}
	if (is_keyboard_action && IsMoochAction(action) &&
		IsRemoteFlyControlled())
	{
		return false;
	}

	const bool result = m_original_input_released_query(input_manager, device,
		action, flags);
	if (is_keyboard_action && !is_remote_thread && result)
		CaptureLocalRelease(action);
	return result;
}

bool __fastcall CoopNetGame::HandleInputHoldDurationQuery(void* input_manager,
	void*, std::uint32_t device, std::uint32_t action, float threshold,
	std::uint32_t flags)
{
	if (!m_original_input_hold_duration_query)
		return false;

	const bool is_remote_thread = IsRemoteInputActiveOnThisThread();
	const bool is_keyboard_action = (action >= kFirstKeyboardActionId &&
		action < kFirstKeyboardActionId + kKeyboardActionCount);

	// 0x488E50 gates on how long an action has been held.  Reconstruct the hold
	// duration on P2 from the held-level stream and honour the exact threshold, so
	// a remote tap does not read as an instantly-completed charge (which made the
	// melee whip's special fire the moment remote fire went down and keep firing).
	if (is_remote_thread && is_keyboard_action)
	{
		if (action == kFireActionId && IsRemoteFlyControlled())
			return false;
		return GetActiveRemoteHold(action, threshold);
	}

	const bool result = m_original_input_hold_duration_query(input_manager,
		device, action, threshold, flags);
	// This answers a derived "held for >= N" condition, not the physical level
	// of the action.  Feeding it into action_down turns a stale charge result
	// into a phantom held LMB on P2 (the whip immediately starts its special).
	// action_down is captured exclusively from the 0x488A70 level query.
	return result;
}

bool __fastcall CoopNetGame::HandleInputAimHoldQuery(void* input_manager,
	void*, std::uint32_t device, std::uint32_t action, std::uint32_t flags,
	float threshold)
{
	if (!m_original_input_aim_hold_query)
		return false;

	// 0x488B00 is what decides the aim branch of 0x5BB1D0: 0x5BB321 asks for
	// 0x10000006 and 0x5BB34D for 0x10000011, and a false from both jumps to the
	// release path at 0x5BB9FD.  Unhooked, P2 answered from the local physical
	// mouse, so P2 entered or left aim together with whoever sat at this machine
	// instead of following the remote player.
	if (IsRemoteInputActiveOnThisThread() &&
		action >= kFirstKeyboardActionId &&
		action < kFirstKeyboardActionId + kKeyboardActionCount)
	{
		if (action == kFireActionId && IsRemoteFlyControlled())
			return false;
		return GetActiveRemoteHold(action, threshold);
	}

	return m_original_input_aim_hold_query(input_manager, device, action, flags,
		threshold);
}

bool __fastcall CoopNetGame::HandleInputRawPressedQuery(void* input_manager,
	void*, void* device, std::uint32_t action, std::uint32_t flags, bool record)
{
	if (!m_original_input_raw_pressed_query)
		return false;
	if (FindFlyRawActionIndex(action) >= 0 && IsRemoteFlyControlled())
		return ConsumeRemoteFlyRawEdge(action, true);
	const bool result = m_original_input_raw_pressed_query(input_manager,
		device, action, flags, record);
	if (result)
		CaptureLocalFlyRaw(action, true, true, false);
	return result;
}

bool __fastcall CoopNetGame::HandleInputRawReleasedQuery(void* input_manager,
	void*, void* device, std::uint32_t action, std::uint32_t flags, bool record)
{
	if (!m_original_input_raw_released_query)
		return false;
	if (FindFlyRawActionIndex(action) >= 0 && IsRemoteFlyControlled())
		return ConsumeRemoteFlyRawEdge(action, false);

	const bool result = m_original_input_raw_released_query(input_manager,
		device, action, flags, record);
	if (result)
		CaptureLocalFlyRaw(action, false, false, true);
	return result;
}

bool __fastcall CoopNetGame::HandleInputRawHeldQuery(void* input_manager,
	void*, void* device, std::uint32_t action, std::uint32_t flags, bool record)
{
	if (!m_original_input_raw_held_query)
		return false;
	if (FindFlyRawActionIndex(action) >= 0 && IsRemoteFlyControlled())
		return GetRemoteFlyRawHeld(action);

	const bool result = m_original_input_raw_held_query(input_manager, device,
		action, flags, record);
	CaptureLocalFlyRaw(action, result, false, false);
	return result;
}

bool __fastcall CoopNetGame::HandleInputThresholdQuery(void* input_manager,
	void*, std::uint32_t device, std::uint32_t action, float threshold,
	std::uint32_t flags)
{
	if (!m_original_input_threshold_query)
		return false;
	// 0x488DC0 also gates on hold time (this is the flamethrower windup and the
	// whip charge state).  Use the same reconstructed hold timing as 0x488E50 and
	// honour the passed threshold instead of returning the raw held level.
	if (IsRemoteInputActiveOnThisThread() &&
		action >= kFirstKeyboardActionId &&
		action < kFirstKeyboardActionId + kKeyboardActionCount)
	{
		if (action == kFireActionId && IsRemoteFlyControlled())
			return false;
		return GetActiveRemoteHold(action, threshold);
	}

	const bool result = m_original_input_threshold_query(input_manager, device,
		action, threshold, flags);
	// 0x488DC0 is likewise a time/threshold gate.  Never overwrite the
	// packet's held level with its result; a threshold passing is not a key-down.
	return result;
}

float CoopNetGame::GetRemoteLookAxis(std::uint32_t axis) const
{
	return axis < 2 ? m_active_remote_input.look_axis[axis] : 0.0f;
}

float __fastcall CoopNetGame::HandleInputAxisQuery(void* input_manager,
	void*, std::uint32_t device, std::uint32_t axis, std::uint32_t flags)
{
	if (!m_original_input_axis_query)
		return 0.0f;

	if (!IsRemoteInputActiveOnThisThread() || axis > 1)
	{
		const float value = m_original_input_axis_query(input_manager, device,
			axis, flags);
		if (axis <= 1 && device == 0)
			CaptureLocalLookAxis(axis, value);
		return value;
	}

	// P2's tick: return remote look_axis from packet
	if (!m_logged_axis_queries[axis])
	{
		m_logged_axis_queries[axis] = true;
		CoopRuntime::Instance().Log(
			"[net-axis pid=%lu] axis=%u device=%u remote-look-backed\r\n",
			GetCurrentProcessId(), axis, device);
	}
	static volatile LONG s_logged_remote_look[2] = {0, 0};
	if (axis <= 1 && InterlockedCompareExchange(&s_logged_remote_look[axis], 1, 0) == 0)
	{
		CoopRuntime::Instance().Log("[netgame-diag] P2 remote look_axis[%u]=%.3f (from packet)\r\n",
			axis, GetRemoteLookAxis(axis));
	}
	return GetRemoteLookAxis(axis);
}

bool CoopNetGame::GetActiveRemoteCameraYaw(float& yaw) const
{
	if (!m_active_remote_input.camera_yaw_valid)
		return false;
	const float value = m_active_remote_input.camera_yaw;
	// A packet from a mismatched or corrupted build must never reach the turn
	// task: the yaw is fed straight into [turn_task+0x10] and atan2 sums.
	if (!(value > -1000.0f && value < 1000.0f))
		return false;
	yaw = value;
	return true;
}

float __fastcall CoopNetGame::HandleCameraYawQuery(void* camera_handler, void*)
{
	if (!m_original_camera_yaw)
		return 0.0f;
	// Only P2's own tick is diverted.  Everything else — P1's controller, the
	// scan/fly/mooch modes, the camera itself — keeps reading the real handler.
	if (IsRemoteInputActiveOnThisThread())
	{
		float remote_yaw = 0.0f;
		if (GetActiveRemoteCameraYaw(remote_yaw))
		{
			if (!m_logged_camera_yaw_override)
			{
				m_logged_camera_yaw_override = true;
				CoopRuntime::Instance().Log(
					"[netgame] P2 body turn uses the remote camera yaw %.3f instead of the shared handler\r\n",
					remote_yaw);
			}
			return remote_yaw;
		}
	}
	return m_original_camera_yaw(camera_handler);
}

void __fastcall CoopNetGame::HandleGPigCameraUpdate(void* mode, void*)
{
	if (!m_original_gpig_camera_update)
		return;
	// 0x5BCF30 is the camera update of the ticking GPig.  There is one camera
	// handler in the process.  P2 must never drive it, and after P1 has handed
	// control to Mooch its Default mode must not re-centre the same handler on
	// Darwin every frame.  Mooch owns its own stock camera path; no fly yaw or
	// position is manufactured here.
	if (IsRemoteInputActiveOnThisThread() || IsLocalFlyControlled())
	{
		if (IsRemoteInputActiveOnThisThread() &&
			!m_logged_camera_update_skip)
		{
			m_logged_camera_update_skip = true;
			CoopRuntime::Instance().Log(
				"[netgame] P2 tick no longer drives the shared camera update 0x5BCF30\r\n");
		}
		return;
	}
	m_original_gpig_camera_update(mode);
}

bool CoopNetGame::InstallActionQueryHook()
{
	if (m_action_query_hooked)
		return true;
	BYTE* target = reinterpret_cast<BYTE*>(kInputActionQuery);
	if (memcmp(target, kExpectedInputActionQuery,
		sizeof(m_original_input_action_query_bytes)) != 0)
	{
		CoopRuntime::Instance().Log(
			"[netgame-error] action-query bytes do not match at 0x%08X\r\n",
			static_cast<unsigned>(kInputActionQuery));
		return false;
	}

	BYTE* trampoline = static_cast<BYTE*>(VirtualAlloc(NULL, 10,
		MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
	if (!trampoline)
		return false;
	memcpy(m_original_input_action_query_bytes, target,
		sizeof(m_original_input_action_query_bytes));
	memcpy(trampoline, m_original_input_action_query_bytes,
		sizeof(m_original_input_action_query_bytes));
	trampoline[5] = 0xE9;
	*reinterpret_cast<int32_t*>(trampoline + 6) = static_cast<int32_t>(
		reinterpret_cast<intptr_t>(target + 5) -
		reinterpret_cast<intptr_t>(trampoline + 10));

	BYTE patch[5] = {0xE9, 0, 0, 0, 0};
	*reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(
		reinterpret_cast<intptr_t>(&HookInputActionQuery) -
		reinterpret_cast<intptr_t>(target + sizeof(patch)));
	DWORD old_protection = 0;
	if (!VirtualProtect(target, sizeof(patch), PAGE_EXECUTE_READWRITE,
		&old_protection))
	{
		VirtualFree(trampoline, 0, MEM_RELEASE);
		return false;
	}
	memcpy(target, patch, sizeof(patch));
	DWORD ignored = 0;
	VirtualProtect(target, sizeof(patch), old_protection, &ignored);
	FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
	m_input_action_trampoline = trampoline;
	m_original_input_action_query =
		reinterpret_cast<InputActionQueryFn>(trampoline);
	m_action_query_hooked = true;
	CoopRuntime::Instance().Log(
		"[netgame] packet-backed action-query hook installed\r\n");
	return true;
}

bool CoopNetGame::InstallActionUpQueryHook()
{
	if (m_action_up_query_hooked)
		return true;
	if (!InstallJmpHookRaw(kInputActionUpQuery, kExpectedInputActionUpQuery,
		sizeof(kExpectedInputActionUpQuery),
		reinterpret_cast<void*>(&HookInputActionUpQuery),
		m_original_input_action_up_query_bytes,
		&m_input_action_up_trampoline,
		"packet-backed action-up-query"))
	{
		return false;
	}
	m_original_input_action_up_query =
		reinterpret_cast<InputActionQueryFn>(m_input_action_up_trampoline);
	m_action_up_query_hooked = true;
	return true;
}

bool CoopNetGame::InstallAxisQueryHook()
{
	if (m_axis_query_hooked)
		return true;
	BYTE* target = reinterpret_cast<BYTE*>(kInputAxisQuery);
	if (memcmp(target, kExpectedInputAxisQuery,
		sizeof(m_original_input_axis_query_bytes)) != 0)
	{
		CoopRuntime::Instance().Log(
			"[netgame-error] axis-query bytes do not match at 0x%08X\r\n",
			static_cast<unsigned>(kInputAxisQuery));
		return false;
	}

	BYTE* trampoline = static_cast<BYTE*>(VirtualAlloc(NULL, 10,
		MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
	if (!trampoline)
		return false;
	memcpy(m_original_input_axis_query_bytes, target,
		sizeof(m_original_input_axis_query_bytes));
	memcpy(trampoline, m_original_input_axis_query_bytes,
		sizeof(m_original_input_axis_query_bytes));
	trampoline[5] = 0xE9;
	*reinterpret_cast<int32_t*>(trampoline + 6) = static_cast<int32_t>(
		reinterpret_cast<intptr_t>(target + 5) -
		reinterpret_cast<intptr_t>(trampoline + 10));

	BYTE patch[5] = {0xE9, 0, 0, 0, 0};
	*reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(
		reinterpret_cast<intptr_t>(&HookInputAxisQuery) -
		reinterpret_cast<intptr_t>(target + sizeof(patch)));
	DWORD old_protection = 0;
	if (!VirtualProtect(target, sizeof(patch), PAGE_EXECUTE_READWRITE,
		&old_protection))
	{
		VirtualFree(trampoline, 0, MEM_RELEASE);
		return false;
	}
	memcpy(target, patch, sizeof(patch));
	DWORD ignored = 0;
	VirtualProtect(target, sizeof(patch), old_protection, &ignored);
	FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
	m_input_axis_trampoline = trampoline;
	m_original_input_axis_query = reinterpret_cast<InputAxisQueryFn>(trampoline);
	m_axis_query_hooked = true;
	CoopRuntime::Instance().Log(
		"[netgame] packet-backed motor-axis hook installed\r\n");
	return true;
}

bool CoopNetGame::InstallThresholdQueryHook()
{
	if (m_threshold_query_hooked)
		return true;
	BYTE* target = reinterpret_cast<BYTE*>(kInputThresholdQuery);
	if (memcmp(target, kExpectedInputThresholdQuery,
		sizeof(m_original_input_threshold_query_bytes)) != 0)
	{
		CoopRuntime::Instance().Log(
			"[netgame-error] threshold-query bytes do not match at 0x%08X\r\n",
			static_cast<unsigned>(kInputThresholdQuery));
		return false;
	}

	BYTE* trampoline = static_cast<BYTE*>(VirtualAlloc(NULL, 10,
		MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
	if (!trampoline)
		return false;
	memcpy(m_original_input_threshold_query_bytes, target,
		sizeof(m_original_input_threshold_query_bytes));
	memcpy(trampoline, m_original_input_threshold_query_bytes,
		sizeof(m_original_input_threshold_query_bytes));
	trampoline[5] = 0xE9;
	*reinterpret_cast<int32_t*>(trampoline + 6) = static_cast<int32_t>(
		reinterpret_cast<intptr_t>(target + 5) -
		reinterpret_cast<intptr_t>(trampoline + 10));

	BYTE patch[5] = {0xE9, 0, 0, 0, 0};
	*reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(
		reinterpret_cast<intptr_t>(&HookInputThresholdQuery) -
		reinterpret_cast<intptr_t>(target + sizeof(patch)));
	DWORD old_protection = 0;
	if (!VirtualProtect(target, sizeof(patch), PAGE_EXECUTE_READWRITE,
		&old_protection))
	{
		VirtualFree(trampoline, 0, MEM_RELEASE);
		return false;
	}
	memcpy(target, patch, sizeof(patch));
	DWORD ignored = 0;
	VirtualProtect(target, sizeof(patch), old_protection, &ignored);
	FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
	m_input_threshold_trampoline = trampoline;
	m_original_input_threshold_query =
		reinterpret_cast<InputThresholdQueryFn>(trampoline);
	m_threshold_query_hooked = true;
	CoopRuntime::Instance().Log(
		"[netgame] packet-backed threshold-query hook installed\r\n");
	return true;
}

void CoopNetGame::RemoveActionQueryHook()
{
	if (!m_action_query_hooked)
		return;
	BYTE* target = reinterpret_cast<BYTE*>(kInputActionQuery);
	DWORD old_protection = 0;
	if (VirtualProtect(target, sizeof(m_original_input_action_query_bytes),
		PAGE_EXECUTE_READWRITE, &old_protection))
	{
		memcpy(target, m_original_input_action_query_bytes,
			sizeof(m_original_input_action_query_bytes));
		DWORD ignored = 0;
		VirtualProtect(target, sizeof(m_original_input_action_query_bytes),
			old_protection, &ignored);
		FlushInstructionCache(GetCurrentProcess(), target,
			sizeof(m_original_input_action_query_bytes));
	}
	if (m_input_action_trampoline)
		VirtualFree(m_input_action_trampoline, 0, MEM_RELEASE);
	m_input_action_trampoline = NULL;
	m_original_input_action_query = NULL;
	m_action_query_hooked = false;
}

void CoopNetGame::RemoveActionUpQueryHook()
{
	if (!m_action_up_query_hooked)
		return;
	RemoveJmpHookRaw(kInputActionUpQuery,
		m_original_input_action_up_query_bytes,
		sizeof(m_original_input_action_up_query_bytes),
		&m_input_action_up_trampoline);
	m_original_input_action_up_query = NULL;
	m_action_up_query_hooked = false;
}

void CoopNetGame::RemoveThresholdQueryHook()
{
	if (!m_threshold_query_hooked)
		return;
	BYTE* target = reinterpret_cast<BYTE*>(kInputThresholdQuery);
	DWORD old_protection = 0;
	if (VirtualProtect(target, sizeof(m_original_input_threshold_query_bytes),
		PAGE_EXECUTE_READWRITE, &old_protection))
	{
		memcpy(target, m_original_input_threshold_query_bytes,
			sizeof(m_original_input_threshold_query_bytes));
		DWORD ignored = 0;
		VirtualProtect(target, sizeof(m_original_input_threshold_query_bytes),
			old_protection, &ignored);
		FlushInstructionCache(GetCurrentProcess(), target,
			sizeof(m_original_input_threshold_query_bytes));
	}
	if (m_input_threshold_trampoline)
		VirtualFree(m_input_threshold_trampoline, 0, MEM_RELEASE);
	m_input_threshold_trampoline = NULL;
	m_original_input_threshold_query = NULL;
	m_threshold_query_hooked = false;
}

void CoopNetGame::RemoveAxisQueryHook()
{
	if (!m_axis_query_hooked)
		return;
	BYTE* target = reinterpret_cast<BYTE*>(kInputAxisQuery);
	DWORD old_protection = 0;
	if (VirtualProtect(target, sizeof(m_original_input_axis_query_bytes),
		PAGE_EXECUTE_READWRITE, &old_protection))
	{
		memcpy(target, m_original_input_axis_query_bytes,
			sizeof(m_original_input_axis_query_bytes));
		DWORD ignored = 0;
		VirtualProtect(target, sizeof(m_original_input_axis_query_bytes),
			old_protection, &ignored);
		FlushInstructionCache(GetCurrentProcess(), target,
			sizeof(m_original_input_axis_query_bytes));
	}
	if (m_input_axis_trampoline)
		VirtualFree(m_input_axis_trampoline, 0, MEM_RELEASE);
	m_input_axis_trampoline = NULL;
	m_original_input_axis_query = NULL;
	m_axis_query_hooked = false;
}

bool CoopNetGame::InstallJmpHookRaw(std::uintptr_t address,
	const std::uint8_t* expected, std::size_t relocate_len, void* hook,
	BYTE* saved_bytes, BYTE** trampoline_out, const char* label)
{
	BYTE* target = reinterpret_cast<BYTE*>(address);
	if (memcmp(target, expected, relocate_len) != 0)
	{
		CoopRuntime::Instance().Log(
			"[netgame-error] %s bytes do not match at 0x%08X\r\n",
			label, static_cast<unsigned>(address));
		return false;
	}

	BYTE* trampoline = static_cast<BYTE*>(VirtualAlloc(NULL, relocate_len + 5,
		MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
	if (!trampoline)
		return false;
	memcpy(saved_bytes, target, relocate_len);
	memcpy(trampoline, saved_bytes, relocate_len);
	trampoline[relocate_len] = 0xE9;
	*reinterpret_cast<int32_t*>(trampoline + relocate_len + 1) =
		static_cast<int32_t>(
			reinterpret_cast<intptr_t>(target + relocate_len) -
			reinterpret_cast<intptr_t>(trampoline + relocate_len + 5));

	// Only the first five bytes become the E9 rel32 detour; any relocated bytes
	// beyond that (edge queries relocate six) are never reached because control
	// transfers at the jump, and they are rewritten verbatim on removal.
	BYTE patch[5] = {0xE9, 0, 0, 0, 0};
	*reinterpret_cast<int32_t*>(patch + 1) = static_cast<int32_t>(
		reinterpret_cast<intptr_t>(hook) -
		reinterpret_cast<intptr_t>(target + sizeof(patch)));
	DWORD old_protection = 0;
	if (!VirtualProtect(target, relocate_len, PAGE_EXECUTE_READWRITE,
		&old_protection))
	{
		VirtualFree(trampoline, 0, MEM_RELEASE);
		return false;
	}
	memcpy(target, patch, sizeof(patch));
	DWORD ignored = 0;
	VirtualProtect(target, relocate_len, old_protection, &ignored);
	FlushInstructionCache(GetCurrentProcess(), target, relocate_len);
	*trampoline_out = trampoline;
	CoopRuntime::Instance().Log("[netgame] %s hook installed\r\n", label);
	return true;
}

void CoopNetGame::RemoveJmpHookRaw(std::uintptr_t address,
	const BYTE* saved_bytes, std::size_t relocate_len, BYTE** trampoline_ptr)
{
	BYTE* target = reinterpret_cast<BYTE*>(address);
	DWORD old_protection = 0;
	if (VirtualProtect(target, relocate_len, PAGE_EXECUTE_READWRITE,
		&old_protection))
	{
		memcpy(target, saved_bytes, relocate_len);
		DWORD ignored = 0;
		VirtualProtect(target, relocate_len, old_protection, &ignored);
		FlushInstructionCache(GetCurrentProcess(), target, relocate_len);
	}
	if (*trampoline_ptr)
		VirtualFree(*trampoline_ptr, 0, MEM_RELEASE);
	*trampoline_ptr = NULL;
}

bool CoopNetGame::InstallPressedQueryHook()
{
	if (m_pressed_query_hooked)
		return true;
	if (!InstallJmpHookRaw(kInputPressedQuery, kExpectedInputPressedQuery,
		sizeof(kExpectedInputPressedQuery),
		reinterpret_cast<void*>(&HookInputPressedQuery),
		m_original_input_pressed_query_bytes, &m_input_pressed_trampoline,
		"packet-backed pressed-edge-query"))
		return false;
	m_original_input_pressed_query =
		reinterpret_cast<InputActionQueryFn>(m_input_pressed_trampoline);
	m_pressed_query_hooked = true;
	return true;
}

void CoopNetGame::RemovePressedQueryHook()
{
	if (!m_pressed_query_hooked)
		return;
	RemoveJmpHookRaw(kInputPressedQuery, m_original_input_pressed_query_bytes,
		sizeof(m_original_input_pressed_query_bytes),
		&m_input_pressed_trampoline);
	m_original_input_pressed_query = NULL;
	m_pressed_query_hooked = false;
}

bool CoopNetGame::InstallReleasedQueryHook()
{
	if (m_released_query_hooked)
		return true;
	if (!InstallJmpHookRaw(kInputReleasedQuery, kExpectedInputReleasedQuery,
		sizeof(kExpectedInputReleasedQuery),
		reinterpret_cast<void*>(&HookInputReleasedQuery),
		m_original_input_released_query_bytes, &m_input_released_trampoline,
		"packet-backed released-edge-query"))
		return false;
	m_original_input_released_query =
		reinterpret_cast<InputActionQueryFn>(m_input_released_trampoline);
	m_released_query_hooked = true;
	return true;
}

void CoopNetGame::RemoveReleasedQueryHook()
{
	if (!m_released_query_hooked)
		return;
	RemoveJmpHookRaw(kInputReleasedQuery, m_original_input_released_query_bytes,
		sizeof(m_original_input_released_query_bytes),
		&m_input_released_trampoline);
	m_original_input_released_query = NULL;
	m_released_query_hooked = false;
}

bool CoopNetGame::InstallHoldDurationQueryHook()
{
	if (m_hold_duration_query_hooked)
		return true;
	if (!InstallJmpHookRaw(kInputHoldDurationQuery,
		kExpectedInputHoldDurationQuery,
		sizeof(kExpectedInputHoldDurationQuery),
		reinterpret_cast<void*>(&HookInputHoldDurationQuery),
		m_original_input_hold_duration_query_bytes,
		&m_input_hold_duration_trampoline,
		"packet-backed hold-duration-query"))
		return false;
	m_original_input_hold_duration_query =
		reinterpret_cast<InputThresholdQueryFn>(m_input_hold_duration_trampoline);
	m_hold_duration_query_hooked = true;
	return true;
}

void CoopNetGame::RemoveHoldDurationQueryHook()
{
	if (!m_hold_duration_query_hooked)
		return;
	RemoveJmpHookRaw(kInputHoldDurationQuery,
		m_original_input_hold_duration_query_bytes,
		sizeof(m_original_input_hold_duration_query_bytes),
		&m_input_hold_duration_trampoline);
	m_original_input_hold_duration_query = NULL;
	m_hold_duration_query_hooked = false;
}

bool CoopNetGame::InstallAimHoldQueryHook()
{
	if (m_aim_hold_query_hooked)
		return true;
	if (!InstallJmpHookRaw(kInputAimHoldQuery, kExpectedInputAimHoldQuery,
		sizeof(kExpectedInputAimHoldQuery),
		reinterpret_cast<void*>(&HookInputAimHoldQuery),
		m_original_input_aim_hold_query_bytes,
		&m_input_aim_hold_trampoline,
		"packet-backed aim-hold-query"))
		return false;
	m_original_input_aim_hold_query =
		reinterpret_cast<InputAimHoldQueryFn>(m_input_aim_hold_trampoline);
	m_aim_hold_query_hooked = true;
	return true;
}

void CoopNetGame::RemoveAimHoldQueryHook()
{
	if (!m_aim_hold_query_hooked)
		return;
	RemoveJmpHookRaw(kInputAimHoldQuery,
		m_original_input_aim_hold_query_bytes,
		sizeof(m_original_input_aim_hold_query_bytes),
		&m_input_aim_hold_trampoline);
	m_original_input_aim_hold_query = NULL;
	m_aim_hold_query_hooked = false;
}

bool CoopNetGame::InstallRawPressedQueryHook()
{
	if (m_raw_pressed_query_hooked)
		return true;
	if (!InstallJmpHookRaw(kInputRawPressedQuery, kExpectedInputRawQuery,
		sizeof(kExpectedInputRawQuery),
		reinterpret_cast<void*>(&HookInputRawPressedQuery),
		m_original_input_raw_pressed_query_bytes,
		&m_input_raw_pressed_trampoline, "packet-backed fly raw-pressed-query"))
	{
		return false;
	}
	m_original_input_raw_pressed_query =
		reinterpret_cast<InputRawQueryFn>(m_input_raw_pressed_trampoline);
	m_raw_pressed_query_hooked = true;
	return true;
}

void CoopNetGame::RemoveRawPressedQueryHook()
{
	if (!m_raw_pressed_query_hooked)
		return;
	RemoveJmpHookRaw(kInputRawPressedQuery,
		m_original_input_raw_pressed_query_bytes,
		sizeof(m_original_input_raw_pressed_query_bytes),
		&m_input_raw_pressed_trampoline);
	m_original_input_raw_pressed_query = NULL;
	m_raw_pressed_query_hooked = false;
}

bool CoopNetGame::InstallRawReleasedQueryHook()
{
	if (m_raw_released_query_hooked)
		return true;
	if (!InstallJmpHookRaw(kInputRawReleasedQuery, kExpectedInputRawQuery,
		sizeof(kExpectedInputRawQuery),
		reinterpret_cast<void*>(&HookInputRawReleasedQuery),
		m_original_input_raw_released_query_bytes,
		&m_input_raw_released_trampoline, "packet-backed fly raw-released-query"))
	{
		return false;
	}
	m_original_input_raw_released_query =
		reinterpret_cast<InputRawQueryFn>(m_input_raw_released_trampoline);
	m_raw_released_query_hooked = true;
	return true;
}

void CoopNetGame::RemoveRawReleasedQueryHook()
{
	if (!m_raw_released_query_hooked)
		return;
	RemoveJmpHookRaw(kInputRawReleasedQuery,
		m_original_input_raw_released_query_bytes,
		sizeof(m_original_input_raw_released_query_bytes),
		&m_input_raw_released_trampoline);
	m_original_input_raw_released_query = NULL;
	m_raw_released_query_hooked = false;
}

bool CoopNetGame::InstallRawHeldQueryHook()
{
	if (m_raw_held_query_hooked)
		return true;
	if (!InstallJmpHookRaw(kInputRawHeldQuery, kExpectedInputRawQuery,
		sizeof(kExpectedInputRawQuery),
		reinterpret_cast<void*>(&HookInputRawHeldQuery),
		m_original_input_raw_held_query_bytes, &m_input_raw_held_trampoline,
		"packet-backed fly raw-held-query"))
	{
		return false;
	}
	m_original_input_raw_held_query =
		reinterpret_cast<InputRawQueryFn>(m_input_raw_held_trampoline);
	m_raw_held_query_hooked = true;
	return true;
}

void CoopNetGame::RemoveRawHeldQueryHook()
{
	if (!m_raw_held_query_hooked)
		return;
	RemoveJmpHookRaw(kInputRawHeldQuery, m_original_input_raw_held_query_bytes,
		sizeof(m_original_input_raw_held_query_bytes),
		&m_input_raw_held_trampoline);
	m_original_input_raw_held_query = NULL;
	m_raw_held_query_hooked = false;
}

bool CoopNetGame::InstallCameraYawHook()
{
	if (m_camera_yaw_hooked)
		return true;
	if (!InstallJmpHookRaw(kCameraYawGetter, kExpectedCameraYawGetter,
		sizeof(kExpectedCameraYawGetter),
		reinterpret_cast<void*>(&HookCameraYaw),
		m_original_camera_yaw_bytes,
		&m_camera_yaw_trampoline,
		"packet-backed camera-yaw"))
		return false;
	m_original_camera_yaw =
		reinterpret_cast<CameraYawFn>(m_camera_yaw_trampoline);
	m_camera_yaw_hooked = true;
	return true;
}

void CoopNetGame::RemoveCameraYawHook()
{
	if (!m_camera_yaw_hooked)
		return;
	RemoveJmpHookRaw(kCameraYawGetter, m_original_camera_yaw_bytes,
		sizeof(m_original_camera_yaw_bytes), &m_camera_yaw_trampoline);
	m_original_camera_yaw = NULL;
	m_camera_yaw_hooked = false;
}

bool CoopNetGame::InstallGPigCameraUpdateHook()
{
	if (m_gpig_camera_update_hooked)
		return true;
	if (!InstallJmpHookRaw(kGPigCameraUpdate, kExpectedGPigCameraUpdate,
		sizeof(kExpectedGPigCameraUpdate),
		reinterpret_cast<void*>(&HookGPigCameraUpdate),
		m_original_gpig_camera_update_bytes,
		&m_gpig_camera_update_trampoline,
		"P1-only shared camera update"))
		return false;
	m_original_gpig_camera_update =
		reinterpret_cast<GPigCameraUpdateFn>(m_gpig_camera_update_trampoline);
	m_gpig_camera_update_hooked = true;
	return true;
}

void CoopNetGame::RemoveGPigCameraUpdateHook()
{
	if (!m_gpig_camera_update_hooked)
		return;
	RemoveJmpHookRaw(kGPigCameraUpdate, m_original_gpig_camera_update_bytes,
		sizeof(m_original_gpig_camera_update_bytes),
		&m_gpig_camera_update_trampoline);
	m_original_gpig_camera_update = NULL;
	m_gpig_camera_update_hooked = false;
}

bool CoopNetGame::InstallDefaultModeUpdateHook()
{
	if (m_default_mode_update_hooked)
		return true;
	if (!InstallJmpHookRaw(kDefaultModeUpdate, kExpectedDefaultModeUpdate,
		sizeof(kExpectedDefaultModeUpdate),
		reinterpret_cast<void*>(&HookDefaultModeUpdate),
		m_original_default_mode_update_bytes,
		&m_default_mode_update_trampoline,
		"packet-backed XGamePad aim-ray"))
	{
		return false;
	}
	m_original_default_mode_update =
		reinterpret_cast<DefaultModeUpdateFn>(m_default_mode_update_trampoline);
	m_default_mode_update_hooked = true;
	return true;
}

void CoopNetGame::RemoveDefaultModeUpdateHook()
{
	if (!m_default_mode_update_hooked)
		return;
	RemoveJmpHookRaw(kDefaultModeUpdate, m_original_default_mode_update_bytes,
		sizeof(m_original_default_mode_update_bytes),
		&m_default_mode_update_trampoline);
	m_original_default_mode_update = NULL;
	m_default_mode_update_hooked = false;
}

bool CoopNetGame::InstallFireHandlerHook()
{
	if (m_fire_handler_hooked)
		return true;
	if (!InstallJmpHookRaw(kFireHandler, kExpectedFireHandler,
		sizeof(kExpectedFireHandler), reinterpret_cast<void*>(&HookFireHandler),
		m_original_fire_handler_bytes, &m_fire_handler_trampoline,
		"packet-backed fire-ray"))
	{
		return false;
	}
	m_original_fire_handler =
		reinterpret_cast<FireHandlerFn>(m_fire_handler_trampoline);
	m_fire_handler_hooked = true;
	return true;
}

void CoopNetGame::RemoveFireHandlerHook()
{
	if (!m_fire_handler_hooked)
		return;
	RemoveJmpHookRaw(kFireHandler, m_original_fire_handler_bytes,
		sizeof(m_original_fire_handler_bytes), &m_fire_handler_trampoline);
	m_original_fire_handler = NULL;
	m_fire_handler_hooked = false;
}

bool CoopNetGame::InstallWeaponAmmoConsumeHook()
{
	if (m_weapon_ammo_consume_hooked)
		return true;
	if (!InstallJmpHookRaw(kWeaponAmmoConsume, kExpectedWeaponAmmoConsume,
		sizeof(kExpectedWeaponAmmoConsume),
		reinterpret_cast<void*>(&HookWeaponAmmoConsume),
		m_original_weapon_ammo_consume_bytes,
		&m_weapon_ammo_consume_trampoline, "P2 ammo ownership"))
	{
		return false;
	}
	m_original_weapon_ammo_consume = reinterpret_cast<WeaponAmmoConsumeFn>(
		m_weapon_ammo_consume_trampoline);
	m_weapon_ammo_consume_hooked = true;
	return true;
}

void CoopNetGame::RemoveWeaponAmmoConsumeHook()
{
	if (!m_weapon_ammo_consume_hooked)
		return;
	RemoveJmpHookRaw(kWeaponAmmoConsume, m_original_weapon_ammo_consume_bytes,
		sizeof(m_original_weapon_ammo_consume_bytes),
		&m_weapon_ammo_consume_trampoline);
	m_original_weapon_ammo_consume = NULL;
	m_weapon_ammo_consume_hooked = false;
}

bool CoopNetGame::InstallTriggerSpawnTraceHook()
{
	if (m_trigger_spawn_trace_hooked)
		return true;
	if (!InstallJmpHookRaw(kTriggerSpawnFromDefinition,
		kExpectedTriggerSpawnFromDefinition,
		sizeof(kExpectedTriggerSpawnFromDefinition),
		reinterpret_cast<void*>(&HookTriggerSpawnFromDefinition),
		m_original_trigger_spawn_bytes, &m_trigger_spawn_trampoline,
		"NPC/monster native spawn trace"))
	{
		return false;
	}
	m_original_trigger_spawn = reinterpret_cast<TriggerSpawnFromDefinitionFn>(
		m_trigger_spawn_trampoline);
	m_trigger_spawn_trace_hooked = true;
	return true;
}

void CoopNetGame::RemoveTriggerSpawnTraceHook()
{
	if (!m_trigger_spawn_trace_hooked)
		return;
	RemoveJmpHookRaw(kTriggerSpawnFromDefinition, m_original_trigger_spawn_bytes,
		sizeof(m_original_trigger_spawn_bytes), &m_trigger_spawn_trampoline);
	m_original_trigger_spawn = NULL;
	m_trigger_spawn_trace_hooked = false;
}

bool CoopNetGame::InstallTriggerFactoryTraceHook()
{
	if (m_trigger_factory_trace_hooked)
		return true;
	if (!InstallJmpHookRaw(kTriggerFactory, kExpectedTriggerFactory,
		sizeof(kExpectedTriggerFactory),
		reinterpret_cast<void*>(&HookTriggerFactory),
		m_original_trigger_factory_bytes, &m_trigger_factory_trampoline,
		"NPC/monster trigger factory trace"))
	{
		return false;
	}
	m_original_trigger_factory = reinterpret_cast<TriggerFactoryFn>(
		m_trigger_factory_trampoline);
	m_trigger_factory_trace_hooked = true;
	return true;
}

void CoopNetGame::RemoveTriggerFactoryTraceHook()
{
	if (!m_trigger_factory_trace_hooked)
		return;
	RemoveJmpHookRaw(kTriggerFactory, m_original_trigger_factory_bytes,
		sizeof(m_original_trigger_factory_bytes), &m_trigger_factory_trampoline);
	m_original_trigger_factory = NULL;
	m_trigger_factory_trace_hooked = false;
}

bool CoopNetGame::InstallTriggerEventTraceHook()
{
	if (m_trigger_event_trace_hooked)
		return true;
	if (!InstallJmpHookRaw(kTriggerEventDispatcher,
		kExpectedTriggerEventDispatcher,
		sizeof(kExpectedTriggerEventDispatcher),
		reinterpret_cast<void*>(&HookTriggerEvent),
		m_original_trigger_event_bytes, &m_trigger_event_trampoline,
		"NPC/monster trigger event trace"))
	{
		return false;
	}
	m_original_trigger_event = reinterpret_cast<TriggerEventFn>(
		m_trigger_event_trampoline);
	m_trigger_event_trace_hooked = true;
	return true;
}

void CoopNetGame::RemoveTriggerEventTraceHook()
{
	if (!m_trigger_event_trace_hooked)
		return;
	RemoveJmpHookRaw(kTriggerEventDispatcher, m_original_trigger_event_bytes,
		sizeof(m_original_trigger_event_bytes), &m_trigger_event_trampoline);
	m_original_trigger_event = NULL;
	m_trigger_event_trace_hooked = false;
}

bool CoopNetGame::InstallInputHook()
{
	if (m_input_hooked)
		return true;
	BYTE* image = reinterpret_cast<BYTE*>(GetModuleHandleW(NULL));
	if (!image)
		return false;
	IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
	IMAGE_NT_HEADERS32* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(
		image + dos->e_lfanew);
	const IMAGE_DATA_DIRECTORY& imports =
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!imports.VirtualAddress)
		return false;

	IMAGE_IMPORT_DESCRIPTOR* descriptor =
		reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
			image + imports.VirtualAddress);
	for (; descriptor->Name; ++descriptor)
	{
		const char* dll_name = reinterpret_cast<const char*>(
			image + descriptor->Name);
		if (_stricmp(dll_name, "user32.dll") != 0 ||
			!descriptor->OriginalFirstThunk)
			continue;

		IMAGE_THUNK_DATA32* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(
			image + descriptor->OriginalFirstThunk);
		IMAGE_THUNK_DATA32* addresses = reinterpret_cast<IMAGE_THUNK_DATA32*>(
			image + descriptor->FirstThunk);
		for (; names->u1.Ordinal; ++names, ++addresses)
		{
			if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal))
				continue;
			const IMAGE_IMPORT_BY_NAME* import_name =
				reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
					image + names->u1.AddressOfData);
			if (strcmp(reinterpret_cast<const char*>(import_name->Name),
				"GetAsyncKeyState") != 0)
				continue;

			m_async_key_state_iat_slot = reinterpret_cast<ULONG_PTR*>(
				&addresses->u1.Function);
			m_original_get_async_key_state =
				reinterpret_cast<GetAsyncKeyStateFn>(*m_async_key_state_iat_slot);
			DWORD old_protection = 0;
			if (!VirtualProtect(m_async_key_state_iat_slot,
				sizeof(*m_async_key_state_iat_slot),
				PAGE_READWRITE, &old_protection))
				return false;
			*m_async_key_state_iat_slot = reinterpret_cast<ULONG_PTR>(
				&HookGetAsyncKeyState);
			DWORD ignored = 0;
			VirtualProtect(m_async_key_state_iat_slot,
				sizeof(*m_async_key_state_iat_slot),
				old_protection, &ignored);
			FlushInstructionCache(GetCurrentProcess(),
				m_async_key_state_iat_slot,
				sizeof(*m_async_key_state_iat_slot));
			m_input_hooked = true;
			CoopRuntime::Instance().Log(
				"[netgame] GetAsyncKeyState IAT hook installed\r\n");
			const bool input_hooks = InstallActionQueryHook() &&
				InstallActionUpQueryHook() &&
				InstallThresholdQueryHook() &&
				InstallAxisQueryHook() && InstallPressedQueryHook() &&
				InstallReleasedQueryHook() && InstallHoldDurationQueryHook() &&
				InstallAimHoldQueryHook() && InstallRawPressedQueryHook() &&
				InstallRawReleasedQueryHook() && InstallRawHeldQueryHook() &&
				InstallCameraYawHook() &&
				InstallGPigCameraUpdateHook() &&
				InstallDefaultModeUpdateHook() && InstallFireHandlerHook() &&
				InstallWeaponAmmoConsumeHook();
			// Trace availability must never disable the input co-op that is already
			// known to work.  It is a read-only discovery aid for world sync.
			if (!InstallTriggerFactoryTraceHook() ||
				!InstallTriggerSpawnTraceHook() ||
				!InstallTriggerEventTraceHook())
			{
				CoopRuntime::Instance().Log(
					"[world-spawn-trace] hook unavailable; world behaviour unchanged\r\n");
			}
			return input_hooks;
		}
	}
	CoopRuntime::Instance().Log(
		"[netgame-error] GetAsyncKeyState import was not found\r\n");
	return false;
}

void CoopNetGame::RemoveInputHook()
{
	RemoveTriggerEventTraceHook();
	RemoveTriggerSpawnTraceHook();
	RemoveTriggerFactoryTraceHook();
	RemoveWeaponAmmoConsumeHook();
	RemoveFireHandlerHook();
	RemoveDefaultModeUpdateHook();
	RemoveGPigCameraUpdateHook();
	RemoveCameraYawHook();
	RemoveRawHeldQueryHook();
	RemoveRawReleasedQueryHook();
	RemoveRawPressedQueryHook();
	RemoveAimHoldQueryHook();
	RemoveHoldDurationQueryHook();
	RemoveReleasedQueryHook();
	RemovePressedQueryHook();
	RemoveAxisQueryHook();
	RemoveThresholdQueryHook();
	RemoveActionUpQueryHook();
	RemoveActionQueryHook();
	if (!m_input_hooked || !m_async_key_state_iat_slot ||
		!m_original_get_async_key_state)
		return;
	if (*m_async_key_state_iat_slot == reinterpret_cast<ULONG_PTR>(
		&HookGetAsyncKeyState))
	{
		DWORD old_protection = 0;
		if (VirtualProtect(m_async_key_state_iat_slot,
			sizeof(*m_async_key_state_iat_slot),
			PAGE_READWRITE, &old_protection))
		{
			*m_async_key_state_iat_slot = reinterpret_cast<ULONG_PTR>(
				m_original_get_async_key_state);
			DWORD ignored = 0;
			VirtualProtect(m_async_key_state_iat_slot,
				sizeof(*m_async_key_state_iat_slot),
				old_protection, &ignored);
		}
	}
	m_input_hooked = false;
	m_async_key_state_iat_slot = NULL;
	m_original_get_async_key_state = NULL;
}
}
