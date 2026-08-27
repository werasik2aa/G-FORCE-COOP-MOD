#include "player2.h"

#include "coop_netgame.h"
#include "coop_runtime.h"
#include "gforce_constants.h"
#include "retail/retail_views.h"
#include "save_sync.h"
#include "world_sync.h"
#include "ServerClient/SteamManager.h"

#include <string.h>

namespace coop
{
using namespace gforce;

namespace
{
	class RemoteInputScope final
	{
	public:
		explicit RemoteInputScope(CoopNetGame& netgame) : netgame_(netgame)
		{
			netgame_.BeginRemoteInput();
		}

		~RemoteInputScope()
		{
			netgame_.EndRemoteInput();
		}

	private:
		RemoteInputScope(const RemoteInputScope&);
		RemoteInputScope& operator=(const RemoteInputScope&);

		CoopNetGame& netgame_;
	};

			void LogNativeObjectReferences(const char* log_tag, const char* tag,
			void* object, uint32_t byte_count, uint32_t limit)
		{
			if (!log_tag || !object || byte_count < sizeof(uint32_t))
				return;

			uint32_t reported = 0;
			for (uint32_t offset = 0x04u;
				offset + sizeof(uint32_t) <= byte_count && reported < limit;
				offset += sizeof(uint32_t))
			{
				void* candidate = NULL;
				void* candidate_vtable = NULL;
				__try
				{
					candidate = *reinterpret_cast<void**>(
						static_cast<BYTE*>(object) + offset);
					const uintptr_t address = reinterpret_cast<uintptr_t>(candidate);
					if (address >= 0x01000000u && address < 0x06000000u)
					{
						candidate_vtable = *reinterpret_cast<void**>(candidate);
						const uintptr_t vtable_address =
							reinterpret_cast<uintptr_t>(candidate_vtable);
						if (vtable_address < 0x00400000u || vtable_address >= 0x00910000u)
							candidate_vtable = NULL;
					}
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					candidate_vtable = NULL;
				}
				if (!candidate_vtable)
					continue;
				CoopRuntime::Instance().Log(
					"[%s] tag=%s offset=%03X ptr=%p vtbl=%p\r\n",
					log_tag, tag ? tag : "unknown", offset, candidate, candidate_vtable);
				++reported;
			}
		}

		class PrimaryGamePadScope final
		{

	public:
		explicit PrimaryGamePadScope(CoopNetGame& netgame, bool activate) :
			netgame_(netgame), original_(NULL), active_(false)
		{
			if (activate)
				active_ = netgame_.BeginRemoteGamePadScope(original_);
		}

		~PrimaryGamePadScope()
		{
			if (active_)
				netgame_.EndRemoteGamePadScope(original_);
		}

	private:
		PrimaryGamePadScope(const PrimaryGamePadScope&);
		PrimaryGamePadScope& operator=(const PrimaryGamePadScope&);

		CoopNetGame& netgame_;
		void* original_;
		bool active_;
	};
}

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
		m_abr_probe_key_was_down(false),
		m_abr_mode_test_key_was_down(false),
			m_local_abr_mode_test_active(false),
			m_local_abr_mode_test_tick_observed(false),
			m_abr_ownership_trace_pending(false),
			m_abr_native_task_player2(NULL),
			m_abr_native_task_configured_player2(NULL),
			m_rdv_lifecycle_player2(NULL),
			m_rdv_lifecycle_items(),

		m_remote_abr_mode_active(false),
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
	if (slot < static_cast<int>(retail::PlayerSlot::LocalP1) ||
		slot > static_cast<int>(retail::PlayerSlot::AuxiliaryP3))
	{
		return NULL;
	}

	retail::PlayerRepository players;
	retail::EntityRef entity = {};
	return players.Get(static_cast<retail::PlayerSlot>(slot), entity) ?
		retail::ToPointer(entity.value) : NULL;
}

void* Player2Module::GetFlyEntity()
{
	retail::PlayerRepository players;
	retail::EntityRef fly = {};
	return players.Get(retail::PlayerSlot::Mooch, fly) ?
		retail::ToPointer(fly.value) : NULL;
}

void Player2Module::PublishDefaultModeActiveEntity(void* entity)
{
	const retail::EntityRef published = { retail::ToAddress(entity) };
	retail::PlayerRepository players;
	retail::EntityRef player2 = {};
	if (players.Get(retail::PlayerSlot::RemoteP2, player2) && published == player2)
	{
		if (!m_logged_blocked_active_publish)
		{
			retail::EntityRef active_a = {};
			retail::EntityRef active_b = {};
			retail::ActiveEntityStore().Read(active_a, active_b);
			CoopRuntime::Instance().Log(
				"[active-guard] blocked Default-mode publish for P2 entity=%p; active remains (%p,%p)\r\n",
				entity, retail::ToPointer(active_a.value),
				retail::ToPointer(active_b.value));
			m_logged_blocked_active_publish = true;
		}
		return;
	}

	retail::ActiveEntityStore().Set(published);
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

	const retail::EntityRef entity_ref = { retail::ToAddress(entity) };
	retail::HandlerRef handler = {};
	retail::ControllerRef controller = {};
	return retail::EntityView(entity_ref).Handler(handler) &&
		retail::HandlerView(handler).Controller(controller) ?
		retail::ToPointer(controller.value) : NULL;
}

		uint32_t Player2Module::GetModeId(void* controller)
		{
			if (!controller)
				return 0;

			const retail::ControllerRef controller_ref = {
				retail::ToAddress(controller)
			};
			retail::ModeId mode_id = 0;
			return retail::ControllerView(controller_ref).CurrentMode(mode_id) ?
				mode_id : 0;
		}

	

		int Player2Module::FindGPigSlot(void* controller)
		{
			if (!controller)
				return 0;

			const retail::ControllerRef controller_ref = {
				retail::ToAddress(controller)
			};
			retail::HandlerRef owner = {};
			if (!retail::ControllerView(controller_ref).Owner(owner))
				return 0;

			retail::PlayerRepository players;
			for (int slot = static_cast<int>(retail::PlayerSlot::LocalP1);
				slot <= static_cast<int>(retail::PlayerSlot::AuxiliaryP3); ++slot)
			{
				retail::EntityRef entity = {};
				retail::HandlerRef handler = {};
				if (players.Get(static_cast<retail::PlayerSlot>(slot), entity) &&
					retail::EntityView(entity).Handler(handler) && handler == owner)
				{
					return slot;
				}
			}
					return 0;
	}

	void Player2Module::LogGPigRuntimeState(const char* tag, uint32_t gpig_id,
		void* entity)
	{
		void* handler = NULL;
		void* controller = NULL;
		void* mode = NULL;
		void* mode_vtable = NULL;
		void* abr_aux_table = NULL;
		uint32_t gate_index = 0;
		uint32_t state_index = 0;
		uint32_t aux_index = 0;
		void* gate_entry = NULL;
		void* state_entry = NULL;
		void* aux_entry = NULL;
		void* controller_state = NULL;
		void* state_entry_vtable = NULL;
		void* state_entry_field4 = NULL;
		uint32_t rdv_task_words[30] = {};
		bool rdv_task_valid = false;
		void* aux_entry_vtable = NULL;
		void* aux_entry_field4 = NULL;
		void* mode_owner_controller = NULL;
		void* handler_4c0 = NULL;
		uint32_t mode_words[5] = {};
		uint32_t handler_4c0_words[4] = {};
		float handler_health = -1.0f;
		uint32_t controller_accumulated_hits = 0;
		uint8_t controller_death_flag = 0;
		float controller_invuln_timer = -1.0f;
		bool rdv_mode_map_valid = false;

		__try
		{
			if (entity)
				handler = *reinterpret_cast<void**>(
					reinterpret_cast<BYTE*>(entity) + kEntityHandlerOffset);
			controller = GetController(entity);
			if (controller)
			{
				controller_accumulated_hits = *reinterpret_cast<uint32_t*>(
					reinterpret_cast<BYTE*>(controller) +
					kControllerAccumulatedHitsOffset);
				controller_death_flag = *reinterpret_cast<uint8_t*>(
					reinterpret_cast<BYTE*>(controller) +
					kControllerDeathFlagOffset);
				controller_invuln_timer = *reinterpret_cast<float*>(
					reinterpret_cast<BYTE*>(controller) +
					kControllerInvulnTimerOffset);
				mode = *reinterpret_cast<void**>(
					reinterpret_cast<BYTE*>(controller) + kControllerModeOffset);
				controller_state = *reinterpret_cast<void**>(
					reinterpret_cast<BYTE*>(controller) + 0x90u);
				if (mode)
				{
					mode_vtable = *reinterpret_cast<void**>(mode);
					if (GetModeId(controller) == kAbrModeId)
					{
						BYTE* const mode_bytes = reinterpret_cast<BYTE*>(mode);
						mode_owner_controller = *reinterpret_cast<void**>(mode_bytes + 0x04u);
						for (size_t index = 0; index != _countof(mode_words); ++index)
						{
							mode_words[index] = *reinterpret_cast<uint32_t*>(
								mode_bytes + 0x20u + index * sizeof(uint32_t));
						}
						if (handler)
						{
							handler_4c0 = reinterpret_cast<BYTE*>(handler) + 0x4C0u;
							for (size_t index = 0; index != _countof(handler_4c0_words); ++index)
							{
								handler_4c0_words[index] = *reinterpret_cast<uint32_t*>(
									reinterpret_cast<BYTE*>(handler_4c0) +
									index * sizeof(uint32_t));
							}
						}
						rdv_mode_map_valid = true;
					}
				}
			}

			// XControllerMode_GPig_RDV reaches this owner table at handler+0x4EC
			// during its native enter path.  These reads only report the existing
			// state; they never create, move, or reassign any game object.
			if (handler)
			{
				handler_health = *reinterpret_cast<float*>(
					reinterpret_cast<BYTE*>(handler) + kHandlerHealthOffset);
				abr_aux_table = *reinterpret_cast<void**>(
					reinterpret_cast<BYTE*>(handler) + 0x4ECu);
				gate_index = *reinterpret_cast<uint32_t*>(0x00915634u);
				state_index = *reinterpret_cast<uint32_t*>(0x0091568Cu);
				aux_index = *reinterpret_cast<uint32_t*>(0x009156A8u);
				if (abr_aux_table && gate_index < 64u)
					gate_entry = reinterpret_cast<void**>(abr_aux_table)[gate_index];
				if (abr_aux_table && state_index < 64u)
					state_entry = reinterpret_cast<void**>(abr_aux_table)[state_index];
				if (abr_aux_table && aux_index < 64u)
					aux_entry = reinterpret_cast<void**>(abr_aux_table)[aux_index];
				if (state_entry)
				{
					state_entry_vtable = *reinterpret_cast<void**>(state_entry);
					state_entry_field4 = *reinterpret_cast<void**>(
						reinterpret_cast<BYTE*>(state_entry) + 0x04u);
					// XMotorTask_RDV's constructor at 0x4BA980 allocates exactly
					// 0x78 bytes. Dump only that statically confirmed layout; it is
					// the native state P1 has and presentation-only P2 lacks.
					if (state_entry_vtable == reinterpret_cast<void*>(0x006FC27Cu))
					{
						for (size_t index = 0; index != _countof(rdv_task_words); ++index)
						{
							rdv_task_words[index] = *reinterpret_cast<uint32_t*>(
								static_cast<BYTE*>(state_entry) + index * sizeof(uint32_t));
						}
						rdv_task_valid = true;
					}
				}
				if (aux_entry)
				{
					aux_entry_vtable = *reinterpret_cast<void**>(aux_entry);
					aux_entry_field4 = *reinterpret_cast<void**>(
						reinterpret_cast<BYTE*>(aux_entry) + 0x04u);
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log(
				"[abr-owner] read fault tag=%s id=0x%08X entity=%p\r\n",
				tag ? tag : "unknown", gpig_id, entity);
			return;
		}

		CoopRuntime::Instance().Log(
			"[abr-owner] tag=%s id=0x%08X entity=%p handler=%p controller=%p "
						"mode=%p mode_vtbl=%p mode_id=0x%08X aux_table=%p "
			"idx=(%u,%u,%u) entries=(%p,%p,%p) controller_state=%p "
			"state_vtbl=%p state_field4=%p state_link=%d aux_vtbl=%p "
			"aux_field4=%p aux_link=%d rdv_map=%d mode_owner=%p owner_match=%d "
			"mode_20_30=(%08X,%08X,%08X,%08X,%08X) handler_4c0=%p "
			"handler_4c0_words=(%08X,%08X,%08X,%08X) health=%.2f hits=%u death=%u invuln=%.3f\r\n",
			tag ? tag : "unknown", gpig_id, entity, handler, controller, mode,
			mode_vtable, GetModeId(controller), abr_aux_table, gate_index, state_index,
			aux_index, gate_entry, state_entry, aux_entry, controller_state,
			state_entry_vtable, state_entry_field4,
			state_entry && state_entry_field4 == controller_state ? 1 : 0,
			aux_entry_vtable, aux_entry_field4,
			aux_entry && aux_entry_field4 == controller_state ? 1 : 0,
			rdv_mode_map_valid ? 1 : 0, mode_owner_controller,
			mode_owner_controller == controller ? 1 : 0, mode_words[0], mode_words[1],
			mode_words[2], mode_words[3], mode_words[4], handler_4c0,
			handler_4c0_words[0], handler_4c0_words[1], handler_4c0_words[2],
			handler_4c0_words[3], handler_health, controller_accumulated_hits,
			static_cast<unsigned>(controller_death_flag), controller_invuln_timer);
		if (rdv_task_valid)
		{
			CoopRuntime::Instance().Log(
				"[abr-rdv-task] tag=%s task=%p words_00_1C=(%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X) "
				"words_20_3C=(%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X) "
				"words_40_5C=(%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X) "
				"words_60_74=(%08X,%08X,%08X,%08X,%08X,%08X)\r\n",
				tag ? tag : "unknown", state_entry,
				rdv_task_words[0], rdv_task_words[1], rdv_task_words[2], rdv_task_words[3],
				rdv_task_words[4], rdv_task_words[5], rdv_task_words[6], rdv_task_words[7],
				rdv_task_words[8], rdv_task_words[9], rdv_task_words[10], rdv_task_words[11],
				rdv_task_words[12], rdv_task_words[13], rdv_task_words[14], rdv_task_words[15],
				rdv_task_words[16], rdv_task_words[17], rdv_task_words[18], rdv_task_words[19],
				rdv_task_words[20], rdv_task_words[21], rdv_task_words[22], rdv_task_words[23],
				rdv_task_words[24], rdv_task_words[25], rdv_task_words[26], rdv_task_words[27],
				rdv_task_words[28], rdv_task_words[29]);
		}

	}

		void Player2Module::LogAbrResourceReferences(const char* tag, void* handler)
		{
			if (!handler)
				return;

			void* array_object = NULL;
			void* array_vtable = NULL;
			void** items = NULL;
			uint32_t array_words[6] = {};
			uint32_t neighbor_a[8] = {};
			uint32_t neighbor_b[8] = {};
			uint32_t count = 0;
			__try
			{
				BYTE* const handler_bytes = static_cast<BYTE*>(handler);
				// XMotorSystem is the derived EXDataArray object itself: its derived
				// vtable is at handler+0x4C0 and the base array vtable is at +0x04.
				// The static constructor at 0x5C0250 confirms count +0x10 and
				// backing XResourceController** +0x14 from this derived-object base.
				array_object = handler_bytes + 0x4C0u;
				array_vtable = *reinterpret_cast<void**>(array_object);
				for (size_t index = 0; index != _countof(array_words); ++index)
				{
					array_words[index] = *reinterpret_cast<uint32_t*>(
						reinterpret_cast<BYTE*>(array_object) +
						index * sizeof(uint32_t));
				}
				// EXDataArray's delete path at 0x5C00A0 uses +0x10 as element
				// count and +0x14 as the XResourceController* backing array.
				count = array_words[4];
				items = reinterpret_cast<void**>(array_words[5]);
				for (size_t index = 0; index != _countof(neighbor_a); ++index)
				{
					neighbor_a[index] = *reinterpret_cast<uint32_t*>(handler_bytes +
						0x480u + index * sizeof(uint32_t));
					neighbor_b[index] = *reinterpret_cast<uint32_t*>(handler_bytes +
						0x4A0u + index * sizeof(uint32_t));
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				CoopRuntime::Instance().Log(
					"[abr-resource] read fault tag=%s handler=%p\r\n",
					tag ? tag : "unknown", handler);
				return;
			}

			CoopRuntime::Instance().Log(
				"[abr-resource] tag=%s handler=%p array=%p vtbl=%p "
				"array_words=(%08X,%08X,%08X,%08X,%08X,%08X) count=%u items=%p\r\n",
				tag ? tag : "unknown", handler, array_object, array_vtable,
				array_words[0], array_words[1], array_words[2], array_words[3],
				array_words[4], array_words[5], count, items);
			CoopRuntime::Instance().Log(
				"[abr-handler-near] tag=%s offsets=480..49C values=(%08X,%08X,%08X,%08X,"
				"%08X,%08X,%08X,%08X)\r\n", tag ? tag : "unknown", neighbor_a[0],
				neighbor_a[1], neighbor_a[2], neighbor_a[3], neighbor_a[4], neighbor_a[5],
				neighbor_a[6], neighbor_a[7]);
			CoopRuntime::Instance().Log(
				"[abr-handler-near] tag=%s offsets=4A0..4BC values=(%08X,%08X,%08X,%08X,"
				"%08X,%08X,%08X,%08X)\r\n", tag ? tag : "unknown", neighbor_b[0],
				neighbor_b[1], neighbor_b[2], neighbor_b[3], neighbor_b[4], neighbor_b[5],
				neighbor_b[6], neighbor_b[7]);

			const uint32_t safe_count = count <= 16u ? count : 16u;
			for (uint32_t index = 0; index != safe_count && items; ++index)
			{
				void* resource = NULL;
				void* vtable = NULL;
				uint32_t field4 = 0;
				__try
				{
					resource = items[index];
					if (resource)
					{
						vtable = *reinterpret_cast<void**>(resource);
						field4 = *reinterpret_cast<uint32_t*>(
							static_cast<BYTE*>(resource) + 0x04u);
					}
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
					CoopRuntime::Instance().Log(
						"[abr-resource] item read fault tag=%s index=%u\r\n",
						tag ? tag : "unknown", index);
					break;
				}
				CoopRuntime::Instance().Log(
					"[abr-resource-item] tag=%s index=%u resource=%p vtbl=%p field4=%08X\r\n",
					tag ? tag : "unknown", index, resource, vtable, field4);

				// RTTI identifies this vtable as XMotorFunction_GPigRDV. Its native
				// init/update code at 0x4F36B0/0x4F3AA0 uses resource+0x18 as
				// owner and the owner's +0x68C/+0x690/+0x694 pointers. Report
				// them only; their classes decide whether any is a presentation actor.
				if (vtable == reinterpret_cast<void*>(0x007040DCu))
				{
					void* rdv_owner = NULL;
					void* candidate_68c = NULL;
					void* candidate_690 = NULL;
					void* candidate_694 = NULL;
					void* vtable_68c = NULL;
					void* vtable_690 = NULL;
					void* vtable_694 = NULL;
					__try
					{
						rdv_owner = *reinterpret_cast<void**>(
							static_cast<BYTE*>(resource) + 0x18u);
						if (rdv_owner)
						{
							BYTE* const owner_bytes = static_cast<BYTE*>(rdv_owner);
							candidate_68c = *reinterpret_cast<void**>(owner_bytes + 0x68Cu);
							candidate_690 = *reinterpret_cast<void**>(owner_bytes + 0x690u);
							candidate_694 = *reinterpret_cast<void**>(owner_bytes + 0x694u);
							if (candidate_68c)
								vtable_68c = *reinterpret_cast<void**>(candidate_68c);
							if (candidate_690)
								vtable_690 = *reinterpret_cast<void**>(candidate_690);
							if (candidate_694)
								vtable_694 = *reinterpret_cast<void**>(candidate_694);
						}
					}
					__except (EXCEPTION_EXECUTE_HANDLER)
					{
						CoopRuntime::Instance().Log(
							"[abr-gpigrdv] read fault tag=%s resource=%p\r\n",
							tag ? tag : "unknown", resource);
						continue;
					}
					CoopRuntime::Instance().Log(
						"[abr-gpigrdv] tag=%s resource=%p owner=%p owner_is_handler=%d "
						"ptrs=(%p,%p,%p) vtbls=(%p,%p,%p)\r\n",
						tag ? tag : "unknown", resource, rdv_owner,
						rdv_owner == handler ? 1 : 0, candidate_68c, candidate_690,
						candidate_694, vtable_68c, vtable_690, vtable_694);

					const char* const item_slots[3] = { "68C", "690", "694" };
					const uint32_t item_type_ids[3] = {
						0x04001A71u, 0x04001A74u, 0x04001A73u
					};
					void* const rdv_items[3] = {
						candidate_68c, candidate_690, candidate_694
					};
					for (size_t item_index = 0; item_index != _countof(rdv_items); ++item_index)
					{
						void* item_handler = NULL;
						void* item_handler_vtable = NULL;
						uint32_t item_state_7e = 0;
						uint32_t item_state_80 = 0;
						uint32_t item_handler_motor_vtable = 0;
						__try
						{
							if (rdv_items[item_index])
							{
								BYTE* const item_bytes = static_cast<BYTE*>(rdv_items[item_index]);
								item_handler = *reinterpret_cast<void**>(
									item_bytes + kEntityHandlerOffset);
								item_state_7e = item_bytes[0x7Eu];
								item_state_80 = item_bytes[0x80u];
								if (item_handler)
								{
									item_handler_vtable = *reinterpret_cast<void**>(item_handler);
									item_handler_motor_vtable = *reinterpret_cast<uint32_t*>(
										static_cast<BYTE*>(item_handler) + 0x4C0u);
								}
							}
						}
						__except (EXCEPTION_EXECUTE_HANDLER)
						{
							CoopRuntime::Instance().Log(
								"[abr-gpigrdv-item] read fault tag=%s slot=%s item=%p\r\n",
								tag ? tag : "unknown", item_slots[item_index], rdv_items[item_index]);
							continue;
						}
						CoopRuntime::Instance().Log(
							"[abr-gpigrdv-item] tag=%s slot=%s type=0x%08X item=%p "
							"handler=%p handler_vtbl=%p handler_is_p1=%d state=(%u,%u) motor_vtbl=%08X\r\n",
							tag ? tag : "unknown", item_slots[item_index], item_type_ids[item_index],
							rdv_items[item_index], item_handler, item_handler_vtable,
							item_handler == handler ? 1 : 0, item_state_7e, item_state_80,
							item_handler_motor_vtable);

													// The first RDV XItem has a dedicated XItemHandler_RDV vtable.
							// Scan its full confirmed 0x5FC-byte allocation and report values that resolve
							// to in-process C++ objects (heap pointer plus image-range vtable).
							// This is diagnostic-only and deliberately does not dereference or
							// modify candidates that fail the two basic validation checks.
						if (item_handler_vtable == reinterpret_cast<void*>(0x0071334Cu))
						{
							uint32_t reported = 0;
							for (uint32_t offset = 0x04u; offset < 0x5FCu && reported < 48u;
								offset += sizeof(uint32_t))
							{
								void* candidate = NULL;
								void* candidate_vtable = NULL;
								__try
								{
									candidate = *reinterpret_cast<void**>(
										static_cast<BYTE*>(item_handler) + offset);
									const uintptr_t address = reinterpret_cast<uintptr_t>(candidate);
									if (address >= 0x01000000u && address < 0x06000000u)
									{
										candidate_vtable = *reinterpret_cast<void**>(candidate);
										const uintptr_t vtable_address =
											reinterpret_cast<uintptr_t>(candidate_vtable);
										if (vtable_address < 0x00400000u || vtable_address >= 0x00910000u)
											candidate_vtable = NULL;
									}
								}
								__except (EXCEPTION_EXECUTE_HANDLER)
								{
									candidate_vtable = NULL;
								}
								if (!candidate_vtable)
									continue;
								CoopRuntime::Instance().Log(
									"[abr-rdv-handler-ref] tag=%s offset=%03X ptr=%p vtbl=%p\r\n",
									tag ? tag : "unknown", offset, candidate, candidate_vtable);
								++reported;
							}

							// XItemHandler_RDV+0x4A4 is the confirmed EXItemAnimator_Script.
							// Its child-record layout is believed to be +0x118 / byte +0x12C /
							// 0x20-byte records, but the child field itself is unconfirmed.
							// Probe every field of each record and retain only validated C++ objects.
							void* script_animator = NULL;
							void* records = NULL;
							uint32_t record_count = 0;
							void* script_vtable = NULL;
							__try
							{
								script_animator = *reinterpret_cast<void**>(
									static_cast<BYTE*>(item_handler) + 0x4A4u);
								if (script_animator)
								{
									script_vtable = *reinterpret_cast<void**>(script_animator);
									if (script_vtable == reinterpret_cast<void*>(0x00721934u))
									{
										BYTE* const script_bytes = static_cast<BYTE*>(script_animator);
										records = *reinterpret_cast<void**>(script_bytes + 0x118u);
										record_count = script_bytes[0x12Cu];
									}
								}
							}
							__except (EXCEPTION_EXECUTE_HANDLER)
							{
								script_animator = NULL;
								script_vtable = NULL;
							}
							CoopRuntime::Instance().Log(
								"[abr-rdv-animator] tag=%s script=%p vtbl=%p records=%p count=%u\r\n",
								tag ? tag : "unknown", script_animator, script_vtable, records, record_count);

							const uintptr_t records_address = reinterpret_cast<uintptr_t>(records);
							if (script_vtable == reinterpret_cast<void*>(0x00721934u) &&
								record_count <= 32u && records_address >= 0x01000000u &&
								records_address < 0x06000000u)
							{
								for (uint32_t record_index = 0; record_index < record_count; ++record_index)
								{
									BYTE* const record = static_cast<BYTE*>(records) + record_index * 0x20u;
									for (uint32_t field_offset = 0; field_offset < 0x20u;
										field_offset += sizeof(uint32_t))
									{
										void* child = NULL;
										void* child_vtable = NULL;
										__try
										{
											child = *reinterpret_cast<void**>(record + field_offset);
											const uintptr_t child_address = reinterpret_cast<uintptr_t>(child);
											if (child_address >= 0x01000000u && child_address < 0x06000000u)
											{
												child_vtable = *reinterpret_cast<void**>(child);
												const uintptr_t child_vtable_address =
													reinterpret_cast<uintptr_t>(child_vtable);
												if (child_vtable_address < 0x00400000u ||
													child_vtable_address >= 0x00910000u)
													child_vtable = NULL;
											}
										}
										__except (EXCEPTION_EXECUTE_HANDLER)
										{
											child_vtable = NULL;
										}
										if (child_vtable)
										{
											CoopRuntime::Instance().Log(
												"[abr-rdv-animator-child] tag=%s index=%u field=%02X ptr=%p vtbl=%p\r\n",
												tag ? tag : "unknown", record_index, field_offset, child, child_vtable);

											// RTTI confirms these as EXItemAnimator_Entity. Constructor
											// size is 0x148 and initializes the two dwords at +0x140.
											// Capture them and scan only validated C++ object references.
											if (child_vtable == reinterpret_cast<void*>(0x00721C0Cu))
											{
												uint32_t field_140 = 0;
												uint32_t field_144 = 0;
												__try
												{
													BYTE* const entity_animator = static_cast<BYTE*>(child);
													field_140 = *reinterpret_cast<uint32_t*>(entity_animator + 0x140u);
													field_144 = *reinterpret_cast<uint32_t*>(entity_animator + 0x144u);
												}
												__except (EXCEPTION_EXECUTE_HANDLER)
												{
													field_140 = 0;
													field_144 = 0;
												}
												CoopRuntime::Instance().Log(
													"[abr-rdv-entity] tag=%s index=%u animator=%p fields_140_144=(%08X,%08X)\r\n",
													tag ? tag : "unknown", record_index, child, field_140, field_144);
												LogNativeObjectReferences("abr-rdv-entity-ref", tag, child, 0x148u, 32u);
											}
										}
									}
								}
							}
						}
					}
				}
			}
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

void Player2Module::LogSharedCameraOwnershipState(const char* tag)
{
	if (!tag)
		tag = "unknown";

	void* const player1 = GetGPigEntity(1);
	void* const player2 = GetGPigEntity(2);
	void* active_a = NULL;
	void* active_b = NULL;
	void* camera = NULL;
	void* camera_target_controller = NULL;
	uint32_t camera_target_id = 0;
	uint32_t requested_state = 0;
	bool read_ok = true;
	__try
	{
		active_a = *reinterpret_cast<void**>(kActiveEntityA);
		active_b = *reinterpret_cast<void**>(kActiveEntityB);
		camera = CameraHandler();
		if (camera)
		{
			BYTE* const camera_bytes = static_cast<BYTE*>(camera);
			camera_target_controller = *reinterpret_cast<void**>(
				camera_bytes + kCameraTargetControllerOffset);
			camera_target_id = *reinterpret_cast<uint32_t*>(camera_bytes +
				kCameraTargetControllerOffset + kCameraTargetIdOffset);
			requested_state = *reinterpret_cast<uint32_t*>(
				camera_bytes + kCameraRequestedStateOffset);
		}
		else
		{
			read_ok = false;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		read_ok = false;
	}

	CoopRuntime::Instance().Log(
		"[abr-ownership] tag=%s read_ok=%d P1=%p mode=0x%08X P2=%p mode=0x%08X active=(%p,%p) camera=%p target_controller=%p target_id=0x%08X requested_state=0x%08X\r\n",
		tag, read_ok ? 1 : 0, player1, GetModeId(GetController(player1)),
		player2, GetModeId(GetController(player2)), active_a, active_b, camera,
		camera_target_controller, camera_target_id, requested_state);
}

void Player2Module::LogAbrTaskConfigurationContext(const char* tag)
{
	if (!tag)
		tag = "unknown";

	void* const context = m_spawn_context;
	void* stock_entity = NULL;
	void* active_a = NULL;
	void* active_b = NULL;
	uint32_t context_flags = 0;
	bool read_ok = context != NULL;
	__try
	{
		if (context)
		{
			BYTE* const context_bytes = static_cast<BYTE*>(context);
			// 0x43F4BC invokes the stock RDV task configurator with this exact
			// level-spawn context in ECX and the spawned P1 handler as its argument.
			// These are read-only observations of the proven caller contract.
			context_flags = *reinterpret_cast<uint32_t*>(context_bytes + 0x10u);
			stock_entity = *reinterpret_cast<void**>(context_bytes + 0xE8u);
		}
		active_a = *reinterpret_cast<void**>(kActiveEntityA);
		active_b = *reinterpret_cast<void**>(kActiveEntityB);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		read_ok = false;
	}

	CoopRuntime::Instance().Log(
		"[abr-task-config] tag=%s read_ok=%d context=%p flags=0x%08X context_entity=%p P1=%p P2=%p active=(%p,%p)\r\n",
		tag, read_ok ? 1 : 0, context, context_flags, stock_entity,
		GetGPigEntity(1), GetGPigEntity(2), active_a, active_b);
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
	// own aim-mode turn consume on the next frame РІР‚вЂќ the same class of defect the
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
	if (!player2)
		return false;

	retail::PlayerRepository players;
	retail::EntityRef player1 = {};
	retail::HandlerRef player1_handler = {};
	uint32_t player1_weapon_type = 0xFFFFFFFFu;
	if (!players.Get(retail::PlayerSlot::LocalP1, player1) ||
		!retail::EntityView(player1).Handler(player1_handler) ||
		!retail::HandlerView(player1_handler).SelectedWeaponType(
			player1_weapon_type))
	{
		return false;
	}
	return ApplyPlayer2WeaponSelection(player2, player1_weapon_type, "local P1");
}

bool Player2Module::ApplyPlayer2WeaponSelection(void* player2,
	uint32_t weapon_type, const char* source)
{
	if (!player2 || weapon_type == 0xFFFFFFFFu)
		return false;

	const retail::EntityRef player2_ref = { retail::ToAddress(player2) };
	retail::HandlerRef player2_handler = {};
	uint32_t player2_weapon_type = 0xFFFFFFFFu;
	if (!retail::EntityView(player2_ref).Handler(player2_handler) ||
		!retail::HandlerView(player2_handler).SelectedWeaponType(player2_weapon_type))
	{
		return false;
	}

	// The handler temporarily reports melee/holster while a draw, jump or attack
	// animation is in progress.  A source sequence, not a transient local handler
	// value, is the authority for a real player selection.
	if (m_last_weapon_type == weapon_type)
		return false;

	m_last_weapon_type = weapon_type;
	__try
	{
		WeaponTypeToItemIdFn weapon_type_to_item_id =
			reinterpret_cast<WeaponTypeToItemIdFn>(kWeaponTypeToItemId);
		const uint32_t item_id = weapon_type_to_item_id(weapon_type);
		if (item_id == kDefaultMeleeItemId && weapon_type != 0x40050001u)
			return false;

		SetSelectedWeaponTypeFn set_selected_weapon_type =
			reinterpret_cast<SetSelectedWeaponTypeFn>(kSetSelectedWeaponType);
		set_selected_weapon_type(retail::ToPointer(player2_handler.value), weapon_type);

		GetCurrentWeaponIdFn get_current_weapon_id =
			reinterpret_cast<GetCurrentWeaponIdFn>(kGetCurrentWeaponId);
		const uint32_t player2_current =
			get_current_weapon_id(retail::ToPointer(player2_handler.value));
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
	if (!player1 || !player1_controller)
		return false;

	// Network creation runs from GameTick only after P1's own stock update.  The
	// original Default-only gate strands a peer that connects after the ABR
	// cutscene, although the proven F6 factory path is safe with P1 already in
	// RDV. Allow precisely those two lifecycle modes, not arbitrary transitions.
	const uint32_t player1_mode = GetModeId(player1_controller);
	if (player1_mode != kDefaultModeId && player1_mode != kAbrModeId)
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
			// Keep the pigs out of each other during cutscenes and shared spawns.
			player2_position.x += 0.5f;
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
			m_abr_ownership_trace_pending = false;
						m_abr_native_task_player2 = NULL;
			m_abr_native_task_configured_player2 = NULL;
			m_rdv_lifecycle_player2 = NULL;
			m_rdv_lifecycle_items[0] = NULL;
			m_rdv_lifecycle_items[1] = NULL;
			m_rdv_lifecycle_items[2] = NULL;

			m_logged_blocked_active_publish = false;

		InterlockedExchange(&m_player2_ready, 1);
	}
			CoopRuntime::Instance().Log("[spawn-%s-result] P1=%p P2=%p P2.handler=%p P2.controller=%p ready=%ld\r\n",
			trigger, player1, player2, player2_handler, player2_controller,
			m_player2_ready);
		LogGPigRuntimeState("manual-P2-factory", kGPig2Id, player2);
		InterlockedExchange(&m_spawn_in_progress, 0);

}

void Player2Module::PollPlayer2SpawnKey()
{
	const bool down = (GetAsyncKeyState(CoopRuntime::Instance().Config().spawn_key) & 0x8000) != 0;
	if (down && !m_spawn_key_was_down)
		SpawnPlayer2FromSnapshot("key");
	m_spawn_key_was_down = down;
}

void Player2Module::PollLocalAbrProbeKey()
{
	const bool down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
	if (down && !m_abr_probe_key_was_down)
	{
		void* const player1 = GetGPigEntity(1);
		void* const player2 = GetGPigEntity(2);
		void* const player1_controller = GetController(player1);
		void* const player2_controller = GetController(player2);
		void* player1_handler = NULL;
		void* player2_handler = NULL;
		__try
		{
			if (player1)
				player1_handler = *reinterpret_cast<void**>(
					static_cast<BYTE*>(player1) + kEntityHandlerOffset);
			if (player2)
				player2_handler = *reinterpret_cast<void**>(
					static_cast<BYTE*>(player2) + kEntityHandlerOffset);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			player1_handler = NULL;
			player2_handler = NULL;
		}
					CoopRuntime::Instance().Log(
				"[abr-local-probe] p1=%p handler=%p controller=%p mode=0x%08X "
				"p2=%p handler=%p controller=%p mode=0x%08X\r\n",
				player1, player1_handler, player1_controller,
				GetModeId(player1_controller), player2, player2_handler,
				player2_controller, GetModeId(player2_controller));
			LogGPigRuntimeState("F1-P1", kGPig1Id, player1);
			LogGPigRuntimeState("F1-P2", kGPig2Id, player2);
			// Unlike the transition snapshot, F1 can be pressed after RDV has
			// advanced for several frames and its deferred presentation resources
			// may have become available. This remains read-only.
						LogAbrResourceReferences("F1-P1", player1_handler);
			LogAbrResourceReferences("F1-P2", player2_handler);
			LogSharedCameraOwnershipState("F1-before-next-P2-tick");
			LogAbrTaskConfigurationContext("F1");
			m_abr_ownership_trace_pending = CoopNetGame::Instance().HasRemotePeer() &&
				player2 != NULL;

		}

	m_abr_probe_key_was_down = down;
}

	void Player2Module::PollLocalAbrModeTestKey()
	{

	const bool down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
	if (down && !m_abr_mode_test_key_was_down)
	{
		void* const player2 = GetGPigEntity(2);
		void* const controller = GetController(player2);
		if (!controller)
		{
			CoopRuntime::Instance().Log(
				"[abr-local-test] F2 ignored: create local P2 with F6 first\r\n");
		}
		else
		{
							m_local_abr_mode_test_active = !m_local_abr_mode_test_active;
				m_local_abr_mode_test_tick_observed = false;
				const uint32_t requested_mode = m_local_abr_mode_test_active ?
					kAbrModeId : kDefaultModeId;
				SelectMode(controller, requested_mode);

			CoopRuntime::Instance().Log(
				"[abr-local-test] F2 requested mode=0x%08X P2=%p controller=%p now=0x%08X\r\n",
				requested_mode, player2, controller, GetModeId(controller));
		}
	}
	m_abr_mode_test_key_was_down = down;
}

bool Player2Module::TryEnsurePlayer2RdvTask(const char* source)
{
	if (!source)
		source = "unknown";

	CoopNetGame& netgame = CoopNetGame::Instance();
	if (!netgame.HasRemotePeer())
	{
		CoopRuntime::Instance().Log(
			"[abr-task] source=%s ignored: this native task experiment is network-only\r\n",
			source);
		return false;
	}

	void* const player1 = GetGPigEntity(1);
	void* const player2 = GetGPigEntity(2);
	if (!player1 || !player2 || GetModeId(GetController(player1)) != kAbrModeId)
	{
		CoopRuntime::Instance().Log(
			"[abr-task] source=%s rejected: P1 ABR and live P2 are required P1=%p P2=%p\r\n",
			source, player1, player2);
		return false;
	}
	if (m_abr_native_task_player2 == player2)
		return true;

	void* player2_handler = NULL;
	void* motor_system = NULL;
	void* state_table = NULL;
	void** resources = NULL;
	void* existing_task = NULL;
	void* created_task = NULL;
	uint32_t resource_count = 0;
	uint32_t state_index = 0;
	bool contract_valid = true;
	__try
	{
		player2_handler = *reinterpret_cast<void**>(
			static_cast<BYTE*>(player2) + kEntityHandlerOffset);
		if (!player2_handler)
		{
			contract_valid = false;
		}
		else
		{
			BYTE* const handler_bytes = static_cast<BYTE*>(player2_handler);
			motor_system = handler_bytes + 0x4C0u;
			state_table = *reinterpret_cast<void**>(handler_bytes + 0x4ECu);
			resource_count = *reinterpret_cast<uint32_t*>(
				static_cast<BYTE*>(motor_system) + 0x10u);
			resources = *reinterpret_cast<void***>(
				static_cast<BYTE*>(motor_system) + 0x14u);
			state_index = *reinterpret_cast<uint32_t*>(kGPigRdvTaskStateIndex);
			if (!state_table || state_index >= 64u || resource_count <= 11u || !resources ||
				!resources[11] || *reinterpret_cast<void**>(resources[11]) !=
				reinterpret_cast<void*>(0x007040DCu))
			{
				contract_valid = false;
			}
			else
			{
				existing_task = reinterpret_cast<void**>(state_table)[state_index];
				if (existing_task && *reinterpret_cast<void**>(existing_task) !=
					reinterpret_cast<void*>(kGPigRdvTaskVtable))
					contract_valid = false;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		contract_valid = false;
	}
	if (!contract_valid)
	{
		CoopRuntime::Instance().Log(
			"[abr-task] source=%s rejected: task factory contract unavailable P2=%p handler=%p state=%p index=%u resources=%p count=%u existing=%p\r\n",
			source, player2, player2_handler, state_table, state_index, resources,
			resource_count, existing_task);
		return false;
	}
		if (existing_task)
		{
			m_abr_native_task_player2 = player2;
			CoopRuntime::Instance().Log(
				"[abr-task] source=%s P2=%p already has native XMotorTask_RDV=%p; no factory call made\r\n",
				source, player2, existing_task);
			return ConfigurePlayer2RdvTask(source, player2, player2_handler,
				existing_task);
		}

	// This is the engine's own lazy factory: it allocates, constructs and inserts
	// XMotorTask_RDV in handler+0x4EC. The test intentionally does not select RDV
	// mode and does not configure task fields; it only proves whether this missing
	// stock object is sufficient for the next controller-mode experiment.
	CoopRuntime::Instance().Log(
		"[abr-task] source=%s native factory begin P2=%p handler=%p motor=%p state=%p index=%u\r\n",
		source, player2, player2_handler, motor_system, state_table, state_index);
	__try
	{
		EnsureGPigRdvTaskFn ensure_task =
			reinterpret_cast<EnsureGPigRdvTaskFn>(kEnsureGPigRdvTask);
		created_task = ensure_task(motor_system, true);
	}
	__except (CoopRuntime::Instance().LogException(
		GetExceptionInformation(), "abr-f7-native-rdv-task-factory"))
	{
		CoopRuntime::Instance().Log(
			"[abr-task] source=%s native factory fault\r\n", source);
		return false;
	}

	void* task_vtable = NULL;
	bool creation_valid = false;
	__try
	{
		void* const table_task = reinterpret_cast<void**>(state_table)[state_index];
		if (created_task == table_task && created_task)
		{
			task_vtable = *reinterpret_cast<void**>(created_task);
			creation_valid = task_vtable == reinterpret_cast<void*>(kGPigRdvTaskVtable);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		creation_valid = false;
	}
	if (!creation_valid)
	{
		CoopRuntime::Instance().Log(
			"[abr-task] source=%s native factory postcondition failed P2=%p task=%p vtbl=%p\r\n",
			source, player2, created_task, task_vtable);
		return false;
	}

			m_abr_native_task_player2 = player2;
		CoopRuntime::Instance().Log(
			"[abr-task] source=%s native factory complete P2=%p XMotorTask_RDV=%p vtbl=%p\r\n",
			source, player2, created_task, task_vtable);
		LogGPigRuntimeState("P2-RDV-task-created", kGPig2Id, player2);
		return ConfigurePlayer2RdvTask(source, player2, player2_handler,
			created_task);
	}

bool Player2Module::ConfigurePlayer2RdvTask(const char* source, void* player2,
	void* player2_handler, void* task)
{
	if (!source)
		source = "unknown";
	if (m_abr_native_task_configured_player2 == player2)
		return true;

	void* const context = m_spawn_context;
	void* const player1 = GetGPigEntity(1);
	void* context_entity = NULL;
	void* active_a = NULL;
	void* active_b = NULL;
	uint32_t context_flags = 0;
	bool contract_valid = context != NULL && player1 != NULL && player2 != NULL &&
		player2_handler != NULL && task != NULL;
	__try
	{
		if (contract_valid)
		{
			BYTE* const context_bytes = static_cast<BYTE*>(context);
			context_flags = *reinterpret_cast<uint32_t*>(context_bytes + 0x10u);
			context_entity = *reinterpret_cast<void**>(context_bytes + 0xE8u);
			active_a = *reinterpret_cast<void**>(kActiveEntityA);
			active_b = *reinterpret_cast<void**>(kActiveEntityB);
			contract_valid = context_entity == player1 && active_a == player1 &&
				active_b == player1 && (context_flags & 0x20000000u) != 0 &&
				*reinterpret_cast<void**>(task) ==
				reinterpret_cast<void*>(kGPigRdvTaskVtable);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		contract_valid = false;
	}
	if (!contract_valid)
	{
		CoopRuntime::Instance().Log(
			"[abr-task-config] source=%s rejected P1=%p P2=%p handler=%p task=%p context=%p flags=0x%08X context_entity=%p active=(%p,%p)\r\n",
			source, player1, player2, player2_handler, task, context, context_flags,
			context_entity, active_a, active_b);
		return false;
	}

	CoopRuntime::Instance().Log(
		"[abr-task-config] source=%s native begin P2=%p task=%p context=%p flags=0x%08X\r\n",
		source, player2, task, context, context_flags);
	__try
	{
		ConfigureGPigRdvTaskFn configure_task =
			reinterpret_cast<ConfigureGPigRdvTaskFn>(kConfigureGPigRdvTask);
		configure_task(context, player2_handler);
	}
	__except (CoopRuntime::Instance().LogException(
		GetExceptionInformation(), "abr-native-rdv-task-configurator"))
	{
		CoopRuntime::Instance().Log(
			"[abr-task-config] source=%s native configurator fault\r\n", source);
		return false;
	}

	bool configured = false;
	uint8_t enabled = 0;
	__try
	{
		configured = *reinterpret_cast<void**>(task) ==
			reinterpret_cast<void*>(kGPigRdvTaskVtable);
		if (configured)
		{
			enabled = *(static_cast<uint8_t*>(task) + 0x30u);
			configured = enabled == 1u;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		configured = false;
	}
	if (!configured)
	{
		CoopRuntime::Instance().Log(
			"[abr-task-config] source=%s postcondition failed P2=%p task=%p enabled=%u\r\n",
			source, player2, task, static_cast<unsigned>(enabled));
		return false;
	}

	m_abr_native_task_configured_player2 = player2;
	CoopRuntime::Instance().Log(
		"[abr-task-config] source=%s native configured P2=%p task=%p enabled=%u\r\n",
		source, player2, task, static_cast<unsigned>(enabled));
	return true;
}

void Player2Module::TracePlayer2RdvItemLifecycle(void* player2)
{
	void* handler = NULL;
	void* items[3] = {};
	bool read_ok = player2 != NULL;
	__try
	{
		if (player2)
			handler = *reinterpret_cast<void**>(
			reinterpret_cast<BYTE*>(player2) + kEntityHandlerOffset);
		if (!handler)
		{
			read_ok = false;
		}
		else
		{
			BYTE* const handler_bytes = static_cast<BYTE*>(handler);
			items[0] = *reinterpret_cast<void**>(handler_bytes + 0x68Cu);
			items[1] = *reinterpret_cast<void**>(handler_bytes + 0x690u);
			items[2] = *reinterpret_cast<void**>(handler_bytes + 0x694u);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		read_ok = false;
	}

	const bool is_new_player = m_rdv_lifecycle_player2 != player2;
	const bool changed = is_new_player ||
		m_rdv_lifecycle_items[0] != items[0] ||
		m_rdv_lifecycle_items[1] != items[1] ||
		m_rdv_lifecycle_items[2] != items[2];
	if (!changed)
		return;

	CoopRuntime::Instance().Log(
		"[p2-rdv-lifecycle] state=%s read_ok=%d P2=%p handler=%p items=(%p,%p,%p) previous=(%p,%p,%p)\r\n",
		is_new_player ? "initial" : "changed", read_ok ? 1 : 0, player2, handler,
		items[0], items[1], items[2], m_rdv_lifecycle_items[0],
		m_rdv_lifecycle_items[1], m_rdv_lifecycle_items[2]);
	m_rdv_lifecycle_player2 = player2;
	m_rdv_lifecycle_items[0] = items[0];
	m_rdv_lifecycle_items[1] = items[1];
	m_rdv_lifecycle_items[2] = items[2];
}

void __fastcall Player2Module::HookControllerUpdate(
	void* controller, void*)
{
	Instance().UpdateController(controller);
}

void Player2Module::TickPlayer1(void* player1_controller)
{
	// The first real P1 controller tick cannot happen in the main menu.  It is
	// therefore the safe boundary for turning a normal loaded save into an IP
	// host, without advertising or probing Steam during startup.
	SaveSync::Instance().CaptureLoadedHostSlot();
	WorldSync::Instance().NotifyLocalWorldReady();
	if (SteamManager)
		SteamManager->NotifyGameWorldReady();

	// The single shared GPig camera belongs to whoever the player is actually
	// driving.  Its mode goes back to Default immediately after the native Mooch
	// hand-off, so use the confirmed network owner state rather than a mode or
	// process-global pointer that lasts only for that transition frame.
	if (!CoopNetGame::Instance().IsLocalFlyControlled() &&
		GetModeId(player1_controller) == kDefaultModeId)
		RefreshCameraForController(player1_controller);
	CoopNetGame::Instance().BeginLocalInputCapture();
		if (!RunStockControllerUpdate(player1_controller, "local-player1"))
			return;
		CoopNetGame::Instance().PublishLocalPlayerMode(
			GetModeId(player1_controller));
		CoopNetGame::Instance().PublishLocalPlayerTransform(GetGPigEntity(1));
	// P1's 0x5BCF30 and 0x5BB1D0 have just finished driving the single shared
	// camera, so this is the only frame point where 0x52AD20 reports P1's own yaw.
	// The remote machine cannot read its P2's camera because P2 owns none there РІР‚вЂќ
	// it consumes this value.
	float local_camera_yaw = 0.0f;
	const bool local_camera_yaw_valid = ReadLocalCameraYaw(local_camera_yaw);
	CoopNetGame::Instance().PublishLocalCameraYaw(
		local_camera_yaw, local_camera_yaw_valid);

	// Network spawning must happen after P1's normal update.  The factory then
	// receives the same settled transform/physics context as the proven-safe
	// manual F6 path.  GameTick itself returns immediately once P2 exists.
		CoopNetGame::Instance().GameTick();
	if (CoopNetGame::Instance().HasRemotePeer())
		TracePlayer2RdvItemLifecycle(GetGPigEntity(2));
		HandlePlayer1ModeTransition(player1_controller);

}

void Player2Module::HandlePlayer1ModeTransition(void* player1_controller)
{
	const uint32_t mode_now = GetModeId(player1_controller);
	if (mode_now == m_last_player1_mode)
		return;

			if (mode_now == kMoochSwitchModeId)
		{
			CoopNetGame::Instance().ConfirmLocalFlyControl();
			CoopNetGame::Instance().RequestRemotePlayerTickDeferral();
		}
		if (mode_now == kAbrModeId || m_last_player1_mode == kAbrModeId)
		{
			const char* const abr_reason = mode_now == kAbrModeId ?
				"P1-ABR-enter" : "P1-ABR-exit";
			void* const player1 = GetGPigEntity(1);
			void* player1_handler = NULL;
			__try
			{
				if (player1)
					player1_handler = *reinterpret_cast<void**>(
						reinterpret_cast<BYTE*>(player1) + kEntityHandlerOffset);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				player1_handler = NULL;
			}
			LogGPigRuntimeState(abr_reason, kGPig1Id, player1);
			CoopNetGame::Instance().LogAbrDef9CandidateState(abr_reason);
			LogAbrResourceReferences(abr_reason, player1_handler);
		}
		m_last_player1_mode = mode_now;

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

	bool Player2Module::RunStockControllerUpdate(void* controller,
		const char* context)
	{
		if (!controller || !m_original_update)
			return false;
		__try
		{
			m_original_update(controller);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log(
				"[controller] stock update fault context=%s controller=%p\r\n",
				context ? context : "unknown", controller);
			return false;
		}
	}

	void Player2Module::UpdateController(void* controller)
	{
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

		const bool is_ready_player2 = slot ==
			static_cast<int>(retail::PlayerSlot::RemoteP2) &&
			CoopRuntime::Instance().Config().enabled &&
			InterlockedCompareExchange(&m_player2_ready, 0, 0) != 0;
		if (!is_ready_player2)
		{
			if (slot == static_cast<int>(retail::PlayerSlot::LocalP1))
			{
				TickPlayer1(controller);
			}
			else if (controller == GetController(GetFlyEntity()))
			{
				void* const fly = GetFlyEntity();
				const uint32_t mode_before = GetModeId(controller);
				if (RunStockControllerUpdate(controller, "fly"))
				{
					CoopNetGame::Instance().ObserveLocalFlyMode(mode_before,
						GetModeId(controller));
					CoopNetGame::Instance().MaintainLocalFlyActiveEntity(fly);
					CoopNetGame::Instance().PublishLocalFlyTransform(fly);
					if (!CoopNetGame::Instance().IsLocalFlyControlled())
						CoopNetGame::Instance().ApplyRemoteFlyTransform(fly);
				}
			}
			else
			{
				RunStockControllerUpdate(controller, "non-player");
			}

			// F6 is edge-triggered and independent of the controller being visited.
				PollPlayer2SpawnKey();
				PollLocalAbrProbeKey();
				PollLocalAbrModeTestKey();

			return;
		}

		CoopNetGame& netgame = CoopNetGame::Instance();
		if (netgame.ConsumeRemotePlayerTickDeferral())
			return;

		void* const player2 = GetGPigEntity(
			static_cast<int>(retail::PlayerSlot::RemoteP2));
		const bool preserve_fly_camera = netgame.IsLocalFlyControlled();
		RemoteInputScope remote_input(netgame);
		PrimaryGamePadScope remote_gamepad(netgame, preserve_fly_camera);

		if (GetModeId(controller) == kInactiveModeId)
		{
			SharedCameraAimState saved_camera_state = {};
			const bool have_saved_camera_state =
				SaveSharedCameraAimState(saved_camera_state);
			SelectMode(controller, kDefaultModeId);
			if (have_saved_camera_state)
				RestoreSharedCameraAimState(saved_camera_state);
		}
		if (!m_player2_default_mode_initialized &&
			GetModeId(controller) == kDefaultModeId)
		{
			ConfigurePlayer2DefaultMode(controller);
			m_player2_default_mode_initialized = true;
			CoopRuntime::Instance().Log(
				"[player2] Default mode initialized mode=0x%08X\r\n",
				GetModeId(controller));
		}

			const bool local_abr_test = m_local_abr_mode_test_active &&
				!m_local_abr_mode_test_tick_observed && !netgame.HasRemotePeer();
			const bool remote_abr = netgame.IsRemoteAbrMode();
			const uint32_t current_mode = GetModeId(controller);
			// F2 remains a standalone diagnostic only. In a network game P2 keeps
			// its confirmed Default control mode: the native RDV presentation is an
			// independent item subsystem, and SelectMode(RDV) is known to be reset by
			// P2's first stock tick.
			if (local_abr_test && current_mode != kAbrModeId)
			{
				SelectMode(controller, kAbrModeId);
				CoopRuntime::Instance().Log(
					"[abr-sync] P2 entered native ABR mode source=local-F2 controller=%p\r\n",
					controller);
			}
			else if (!local_abr_test && current_mode == kAbrModeId)
			{
				SelectMode(controller, kDefaultModeId);
				CoopRuntime::Instance().Log(
					"[abr-sync] P2 returned to Default mode controller=%p\r\n",
					controller);
			}
			m_remote_abr_mode_active = remote_abr;

		uint32_t remote_weapon_type = 0xFFFFFFFFu;
		if (netgame.GetActiveRemoteWeaponType(remote_weapon_type))
			ApplyPlayer2WeaponSelection(player2, remote_weapon_type, "remote P1");
		netgame.ArmRemoteP2AmmoOwner(player2);

		const bool observe_local_abr_tick = local_abr_test &&
			GetModeId(controller) == kAbrModeId;
		if (observe_local_abr_tick)
			LogGPigRuntimeState("F2-P2-before-stock-tick", kGPig2Id, player2);

		SharedCameraAimState saved_fly_camera_state = {};
		const bool restore_fly_camera = preserve_fly_camera &&
			SaveSharedCameraAimState(saved_fly_camera_state);
		const bool stock_update_completed =
			RunStockControllerUpdate(controller, "remote-player2");
		if (restore_fly_camera)
			RestoreSharedCameraAimState(saved_fly_camera_state);
					if (!stock_update_completed)
				return;

			if (m_abr_ownership_trace_pending)
			{
				LogSharedCameraOwnershipState("F1-after-P2-stock-tick");
				m_abr_ownership_trace_pending = false;
			}

			// F7 proved that this engine-owned factory is the missing P2 Drive-side
			// state and that the following stock frames create the native ABR actor
			// themselves. Run it once at the identical post-P2-tick boundary used by
			// the former presentation experiment. It never creates presentation items
			// SelectMode(RDV), or writes a transform/state-table entry directly.
			if (netgame.HasRemotePeer())
			{
				TryEnsurePlayer2RdvTask("network-post-P2-tick");
				TracePlayer2RdvItemLifecycle(player2);
			}

			if (observe_local_abr_tick)

		{
			LogGPigRuntimeState("F2-P2-after-stock-tick", kGPig2Id, player2);
			m_local_abr_mode_test_tick_observed = true;
			CoopRuntime::Instance().Log(
				"[abr-local-test] first stock P2 ABR tick completed mode_now=0x%08X; "
				"F2 will not reselect until toggled again\r\n",
				GetModeId(controller));
		}

		netgame.ApplyRemotePlayerTransform(player2);
		if (!m_logged_player2)
		{
			CoopRuntime::Instance().Log(
				"[player2] packet-driven update entity=%p controller=%p mode=0x%08X\r\n",
				player2, controller, GetModeId(controller));
			m_logged_player2 = true;
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
		LogGPigRuntimeState("stock-factory", gpig_id, player1);

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
