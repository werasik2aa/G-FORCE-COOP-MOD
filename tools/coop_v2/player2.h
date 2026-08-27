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

private:
	struct Vec4
	{
		float x;
		float y;
		float z;
		float w;
	};

	typedef void* (__cdecl* SpawnGPigFn)(
		const Vec4*, const Vec4*, uint32_t, void*);
		typedef void (__thiscall* ControllerUpdateFn)(void*);
		// Native lazy factory 0x40C9F0: __thiscall(XMotorSystem, bool create).
		// It owns allocation and insertion of XMotorTask_RDV at handler+0x4EC.
		typedef void* (__thiscall* EnsureGPigRdvTaskFn)(void*, bool);
		// Stock post-spawn configurator 0x43EE20: __thiscall(level spawn context,
		// spawned GPig handler). P1 calls it after its RDV task factory and it
		// initializes the task's native activation state without manual field writes.
		typedef void (__thiscall* ConfigureGPigRdvTaskFn)(void*, void*);

	typedef void* (__thiscall* TriggerCloneFn)(void*);
	typedef void (__thiscall* TriggerSpawnFn)(void*);
	// 0x4B7050 takes the target mode and a force/reselect flag.  The native
	// function ends in `ret 8`; omitting the flag corrupts its stack contract.
	typedef bool (__thiscall* SelectModeFn)(void*, uint32_t, bool);
	typedef void* (__thiscall* GetCameraHandlerFn)(void*);
	typedef void (__thiscall* RefreshGPigCameraFn)(void*);
	typedef uint32_t (__thiscall* CameraStateGetIdFn)(void*);
	typedef void* (__thiscall* CameraStateGetObjectFn)(void*, uint32_t);
	typedef float (__thiscall* CameraYawFn)(void*);
	typedef uint32_t (__thiscall* GetCurrentWeaponIdFn)(void*);
	typedef void (__thiscall* SetSelectedWeaponTypeFn)(void*, uint32_t);
	typedef uint32_t (__cdecl* WeaponTypeToItemIdFn)(uint32_t);

	Player2Module();
	~Player2Module() = default;
	Player2Module(const Player2Module&) = delete;
	Player2Module& operator=(const Player2Module&) = delete;

	void* GetGPigEntity(int slot);
	void* GetFlyEntity();
		void* GetController(void* entity);
	uint32_t GetModeId(void* controller);
			int FindGPigSlot(void* controller);
		void LogGPigRuntimeState(const char* tag, uint32_t gpig_id, void* entity);

	void SelectMode(void* controller, uint32_t mode_id);

	bool RefreshCameraForController(void* controller);
	uint32_t RestorePlayer1CameraTarget();
	// There is only one camera handler in the process: 0x515C80 returns
	// [[0x915738+0x18]+0x144] and 0x915750 is the level singleton, so P1 and P2
	// share every field 0x5BB1D0 writes there, including the turn magnitude that
	// decides whether the body follows the camera yaw at all.
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
	// Reads 0x52AD20 on the shared handler.  Only meaningful right after P1's own
	// controller tick, because that is when the handler still holds P1's camera.
			bool ReadLocalCameraYaw(float& yaw);
		void LogSharedCameraOwnershipState(const char* tag);
		void LogAbrTaskConfigurationContext(const char* tag);
		bool SaveSharedCameraAimState(SharedCameraAimState& saved);

	void RestoreSharedCameraAimState(const SharedCameraAimState& saved);
	bool SyncPlayer2WeaponSelection(void* player2);
			bool ApplyPlayer2WeaponSelection(void* player2, uint32_t weapon_type,
			const char* source);

		void SpawnPlayer2FromSnapshot(const char* trigger);
			void PollPlayer2SpawnKey();
		void PollLocalAbrProbeKey();
		void PollLocalAbrModeTestKey();
		bool TryEnsurePlayer2RdvTask(const char* source);
		bool ConfigurePlayer2RdvTask(const char* source, void* player2,
			void* player2_handler, void* task);
		void TracePlayer2RdvItemLifecycle(void* player2);

		// P1's stock tick, wrapped in the local half of the network bracket.  Called
	// only from P1's own array slot, with the controller the game itself handed to
	// the hook: re-issuing the tick from anywhere else has to re-resolve
	// [[entity+0x144]+0x510], and that walk stops naming the controller that holds
	// the mode the moment P1 switches to the fly.
			void TickPlayer1(void* player1_controller);
		void HandlePlayer1ModeTransition(void* player1_controller);
		void LogAbrResourceReferences(const char* tag, void* handler);

	// P2 owns a distinct Default-mode instance.  It must remain fully active for
	// packet input, but must not occupy the one exclusive Default ownership bit
	// that the stock single-player Mooch hand-off needs for P1.
	void ConfigurePlayer2DefaultMode(void* controller);
			void UpdateController(void* controller);
		bool RunStockControllerUpdate(void* controller, const char* context);
		void* SpawnGPig(const Vec4* position, const Vec4* rotation,
		uint32_t gpig_id, void* context);
	bool PatchSpawnCall(
		uintptr_t address, const BYTE expected[5], BYTE original[5]);
	bool PatchDefaultModeActivePublish();

	static void __fastcall HookControllerUpdate(void* controller, void*);
	static void* __cdecl HookSpawnGPig(const Vec4* position,
		const Vec4* rotation, uint32_t gpig_id, void* context);
	volatile LONG m_player2_ready;
	volatile LONG m_spawn_snapshot_ready;
	volatile LONG m_spawn_in_progress;
	// This is a mod-side one-shot guard, not a field in the game entity.  P2
	// must run Default mode to consume its packet input, but SelectMode must
	// never be retried during an unrelated Darwin-to-Mooch transition.
	bool m_player2_default_mode_initialized;
	bool m_logged_player2;
	bool m_logged_blocked_active_publish;
		void* m_last_logged_mooch_controller;
	

	// Last observed mode id of P1's controller, so the diagnostic below fires on

	// the transition only and never every frame.
			uint32_t m_last_player1_mode;
			bool m_spawn_key_was_down;
		bool m_abr_probe_key_was_down;
		bool m_abr_mode_test_key_was_down;
		bool m_local_abr_mode_test_active;
		bool m_local_abr_mode_test_tick_observed;
		bool m_abr_ownership_trace_pending;
		void* m_abr_native_task_player2;
		void* m_abr_native_task_configured_player2;
		void* m_rdv_lifecycle_player2;
		void* m_rdv_lifecycle_items[3];

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
};
}
