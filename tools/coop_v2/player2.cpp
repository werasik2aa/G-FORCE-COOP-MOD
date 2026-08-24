#include "player2.h"

#include "coop_netgame.h"
#include "coop_runtime.h"
#include "gforce_constants.h"

#include <string.h>

namespace coop
{
using namespace gforce;

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
	m_logged_player2(false),
	m_logged_blocked_active_publish(false),
	m_last_logged_mooch_controller(NULL),
	m_last_player1_mode(0),
	m_spawn_key_was_down(false),
	m_last_weapon_type(0xFFFFFFFFu),
	m_spawn_context(NULL),
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

void* Player2Module::GetGPigEntity(int slot)
{
	if (slot < 1 || slot > 3)
		return NULL;
	__try
	{
		return reinterpret_cast<void**>(kGPigEntityArray)[slot];
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return NULL;
	}
}

void* Player2Module::GetFlyEntity()
{
	__try
	{
		return *reinterpret_cast<void**>(kFlyEntity);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return NULL;
	}
}

void Player2Module::PublishDefaultModeActiveEntity(void* entity)
{
	if (entity == GetGPigEntity(2))
	{
		if (!m_logged_blocked_active_publish)
		{
			CoopRuntime::Instance().Log("[active-guard] blocked Default-mode publish for P2 entity=%p; active remains (%p,%p)\r\n",
				entity, *reinterpret_cast<void**>(kActiveEntityA),
				*reinterpret_cast<void**>(kActiveEntityB));
			m_logged_blocked_active_publish = true;
		}
		return;
	}

	*reinterpret_cast<void**>(kActiveEntityA) = entity;
	*reinterpret_cast<void**>(kActiveEntityB) = entity;
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
		return NULL;
	__try
	{
		BYTE* handler = *reinterpret_cast<BYTE**>(
			reinterpret_cast<BYTE*>(entity) + kEntityHandlerOffset);
		return handler ? *reinterpret_cast<void**>(handler + kHandlerControllerOffset) : NULL;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return NULL;
	}
}

uint32_t Player2Module::GetModeId(void* controller)
{
	if (!controller)
		return 0;
	__try
	{
		BYTE* mode = *reinterpret_cast<BYTE**>(
			reinterpret_cast<BYTE*>(controller) + kControllerModeOffset);
		return mode ? *reinterpret_cast<uint32_t*>(mode + kModeIdOffset) : 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
}

int Player2Module::FindGPigSlot(void* controller)
{
	if (!controller)
		return 0;
	__try
	{
		void* owner = *reinterpret_cast<void**>(
			reinterpret_cast<BYTE*>(controller) + kControllerOwnerOffset);
		for (int slot = 1; slot <= 3; ++slot)
		{
			BYTE* entity = static_cast<BYTE*>(GetGPigEntity(slot));
			if (entity && *reinterpret_cast<void**>(entity + kEntityHandlerOffset) == owner)
				return slot;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
	return 0;
}

void Player2Module::SelectMode(void* controller, uint32_t mode_id)
{
	if (!controller)
		return;
	SelectModeFn select_mode = reinterpret_cast<SelectModeFn>(kSelectMode);
	__try
	{
		select_mode(controller, mode_id, false);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log("[error] mode switch fault controller=%p mode=0x%08X\r\n",
			controller, mode_id);
	}
}

bool Player2Module::RefreshCameraForController(void* controller)
{
	if (!controller)
		return false;

	__try
	{
		// The controller tick reads its aim context from this process-global
		// camera handler.  Refresh it for the controller about to consume input;
		// P1 is refreshed again after P2's network tick.
		RefreshGPigCameraFn refresh_camera =
			reinterpret_cast<RefreshGPigCameraFn>(kRefreshGPigCamera);
		refresh_camera(controller);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log(
			"[error] failed to refresh GPig camera context controller=%p\r\n",
			controller);
		return false;
	}
}

uint32_t Player2Module::RestorePlayer1CameraTarget()
{
	void* player1 = GetGPigEntity(1);
	void* player1_controller = GetController(player1);
	if (!player1_controller)
		return 0xFFFFFFFFu;

	__try
	{
		// Besides choosing P1, the refresh rebuilds the follow anchor at
		// camera_handler+0x940..0x94C; changing only a target ID is insufficient.
		if (!RefreshCameraForController(player1_controller))
			return 0xFFFFFFFFu;

		GetCameraHandlerFn get_camera_handler =
			reinterpret_cast<GetCameraHandlerFn>(kGetCameraHandler);
		void* camera_handler = get_camera_handler(
			reinterpret_cast<void*>(kCameraManager));
		if (!camera_handler)
			return 0xFFFFFFFFu;

		return *reinterpret_cast<uint32_t*>(
			reinterpret_cast<BYTE*>(camera_handler) +
			kCameraTargetControllerOffset + kCameraTargetIdOffset);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log("[error] failed to restore the camera target to P1\r\n");
		return 0xFFFFFFFFu;
	}
}

void* Player2Module::CameraHandler()
{
	__try
	{
		GetCameraHandlerFn get_camera_handler =
			reinterpret_cast<GetCameraHandlerFn>(kGetCameraHandler);
		return get_camera_handler(reinterpret_cast<void*>(kCameraManager));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return NULL;
	}
}

float* Player2Module::CameraFollowTurn()
{
	// The yaw block at 0x5BBA98 reads exactly this float: 0x4B6F40 on the
	// sub-state machine at handler+0x498 must report the follow state
	// 0x44110010, and then 0x4B70E0 hands back the state object whose +0x3C is
	// compared against [0x8B7824].  Past that threshold the body is given its
	// own yaw instead of the camera yaw and stops turning.
	BYTE* handler = reinterpret_cast<BYTE*>(CameraHandler());
	if (!handler)
		return NULL;

	__try
	{
		void* machine = handler + kCameraStateMachineOffset;
		CameraStateGetIdFn get_id =
			reinterpret_cast<CameraStateGetIdFn>(kCameraStateMachineGetId);
		if (get_id(machine) != kCameraFollowStateId)
			return NULL;
		CameraStateGetObjectFn get_object =
			reinterpret_cast<CameraStateGetObjectFn>(
				kCameraStateMachineGetObject);
		BYTE* state = reinterpret_cast<BYTE*>(
			get_object(machine, kCameraFollowStateId));
		if (!state)
			return NULL;
		return reinterpret_cast<float*>(state + kCameraStateTurnOffset);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return NULL;
	}
}

bool Player2Module::ReadLocalCameraYaw(float& yaw)
{
	// 0x52AD20 is hooked by CoopNetGame, but the hook only substitutes a remote
	// value while remote input is active on the calling thread.  This runs right
	// after P1's own tick, so it returns the genuine local camera yaw.
	void* handler = CameraHandler();
	if (!handler)
		return false;

	float value = 0.0f;
	__try
	{
		CameraYawFn camera_yaw = reinterpret_cast<CameraYawFn>(kCameraYawGetter);
		value = camera_yaw(handler);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}

	if (!(value > -1000.0f && value < 1000.0f))
		return false;
	yaw = value;
	return true;
}

bool Player2Module::SaveSharedCameraAimState(SharedCameraAimState& saved)
{
	// 0x5BB1D0 is the aim/weapon state machine of XControllerMode_GPig_Default.
	// It fetches the camera handler from the global camera manager with no player
	// argument, and that handler is a single process-wide object, so everything it
	// writes there belongs to whichever entity happens to be ticking.  P1 ticks
	// first, so without this save/restore P2's tick is what P1's camera and P1's
	// own aim-mode turn consume on the next frame — the same class of defect the
	// inactive mode 0x5B7D60 had with handler+0x91C/+0x920.
	saved.has_assist = false;
	saved.has_yaw_state = false;
	saved.has_follow_turn = false;

	BYTE* handler = reinterpret_cast<BYTE*>(CameraHandler());
	if (!handler)
		return false;

	__try
	{
		const float* assist = reinterpret_cast<const float*>(
			handler + kCameraAimAssistOffset);
		for (size_t i = 0; i < kCameraAimAssistFloats; ++i)
			saved.assist[i] = assist[i];
		saved.has_assist = true;

		const float* yaw_state = reinterpret_cast<const float*>(
			handler + kCameraAimYawStateOffset);
		for (size_t i = 0; i < kCameraAimYawStateFloats; ++i)
			saved.yaw_state[i] = yaw_state[i];
		saved.has_yaw_state = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}

	const float* follow_turn = CameraFollowTurn();
	if (follow_turn)
	{
		__try
		{
			saved.follow_turn = *follow_turn;
			saved.has_follow_turn = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	return saved.has_assist || saved.has_yaw_state || saved.has_follow_turn;
}

void Player2Module::RestoreSharedCameraAimState(
	const SharedCameraAimState& saved)
{
	BYTE* handler = reinterpret_cast<BYTE*>(CameraHandler());
	if (!handler)
		return;

	__try
	{
		if (saved.has_assist)
		{
			float* assist = reinterpret_cast<float*>(
				handler + kCameraAimAssistOffset);
			for (size_t i = 0; i < kCameraAimAssistFloats; ++i)
				assist[i] = saved.assist[i];
		}
		if (saved.has_yaw_state)
		{
			float* yaw_state = reinterpret_cast<float*>(
				handler + kCameraAimYawStateOffset);
			for (size_t i = 0; i < kCameraAimYawStateFloats; ++i)
				yaw_state[i] = saved.yaw_state[i];
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log(
			"[error] failed to restore the shared camera aim yaw state\r\n");
	}

	if (!saved.has_follow_turn)
		return;
	float* follow_turn = CameraFollowTurn();
	if (!follow_turn)
		return;
	__try
	{
		*follow_turn = saved.follow_turn;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log(
			"[error] failed to restore the shared camera follow turn\r\n");
	}
}

bool Player2Module::SyncPlayer2WeaponSelection(void* player2)
{
	void* player1 = GetGPigEntity(1);
	if (!player1 || !player2)
		return false;

	__try
	{
		BYTE* player1_handler = *reinterpret_cast<BYTE**>(
			reinterpret_cast<BYTE*>(player1) + kEntityHandlerOffset);
		if (!player1_handler)
			return false;

		const uint32_t player1_weapon_type = *reinterpret_cast<uint32_t*>(
			player1_handler + kHandlerSelectedWeaponTypeOffset);
		return ApplyPlayer2WeaponSelection(player2, player1_weapon_type,
			"local P1");
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log("[error] exception while reading P1 weapon selection\r\n");
		return false;
	}
}

bool Player2Module::ApplyPlayer2WeaponSelection(void* player2,
	uint32_t weapon_type, const char* source)
{
	if (!player2 || weapon_type == 0xFFFFFFFFu)
		return false;

	__try
	{
		BYTE* player2_handler = *reinterpret_cast<BYTE**>(
			reinterpret_cast<BYTE*>(player2) + kEntityHandlerOffset);
		if (!player2_handler)
			return false;

		const uint32_t player2_weapon_type = *reinterpret_cast<uint32_t*>(
			player2_handler + kHandlerSelectedWeaponTypeOffset);
		// The handler temporarily reports melee/holster while a draw, jump or
		// attack animation is in progress.  Re-sending a full selected-type change
		// on every such transient restarts expensive weapon state work and freezes
		// both windows.  The remote source sequence is the authority for a real
		// player selection, so send the setter only when that source type changed.
		if (m_last_weapon_type == weapon_type)
			return false;

		m_last_weapon_type = weapon_type;
		WeaponTypeToItemIdFn weapon_type_to_item_id =
			reinterpret_cast<WeaponTypeToItemIdFn>(kWeaponTypeToItemId);
		const uint32_t item_id = weapon_type_to_item_id(weapon_type);
		if (item_id == kDefaultMeleeItemId && weapon_type != 0x40050001u)
			return false;

		SetSelectedWeaponTypeFn set_selected_weapon_type =
			reinterpret_cast<SetSelectedWeaponTypeFn>(kSetSelectedWeaponType);
		set_selected_weapon_type(player2_handler, weapon_type);

		GetCurrentWeaponIdFn get_current_weapon_id =
			reinterpret_cast<GetCurrentWeaponIdFn>(kGetCurrentWeaponId);
		const uint32_t player2_current =
			get_current_weapon_id(player2_handler);
		CoopRuntime::Instance().Log("[weapon-selection] source=%s type=0x%08X item=0x%08X P2.old_type=0x%08X P2.current_before=0x%08X action=%s\r\n",
			source ? source : "unknown", weapon_type, item_id,
			player2_weapon_type, player2_current,
			item_id == kDefaultMeleeItemId ? "holster" : "select");
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log("[error] exception while applying P2 weapon selection\r\n");
		return false;
	}
}

bool Player2Module::EnsureNetworkPlayer2()
{
	void* player2 = GetGPigEntity(2);
	void* controller = GetController(player2);
	if (player2 && controller)
	{
		InterlockedExchange(&m_player2_ready, 1);
		return true;
	}
	if (InterlockedCompareExchange(&m_spawn_snapshot_ready, 0, 0) == 0 ||
		!m_spawn_context)
		return false;
	void* player1 = GetGPigEntity(1);
	void* player1_controller = GetController(player1);
	if (!player1 || !player1_controller ||
		GetModeId(player1_controller) != kDefaultModeId)
		return false;
	SpawnPlayer2FromSnapshot("network");
	return InterlockedCompareExchange(&m_player2_ready, 0, 0) != 0;
}

void Player2Module::SpawnPlayer2FromSnapshot(const char* trigger)
{
	if (!trigger)
		trigger = "unknown";
	if (InterlockedCompareExchange(&m_spawn_in_progress, 1, 0) != 0)
		return;
	if (!CoopRuntime::Instance().Config().enabled)
	{
		CoopRuntime::Instance().Log("[spawn-%s] co-op is disabled in coop.ini\r\n",
			trigger);
		InterlockedExchange(&m_spawn_in_progress, 0);
		return;
	}
	if (GetGPigEntity(2))
	{
		CoopRuntime::Instance().Log("[spawn-%s] player 2 already exists\r\n",
			trigger);
		InterlockedExchange(&m_spawn_in_progress, 0);
		return;
	}
	if (InterlockedCompareExchange(&m_spawn_snapshot_ready, 0, 0) == 0 ||
		!m_spawn_context)
	{
		CoopRuntime::Instance().Log(
			"[spawn-%s] no stock player spawn snapshot yet\r\n", trigger);
		InterlockedExchange(&m_spawn_in_progress, 0);
		return;
	}

	void* player1 = GetGPigEntity(1);
	Vec4 player2_position = m_spawn_position;
	Vec4 player2_rotation = m_spawn_rotation;
	bool used_live_transform = false;
	__try
	{
		if (player1)
		{
			player2_position = *reinterpret_cast<Vec4*>(
				reinterpret_cast<BYTE*>(player1) + kEntityPositionOffset);
			player2_rotation = *reinterpret_cast<Vec4*>(
				reinterpret_cast<BYTE*>(player1) + kEntityRotationOffset);
			used_live_transform = true;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		used_live_transform = false;
	}

	SpawnGPigFn spawn = reinterpret_cast<SpawnGPigFn>(kSpawnGPig);
	void* player2 = NULL;
	CoopRuntime::Instance().Log("[spawn-%s] creating P2 id=0x%08X context=%p transform=%s position=(%.3f, %.3f, %.3f)\r\n",
		trigger, kGPig2Id, m_spawn_context,
		used_live_transform ? "live-P1" : "stock-snapshot",
		player2_position.x, player2_position.y, player2_position.z);
	__try
	{
		player2 = spawn(&player2_position, &player2_rotation,
			kGPig2Id, m_spawn_context);
	}
	__except (CoopRuntime::Instance().LogException(GetExceptionInformation(), "player2-factory-from-key"))
	{
		player2 = NULL;
	}

	void* player2_handler = NULL;
	void* player2_controller = GetController(player2);
	__try
	{
		if (player2)
			player2_handler = *reinterpret_cast<void**>(
				reinterpret_cast<BYTE*>(player2) + kEntityHandlerOffset);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
	if (player2 && player2_controller)
	{
		m_last_weapon_type = 0xFFFFFFFFu;
		m_player2_default_mode_initialized = false;
		m_logged_blocked_active_publish = false;
		InterlockedExchange(&m_player2_ready, 1);
	}
	CoopRuntime::Instance().Log("[spawn-%s-result] P1=%p P2=%p P2.handler=%p P2.controller=%p ready=%ld\r\n",
		trigger, player1, player2, player2_handler, player2_controller,
		m_player2_ready);
	InterlockedExchange(&m_spawn_in_progress, 0);
}

void Player2Module::PollPlayer2SpawnKey()
{
	const bool down = (GetAsyncKeyState(CoopRuntime::Instance().Config().spawn_key) & 0x8000) != 0;
	if (down && !m_spawn_key_was_down)
		SpawnPlayer2FromSnapshot("key");
	m_spawn_key_was_down = down;
}

void __fastcall Player2Module::HookControllerUpdate(
	void* controller, void*)
{
	Instance().UpdateController(controller);
}

void Player2Module::TickPlayer1(void* player1_controller)
{
	// The single shared GPig camera belongs to whoever the player is actually
	// driving.  Its mode goes back to Default immediately after the native Mooch
	// hand-off, so use the confirmed network owner state rather than a mode or
	// process-global pointer that lasts only for that transition frame.
	if (!CoopNetGame::Instance().IsLocalFlyControlled() &&
		GetModeId(player1_controller) == kDefaultModeId)
		RefreshCameraForController(player1_controller);
	CoopNetGame::Instance().BeginLocalInputCapture();
	m_original_update(player1_controller);
	CoopNetGame::Instance().PublishLocalPlayerTransform(GetGPigEntity(1));
	// P1's 0x5BCF30 and 0x5BB1D0 have just finished driving the single shared
	// camera, so this is the only frame point where 0x52AD20 reports P1's own yaw.
	// The remote machine cannot read its P2's camera because P2 owns none there —
	// it consumes this value.
	float local_camera_yaw = 0.0f;
	const bool local_camera_yaw_valid = ReadLocalCameraYaw(local_camera_yaw);
	CoopNetGame::Instance().PublishLocalCameraYaw(
		local_camera_yaw, local_camera_yaw_valid);

	// Network spawning must happen after P1's normal update.  The factory then
	// receives the same settled transform/physics context as the proven-safe
	// manual F6 path.  GameTick itself returns immediately once P2 exists.
	CoopNetGame::Instance().GameTick();
	LogPlayer1ModeEdge(player1_controller);
}

void Player2Module::LogPlayer1ModeEdge(void* player1_controller)
{
	// Edge-triggered only: one line per mode transition, never per frame.  A mode
	// change is exactly the event that goes wrong on the fly switch, and this
	// records the whole state it depends on - who is the active entity, whether
	// the fly entity exists and what mode its own controller sits in.
	const uint32_t mode_now = GetModeId(player1_controller);
	if (mode_now != m_last_player1_mode)
	{
		void* player1 = GetGPigEntity(1);
		void* fly = NULL;
		void* fly_controller = NULL;
		uint32_t fly_mode = 0;
		void* active_a = NULL;
		void* active_b = NULL;
		__try
		{
			fly = *reinterpret_cast<void**>(kFlyEntity);
			active_a = *reinterpret_cast<void**>(kActiveEntityA);
			active_b = *reinterpret_cast<void**>(kActiveEntityB);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		fly_controller = GetController(fly);
		fly_mode = GetModeId(fly_controller);
		CoopRuntime::Instance().Log("[p1-mode] 0x%08X -> 0x%08X active=(%p,%p) p1=%p fly=%p fly_controller=%p fly_mode=0x%08X\r\n",
			m_last_player1_mode, mode_now, active_a, active_b, player1, fly,
			fly_controller, fly_mode);
		if (mode_now == kMoochSwitchModeId)
		{
			// This is the only confirmation point: 0x5BBC80 accepted the local
			// action and selected the EXE's real Darwin-to-Mooch hand-off mode.
			// Do not infer ownership from the globals, which are reset on the
			// following stock tick.
			CoopNetGame::Instance().ConfirmLocalFlyControl();
			CoopNetGame::Instance().RequestRemotePlayerTickDeferral();
		}
		m_last_player1_mode = mode_now;
	}
}

void Player2Module::ConfigurePlayer2DefaultMode(void* controller)
{
	if (!controller || GetModeId(controller) != kDefaultModeId)
		return;

	__try
	{
		BYTE* const mode = *reinterpret_cast<BYTE**>(
			static_cast<BYTE*>(controller) + kControllerModeOffset);
		if (!mode)
			return;

		uint32_t* const conflict_mask = reinterpret_cast<uint32_t*>(
			mode + kModeConflictMaskOffset);
		const uint32_t original_mask = *conflict_mask;
		if (original_mask != kDefaultModeConflictMask)
		{
			CoopRuntime::Instance().Log(
				"[arbiter] P2 Default conflict mask unexpected: 0x%08X; left unchanged\r\n",
				original_mask);
			return;
		}

		// The modes are per-controller objects: the observed P2 Default instance
		// differs from P1's Mooch instance.  0x5BFF80 compares the bit newly
		// required by P1's Mooch -> Default hand-off (bit 0x1) against every
		// other controller.  P2 has no local camera/active-entity ownership, so
		// leaving that bit set falsely makes it a competing single-player owner.
		// Retain the rest of P2's native Default mask and its full stock tick.
		*conflict_mask = original_mask & ~kP2DefaultExclusiveMask;
		CoopRuntime::Instance().Log(
			"[arbiter] P2 Default mode=%p conflict mask 0x%08X -> 0x%08X\r\n",
			mode, original_mask, *conflict_mask);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log(
			"[arbiter] unable to configure P2 Default conflict mask\r\n");
	}
}

void Player2Module::UpdateController(void* controller)
{
	// Every controller is ticked exactly once, exactly where the game asked for
	// it.  The previous build deferred P1's tick out of P1's own array slot into
	// P2's, and that is what broke the fly: the tick was re-issued on whatever
	// [[entity+0x144]+0x510] resolved to at that later point, which on the frame
	// Mooch (0x61000065) is selected is no longer the object holding that mode.
	// So 0x5BC050 - the only code in the EXE that ever selects 0x6100003B again -
	// never ran, and the log shows exactly that: the 0x6100003B -> 0x61000065 edge
	// appears and the return edge never does.  Nothing below may skip, relocate or
	// synthesise a stock tick for a controller the player drives.
	const int slot = FindGPigSlot(controller);
	const uint32_t controller_mode = GetModeId(controller);
	if (controller_mode == kMoochSwitchModeId &&
		m_last_logged_mooch_controller != controller)
	{
		m_last_logged_mooch_controller = controller;
		CoopRuntime::Instance().Log(
			"[mooch-route] controller=%p slot=%d p1.controller=%p p2.controller=%p fly.controller=%p\r\n",
			controller, slot, GetController(GetGPigEntity(1)),
			GetController(GetGPigEntity(2)), GetController(GetFlyEntity()));
	}
	else if (controller_mode != kMoochSwitchModeId &&
		m_last_logged_mooch_controller == controller)
	{
		m_last_logged_mooch_controller = NULL;
	}
	const bool player2_slot = slot == 2 &&
		CoopRuntime::Instance().Config().enabled &&
		InterlockedCompareExchange(&m_player2_ready, 0, 0) != 0;

	if (!player2_slot)
	{
		// Slot 1 is P1: its stock tick plus the local half of the network bracket,
		// in its own slot and nowhere else.  Everything else - the fly's own
		// controller included, which FindGPigSlot reports as 0 because it is entity
		// slot 4 (0x9128E8) and not a guinea pig - gets the stock tick untouched.
		if (slot == 1)
			TickPlayer1(controller);
		else if (controller == GetController(GetFlyEntity()))
		{
			void* fly = GetFlyEntity();
			const uint32_t fly_mode_before = GetModeId(controller);
			m_original_update(controller);
			// Capture after Mooch's native motor has produced this frame's final
			// position.  The owner state itself was confirmed from P1's native
			// Darwin-to-Mooch hand-off and is not inferred here.
			CoopNetGame::Instance().ObserveLocalFlyMode(fly_mode_before,
				GetModeId(controller));
			CoopNetGame::Instance().MaintainLocalFlyActiveEntity(fly);
			CoopNetGame::Instance().PublishLocalFlyTransform(fly);
			if (!CoopNetGame::Instance().IsLocalFlyControlled())
				CoopNetGame::Instance().ApplyRemoteFlyTransform(fly);
		}
		else
			m_original_update(controller);
		// Edge-triggered on the key itself, so polling it from whichever controller
		// the game happens to visit is free and keeps F6 alive even in the frames
		// where P1's own array slot is not reached at all.
		PollPlayer2SpawnKey();
		return;
	}

	// P2 is driven entirely by the packet: read the keys, run the stock tick with
	// them, then set the position.  Nothing else.  The active-entity swap, the
	// input-device override, the shared-camera aim save/restore, the peer/mode
	// freezes and the camera-target restore that used to live here were all
	// scaffolding for the old locally mirrored P2, which took P1's camera and
	// active-entity slot because it shared P1's input.  A packet-driven P2 needs
	// none of it.
	// The Q edge is local-only: the engine hands Darwin's active entity to Mooch
	// through process-global state.  P2's full stock tick must not run inside
	// that hand-off, because its packet input context also covers nested calls on
	// this same main thread.  This skips one P2 tick only; normal packet-driven
	// movement and animation resume on the following frame.
	if (CoopNetGame::Instance().ConsumeRemotePlayerTickDeferral())
		return;

	void* player2 = GetGPigEntity(2);
	bool remote_input_active = false;
	bool remote_gamepad_active = false;
	void* primary_gamepad = NULL;
	// P2 Default still writes the one shared aim/crosshair handler even though
	// its camera update is skipped.  Preserve only the fly-owned state around
	// the otherwise normal P2 tick; P2 still consumes its packet and animates.
	SharedCameraAimState saved_fly_camera_state = {};
	const bool preserve_fly_camera =
		CoopNetGame::Instance().IsLocalFlyControlled();
	bool fly_camera_state_saved = false;

	__try
	{
		// Read the packet.  For the length of P2's tick the stock keyboard action
		// code consumes the remote snapshot while P1 keeps reading the physical
		// keyboard.  With no packet the snapshot is zeroed, so P2 simply idles -
		// BuildRemoteScanCodeState reports every key up and no edge is forged.
		CoopNetGame::Instance().BeginRemoteInput();
		remote_input_active = true;
		// GPig_Default::main Update does not consistently honour the pad passed to
		// its inner calls: it reloads [0x9905CC] for 0x5B92A0, 0x5BB1D0 and other
		// state machines.  Leaving that pointer on P1 lets P2 reset the physical
		// pad state which Fly_Active reads for its movable turn crosshair.  The
		// private pad already exists for P2; expose it only for this synchronous
		// P2 tick, then restore P1 before the next controller (including the fly)
		// is updated.
		remote_gamepad_active =
			CoopNetGame::Instance().BeginRemoteGamePadScope(primary_gamepad);
		// Default is the P2 controller's native input/animation state.  Its Enter
		// routine reads the pad immediately, so selecting it outside this bracket
		// incorrectly consumes P1's physical key edge.  This mod-side guard is
		// reset only when a new P2 is spawned: it never retries a mode selection
		// while P1 is changing to or from Mooch.
		if (!m_player2_default_mode_initialized &&
			!CoopNetGame::Instance().IsLocalFlyControlled())
		{
			if (GetModeId(controller) == kInactiveModeId)
			{
				// Default::Enter runs 0x5BB1D0 immediately.  Unlike the later
				// per-frame camera update that is already skipped for P2, this one
				// writes the singleton camera handler during the one-off mode
				// selection.  Preserve P1's handler around that Enter so P2 gets a
				// real Default state without leaving its aim/follow state behind for
				// P1's subsequent Mooch transition.
				SharedCameraAimState saved_camera_state = {};
				const bool have_saved_camera_state =
					SaveSharedCameraAimState(saved_camera_state);
				SelectMode(controller, kDefaultModeId);
				if (have_saved_camera_state)
					RestoreSharedCameraAimState(saved_camera_state);
			}
			ConfigurePlayer2DefaultMode(controller);
			m_player2_default_mode_initialized = true;
			CoopRuntime::Instance().Log(
				"[player2] one-time Default initialization mode=0x%08X\r\n",
				GetModeId(controller));
		}
		uint32_t remote_weapon_type = 0xFFFFFFFFu;
		if (CoopNetGame::Instance().GetActiveRemoteWeaponType(remote_weapon_type))
			ApplyPlayer2WeaponSelection(player2, remote_weapon_type, "remote P1");
		CoopNetGame::Instance().ArmRemoteP2AmmoOwner(player2);
		if (preserve_fly_camera)
			fly_camera_state_saved =
				SaveSharedCameraAimState(saved_fly_camera_state);
		m_original_update(controller);
		if (fly_camera_state_saved)
		{
			RestoreSharedCameraAimState(saved_fly_camera_state);
			fly_camera_state_saved = false;
		}
		// Set the position.  A correction toward the sender's own transform, and a
		// no-op until a packet with a non-zero sequence has arrived.
		CoopNetGame::Instance().ApplyRemotePlayerTransform(player2);
		if (remote_gamepad_active)
		{
			CoopNetGame::Instance().EndRemoteGamePadScope(primary_gamepad);
			remote_gamepad_active = false;
		}
		CoopNetGame::Instance().EndRemoteInput();
		remote_input_active = false;

		if (!m_logged_player2)
		{
			CoopRuntime::Instance().Log("[player2] packet-driven update entity=%p controller=%p mode=0x%08X\r\n",
				player2, controller, GetModeId(controller));
			m_logged_player2 = true;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		if (fly_camera_state_saved)
			RestoreSharedCameraAimState(saved_fly_camera_state);
		if (remote_gamepad_active)
			CoopNetGame::Instance().EndRemoteGamePadScope(primary_gamepad);
		if (remote_input_active)
			CoopNetGame::Instance().EndRemoteInput();
		CoopRuntime::Instance().Log("[error] exception in player 2 controller update\r\n");
	}
}

void* __cdecl Player2Module::HookSpawnGPig(
	const Vec4* position, const Vec4* rotation, uint32_t gpig_id, void* context)
{
	return Instance().SpawnGPig(position, rotation, gpig_id, context);
}

void* Player2Module::SpawnGPig(
	const Vec4* position, const Vec4* rotation, uint32_t gpig_id, void* context)
{
	SpawnGPigFn spawn = reinterpret_cast<SpawnGPigFn>(kSpawnGPig);
	CoopRuntime::Instance().Log("[spawn-enter] id=0x%08X position=%p rotation=%p context=%p\r\n",
		gpig_id, position, rotation, context);
	void* player1 = NULL;
	__try
	{
		player1 = spawn(position, rotation, gpig_id, context);
	}
	__except (CoopRuntime::Instance().LogException(GetExceptionInformation(), "stock-player1-factory"))
	{
		return NULL;
	}
	CoopRuntime::Instance().Log("[spawn-stock-ok] id=0x%08X entity=%p\r\n", gpig_id, player1);

	if (gpig_id != kGPig1Id || !player1)
		return player1;

	__try
	{
		if (!position || !rotation)
			return player1;
		m_spawn_position = *position;
		m_spawn_rotation = *rotation;
		m_spawn_context = context;
		InterlockedExchange(&m_spawn_snapshot_ready, 1);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		CoopRuntime::Instance().Log("[error] invalid stock spawn arguments; F6 spawn is unavailable\r\n");
		return player1;
	}
	CoopRuntime::Instance().Log("[spawn-snapshot] P1=%p context=%p; press F6 after the cutscene to create P2\r\n",
		player1, context);
	return player1;
}

bool Player2Module::PatchSpawnCall(uintptr_t address, const BYTE expected[5], BYTE original[5])
{
	BYTE* call = reinterpret_cast<BYTE*>(address);
	if (memcmp(call, expected, 5) != 0)
	{
		CoopRuntime::Instance().Log("[error] spawn CALL mismatch at 0x%08X\r\n",
			static_cast<unsigned>(address));
		return false;
	}
	memcpy(original, call, 5);
	BYTE replacement[5] = {0xE8, 0, 0, 0, 0};
	const intptr_t displacement =
		reinterpret_cast<BYTE*>(&HookSpawnGPig) - (call + 5);
	const int32_t relative = static_cast<int32_t>(displacement);
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
	const int32_t relative = static_cast<int32_t>(displacement);
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
