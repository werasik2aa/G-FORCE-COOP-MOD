#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

namespace coop
{
	class Player2Module final
	{
	public:
		static Player2Module& Instance();

		bool Install();
		void Remove();
		void PublishDefaultModeActiveEntity(void* entity);
		bool EnsureNetworkPlayer2();
		// Called during native save load / transition to clear P2 state
		// and prevent crashes from stale entity pointers.
		void ResetForWorldLoad();

	private:
		struct Vec4
		{
			float x;
			float y;
			float z;
			float w;
		};

		typedef void* (__cdecl* SpawnGPigFn)(const Vec4*, const Vec4*, uint32_t, void*);
		typedef void(__thiscall* ControllerUpdateFn)(void*);
		typedef void* (__thiscall* EnsureGPigRdvTaskFn)(void*, bool);
		typedef void(__thiscall* ConfigureGPigRdvTaskFn)(void*, void*);

		typedef void* (__thiscall* TriggerCloneFn)(void*);
		typedef void(__thiscall* TriggerSpawnFn)(void*);
		typedef bool(__thiscall* SelectModeFn)(void*, uint32_t, bool);
		typedef void* (__thiscall* GetCameraHandlerFn)(void*);
		typedef void(__thiscall* RefreshGPigCameraFn)(void*);
		typedef uint32_t(__thiscall* CameraStateGetIdFn)(void*);
		typedef void* (__thiscall* CameraStateGetObjectFn)(void*, uint32_t);
		typedef float(__thiscall* CameraYawFn)(void*);
		typedef uint32_t(__thiscall* GetCurrentWeaponIdFn)(void*);
		typedef void(__thiscall* SetSelectedWeaponTypeFn)(void*, uint32_t);
		typedef uint32_t(__cdecl* WeaponTypeToItemIdFn)(uint32_t);

		Player2Module();
		~Player2Module() = default;
		Player2Module(const Player2Module&) = delete;
		Player2Module& operator=(const Player2Module&) = delete;

		void* GetGPigEntity(int slot);
		void* GetFlyEntity();
		void* GetController(void* entity);
		uint32_t GetModeId(void* controller);
		int FindGPigSlot(void* controller);
		bool IsGPigDeathMode(void* controller) const;

		void SelectMode(void* controller, uint32_t mode_id);

		bool RefreshCameraForController(void* controller);
		uint32_t RestorePlayer1CameraTarget();

		struct SharedCameraAimState
		{
			float assist[2];
			float yaw_state[6];
			float follow_turn;
			bool has_assist;
			bool has_yaw_state;
			bool has_follow_turn;
		};

		void* CameraHandler();

		float* CameraFollowTurn();
		bool ReadLocalCameraYaw(float& yaw);
		bool SaveSharedCameraAimState(SharedCameraAimState& saved);

		void RestoreSharedCameraAimState(const SharedCameraAimState& saved);
		bool SyncPlayer2WeaponSelection(void* player2);

		bool ApplyPlayer2WeaponSelection(void* player2, uint32_t weapon_type, const char* source);

		void SpawnPlayer2FromSnapshot(const char* trigger);
		bool TryEnsurePlayer2RdvTask(const char* source);
		bool ConfigurePlayer2RdvTask(const char* source, void* player2, void* player2_handler, void* task);

		void TickPlayer1(void* player1_controller);
		void HandlePlayer1ModeTransition(void* player1_controller);

		void ConfigurePlayer2DefaultMode(void* controller);
		void UpdateController(void* controller);
		bool RunStockControllerUpdate(void* controller, const char* context);
		void* SpawnGPig(const Vec4* position, const Vec4* rotation, uint32_t gpig_id, void* context);
		bool PatchSpawnCall(uintptr_t address, const BYTE expected[5], BYTE original[5]);
		bool PatchDefaultModeActivePublish();

		static void __fastcall HookControllerUpdate(void* controller, void*);
		static void* __cdecl HookSpawnGPig(const Vec4* position, const Vec4* rotation, uint32_t gpig_id, void* context);

		volatile LONG m_player2_ready;
		volatile LONG m_spawn_snapshot_ready;
		volatile LONG m_spawn_in_progress;
		bool m_player2_default_mode_initialized;
		bool m_logged_blocked_active_publish;
		bool m_logged_player2;

		uint32_t m_last_player1_mode;

		void* m_abr_native_task_player2;
		void* m_abr_native_task_configured_player2;

		bool m_remote_abr_mode_active;
		uint32_t m_last_weapon_type;
		Vec4 m_spawn_position;
		Vec4 m_spawn_rotation;
		void* m_spawn_context;
		BYTE m_original_spawn_call1[5];
		BYTE m_original_spawn_call2[5];
		BYTE m_original_default_mode_active_stores[10];
		bool m_default_mode_active_stores_patched;
		ControllerUpdateFn m_original_update;

		bool m_spawn_key_was_down;
		bool m_npc_spawn_key_was_down;
		bool m_fly_controlled_last;
	};
}
