#pragma once

#include "retail/retail_types.h"

namespace coop
{
	// The retail game has exactly one process-global camera handler.  This
	// coordinator keeps that fact out of player-slot and network code: callers
	// state which controller is about to own the camera, while this class owns
	// the narrow native calls and the temporary state snapshot.
	class SharedCameraCoordinator final
	{
	public:
		struct AimState final
		{
			retail::CameraAimAssistState assist;
			retail::CameraAimYawState yaw_state;
			retail::CameraFlyTransientState fly_transient;
			float follow_turn;
			bool has_assist;
			bool has_yaw_state;
			bool has_fly_transient;
			bool has_follow_turn;
		};

		bool RefreshForController(void* controller) const;
		bool ReadLocalYaw(float& yaw) const;
		bool SaveAimState(AimState& saved) const;
		void RestoreAimState(const AimState& saved) const;

	private:
		retail::CameraHandlerRef CameraHandler() const;
		bool GetFollowState(retail::CameraStateRef& state) const;
		bool ReadFollowTurn(float& turn) const;
		bool WriteFollowTurn(float turn) const;
	};
}
