#include "shared_camera.h"

#include "coop_runtime.h"
#include "gforce_constants.h"
#include "retail/retail_views.h"

namespace coop
{
	namespace
	{
		using GetCameraHandlerFn = void* (__thiscall*)(void*);
		using RefreshGPigCameraFn = void(__thiscall*)(void*);
		using CameraStateGetIdFn = std::uint32_t(__thiscall*)(void*);
		using CameraStateGetObjectFn = void* (__thiscall*)(void*, std::uint32_t);
		using CameraYawFn = float(__thiscall*)(void*);
	}

	bool SharedCameraCoordinator::RefreshForController(void* controller) const
	{
		if (!controller)
			return false;

		__try
		{
			// The controller tick reads its aim context from the one shared camera
			// handler.  The caller owns ordering: P1 is refreshed again after a
			// remote-controller tick has finished.
			const RefreshGPigCameraFn refresh_camera =
				reinterpret_cast<RefreshGPigCameraFn>(gforce::kRefreshGPigCamera);
			refresh_camera(controller);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log(
				"[camera] failed to refresh GPig camera context controller=%p\r\n",
				controller);
			return false;
		}
	}

	retail::CameraHandlerRef SharedCameraCoordinator::CameraHandler() const
	{
		retail::CameraHandlerRef handler = {};
		__try
		{
			const GetCameraHandlerFn get_camera_handler =
				reinterpret_cast<GetCameraHandlerFn>(gforce::kGetCameraHandler);
			handler.value = retail::ToAddress(get_camera_handler(
				retail::ToPointer(gforce::kCameraManager)));
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return {};
		}
		return handler;
	}

	bool SharedCameraCoordinator::GetFollowState(
		retail::CameraStateRef& state) const
	{
		// The yaw block at 0x5BBA98 reads this float: 0x4B6F40 on the
		// sub-state machine at handler+0x498 must report the follow state
		// 0x44110010, then 0x4B70E0 returns the state object whose +0x3C is
		// compared against [0x8B7824].  Past that threshold the body gets its
		// own yaw rather than camera yaw and stops turning.
		state = {};
		const retail::CameraHandlerRef handler = CameraHandler();
		retail::Address machine = 0;
		if (!handler || !retail::CameraHandlerView(handler).
			StateMachineAddress(machine))
		{
			return false;
		}

		__try
		{
			const CameraStateGetIdFn get_id =
				reinterpret_cast<CameraStateGetIdFn>(
					gforce::kCameraStateMachineGetId);
			if (get_id(retail::ToPointer(machine)) != gforce::kCameraFollowStateId)
				return false;
			const CameraStateGetObjectFn get_object =
				reinterpret_cast<CameraStateGetObjectFn>(
					gforce::kCameraStateMachineGetObject);
			state.value = retail::ToAddress(get_object(retail::ToPointer(machine),
				gforce::kCameraFollowStateId));
			return static_cast<bool>(state);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			state = {};
			return false;
		}
	}

	bool SharedCameraCoordinator::ReadFollowTurn(float& turn) const
	{
		retail::CameraStateRef state = {};
		return GetFollowState(state) &&
			retail::CameraStateView(state).FollowTurn(turn);
	}

	bool SharedCameraCoordinator::WriteFollowTurn(float turn) const
	{
		retail::CameraStateRef state = {};
		return GetFollowState(state) &&
			retail::CameraStateView(state).SetFollowTurn(turn);
	}

	bool SharedCameraCoordinator::ReadLocalYaw(float& yaw) const
	{
		// 0x52AD20 is hooked by CoopNetGame, but outside a remote-input scope its
		// trampoline returns the yaw of whichever local native controller just
		// ticked.  This lets local Mooch publish its yaw after its own Fly tick.
		const retail::CameraHandlerRef handler = CameraHandler();
		if (!handler)
			return false;

		float value = 0.0f;
		__try
		{
			const CameraYawFn camera_yaw =
				reinterpret_cast<CameraYawFn>(gforce::kCameraYawGetter);
			value = camera_yaw(retail::ToPointer(handler.value));
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

	bool SharedCameraCoordinator::SaveAimState(AimState& saved) const
	{
		// 0x5BB1D0 fetches this handler globally.  P1 ticks first, so a remote
		// controller must never leave its aim state in the process-global camera.
		saved.has_assist = false;
		saved.has_yaw_state = false;
		saved.has_fly_transient = false;
		saved.has_follow_turn = false;

		const retail::CameraHandlerRef handler = CameraHandler();
		if (!handler)
			return false;

		const retail::CameraHandlerView handler_view(handler);
		if (handler_view.ReadAimAssist(saved.assist))
			saved.has_assist = true;
		if (handler_view.ReadAimYawState(saved.yaw_state))
			saved.has_yaw_state = true;
		if (handler_view.ReadFlyTransientState(saved.fly_transient))
			saved.has_fly_transient = true;
		if (ReadFollowTurn(saved.follow_turn))
			saved.has_follow_turn = true;

		return saved.has_assist || saved.has_yaw_state ||
			saved.has_fly_transient || saved.has_follow_turn;
	}

	void SharedCameraCoordinator::RestoreAimState(const AimState& saved) const
	{
		const retail::CameraHandlerRef handler = CameraHandler();
		if (!handler)
			return;

		const retail::CameraHandlerView handler_view(handler);
		bool restore_failed = false;
		if (saved.has_assist && !handler_view.WriteAimAssist(saved.assist))
			restore_failed = true;
		if (saved.has_yaw_state && !handler_view.WriteAimYawState(saved.yaw_state))
			restore_failed = true;
		if (saved.has_fly_transient &&
			!handler_view.WriteFlyTransientState(saved.fly_transient))
		{
			restore_failed = true;
		}
		if (restore_failed)
		{
			CoopRuntime::Instance().Log(
				"[camera] failed to restore the shared aim/yaw/Fly state\r\n");
		}

		if (saved.has_follow_turn && !WriteFollowTurn(saved.follow_turn))
		{
			CoopRuntime::Instance().Log(
				"[camera] failed to restore the shared follow turn\r\n");
		}
	}
}
