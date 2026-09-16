#include "coop_netgame.h"

#include "coop_runtime.h"
#include "chat_overlay.h"
#include "gforce_constants.h"
#include "protocol/packet_view.h"
#include "player2.h"
#include "save_sync.h"
#include "shared_camera.h"
#include "retail/retail_types.h"
#include "retail/retail_views.h"
#include "world_sync.h"

#include "ServerClient/MClient.h"
#include "ServerClient/MServer.h"
#include "ServerClient/MServerONLINE.h"
#include "ServerClient/SteamManager.h"

#include <float.h>
#include <math.h>
#include <string.h>
#include <intrin.h>

namespace coop
{
	constexpr DWORD kInputSendIntervalMs = 16;
	constexpr DWORD kNetworkSpawnDelayMs = 1000;
	constexpr DWORD kRemoteFlyAbilityEventLifetimeMs = 3000;
	constexpr std::size_t kFlyDualLaserRouteItemCount = 2;
	// Confirmed at Fly_Active 0x005B6017..0x005B608F: its cached aim direction
	// is multiplied by 100 before it is written to the controller-local aim task.
	constexpr float kFlyDualLaserTargetDistance = 100.0f;
	constexpr DWORD kInvalidInputTraceIntervalMs = 1000;

	const std::uint32_t kFlyRawActionIds[kCoopFlyRawActionCount] = {
		0x40080046u,
		0x40080047u,
		0x40080029u,
		0x40080034u,
		0x40080036u,
		0x4008000Bu
	};
	namespace
	{
	int FindFlyRawActionIndex(std::uint32_t action)
	{
		for (std::uint32_t index = 0; index < kCoopFlyRawActionCount; ++index)
		{
			if (kFlyRawActionIds[index] == action)
				return static_cast<int>(index);
		}
		return -1;
	}

	bool IsFlyActiveRawPressedQuery(std::uint32_t action,
		std::uintptr_t caller_return_address)
	{
		return (action == gforce::kFlyDualLaserRawActionId &&
				caller_return_address ==
					gforce::kFlyDualLaserRawPressedQueryReturn) ||
			(action == 0x40080036u &&
				caller_return_address == 0x005B62EDu) ||
			(action == 0x4008000Bu &&
				caller_return_address == 0x005B6321u);
	}

	bool IsFlyActiveRawHeldQuery(std::uint32_t action,
		std::uintptr_t caller_return_address)
	{
		return (action == 0x40080046u &&
				(caller_return_address == 0x005B5108u ||
					caller_return_address == 0x005B5B7Eu)) ||
			(action == 0x40080047u &&
				(caller_return_address == 0x005B5164u ||
					caller_return_address == 0x005B5BDAu)) ||
			(action == 0x40080034u &&
				caller_return_address == 0x005B61A3u);
	}

	bool TryDescribeObjectRttiName(std::uintptr_t vtable, char* out,
		std::size_t out_size)
	{
		if (!out || out_size == 0)
			return false;
		out[0] = '\0';
		if (vtable < sizeof(std::uintptr_t))
			return false;

		__try
		{
			// GForce.exe is a 32-bit MSVC binary. Its vtable[-1] is a complete
			// object locator; the type descriptor pointer is +0x0C and its
			// decorated name begins after two pointers. This is diagnostics only.
			const std::uintptr_t complete_object_locator =
				*reinterpret_cast<const std::uintptr_t*>(
					vtable - sizeof(std::uintptr_t));
			if (!complete_object_locator)
				return false;
			const std::uintptr_t type_descriptor =
				*reinterpret_cast<const std::uintptr_t*>(
					complete_object_locator + 0x0Cu);
			if (!type_descriptor)
				return false;
			const char* decorated_name = reinterpret_cast<const char*>(
				type_descriptor + sizeof(std::uintptr_t) * 2u);
			if (decorated_name[0] != '.' || decorated_name[1] != '?' ||
				(decorated_name[2] != 'A' && decorated_name[2] != 'B') ||
				(decorated_name[3] != 'V' && decorated_name[3] != 'U'))
			{
				return false;
			}

			const char* cursor = decorated_name + 4;
			std::size_t copied = 0;
			while (copied + 1 < out_size)
			{
				const char character = *cursor++;
				if (character == '\0')
					break;
				if (character == '@' && *cursor == '@')
					break;
				const unsigned char printable =
					static_cast<unsigned char>(character);
				if (printable < 0x20u || printable > 0x7Eu)
				{
					out[0] = '\0';
					return false;
				}
				out[copied++] = character;
			}
			out[copied] = '\0';
			return copied != 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out[0] = '\0';
			return false;
		}
	}


	// An object relay can enter the forwarder, and a direct forwarder can in turn
	// dispatch another relay. The peer recreates all nested calls while replaying
	// their outer native route, so only a top-level route may become a packet.
	thread_local std::uint32_t g_object_event_route_depth = 0;
	thread_local std::uint32_t g_remote_object_event_replay_depth = 0;

	bool IsRemoteObjectEventReplayActive()
	{
		return g_remote_object_event_replay_depth != 0;
	}

	bool IsObjectEventRouteNested()
	{
		return g_object_event_route_depth != 0;
	}

	bool GetObjectEventReceiver(void*& receiver)
	{
		// `sub_41E890` is `mov ecx, 0x00912AA8`, not `mov ecx, [0x00912AA8]`.
		// Replaying a direct route must preserve that literal ECX receiver.
		receiver = reinterpret_cast<void*>(gforce::kObjectEventReceiverAddress);
		return true;
	}

	bool IsCanonicalObjectEventReceiver(void* receiver)
	{
		void* canonical_receiver = nullptr;
		return receiver && GetObjectEventReceiver(canonical_receiver) &&
			receiver == canonical_receiver;
	}

	bool TryReadObjectVtable(void* object, std::uint32_t& out_vtable)
	{
		out_vtable = 0;
		if (!object)
			return false;
		__try
		{
			out_vtable = *reinterpret_cast<const std::uint32_t*>(object);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out_vtable = 0;
		}
		return out_vtable != 0;
	}

	bool ResolveFlyDualLaserRouteItems(void* fly,
		retail::FlyDualLaserRouteItemRef
			(&items)[kFlyDualLaserRouteItemCount],
		std::uint32_t (&item_ids)[kFlyDualLaserRouteItemCount])
	{
		for (std::size_t index = 0; index < kFlyDualLaserRouteItemCount; ++index)
		{
			items[index] = {};
			item_ids[index] = 0;
		}
		if (!fly)
			return false;

		const retail::EntityRef fly_ref = { retail::ToAddress(fly) };
		retail::HandlerRef handler = {};
		retail::InventoryRef inventory = {};
		bool uses_alternate_item_set = false;
		if (!retail::EntityView(fly_ref).Handler(handler) ||
			!retail::HandlerView(handler).Inventory(inventory) ||
			!retail::HandlerView(handler).FlyDualLaserUsesAlternateItemSet(
				uses_alternate_item_set))
		{
			return false;
		}

		const std::uint32_t item_base = uses_alternate_item_set ?
			gforce::kFlyDualLaserAlternateItemBase :
			gforce::kFlyDualLaserDefaultItemBase;
		const std::uint32_t route_slots[kFlyDualLaserRouteItemCount] = {
			gforce::kFlyDualLaserFirstRouteSlot,
			gforce::kFlyDualLaserSecondRouteSlot
		};
		for (std::size_t index = 0; index < kFlyDualLaserRouteItemCount; ++index)
		{
			const std::uint32_t expected_item_id =
				item_base + static_cast<std::uint32_t>(index);
			retail::FlyDualLaserRouteItemRef item = {};
			std::uint32_t actual_item_id = 0;
			const bool found_matching_item =
				retail::NativeGameApi::FindFlyDualLaserRouteItem(inventory,
					route_slots[index], item) &&
				item &&
				retail::FlyDualLaserRouteItemView(item).ItemId(actual_item_id) &&
				actual_item_id == expected_item_id;
			if (!found_matching_item)
			{
				item = {};
				actual_item_id = 0;
				if (!retail::NativeGameApi::ResolveFlyDualLaserRouteItem(inventory,
					expected_item_id, item) ||
					!item ||
					!retail::FlyDualLaserRouteItemView(item).ItemId(
						actual_item_id) ||
					actual_item_id != expected_item_id)
				{
					return false;
				}
			}
			items[index] = item;
			item_ids[index] = expected_item_id;
		}
		return true;
	}

	bool SetFlyDualLaserRouteItemsActive(
		const retail::FlyDualLaserRouteItemRef
			(&items)[kFlyDualLaserRouteItemCount],
		const std::uint32_t (&item_ids)[kFlyDualLaserRouteItemCount],
		bool active)
	{
		for (std::size_t index = 0; index < kFlyDualLaserRouteItemCount; ++index)
		{
			std::uint32_t actual_item_id = 0;
			if (!items[index] || item_ids[index] == 0 ||
				!retail::FlyDualLaserRouteItemView(items[index]).ItemId(
					actual_item_id) ||
				actual_item_id != item_ids[index])
			{
				return false;
			}
		}
		for (std::size_t index = 0; index < kFlyDualLaserRouteItemCount; ++index)
		{
			if (!retail::FlyDualLaserRouteItemView(items[index]).SetEffectActive(
				active))
			{
				return false;
			}
		}
		return true;
	}

	bool IsFiniteFloat(float value)
	{
		// Comparisons reject NaN, and +/- infinity lie outside FLT_MAX. Do not
		// depend on CRT-specific finite helpers in this x86 injection path.
		return value >= -FLT_MAX && value <= FLT_MAX;
	}

	bool IsFiniteFloatArray(const float* values, std::size_t count)
	{
		if (!values)
			return false;
		for (std::size_t index = 0; index < count; ++index)
		{
			if (!IsFiniteFloat(values[index]))
				return false;
		}
		return true;
	}

	bool IsFiniteWireTransform(const float position[4], const float rotation[4])
	{
		return IsFiniteFloatArray(position, 4) &&
			IsFiniteFloatArray(rotation, 4);
	}

	// Convert the fixed-size wire representation only after validating every
	// component.  Feature code should work with the typed retail transform.
	bool DecodeFiniteWireTransform(const float position[4], const float rotation[4],
		retail::Transform& transform)
	{
		transform = {};
		if (!IsFiniteWireTransform(position, rotation))
			return false;

		memcpy(&transform.position, position, sizeof(transform.position));
		memcpy(&transform.rotation, rotation, sizeof(transform.rotation));
		return true;
	}

	bool IsFiniteRetailTransform(const retail::Transform& transform)
	{
		return IsFiniteFloat(transform.position.x) &&
			IsFiniteFloat(transform.position.y) &&
			IsFiniteFloat(transform.position.z) &&
			IsFiniteFloat(transform.position.w) &&
			IsFiniteFloat(transform.rotation.x) &&
			IsFiniteFloat(transform.rotation.y) &&
			IsFiniteFloat(transform.rotation.z) &&
			IsFiniteFloat(transform.rotation.w);
	}

	bool IsValidAimRay(const retail::AimRay& aim_ray)
	{
		const retail::Vec3& direction = aim_ray.direction;
		const float length_squared = direction.x * direction.x +
			direction.y * direction.y + direction.z * direction.z;
		return IsFiniteFloat(aim_ray.origin.x) &&
			IsFiniteFloat(aim_ray.origin.y) &&
			IsFiniteFloat(aim_ray.origin.z) &&
			IsFiniteFloat(direction.x) &&
			IsFiniteFloat(direction.y) &&
			IsFiniteFloat(direction.z) &&
			length_squared > 0.25f && length_squared < 4.0f;
	}

	bool ReadValidAimRay(const void* input_manager, retail::AimRay& out)
	{
		out = {};
		if (!input_manager)
			return false;
		retail::InputManagerRef input_manager_ref = {};
		input_manager_ref.value = retail::ToAddress(input_manager);
		return retail::InputManagerView(input_manager_ref).ReadAimRay(out) &&
			IsValidAimRay(out);
	}

	bool BuildFlyDualLaserTarget(const retail::Vec3& origin,
		const retail::Vec3& direction, float out_target[3])
	{
		if (!out_target || !IsFiniteFloat(origin.x) ||
			!IsFiniteFloat(origin.y) || !IsFiniteFloat(origin.z) ||
			!IsFiniteFloat(direction.x) || !IsFiniteFloat(direction.y) ||
			!IsFiniteFloat(direction.z))
		{
			return false;
		}

		out_target[0] = origin.x + direction.x * kFlyDualLaserTargetDistance;
		out_target[1] = origin.y + direction.y * kFlyDualLaserTargetDistance;
		out_target[2] = origin.z + direction.z * kFlyDualLaserTargetDistance;
		return IsFiniteFloatArray(out_target, 3);
	}

	bool SetFlyDualLaserPresentationTarget(void* fly,
		const float target[3])
	{
		if (!fly || !IsFiniteFloatArray(target, 3))
			return false;

		const retail::EntityRef fly_ref = { retail::ToAddress(fly) };
		retail::HandlerRef handler = {};
		retail::ControllerRef controller = {};
		if (!retail::EntityView(fly_ref).Handler(handler) ||
			!retail::HandlerView(handler).Controller(controller))
		{
			return false;
		}

		const retail::Vec4 retail_target = {
			target[0], target[1], target[2], 0.0f
		};
		return retail::NativeGameApi::SetFlyDualLaserPresentationTarget(
			controller, retail_target);
	}

	bool IsSafeInputSnapshot(const CoopInput& input, const char*& reason)
	{
		if (!IsFiniteWireTransform(input.position, input.rotation))
		{
			reason = "P1-transform";
			return false;
		}
		if (!IsFiniteFloatArray(input.analog_axis, kCoopInputAnalogAxisCount))
		{
			reason = "analog-axis";
			return false;
		}
		if (!IsFiniteFloatArray(input.aim_origin, 3) ||
			!IsFiniteFloatArray(input.aim_direction, 3))
		{
			reason = "aim-ray";
			return false;
		}
		if (input.camera_yaw_valid != 0 &&
			(!IsFiniteFloat(input.camera_yaw) ||
				input.camera_yaw <= -1000.0f || input.camera_yaw >= 1000.0f))
		{
			reason = "camera-yaw";
			return false;
		}
		if (input.fly_controlled != 0 && input.fly_transform_sequence != 0 &&
			!IsFiniteWireTransform(input.fly_position, input.fly_rotation))
		{
			reason = "Fly-transform";
			return false;
		}
		reason = nullptr;
		return true;
	}
	}

	namespace
	{
		enum class PlayerHealthOwner
		{
			None,
			LocalP1,
			RemoteP2
		};

		PlayerHealthOwner IdentifyPlayerHealthOwner(const void* component)
		{
			if (!component)
				return PlayerHealthOwner::None;

			const retail::HealthComponentRef expected = {
				retail::ToAddress(component)
			};
			retail::EntitySlotRepository players;
			retail::HandlerRef handler = {};
			retail::HealthComponentRef health_component = {};
			if (players.GetHandler(retail::EntitySlot::LocalP1, handler) &&
				retail::HandlerView(handler).HealthComponent(health_component) &&
				health_component == expected)
			{
				return PlayerHealthOwner::LocalP1;
			}
			if (players.GetHandler(retail::EntitySlot::RemoteP2, handler) &&
				retail::HandlerView(handler).HealthComponent(health_component) &&
				health_component == expected)
			{
				return PlayerHealthOwner::RemoteP2;
			}
			return PlayerHealthOwner::None;
		}

		const char* PlayerHealthOwnerName(PlayerHealthOwner owner)
		{
			switch (owner)
			{
			case PlayerHealthOwner::LocalP1:
				return "P1";
			case PlayerHealthOwner::RemoteP2:
				return "P2";
			default:
				return "unknown";
			}
		}

		bool ReadHealthComponentSlot(const void* component, std::uint32_t slot,
			float& out)
		{
			const retail::HealthComponentRef reference = {
				retail::ToAddress(component)
			};
			return retail::HealthComponentView(reference).ReadSlot(slot, out);
		}
	}

	SHORT WINAPI HookGetAsyncKeyState(int virtual_key)
	{
		return coop::CoopNetGame::Instance().HandleGetAsyncKeyState(virtual_key);
	}

	bool __fastcall HookInputActionQuery(void* input_manager, void* edx,
		std::uint32_t device, std::uint32_t action, std::uint32_t flags)
	{
		return coop::CoopNetGame::Instance().HandleInputActionQuery(
			input_manager, edx, device, action, flags,
			reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
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
			input_manager, edx, device, action, flags,
			reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
	}

	bool __fastcall CoopNetGame::HookAbrAttackPredicate(void* mode, void*)
	{
		return Instance().HandleAbrAttackPredicate(mode);
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
			input_manager, edx, device, action, flags, record,
			reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
	}

	bool __fastcall HookInputRawReleasedQuery(void* input_manager, void* edx,
		void* device, std::uint32_t action, std::uint32_t flags, bool record)
	{
		return coop::CoopNetGame::Instance().HandleInputRawReleasedQuery(
			input_manager, edx, device, action, flags, record,
			reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
	}

	bool __fastcall HookInputRawHeldQuery(void* input_manager, void* edx,
		void* device, std::uint32_t action, std::uint32_t flags, bool record)
	{
		return coop::CoopNetGame::Instance().HandleInputRawHeldQuery(
			input_manager, edx, device, action, flags, record,
			reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
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

	std::uint32_t NextNonZeroSequence(std::uint32_t& sequence)
	{
		++sequence;
		if (sequence == 0)
			++sequence;
		return sequence;
	}

	bool IsNewerSnapshotSequence(std::uint32_t candidate, std::uint32_t accepted)
	{
		// Zero is the wire sentinel before a snapshot has a live transform. Local
		// publication skips zero on wrap, so every nonzero sequence uses the
		// signed-difference rule. A zero remains acceptable only before any live
		// sequence has been accepted.
		if (candidate == 0)
			return accepted == 0;
		return accepted == 0 ||
			static_cast<std::int32_t>(candidate - accepted) > 0;
	}

	bool IsSameOrNewerNonZeroSequence(std::uint32_t candidate,
		std::uint32_t baseline)
	{
		return candidate != 0 &&
			(candidate == baseline ||
				static_cast<std::int32_t>(candidate - baseline) > 0);
	}

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
		m_remote_player_frame_transform(),
		m_remote_player_frame_entity(nullptr),
		m_remote_player_frame_sequence(0),
		m_remote_player_frame_transform_valid(false),
		m_last_invalid_input_trace_tick(0),
		m_last_remote_fly_deactivation_suppression_tick(0),
		m_peer_connected_tick(0),

		m_logged_spawn(0),
		m_remote_input_active(0),
		m_keyboard_state_swapped(false),
		m_logged_keyboard_state_swap(false),
		m_input_hooked(false),
		m_state_machine_select_state_hooked(false),
		m_action_query_hooked(false),
		m_action_up_query_hooked(false),
		m_threshold_query_hooked(false),
		m_axis_query_hooked(false),
		m_abr_attack_predicate_hooked(false),
		m_abr_attack_predicate_seen_in_scope(false),
		m_remote_abr_aim_ray_applied(false),
		m_remote_abr_aim_input_manager(),
		m_remote_abr_saved_aim_ray(),
		m_async_key_state_iat_slot(nullptr),
		m_original_get_async_key_state(nullptr),
		m_language_select_trampoline(nullptr),
		m_original_language_select(nullptr),
		m_language_select_hooked(false),
		m_logged_lang_override(false),
		m_state_machine_select_state_trampoline(nullptr),
		m_original_state_machine_select_state(nullptr),
		m_input_action_trampoline(nullptr),
		m_original_input_action_query(nullptr),
		m_input_action_up_trampoline(nullptr),
		m_original_input_action_up_query(nullptr),
		m_input_threshold_trampoline(nullptr),
		m_original_input_threshold_query(nullptr),
		m_input_axis_trampoline(nullptr),
		m_original_input_axis_query(nullptr),
		m_abr_attack_predicate_trampoline(nullptr),
		m_original_abr_attack_predicate(nullptr),
		m_logged_remote_gamepad(false),
		m_remote_gamepad_unavailable(false),
		m_remote_gamepad(nullptr),
		m_default_mode_update_trampoline(nullptr),
		m_original_default_mode_update(nullptr),
		m_fire_handler_trampoline(nullptr),
		m_original_fire_handler(nullptr),
		m_fire_handler_hooked(false),
		m_weapon_ammo_consume_trampoline(nullptr),
		m_original_weapon_ammo_consume(nullptr),
		m_weapon_ammo_consume_hooked(false),
		m_health_component_set_trampoline(nullptr),
		m_original_health_component_set(nullptr),
		m_health_component_set_hooked(false),
		m_health_component_add_trampoline(nullptr),
		m_original_health_component_add(nullptr),
		m_health_component_add_hooked(false),
		m_health_component_subtract_trampoline(nullptr),
		m_original_health_component_subtract(nullptr),
		m_health_component_subtract_hooked(false),
		m_trigger_spawn_trampoline(nullptr),

		m_original_trigger_spawn(nullptr),
		m_trigger_spawn_hooked(false),
		m_trigger_factory_trampoline(nullptr),
		m_original_trigger_factory(nullptr),
		m_trigger_factory_hooked(false),
		m_trigger_event_trampoline(nullptr),
		m_original_trigger_event(nullptr),
		m_trigger_event_hooked(false),
		m_global_event_forwarder_trampoline(nullptr),
		m_original_global_event_forwarder(nullptr),
		m_global_event_forwarder_hooked(false),
		m_object_event_relay_trampoline(nullptr),
		m_original_object_event_relay(nullptr),
		m_object_event_relay_hooked(false),
		m_object_event_forwarder_trampoline(nullptr),
		m_original_object_event_forwarder(nullptr),
		m_object_event_forwarder_hooked(false),
		m_load_game_hooked(false),
		m_native_save_load_trampoline(nullptr),
		m_original_native_save_load(nullptr),
		m_remote_input_thread_id(0),

		m_local_transform_sequence(0),
		m_local_fly_transform_sequence(0),
		m_last_accepted_remote_input_sequence(0),
		m_pending_remote_fly_zero_owner_input_sequence(0),
		m_outgoing_fly_ability_head(0),
		m_outgoing_fly_ability_count(0),
		m_incoming_fly_ability_head(0),
		m_incoming_fly_ability_count(0),
		m_local_fly_ability_sequence(0),
		m_last_remote_fly_ability_sequence(0),
		m_remote_fly_laser_pulse_active(false),
		m_remote_ledge_release_edge_armed(false),
		m_remote_ledge_release_edge_consumed(false),
		m_fly_native_pass_active(0),
		m_fly_native_synthetic_press_mask(0),
		m_fly_native_pass_thread_id(0),
		m_fly_native_pass_controller(nullptr),
		m_debug_fly_laser_pulse_active(false),
		m_local_fly_active_seen(false),
		m_local_mooch_exit_key_down(false),
		m_local_fly_deactivation_seen(false),

		m_logged_fly_active_entity_repair(false),
		m_local_weapon_sequence(0),
		m_last_local_weapon_type(0xFFFFFFFFu),
		m_logged_remote_transform(false),
		m_logged_remote_abr_transform(false)
	{
		InitializeSRWLock(&m_input_lock);
		InitializeSRWLock(&m_fly_ability_lock);
		ZeroMemory(&m_remote_input, sizeof(m_remote_input));
		ZeroMemory(&m_active_remote_input, sizeof(m_active_remote_input));
		ZeroMemory(&m_local_input, sizeof(m_local_input));
		ZeroMemory(m_outgoing_fly_abilities, sizeof(m_outgoing_fly_abilities));
		ZeroMemory(m_incoming_fly_abilities, sizeof(m_incoming_fly_abilities));
		ZeroMemory(m_remote_fly_laser_route_items,
			sizeof(m_remote_fly_laser_route_items));
		ZeroMemory(m_remote_fly_laser_item_ids,
			sizeof(m_remote_fly_laser_item_ids));
		ZeroMemory(m_debug_fly_laser_route_items,
			sizeof(m_debug_fly_laser_route_items));
		ZeroMemory(m_debug_fly_laser_item_ids,
			sizeof(m_debug_fly_laser_item_ids));
		m_saved_keyboard_state = {};
		m_saved_keyboard_state_secondary = {};
		m_active_remote_scan_codes = {};
		ZeroMemory(m_original_input_action_query_bytes,

			sizeof(m_original_input_action_query_bytes));
		ZeroMemory(m_original_input_action_up_query_bytes,
			sizeof(m_original_input_action_up_query_bytes));
		ZeroMemory(m_original_input_threshold_query_bytes,
			sizeof(m_original_input_threshold_query_bytes));
		ZeroMemory(m_original_input_axis_query_bytes,
			sizeof(m_original_input_axis_query_bytes));
		ZeroMemory(m_original_abr_attack_predicate_bytes,
			sizeof(m_original_abr_attack_predicate_bytes));
		ZeroMemory(m_original_state_machine_select_state_bytes,
			sizeof(m_original_state_machine_select_state_bytes));
		ZeroMemory(m_original_native_save_load_bytes,
			sizeof(m_original_native_save_load_bytes));

		ZeroMemory(m_prev_local_action_down, sizeof(m_prev_local_action_down));
		ZeroMemory(m_prev_remote_action_down, sizeof(m_prev_remote_action_down));
		ZeroMemory(m_prev_remote_press_seq, sizeof(m_prev_remote_press_seq));
		m_pressed_query_hooked = false;
		m_released_query_hooked = false;
		m_hold_duration_query_hooked = false;
		m_aim_hold_query_hooked = false;
		m_camera_yaw_hooked = false;
		m_gpig_camera_update_hooked = false;
		m_default_mode_update_hooked = false;
		m_input_pressed_trampoline = nullptr;
		m_original_input_pressed_query = nullptr;
		m_input_released_trampoline = nullptr;
		m_original_input_released_query = nullptr;
		m_input_hold_duration_trampoline = nullptr;
		m_original_input_hold_duration_query = nullptr;
		m_input_aim_hold_trampoline = nullptr;
		m_original_input_aim_hold_query = nullptr;
		m_camera_yaw_trampoline = nullptr;
		m_original_camera_yaw = nullptr;
		m_gpig_camera_update_trampoline = nullptr;
		m_original_gpig_camera_update = nullptr;
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
		ZeroMemory(m_original_object_event_relay_bytes,
			sizeof(m_original_object_event_relay_bytes));
		ZeroMemory(m_original_object_event_forwarder_bytes,
			sizeof(m_original_object_event_forwarder_bytes));
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
		ZeroMemory(m_input_edge_trace_slots, sizeof(m_input_edge_trace_slots));
		ZeroMemory(m_object_diagnostic_trace_slots,
			sizeof(m_object_diagnostic_trace_slots));
		m_logged_remote_p2_ammo_restore = false;
		m_remote_p2_weapon_record = nullptr;
		m_input_raw_pressed_trampoline = nullptr;
		m_original_input_raw_pressed_query = nullptr;
		m_input_raw_released_trampoline = nullptr;
		m_original_input_raw_released_query = nullptr;
		m_input_raw_held_trampoline = nullptr;
		m_original_input_raw_held_query = nullptr;
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
		AcquireSRWLockExclusive(&m_input_lock);
		m_last_invalid_input_trace_tick = 0;
		m_last_remote_fly_deactivation_suppression_tick = 0;
		m_last_accepted_remote_input_sequence = 0;
		m_pending_remote_fly_zero_owner_input_sequence = 0;
		ReleaseSRWLockExclusive(&m_input_lock);
		ClearFlyAbilityQueues();
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
		m_last_accepted_remote_input_sequence = 0;
		m_pending_remote_fly_zero_owner_input_sequence = 0;
		m_local_fly_deactivation_seen = false;
		m_last_invalid_input_trace_tick = 0;
		m_last_remote_fly_deactivation_suppression_tick = 0;
		ReleaseSRWLockExclusive(&m_input_lock);
		ClearFlyAbilityQueues();
		m_last_remote_transform_apply_tick = 0;
		ChatOverlay::Instance().OnPeerDisconnected();
		WorldSync::Instance().OnPeerDisconnected();
		CoopRuntime::Instance().Log("[netgame] remote peer disconnected\r\n");
	}

	void CoopNetGame::ClearFlyAbilityQueues()
	{
		AcquireSRWLockExclusive(&m_fly_ability_lock);
		ZeroMemory(m_outgoing_fly_abilities, sizeof(m_outgoing_fly_abilities));
		ZeroMemory(m_incoming_fly_abilities, sizeof(m_incoming_fly_abilities));
		m_outgoing_fly_ability_head = 0;
		m_outgoing_fly_ability_count = 0;
		m_incoming_fly_ability_head = 0;
		m_incoming_fly_ability_count = 0;
		m_local_fly_ability_sequence = 0;
		m_last_remote_fly_ability_sequence = 0;
		ReleaseSRWLockExclusive(&m_fly_ability_lock);
	}

	void CoopNetGame::ClearIncomingFlyAbilityEvents()
	{
		AcquireSRWLockExclusive(&m_fly_ability_lock);
		ZeroMemory(m_incoming_fly_abilities, sizeof(m_incoming_fly_abilities));
		m_incoming_fly_ability_head = 0;
		m_incoming_fly_ability_count = 0;
		// Keep the last accepted reliable sequence across a 1 -> 0 Fly ownership
		// edge. Otherwise a delayed old laser packet could be accepted after the
		// next remote Fly entry. A peer reconnect/world reset uses
		// ClearFlyAbilityQueues and deliberately resets the sequence instead.
		ReleaseSRWLockExclusive(&m_fly_ability_lock);
	}

	void CoopNetGame::OnRemoteFlyAbilityPacket(const void* data,
		std::uint32_t size)
	{
		const protocol::PacketView view(data, size);
		protocol::FlyAbilityPacket packet = {};
		if (view.Kind() != protocol::PacketKind::FlyAbility ||
			!view.CopyUncompressedExact(packet) ||
			packet.sequence == 0 ||
			packet.ability != protocol::FlyAbility::DualLaser ||
			!IsFiniteFloatArray(packet.laser_target, 3))
		{
			CoopRuntime::Instance().Log(
				"[fly-laser] rejected malformed/invalid ability packet size=%u\r\n",
				size);
			return;
		}

		bool queued = false;
		bool overflow = false;
		AcquireSRWLockExclusive(&m_fly_ability_lock);
		if (IsNewerSnapshotSequence(packet.sequence,
			m_last_remote_fly_ability_sequence))
		{
			if (m_incoming_fly_ability_count < kFlyAbilityQueueCapacity)
			{
				const std::uint32_t index =
					(m_incoming_fly_ability_head +
						m_incoming_fly_ability_count) %
					kFlyAbilityQueueCapacity;
				m_incoming_fly_abilities[index].packet = packet;
				m_incoming_fly_abilities[index].received_tick = GetTickCount();
				++m_incoming_fly_ability_count;
				m_last_remote_fly_ability_sequence = packet.sequence;
				queued = true;
			}
			else
			{
				overflow = true;
			}
		}
		ReleaseSRWLockExclusive(&m_fly_ability_lock);

		if (queued)
		{
			CoopRuntime::Instance().Log(
				"[fly-laser] queued reliable remote event=%u fly_seq=%u\r\n",
				packet.sequence,
				packet.source_fly_transform_sequence);
		}
		else if (overflow)
		{
			CoopRuntime::Instance().Log(
				"[fly-laser] incoming queue full; dropped remote event=%u\r\n",
				packet.sequence);
		}
	}

	void CoopNetGame::OnRemotePacket(
		const void* data, std::uint32_t size)
	{
		const protocol::PacketView view(data, size);
		CoopInputPacket packet = {};
		if (view.Kind() != protocol::PacketKind::Input ||
			!view.CopyUncompressedExact(packet))
		{
			return;
		}
		const char* invalid_reason = nullptr;
		if (!IsSafeInputSnapshot(packet.input, invalid_reason))
		{
			bool trace = false;
			const DWORD now = GetTickCount();
			AcquireSRWLockExclusive(&m_input_lock);
			if (m_last_invalid_input_trace_tick == 0 ||
				static_cast<DWORD>(now - m_last_invalid_input_trace_tick) >=
					kInvalidInputTraceIntervalMs)
			{
				m_last_invalid_input_trace_tick = now;
				trace = true;
			}
			ReleaseSRWLockExclusive(&m_input_lock);
			if (trace)
			{
				CoopRuntime::Instance().Log(
					"[net-input-reject] non-finite %s in snapshot seq=%u; retained last valid state\r\n",
					invalid_reason ? invalid_reason : "value",
					packet.input.transform_sequence);
			}
			return;
		}

		// This is a state buffer, not a one-frame input event.  It deliberately
		// remains valid until a newer packet replaces it, so held keys survive
		// packet pacing and an out-of-order unreliable packet cannot roll P2 or
		// Mooch back to an older transform/rotation.
		bool client_yielded_fly = false;
		bool remote_fly_ownership_changed = false;
		bool remote_fly_owned_before = false;
		bool remote_fly_owned_after = false;
		bool remote_fly_raw_owned_before = false;
		bool remote_fly_zero_owner_queued = false;
		std::uint32_t remote_fly_sequence_after = 0;
		std::uint32_t input_sequence_after = 0;
		bool packet_accepted = false;
		AcquireSRWLockExclusive(&m_input_lock);
		if (IsNewerSnapshotSequence(packet.input.transform_sequence,
			m_last_accepted_remote_input_sequence))
		{
			// Keep the raw ownership edge separate from the presentation predicate:
			// fly_controlled may briefly be set before the first transform is
			// published, while a real exit intentionally clears that transform
			// sequence.  The ordered raw 1 -> 0 edge is the lifecycle authority.
			remote_fly_raw_owned_before = m_remote_input.fly_controlled != 0;
			remote_fly_owned_before = m_remote_input.fly_controlled != 0 &&
				m_remote_input.fly_transform_sequence != 0;
			m_last_accepted_remote_input_sequence = packet.input.transform_sequence;
			const bool remote_fly_controlled = packet.input.fly_controlled != 0 &&
				packet.input.fly_transform_sequence != 0;
			// The whole input snapshot is sequenced, including fly_controlled. A
			// receiver-local Fly_Deactivated transition is not authority to rewrite
			// this packet: the owner publishes an ordered zero-owner snapshot on its
			// own exit, and older live snapshots are rejected above.
			m_remote_input = packet.input;
			if (m_remote_input.fly_controlled != 0)
			{
				// A newer peer ownership claim supersedes an exit that the game
				// thread has not consumed yet.
				m_pending_remote_fly_zero_owner_input_sequence = 0;
			}
			else if (remote_fly_raw_owned_before)
			{
				m_pending_remote_fly_zero_owner_input_sequence =
					m_remote_input.transform_sequence;
				remote_fly_zero_owner_queued = true;
			}
			// Mooch is a single world object.  If both peers obtain the local hand-off
			// in the same network window, the client yields to the host.  In all normal
			// cases only one side has fly_controlled set, so this branch is untouched.
			if (remote_fly_controlled && m_local_input.fly_controlled != 0 &&
				IsClient())
			{
				ClearLocalFlyOwnershipLocked();
				m_local_fly_active_seen = false;
				client_yielded_fly = true;
			}
			packet_accepted = true;
			remote_fly_owned_after = m_remote_input.fly_controlled != 0 &&
				m_remote_input.fly_transform_sequence != 0;
			remote_fly_ownership_changed = remote_fly_owned_before !=
				remote_fly_owned_after;
			remote_fly_sequence_after = m_remote_input.fly_transform_sequence;
			input_sequence_after = m_remote_input.transform_sequence;
		}
		ReleaseSRWLockExclusive(&m_input_lock);
		if (!packet_accepted)
			return;
		if (remote_fly_owned_before && !remote_fly_owned_after)
			ClearIncomingFlyAbilityEvents();
		if (client_yielded_fly)
		{
			CoopRuntime::Instance().Log(
				"[fly] simultaneous claim: client yielded Mooch to host\r\n");
		}
		if (remote_fly_ownership_changed)
		{
			CoopRuntime::Instance().Log(
				"[fly-packet] remote ownership %u -> %u input_seq=%u fly_seq=%u\r\n",
				remote_fly_owned_before ? 1u : 0u,
				remote_fly_owned_after ? 1u : 0u, input_sequence_after,
				remote_fly_sequence_after);
		}
		if (remote_fly_zero_owner_queued)
		{
			CoopRuntime::Instance().Log(
				"[fly-lifecycle] queued ordered remote zero-owner input_seq=%u; awaiting game-thread native transition\r\n",
				input_sequence_after);
		}
	}

	void CoopNetGame::ClearFlyInputLocked(CoopInput& input)
	{
		ZeroMemory(input.fly_position, sizeof(input.fly_position));
		ZeroMemory(input.fly_rotation, sizeof(input.fly_rotation));
		input.fly_transform_sequence = 0;
		input.fly_controlled = 0;
		input.fly_debug_fire_sequence = 0;
		input.fly_raw_down = 0;
		ZeroMemory(input.fly_raw_press_seq, sizeof(input.fly_raw_press_seq));
		ZeroMemory(input.fly_raw_release_seq, sizeof(input.fly_raw_release_seq));
	}

	void CoopNetGame::ClearLocalFlyOwnershipLocked()
	{
		ClearFlyInputLocked(m_local_input);
		m_local_input.transform_sequence =
			NextNonZeroSequence(m_local_transform_sequence);
	}

	bool CoopNetGame::IsGameForeground() const
	{
		if (ChatOverlay::Instance().IsInputActive())
			return false;
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

	void CoopNetGame::CaptureLocalAnalogAxis(std::uint32_t axis, float value)
	{
		if (axis >= kCoopInputAnalogAxisCount || !IsFiniteFloat(value))
			return;
		AcquireSRWLockExclusive(&m_input_lock);
		m_local_input.analog_axis[axis] = value;
		ReleaseSRWLockExclusive(&m_input_lock);
	}

	void CoopNetGame::CaptureLocalAimRay(const void* input_manager)
	{
		retail::AimRay aim_ray = {};
		if (!ReadValidAimRay(input_manager, aim_ray))
			return;

		AcquireSRWLockExclusive(&m_input_lock);
		memcpy(m_local_input.aim_origin, &aim_ray.origin,
			sizeof(aim_ray.origin));
		memcpy(m_local_input.aim_direction, &aim_ray.direction,
			sizeof(aim_ray.direction));
		ReleaseSRWLockExclusive(&m_input_lock);
	}

	void* CoopNetGame::GetRemoteGamePad()
	{
		if (m_remote_gamepad)
			return m_remote_gamepad;
		if (m_remote_gamepad_unavailable)
			return nullptr;

		// 0x487F10 is deliberately not called. It registers its argument by writing
		// 0x9905CC, the process-global P1 camera/input bridge. Default mode already
		// accepts an XGamePad argument, so P2 needs a constructed object only.
		void* const memory = VirtualAlloc(nullptr, kXGamePadSize,
			MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!memory)
		{
			m_remote_gamepad_unavailable = true;
			CoopRuntime::Instance().Log(
				"[p2-gamepad] allocation failed; using P1 pad fallback\r\n");
			return nullptr;
		}

		retail::GamePadRef gamepad = {};
		if (!retail::NativeGameApi::ConstructGamePad(memory, gamepad) ||
			!gamepad)
		{
			VirtualFree(memory, 0, MEM_RELEASE);
			m_remote_gamepad_unavailable = true;
			CoopRuntime::Instance().Log(
				"[p2-gamepad] constructor fault; using P1 pad fallback\r\n");
			return nullptr;
		}

		m_remote_gamepad = retail::ToPointer(gamepad.value);
		if (!m_logged_remote_gamepad)
		{
			retail::GamePadRef primary_gamepad = {};
			retail::PrimaryGamePadStore primary_gamepad_store;
			const bool primary_gamepad_read =
				primary_gamepad_store.Read(primary_gamepad);
			CoopRuntime::Instance().Log(
				"[p2-gamepad] private XGamePad=%p created; P1 global pad remains=%p\r\n",
				m_remote_gamepad,
				primary_gamepad_read ? retail::ToPointer(primary_gamepad.value) : nullptr);
			m_logged_remote_gamepad = true;
		}
		return m_remote_gamepad;
	}

	bool CoopNetGame::BeginRemoteGamePadScope(void*& primary_gamepad)
	{
		primary_gamepad = nullptr;
		void* const remote_gamepad = GetRemoteGamePad();
		if (!remote_gamepad)
			return false;

		const retail::GamePadRef remote_gamepad_ref = {
			retail::ToAddress(remote_gamepad)
		};
		retail::GamePadRef previous_gamepad = {};
		if (!retail::PrimaryGamePadStore().Replace(remote_gamepad_ref,
			previous_gamepad))
		{
			CoopRuntime::Instance().Log(
				"[p2-gamepad] unable to scope the primary XGamePad\r\n");
			return false;
		}
		primary_gamepad = retail::ToPointer(previous_gamepad.value);
		return true;
	}

	void CoopNetGame::EndRemoteGamePadScope(void* primary_gamepad)
	{
		const retail::GamePadRef previous_gamepad = {
			retail::ToAddress(primary_gamepad)
		};
		if (!retail::PrimaryGamePadStore().Restore(previous_gamepad))
		{
			CoopRuntime::Instance().Log(
				"[p2-gamepad] unable to restore the primary XGamePad\r\n");
		}
	}

	bool CoopNetGame::ApplyActiveRemoteAimRay(void* input_manager,
		retail::AimRay& saved_ray) const
	{
		if (!input_manager || m_active_remote_input.transform_sequence == 0)
		{
			return false;
		}

		const float* const remote_direction = m_active_remote_input.aim_direction;
		const float length_squared = remote_direction[0] * remote_direction[0] +
			remote_direction[1] * remote_direction[1] +
			remote_direction[2] * remote_direction[2];
		if (!(length_squared > 0.25f && length_squared < 4.0f))
			return false;

		retail::AimRay remote_ray = {};
		memcpy(&remote_ray.origin, m_active_remote_input.aim_origin,
			sizeof(remote_ray.origin));
		memcpy(&remote_ray.direction, m_active_remote_input.aim_direction,
			sizeof(remote_ray.direction));

		retail::InputManagerRef input_manager_ref = {};
		input_manager_ref.value = retail::ToAddress(input_manager);
		retail::InputManagerView input_manager_view(input_manager_ref);
		return input_manager_view.ReadAimRay(saved_ray) &&
			input_manager_view.WriteAimRay(remote_ray);
	}

	void CoopNetGame::RestoreAimRay(void* input_manager,
		const retail::AimRay& saved_ray) const
	{
		if (!input_manager)
			return;

		retail::InputManagerRef input_manager_ref = {};
		input_manager_ref.value = retail::ToAddress(input_manager);
		retail::InputManagerView(input_manager_ref).WriteAimRay(saved_ray);
	}

	void __fastcall CoopNetGame::HookDefaultModeUpdate(void* mode, void*,
		void* input_manager, void* mode_context)
	{
		Instance().HandleDefaultModeUpdate(mode, input_manager, mode_context);
	}

	bool __fastcall CoopNetGame::HookStateMachineSelectState(void* controller,
		void*, std::uint32_t mode_id, bool force_reselect)
	{
		return Instance().HandleStateMachineSelectState(controller, mode_id,
			force_reselect);
	}

	void CoopNetGame::HandleDefaultModeUpdate(void* mode, void* input_manager,
		void* mode_context)
	{
		if (!m_original_default_mode_update)
			return;

		// The normal Default-mode caller takes the globally registered P1 pad and
		// passes it here. P2's networked keyboard actions are supplied by the
		// packet-backed query hooks, but its stock motor also makes raw XGamePad
		// reads that a freshly constructed private pad cannot answer. Keep the
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

		// Capture P1's stock ray for the packet. P2's ray is substituted only at
		// the exact fire-handler call, then immediately restored.
		if (!IsRemoteInputActiveOnThisThread())
			CaptureLocalAimRay(mode_input);

	}

	bool CoopNetGame::HandleStateMachineSelectState(void* controller,
		std::uint32_t mode_id, bool force_reselect)
	{
		if (!m_original_state_machine_select_state)
			return false;

		// Fly_Active::Update may ask the controller dispatcher for a transient
		// Respawn/Orbit transition while the ability is being replayed. The native
		// update is running as a shadow pass: let it prepare the laser, but do not
		// let that one pass change the controller's real mode or camera ownership.
		if (IsFlyNativeAbilityPassActiveForController(controller))
			return true;

		// Fly_Deactivated::Enter resets visual and task state before the old
		// receiver-side transform patch can run. While a peer is still the sole
		// owner, that local transition has no authority and would hide/desynchronise
		// a living remote Mooch. Returning the dispatcher's normal success value
		// preserves the current presentation mode without entering Deactivated.
		if (mode_id == kFlyDeactivatedModeId &&
			IsRemotePresentationMoochController(controller))
		{
			const DWORD now = GetTickCount();
			if (m_last_remote_fly_deactivation_suppression_tick == 0 ||
				static_cast<DWORD>(now -
					m_last_remote_fly_deactivation_suppression_tick) >= 1000)
			{
				m_last_remote_fly_deactivation_suppression_tick = now;
				CoopRuntime::Instance().Log(
					"[fly-lifecycle] suppressed receiver Fly_Deactivated controller=%p force=%u; remote owner remains authoritative\r\n",
					controller,
					force_reselect ? 1u : 0u);
			}
			return true;
		}

		const bool selected = m_original_state_machine_select_state(controller,
			mode_id, force_reselect);
		if (selected)
		{
			const retail::StateMachineRef state_machine_ref = {
				retail::ToAddress(controller)
			};
			if (mode_id == gforce::kGPigCombatRdvFireModeId &&
				retail::StateMachineView(state_machine_ref).
					ContainsRegisteredVTable(gforce::kGPigCombatRdvFireVtable))
			{
				// This is the confirmed ABR combat route, not the shared numeric
				// Fire state used by other GPig combat machines. Remote replay reaches
				// it through the fire-only input scope of the P2-owned ABR controller.
				CoopRuntime::Instance().Log(
					"[abr-fire] native GPigCombatRDV Fire selected machine=%p force=%u\r\n",
					controller, force_reselect ? 1u : 0u);
			}
			// The dispatcher also owns nested motors. Player2Module identifies the
			// machine from its registry before observing it, so this cannot turn a
			// reused numeric mode ID from another class into a Ledge transition.
			Player2Module::Instance().ObserveInnerStateSelection(controller, mode_id);
		}
		return selected;
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

		const std::uint32_t fire_action_index =
			kFireActionId - kFirstKeyboardActionId;
		const bool remote_input_active = IsRemoteInputActiveOnThisThread();
		const bool remote_fire_edge = remote_input_active &&
			(m_remote_press_edge[fire_action_index] ||
				m_remote_release_edge[fire_action_index]);
		retail::AimRay saved_ray = {};
		const bool remote_ray_applied = remote_input_active &&
			ApplyActiveRemoteAimRay(input_manager, saved_ray);
		if (remote_fire_edge)
		{
			CoopRuntime::Instance().Log(
				"[remote-fire-native] mode=%p press=%u release=%u ray=%u transform=%u player=(%.2f,%.2f,%.2f) aim=(%.3f,%.3f,%.3f)\r\n",
				mode, m_remote_press_edge[fire_action_index] ? 1u : 0u,
				m_remote_release_edge[fire_action_index] ? 1u : 0u,
				remote_ray_applied ? 1u : 0u,
				m_active_remote_input.transform_sequence,
				m_active_remote_input.position[0], m_active_remote_input.position[1],
				m_active_remote_input.position[2],
				m_active_remote_input.aim_direction[0],
				m_active_remote_input.aim_direction[1],
				m_active_remote_input.aim_direction[2]);
		}
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

	bool CoopNetGame::PollLocalAbrPropulsionDirection(float& direction)
	{
		direction = 0.0f;
		if (!m_original_input_axis_query || !IsGameForeground())
			return false;

		retail::GamePadRef input_manager = {};
		retail::EntitySlotRepository players;
		const retail::EntityRef player1 = players.GetSelectable(
			retail::EntitySlot::LocalP1);
		std::uint32_t device = 0;
		if (!player1 ||
			!retail::PrimaryGamePadStore().Read(input_manager) || !input_manager ||
			!retail::GamePointerStore().InputDevice(device))
		{
			return false;
		}

		__try
		{
			// ABR's 0x10000000 query is a one-shot vehicle action, not the
			// continuous WASD state (Space happened to satisfy it in the live test).
			// The native RDV update consumes logical movement axes 0/1, so use those
			// same binding-aware values to choose propulsion. Retail turns axis 1
			// into its forward component as -axis_y: W/up is negative, while S/down
			// is positive. Pure lateral input retains the game's forward propulsion.
			const float axis_x = m_original_input_axis_query(
				retail::ToPointer(input_manager.value), device, 0u, 1u);
			const float axis_y = m_original_input_axis_query(
				retail::ToPointer(input_manager.value), device, 1u, 1u);
			if (!_finite(axis_x) || !_finite(axis_y))
				return false;
			CaptureLocalAnalogAxis(0u, axis_x);
			CaptureLocalAnalogAxis(1u, axis_y);
			if (axis_y < -0.05f)
				direction = 1.0f;
			else if (axis_y > 0.05f)
				direction = -1.0f;
			else if (fabsf(axis_x) > 0.05f)
				direction = 1.0f;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
		return true;
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
		if (action_index == kMoochActionIndex && is_down && !was_down)
			ConsumeLocalMoochFlyExit();
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
		if (action_index == kMoochActionIndex)
			ConsumeLocalMoochFlyExit();
		AcquireSRWLockExclusive(&m_input_lock);
		if ((m_local_press_recorded[word] & (1u << bit)) == 0)
		{
			m_local_press_recorded[word] |= 1u << bit;
			m_local_input.action_press_seq[action_index]++;
			m_local_input.action_down[word] |= 1u << bit;
		}
		ReleaseSRWLockExclusive(&m_input_lock);
		// Correlation marker for saberizer activation sync: a local fire press
		// while P1 holds the saberizer. Whatever the beam hits (or wakes)
		// must be traceable in the log right after this line.
		if (action == kFireActionId)
		{
			retail::EntitySlotRepository players;
			retail::EntitySlotBinding local = {};
			std::uint32_t selected_weapon = 0xFFFFFFFFu;
			if (players.GetBinding(retail::EntitySlot::LocalP1, local) &&
				local.handler &&
				retail::HandlerView(local.handler).SelectedWeaponType(
					selected_weapon) &&
				selected_weapon == gforce::kSaberizerWeaponType)
			{
				retail::Transform player_transform = {};
				const bool have_pos =
					retail::EntityView(local.entity).ReadTransform(
						player_transform) &&
					IsFiniteRetailTransform(player_transform);
				CoopRuntime::Instance().Log(
					have_pos ?
					"[saberizer] local P1 fired p1_pos=(%.2f,%.2f,%.2f)\r\n" :
					"[saberizer] local P1 fired p1_pos=unavailable\r\n",
					player_transform.position.x, player_transform.position.y,
					player_transform.position.z);
			}
		}
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

	bool CoopNetGame::ClaimLocalInputEdgeTrace(std::uint32_t action,
		std::uintptr_t caller_return_address, bool raw)
	{
		// Some retail raw "pressed" queries are level-like for several frames.
		// This is trace-only state: suppress repeats from one consumer for a short
		// interval without touching the native result or co-op input sequencing.
		const DWORD now = GetTickCount();
		int replacement_index = 0;
		DWORD oldest_tick = MAXDWORD;
		AcquireSRWLockExclusive(&m_input_lock);
		for (int index = 0; index < kInputEdgeTraceSlotCount; ++index)
		{
			InputEdgeTraceSlot& slot = m_input_edge_trace_slots[index];
			if (slot.action == action &&
				slot.caller_return_address == caller_return_address && slot.raw == raw)
			{
				if (static_cast<DWORD>(now - slot.tick) < 1000u)
				{
					ReleaseSRWLockExclusive(&m_input_lock);
					return false;
				}
				slot.tick = now;
				ReleaseSRWLockExclusive(&m_input_lock);
				return true;
			}
			if (slot.tick < oldest_tick)
			{
				oldest_tick = slot.tick;
				replacement_index = index;
			}
		}

		InputEdgeTraceSlot& replacement = m_input_edge_trace_slots[replacement_index];
		replacement.action = action;
		replacement.caller_return_address = caller_return_address;
		replacement.tick = now;
		replacement.raw = raw;
		ReleaseSRWLockExclusive(&m_input_lock);
		return true;
	}

	bool CoopNetGame::ClaimObjectDiagnosticTrace(std::uint32_t route,
		std::uint32_t event_code, std::uintptr_t caller_return_address,
		std::uintptr_t object_vtable)
	{
		// Candidate object paths can be level-polled.  This keeps an actual hit
		// visible while preserving the native call and preventing log flooding.
		const DWORD now = GetTickCount();
		int replacement_index = 0;
		DWORD oldest_tick = MAXDWORD;
		AcquireSRWLockExclusive(&m_input_lock);
		for (int index = 0; index < kObjectDiagnosticTraceSlotCount; ++index)
		{
			ObjectDiagnosticTraceSlot& slot =
				m_object_diagnostic_trace_slots[index];
			if (slot.route == route && slot.event_code == event_code &&
				slot.caller_return_address == caller_return_address &&
				slot.object_vtable == object_vtable)
			{
				if (static_cast<DWORD>(now - slot.tick) < 1000u)
				{
					ReleaseSRWLockExclusive(&m_input_lock);
					return false;
				}
				slot.tick = now;
				ReleaseSRWLockExclusive(&m_input_lock);
				return true;
			}
			if (slot.tick < oldest_tick)
			{
				oldest_tick = slot.tick;
				replacement_index = index;
			}
		}

		ObjectDiagnosticTraceSlot& replacement =
			m_object_diagnostic_trace_slots[replacement_index];
		replacement.route = route;
		replacement.event_code = event_code;
		replacement.caller_return_address = caller_return_address;
		replacement.object_vtable = object_vtable;
		replacement.tick = now;
		ReleaseSRWLockExclusive(&m_input_lock);
		return true;
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

	bool CoopNetGame::HandleAbrAttackPredicate(void* mode)
	{
		if (!m_original_abr_attack_predicate)
			return false;
		const bool stock_result = m_original_abr_attack_predicate(mode);
		if (!mode || !IsRemoteInputActiveOnThisThread() ||
			!HasActiveRemotePressedEdge(kAbrFireActionId) ||
			m_active_remote_input.player_mode != kAbrModeId)
		{
			return stock_result;
		}

		const retail::ModeRef mode_ref = { retail::ToAddress(mode) };
		retail::ControllerRef controller = {};
		retail::EntitySlotRepository players;
		retail::EntitySlotBinding remote_p2 = {};
		retail::ModeRef registered_abr_mode = {};
		if (!retail::ModeView(mode_ref).Controller(controller) ||
			!players.GetBinding(retail::EntitySlot::RemoteP2, remote_p2) ||
			controller != remote_p2.controller ||
			!retail::ControllerView(controller).RegisteredMode(
				kAbrModeId, registered_abr_mode) ||
			mode_ref != registered_abr_mode)
		{
			return stock_result;
		}
		// Count the predicate only after proving that this is RemoteP2's exact
		// registered ABR mode. The hook can be reached by unrelated controller
		// work; treating any invocation as success used to suppress the fallback
		// while producing no projectile at all.
		m_abr_attack_predicate_seen_in_scope = true;
		if (stock_result)
			return true;

		// The peer packet proves a fresh ABR edge and this exact registered mode
		// owns RemoteP2. This does not require the outer controller's current mode:
		// receiver-side ABR presentation deliberately avoids taking camera/HUD
		// ownership from P1.
		CoopRuntime::Instance().Log(
			"[abr-fire] allowing RemoteP2 ABR attack block after native predicate rejected edge\r\n");
		return true;
	}

	bool CoopNetGame::GetActiveRemotePlayerTransform(
		retail::Transform& transform, std::uint32_t& transform_sequence,
		std::uint32_t& player_mode) const
	{
		transform = {};
		transform_sequence = 0;
		player_mode = 0;
		if (!IsRemoteInputActiveOnThisThread() ||
			m_active_remote_input.transform_sequence == 0 ||
			!DecodeFiniteWireTransform(m_active_remote_input.position,
				m_active_remote_input.rotation, transform))
		{
			return false;
		}

		transform_sequence = m_active_remote_input.transform_sequence;
		player_mode = m_active_remote_input.player_mode;
		return true;
	}

	bool CoopNetGame::HasActiveRemotePressedEdge(std::uint32_t action) const
	{
		if (!IsRemoteInputActiveOnThisThread() ||
			action < kFirstKeyboardActionId ||
			action >= kFirstKeyboardActionId + kKeyboardActionCount)
		{
			return false;
		}
		const std::uint32_t action_index = action - kFirstKeyboardActionId;
		return m_remote_press_edge[action_index];
	}

	bool CoopNetGame::ArmRemoteLedgeReleaseEdge()
	{
		if (!IsRemoteInputActiveOnThisThread() ||
			m_active_remote_input.transform_sequence == 0 ||
			m_remote_ledge_release_edge_armed)
		{
			return false;
		}
		m_remote_ledge_release_edge_armed = true;
		m_remote_ledge_release_edge_consumed = false;
		return true;
	}

	bool CoopNetGame::FinishRemoteLedgeReleaseEdge(bool& consumed)
	{
		consumed = m_remote_ledge_release_edge_consumed;
		const bool was_armed = m_remote_ledge_release_edge_armed;
		m_remote_ledge_release_edge_armed = false;
		m_remote_ledge_release_edge_consumed = false;
		return was_armed;
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
			ClearLocalFlyOwnershipLocked();
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

	bool CoopNetGame::ConsumeRemoteFlyZeroOwnerTransition(
		std::uint32_t& input_sequence)
	{
		input_sequence = 0;
		AcquireSRWLockExclusive(&m_input_lock);
		if (m_pending_remote_fly_zero_owner_input_sequence == 0)
		{
			ReleaseSRWLockExclusive(&m_input_lock);
			return false;
		}

		// Do not let an exit received in an earlier network window fire after
		// either peer owns Mooch again.  The newer live snapshot or local hand-off
		// wins; neither needs a fabricated receiver-side death.
		if (m_local_input.fly_controlled != 0 || m_remote_input.fly_controlled != 0)
		{
			m_pending_remote_fly_zero_owner_input_sequence = 0;
			ReleaseSRWLockExclusive(&m_input_lock);
			return false;
		}

		input_sequence = m_pending_remote_fly_zero_owner_input_sequence;
		m_pending_remote_fly_zero_owner_input_sequence = 0;
		ReleaseSRWLockExclusive(&m_input_lock);
		return true;
	}

	bool CoopNetGame::IsRemoteFlyControlledForInputQuery() const
	{
		if (IsRemoteInputActiveOnThisThread())
		{
			return m_active_remote_input.fly_controlled != 0 &&
				m_active_remote_input.fly_transform_sequence != 0;
		}
		return IsRemoteFlyControlled();
	}

	bool CoopNetGame::IsRemotePresentationMoochController(void* controller) const
	{
		if (!controller || IsLocalFlyControlled() || !IsRemoteFlyControlled())
			return false;

		retail::EntitySlotRepository players;
		retail::ControllerRef fly_controller = {};
		return players.GetController(retail::EntitySlot::Mooch, fly_controller) &&
			retail::ToPointer(fly_controller.value) == controller;
	}

	bool CoopNetGame::GetRemoteFlyRawHeld(std::uint32_t action) const
	{
		const int raw_index = FindFlyRawActionIndex(action);
		if (raw_index < 0)
			return false;
		if (IsRemoteInputActiveOnThisThread())
		{
			return m_active_remote_input.fly_controlled != 0 &&
				m_active_remote_input.fly_transform_sequence != 0 &&
				(m_active_remote_input.fly_raw_down &
					(1u << static_cast<std::uint32_t>(raw_index))) != 0;
		}
		CoopInput remote = {};
		return IsRemoteFlyControlled() && GetRemoteInput(remote) &&
			remote.fly_controlled != 0 &&
			remote.fly_transform_sequence != 0 &&
			(remote.fly_raw_down & (1u << static_cast<std::uint32_t>(raw_index))) != 0;
	}

	bool CoopNetGame::ConsumeRemoteFlyRawEdge(std::uint32_t action, bool pressed)
	{
		const int raw_index = FindFlyRawActionIndex(action);
		if (raw_index < 0)
			return false;

		const bool scoped_remote_input = IsRemoteInputActiveOnThisThread();
		AcquireSRWLockExclusive(&m_input_lock);
		const CoopInput& remote = scoped_remote_input ? m_active_remote_input :
			m_remote_input;
		const bool remote_controls_fly = remote.fly_controlled != 0 &&
			remote.fly_transform_sequence != 0;
		std::uint8_t* previous = pressed ? m_prev_remote_fly_raw_press_seq :
			m_prev_remote_fly_raw_release_seq;
		const std::uint8_t current = pressed ?
			remote.fly_raw_press_seq[raw_index] :
			remote.fly_raw_release_seq[raw_index];
		const bool edge = remote_controls_fly && previous[raw_index] != current;
		previous[raw_index] = current;
		ReleaseSRWLockExclusive(&m_input_lock);
		return edge;
	}

	bool CoopNetGame::ReadFlyControlActiveState(void* fly, bool& active) const
	{
		active = false;
		if (!fly)
			return false;

		const retail::EntityRef fly_ref = { retail::ToAddress(fly) };
		retail::HandlerRef handler_ref = {};
		std::uint32_t state_index = 0;
		return retail::EntityView(fly_ref).Handler(handler_ref) &&
			retail::ReadFlyActiveStateIndex(state_index) &&
			retail::HandlerView(handler_ref).FlyControlActive(state_index,
				active);
	}

	bool CoopNetGame::IsFlyNativeAbilityPassActiveOnThisThread() const
	{
		return InterlockedCompareExchange(
			const_cast<volatile LONG*>(&m_fly_native_pass_active), 0, 0) != 0 &&
			m_fly_native_pass_thread_id == GetCurrentThreadId();
	}

	bool CoopNetGame::IsFlyNativeAbilityPassActiveForController(
		void* controller) const
	{
		return controller && IsFlyNativeAbilityPassActiveOnThisThread() &&
			m_fly_native_pass_controller == controller;
	}

	bool CoopNetGame::RememberFlyDualLaserPulse(void* fly, bool remote_owner)
	{
		retail::FlyDualLaserRouteItemRef route_items[
			kFlyDualLaserRouteItemCount] = {};
		std::uint32_t item_ids[kFlyDualLaserRouteItemCount] = {};
		if (!ResolveFlyDualLaserRouteItems(fly, route_items, item_ids))
			return false;

		for (std::size_t index = 0; index < kFlyDualLaserRouteItemCount;
			++index)
		{
			bool active = false;
			if (!retail::FlyDualLaserRouteItemView(route_items[index]).
				EffectActive(active) || !active)
			{
				return false;
			}
		}

		retail::FlyDualLaserRouteItemRef* saved_items = remote_owner ?
			m_remote_fly_laser_route_items : m_debug_fly_laser_route_items;
		std::uint32_t* saved_ids = remote_owner ?
			m_remote_fly_laser_item_ids : m_debug_fly_laser_item_ids;
		bool* saved_active = remote_owner ?
			&m_remote_fly_laser_pulse_active :
			&m_debug_fly_laser_pulse_active;
		for (std::size_t index = 0; index < kFlyDualLaserRouteItemCount;
			++index)
		{
			saved_items[index] = route_items[index];
			saved_ids[index] = item_ids[index];
		}
		*saved_active = true;
		return true;
	}

	bool CoopNetGame::RunFlyNativeDualLaserPass(void* fly,
		const float target[3], bool remote_owner)
	{
		if (!fly || !target || !IsFiniteFloatArray(target, 3) ||
			!m_state_machine_select_state_hooked)
		{
			return false;
		}

		const retail::EntityRef fly_ref = { retail::ToAddress(fly) };
		retail::HandlerRef handler_ref = {};
		retail::ControllerRef controller_ref = {};
		if (!retail::EntityView(fly_ref).Handler(handler_ref) ||
			!retail::HandlerView(handler_ref).Controller(controller_ref))
		{
			return false;
		}

		const retail::ControllerView controller_view(controller_ref);
		retail::ModeRef active_mode = {};
		if (!controller_view.RegisteredMode(gforce::kFlyActiveModeId,
			active_mode))
		{
			CoopRuntime::Instance().Log(
				"[fly-native] Fly_Active is not registered on controller=%p\r\n",
				retail::ToPointer(controller_ref.value));
			return false;
		}

		bool fly_control_was_active = false;
		if (!ReadFlyControlActiveState(fly, fly_control_was_active))
			return false;

		std::uint8_t active_mode_was_entered = 0;
		const retail::ModeView active_mode_view(active_mode);
		if (!active_mode_view.FlyActiveEntered(active_mode_was_entered) ||
			!active_mode_view.SetFlyActiveEntered(1))
		{
			return false;
		}
		bool active_mode_changed = true;

		bool remote_input_started = false;
		bool remote_gamepad_scoped = false;
		void* previous_gamepad = nullptr;
		retail::GamePadRef primary_gamepad = {};
		retail::InputManagerRef native_input_ref = {};
		retail::AimRay saved_native_ray = {};
		bool restore_native_ray = false;
		bool ready = SetFlyControlActiveState(fly, true);
		void* remote_gamepad = nullptr;
		if (remote_owner && ready)
		{
			if (!IsRemoteInputActiveOnThisThread())
			{
				BeginRemoteInput();
				remote_input_started = true;
			}
			ready = m_active_remote_input.fly_controlled != 0 &&
				m_active_remote_input.fly_transform_sequence != 0;
			if (ready)
			{
				remote_gamepad = GetRemoteGamePad();
				ready = remote_gamepad &&
					BeginRemoteGamePadScope(previous_gamepad);
				remote_gamepad_scoped = ready;
			}
		}

		if (!remote_owner && ready)
		{
			ready = retail::PrimaryGamePadStore().Read(primary_gamepad) &&
				primary_gamepad;
		}

		if (ready)
		{
			const void* native_gamepad = remote_owner ? remote_gamepad :
				retail::ToPointer(primary_gamepad.value);
			native_input_ref = { retail::ToAddress(native_gamepad) };
			retail::InputManagerView native_input(native_input_ref);
			retail::Transform fly_transform = {};
			retail::AimRay native_ray = {};
			ready = native_gamepad &&
				retail::EntityView(fly_ref).ReadTransform(fly_transform) &&
				IsFiniteRetailTransform(fly_transform) &&
				native_input.ReadAimRay(saved_native_ray);
			if (ready)
			{
				// The native block reads the origin from XGamePad. During normal Fly
				// control that cache is already centered on Mooch; F1 and the remote
				// replay must provide the same invariant explicitly. Aim at the exact
				// replicated endpoint, but start the ray at the current Fly transform.
				const float dx = target[0] - fly_transform.position.x;
				const float dy = target[1] - fly_transform.position.y;
				const float dz = target[2] - fly_transform.position.z;
				const float length = sqrtf(dx * dx + dy * dy + dz * dz);
				if (length > 0.5f && IsFiniteFloat(length))
				{
					native_ray.direction.x = dx / length;
					native_ray.direction.y = dy / length;
					native_ray.direction.z = dz / length;
				}
				else
				{
					// A degenerate endpoint is not expected from a real shot. Prefer the
					// sender's direction for a remote replay, then keep the pad's direction
					// as a safe fallback for a standalone debug press.
					if (remote_owner)
						memcpy(&native_ray.direction, m_active_remote_input.aim_direction,
							sizeof(native_ray.direction));
					if (!IsValidAimRay(native_ray))
						native_ray.direction = saved_native_ray.direction;
				}
				native_ray.origin.x = fly_transform.position.x;
				native_ray.origin.y = fly_transform.position.y;
				native_ray.origin.z = fly_transform.position.z;
				restore_native_ray = true;
				ready = IsValidAimRay(native_ray) &&
					native_input.WriteAimRay(native_ray);
			}
		}

		SharedCameraCoordinator camera;
		SharedCameraCoordinator::AimState saved_camera_state = {};
		const bool restore_camera = ready &&
			camera.SaveAimState(saved_camera_state);
		retail::ActiveEntityStore active_entities;
		retail::EntityRef saved_active_a = {};
		retail::EntityRef saved_active_b = {};
		const bool restore_active_entities = ready &&
			active_entities.Read(saved_active_a, saved_active_b);

		bool native_completed = false;
		bool effect_ready = false;
		if (ready)
		{
			m_fly_native_pass_controller =
				retail::ToPointer(controller_ref.value);
			m_fly_native_pass_thread_id = GetCurrentThreadId();
			const int dual_laser_raw_index =
				FindFlyRawActionIndex(kFlyDualLaserRawActionId);
			const LONG dual_laser_raw_bit = dual_laser_raw_index >= 0 ?
				static_cast<LONG>(1u << static_cast<std::uint32_t>(
					dual_laser_raw_index)) : 0;
			InterlockedExchange(&m_fly_native_synthetic_press_mask,
				dual_laser_raw_bit);
			InterlockedExchange(&m_fly_native_pass_active, 1);

			native_completed =
				retail::NativeGameApi::RunFlyActiveUpdate(active_mode);
			if (native_completed)
				effect_ready = RememberFlyDualLaserPulse(fly, remote_owner);

			InterlockedExchange(&m_fly_native_synthetic_press_mask, 0);
			InterlockedExchange(&m_fly_native_pass_active, 0);
			m_fly_native_pass_thread_id = 0;
			m_fly_native_pass_controller = nullptr;
		}

		if (restore_camera)
			camera.RestoreAimState(saved_camera_state);
		if (restore_native_ray &&
			!retail::InputManagerView(native_input_ref).WriteAimRay(saved_native_ray))
		{
			CoopRuntime::Instance().Log(
				"[fly-native] failed to restore XGamePad aim ray\r\n");
		}
		if (restore_active_entities &&
			!active_entities.Restore(saved_active_a, saved_active_b))
		{
			CoopRuntime::Instance().Log(
				"[fly-native] failed to restore active-entity globals\r\n");
		}
		if (remote_gamepad_scoped)
			EndRemoteGamePadScope(previous_gamepad);
		if (remote_input_started)
			EndRemoteInput();
		if (active_mode_changed &&
			!active_mode_view.SetFlyActiveEntered(active_mode_was_entered))
		{
			CoopRuntime::Instance().Log(
				"[fly-native] failed to restore Fly_Active mode-local flag\r\n");
		}
		if (!SetFlyControlActiveState(fly, fly_control_was_active))
		{
			CoopRuntime::Instance().Log(
				"[fly-native] failed to restore Fly control state\r\n");
		}

		if (!native_completed)
		{
			CoopRuntime::Instance().Log(
				"[fly-native] native Fly_Active update did not complete remote=%u\r\n",
				remote_owner ? 1u : 0u);
		}
		else if (!effect_ready)
		{
			CoopRuntime::Instance().Log(
				"[fly-native] Fly_Active returned without arming both laser items remote=%u\r\n",
				remote_owner ? 1u : 0u);
		}
		return native_completed && effect_ready;
	}

	bool CoopNetGame::ApplyDirectFlyDualLaserPulse(void* fly,
		const float target[3], bool remote_owner)
	{
		if (!fly || !target || !IsFiniteFloatArray(target, 3) ||
			!SetFlyDualLaserPresentationTarget(fly, target))
		{
			return false;
		}

		retail::FlyDualLaserRouteItemRef route_items[
			kFlyDualLaserRouteItemCount] = {};
		std::uint32_t item_ids[kFlyDualLaserRouteItemCount] = {};
		if (!ResolveFlyDualLaserRouteItems(fly, route_items, item_ids) ||
			!SetFlyDualLaserRouteItemsActive(route_items, item_ids, true))
		{
			SetFlyDualLaserRouteItemsActive(route_items, item_ids, false);
			return false;
		}

		retail::FlyDualLaserRouteItemRef* saved_items = remote_owner ?
			m_remote_fly_laser_route_items : m_debug_fly_laser_route_items;
		std::uint32_t* saved_ids = remote_owner ?
			m_remote_fly_laser_item_ids : m_debug_fly_laser_item_ids;
		bool* saved_active = remote_owner ?
			&m_remote_fly_laser_pulse_active :
			&m_debug_fly_laser_pulse_active;
		for (std::size_t index = 0; index < kFlyDualLaserRouteItemCount;
			++index)
		{
			saved_items[index] = route_items[index];
			saved_ids[index] = item_ids[index];
		}
		*saved_active = true;
		return true;
	}

	void CoopNetGame::QueueLocalFlyDualLaserEvent(const void* input_manager)
	{
		if (!HasRemotePeer() || !IsLocalFlyControlled())
			return;

		// Fly_Active computes the aim-task target from this same input-manager
		// snapshot immediately before its raw pressed query. Preserve that target
		// on the wire; the receiver must not fall back to its own P1/camera ray.
		retail::AimRay aim_ray = {};
		float laser_target[3] = {};
		if (!ReadValidAimRay(input_manager, aim_ray) ||
			!BuildFlyDualLaserTarget(aim_ray.origin, aim_ray.direction,
				laser_target))
		{
			CoopRuntime::Instance().Log(
				"[fly-laser] skipped local event: no valid Fly aim target\r\n");
			return;
		}

		// This raw edge executes inside the current Fly tick. That tick publishes the
		// next transform epoch immediately afterward; a later global movement pass
		// may additionally publish the settled root. Tag the first post-tick epoch
		// rather than the snapshot from before this turn/shot.
		std::uint32_t source_fly_transform_sequence = 0;
		AcquireSRWLockShared(&m_input_lock);
		if (m_local_input.fly_controlled != 0)
		{
			source_fly_transform_sequence =
				m_local_input.fly_transform_sequence;
			NextNonZeroSequence(source_fly_transform_sequence);
		}
		ReleaseSRWLockShared(&m_input_lock);

		bool queued = false;
		bool overflow = false;
		std::uint32_t event_sequence = 0;
		AcquireSRWLockExclusive(&m_fly_ability_lock);
		if (m_outgoing_fly_ability_count < kFlyAbilityQueueCapacity)
		{
			const std::uint32_t index =
				(m_outgoing_fly_ability_head + m_outgoing_fly_ability_count) %
				kFlyAbilityQueueCapacity;
			FlyAbilityQueueEntry& entry = m_outgoing_fly_abilities[index];
			entry = {};
			protocol::InitializeFixedPacket(entry.packet,
				protocol::PacketKind::FlyAbility);
			entry.packet.sequence =
				NextNonZeroSequence(m_local_fly_ability_sequence);
			entry.packet.ability = protocol::FlyAbility::DualLaser;
			entry.packet.source_fly_transform_sequence =
				source_fly_transform_sequence;
			entry.packet.laser_target[0] = laser_target[0];
			entry.packet.laser_target[1] = laser_target[1];
			entry.packet.laser_target[2] = laser_target[2];
			entry.received_tick = GetTickCount();
			event_sequence = entry.packet.sequence;
			++m_outgoing_fly_ability_count;
			queued = true;
		}
		else
		{
			overflow = true;
		}
		ReleaseSRWLockExclusive(&m_fly_ability_lock);

		if (queued)
		{
			CoopRuntime::Instance().Log(
				"[fly-laser] queued local reliable event=%u post_tick_fly_seq=%u target=(%.2f, %.2f, %.2f)\r\n",
				event_sequence,
				source_fly_transform_sequence, laser_target[0], laser_target[1],
				laser_target[2]);
		}
		else if (overflow)
		{
			CoopRuntime::Instance().Log(
				"[fly-laser] outgoing queue full; local laser event dropped\r\n");
		}
	}

	void CoopNetGame::BeginRemoteFlyDualLaserPresentationTick()
	{
		// Called only by the Mooch controller's game-thread update, before its
		// stock tick. A native laser item armed by the previous ability survives
		// only until this next receiver tick.
		ClearRemoteFlyDualLaserPulse();
	}

	void CoopNetGame::ClearRemoteFlyDualLaserPulse()
	{
		if (!m_remote_fly_laser_pulse_active)
			return;

		if (!SetFlyDualLaserRouteItemsActive(m_remote_fly_laser_route_items,
			m_remote_fly_laser_item_ids, false))
		{
			CoopRuntime::Instance().Log(
				"[fly-laser] could not clear prior receiver-side laser pulse\r\n");
		}
		ZeroMemory(m_remote_fly_laser_route_items,
			sizeof(m_remote_fly_laser_route_items));
		ZeroMemory(m_remote_fly_laser_item_ids,
			sizeof(m_remote_fly_laser_item_ids));
		m_remote_fly_laser_pulse_active = false;
	}

	void CoopNetGame::ClearDebugFlyDualLaserPulse()
	{
		if (!m_debug_fly_laser_pulse_active)
			return;

		if (!SetFlyDualLaserRouteItemsActive(m_debug_fly_laser_route_items,
			m_debug_fly_laser_item_ids, false))
		{
			CoopRuntime::Instance().Log(
				"[debug-F1] could not clear prior standalone Mooch laser pulse\r\n");
		}
		ZeroMemory(m_debug_fly_laser_route_items,
			sizeof(m_debug_fly_laser_route_items));
		ZeroMemory(m_debug_fly_laser_item_ids,
			sizeof(m_debug_fly_laser_item_ids));
		m_debug_fly_laser_pulse_active = false;
	}

	bool CoopNetGame::ConsumeReadyRemoteFlyDualLaserEvent(
		std::uint32_t remote_fly_transform_sequence,
		protocol::FlyAbilityPacket& event)
	{
		event = {};
		if (remote_fly_transform_sequence == 0)
			return false;

		const DWORD now = GetTickCount();
		std::uint32_t expired_sequence = 0;
		AcquireSRWLockExclusive(&m_fly_ability_lock);
		while (m_incoming_fly_ability_count != 0)
		{
			FlyAbilityQueueEntry& entry =
				m_incoming_fly_abilities[m_incoming_fly_ability_head];
			if (static_cast<DWORD>(now - entry.received_tick) <
				kRemoteFlyAbilityEventLifetimeMs)
			{
				break;
			}

			expired_sequence = entry.packet.sequence;
			entry = {};
			m_incoming_fly_ability_head =
				(m_incoming_fly_ability_head + 1) % kFlyAbilityQueueCapacity;
			--m_incoming_fly_ability_count;
		}

		if (m_incoming_fly_ability_count != 0)
		{
			FlyAbilityQueueEntry& entry =
				m_incoming_fly_abilities[m_incoming_fly_ability_head];
			const bool epoch_ready =
				entry.packet.source_fly_transform_sequence == 0 ||
				IsSameOrNewerNonZeroSequence(remote_fly_transform_sequence,
					entry.packet.source_fly_transform_sequence);
			if (entry.packet.ability == protocol::FlyAbility::DualLaser &&
				entry.packet.sequence != 0 && epoch_ready)
			{
				event = entry.packet;
				entry = {};
				m_incoming_fly_ability_head =
					(m_incoming_fly_ability_head + 1) %
					kFlyAbilityQueueCapacity;
				--m_incoming_fly_ability_count;
			}
		}
		ReleaseSRWLockExclusive(&m_fly_ability_lock);

		if (expired_sequence != 0)
		{
			CoopRuntime::Instance().Log(
				"[fly-laser] expired remote event=%u before native presentation\r\n",
				expired_sequence);
		}
		return event.sequence != 0;
	}

	bool CoopNetGame::ApplyRemoteFlyAimMotor(void* fly)
	{
		if (!fly || IsLocalFlyControlled() || !IsRemoteFlyControlled())
			return false;

		CoopInput remote = {};
		if (!GetRemoteInput(remote) || remote.fly_controlled == 0 ||
			remote.fly_transform_sequence == 0)
		{
			return false;
		}

		retail::AimRay remote_aim = {};
		memcpy(&remote_aim.origin, remote.aim_origin, sizeof(remote_aim.origin));
		memcpy(&remote_aim.direction, remote.aim_direction,
			sizeof(remote_aim.direction));
		if (!IsValidAimRay(remote_aim))
			return false;

		const float direction_length = sqrtf(
			remote_aim.direction.x * remote_aim.direction.x +
			remote_aim.direction.y * remote_aim.direction.y +
			remote_aim.direction.z * remote_aim.direction.z);
		if (!(direction_length > 0.5f) || !IsFiniteFloat(direction_length))
			return false;

		const retail::EntityRef fly_ref = { retail::ToAddress(fly) };
		retail::HandlerRef handler = {};
		retail::ControllerRef controller = {};
		std::uint32_t fly_state_index = 0;
		retail::MotorTaskRef fly_state = {};
		if (!retail::EntityView(fly_ref).Handler(handler) ||
			!retail::HandlerView(handler).Controller(controller) ||
			!retail::ReadFlyActiveStateIndex(fly_state_index) ||
			!retail::HandlerView(handler).FlyFlyMotionState(fly_state_index,
				fly_state))
		{
			return false;
		}

		const float inverse_length = 1.0f / direction_length;
		const float direction_x = remote_aim.direction.x * inverse_length;
		const float direction_y = remote_aim.direction.y * inverse_length;
		const float direction_z = remote_aim.direction.z * inverse_length;
		float clamped_y = direction_y;
		if (clamped_y < -1.0f)
			clamped_y = -1.0f;
		else if (clamped_y > 1.0f)
			clamped_y = 1.0f;

		// This is the exact inverse of FlyFly's stock direction formula in
		// 0x005101F0: (sin(yaw)*cos(pitch), -sin(pitch),
		// cos(yaw)*cos(pitch)). Unlike XMotorTask_Aim, this state drives the
		// physical Mooch LookAt motor rather than only the laser endpoint.
		const float yaw = atan2f(direction_x, direction_z);
		const float pitch = -asinf(clamped_y);
		if (!IsFiniteFloat(yaw) || !IsFiniteFloat(pitch))
			return false;

		const retail::FlyFlyMotionStateView fly_state_view(fly_state);
		return fly_state_view.SetImmediateDirection(yaw, pitch) &&
			retail::NativeGameApi::SubmitFlyFlyBodyDirection(controller, fly_state);
	}

	bool CoopNetGame::ApplyRemoteFlyDualLaserPresentation(void* fly)
	{
		if (!fly || IsLocalFlyControlled() || !IsRemoteFlyControlled())
			return false;

		CoopInput remote = {};
		if (!GetRemoteInput(remote) || remote.fly_controlled == 0 ||
			remote.fly_transform_sequence == 0)
		{
			return false;
		}

		protocol::FlyAbilityPacket event = {};
		if (!ConsumeReadyRemoteFlyDualLaserEvent(
			remote.fly_transform_sequence, event))
		{
			return false;
		}

		// Feed the exact Fly raw pressed branch. This lets the retail update run
		// its aim preparation, item activation and contextual reaction route on
		// this process, while the state/camera guard keeps the receiver in Idle.
		if (RunFlyNativeDualLaserPass(fly, event.laser_target, true))
		{
			CoopRuntime::Instance().Log(
				"[fly-laser] applied remote event=%u through native Fly_Active raw button target=(%.2f, %.2f, %.2f); receiver remains presentation-only\r\n",
				event.sequence, event.laser_target[0], event.laser_target[1],
				event.laser_target[2]);
			return true;
		}

		// Keep a visible fallback for a profile/runtime mismatch. It is explicitly
		// reported as presentation-only: gameplay reactions require the native path.
		if (ApplyDirectFlyDualLaserPulse(fly, event.laser_target, true))
		{
			CoopRuntime::Instance().Log(
				"[fly-laser] remote event=%u fell back to direct presentation; native reactions are unavailable\r\n",
				event.sequence);
			return true;
		}
		CoopRuntime::Instance().Log(
			"[fly-laser] dropped remote event=%u: native and direct presentation paths failed\r\n",
			event.sequence);
		return false;
	}

	bool CoopNetGame::SetFlyControlActiveState(void* fly, bool active) const
	{
		if (!fly)
			return false;
		const retail::EntityRef fly_ref = { retail::ToAddress(fly) };
		retail::HandlerRef handler_ref = {};
		if (!retail::EntityView(fly_ref).Handler(handler_ref))
			return false;

		std::uint32_t state_index = 0;
		return retail::ReadFlyActiveStateIndex(state_index) &&
			retail::HandlerView(handler_ref).SetFlyControlActive(state_index, active);
	}

	bool CoopNetGame::RequestDebugFlyDualLaser()
	{
		// F1 is deliberately independent of ownership and Q/HUD state. It sends
		// the same raw pressed edge into a shadow Fly_Active update, so one window
		// can test the real Mooch attack without a second client.
		ClearDebugFlyDualLaserPulse();

		retail::EntitySlotRepository players;
		retail::EntityRef fly = {};
		retail::Transform fly_transform = {};
		if (!players.Get(retail::EntitySlot::Mooch, fly) || !fly ||
			!retail::EntityView(fly).ReadTransform(fly_transform) ||
			!IsFiniteRetailTransform(fly_transform))
		{
			CoopRuntime::Instance().Log(
				"[debug-F1] unavailable: Mooch transform is not ready\r\n");
			return false;
		}

		retail::GamePadRef primary_gamepad = {};
		retail::AimRay aim_ray = {};
		if (!retail::PrimaryGamePadStore().Read(primary_gamepad) ||
			!primary_gamepad ||
			!ReadValidAimRay(retail::ToPointer(primary_gamepad.value), aim_ray))
		{
			CoopRuntime::Instance().Log(
				"[debug-F1] unavailable: P1 aim ray is not ready\r\n");
			return false;
		}

		float laser_target[3] = {};
		if (!BuildFlyDualLaserTarget(aim_ray.origin, aim_ray.direction,
			laser_target))
		{
			CoopRuntime::Instance().Log(
				"[debug-F1] unavailable: P1 aim target is invalid\r\n");
			return false;
		}

		void* const fly_pointer = retail::ToPointer(fly.value);
		if (RunFlyNativeDualLaserPass(fly_pointer, laser_target, false))
		{
			CoopRuntime::Instance().Log(
				"[debug-F1] native Mooch dual-laser raw button fired target=(%.2f, %.2f, %.2f); no client/Q/HUD required\r\n",
				laser_target[0], laser_target[1], laser_target[2]);
			return true;
		}

		if (ApplyDirectFlyDualLaserPulse(fly_pointer, laser_target, false))
		{
			CoopRuntime::Instance().Log(
				"[debug-F1] native Mooch raw button unavailable; direct visual fallback target=(%.2f, %.2f, %.2f)\r\n",
				laser_target[0], laser_target[1], laser_target[2]);
			return true;
		}

		CoopRuntime::Instance().Log(
			"[debug-F1] unavailable: native and direct Mooch laser paths failed\r\n");
		return false;
	}

	void CoopNetGame::TickDebugFlyDualLaser()
	{
		// DebugActions calls this before sampling F1. A pulse therefore lasts one
		// full foreground game tick and never waits for Fly_Active or a client.
		ClearDebugFlyDualLaserPulse();
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

		const retail::EntityRef fly_ref = { retail::ToAddress(fly) };
		retail::EntityRef active_a = {};
		retail::EntityRef active_b = {};
		retail::ActiveEntityStore active_entities;
		if (!active_entities.Read(active_a, active_b) ||
			!active_entities.Set(fly_ref))
		{
			CoopRuntime::Instance().Log(
				"[fly] unable to restore active Mooch entity\r\n");
			return;
		}
		const bool repaired = active_a != fly_ref || active_b != fly_ref;
		if (repaired && !m_logged_fly_active_entity_repair)
		{
			m_logged_fly_active_entity_repair = true;
			CoopRuntime::Instance().Log(
				"[fly] restored native active entity to Mooch after fly tick\r\n");
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
			ClearFlyInputLocked(m_local_input);
			m_local_input.fly_controlled = 1;
			m_local_fly_active_seen = false;
			m_local_fly_deactivation_seen = false;
			// Confirm happens after the same physical Mooch press has selected
			// 0x61000065.  The pressed-edge path can still observe that entry press
			// on the following tick, so keep it consumed until the key is released.
			m_local_mooch_exit_key_down = true;
			m_logged_fly_active_entity_repair = false;
			// A local hand-off is newer than any not-yet-consumed peer exit.
			m_pending_remote_fly_zero_owner_input_sequence = 0;
			became_owner = true;
		}
		ReleaseSRWLockExclusive(&m_input_lock);
		if (became_owner)
		{
			// The native Fly tick owns the next activation transition. This is only an
			// ownership marker; it must not inspect or synthesize a weapon command.
			CoopRuntime::Instance().Log(
				"[fly] local Mooch ownership confirmed; awaiting native Fly activation\r\n");
		}
	}

	void CoopNetGame::ObserveLocalFlyMode(std::uint32_t mode_before,
		std::uint32_t mode_after)
	{
		bool active_seen = false;
		bool local_exit_started = false;
		bool receiver_deactivation_observed = false;
		bool local_owner_before_deactivation = false;
		bool remote_owner_before_deactivation = false;
		std::uint32_t local_sequence_before_deactivation = 0;
		std::uint32_t local_exit_input_sequence = 0;
		std::uint32_t remote_sequence_before_deactivation = 0;
		std::uint32_t remote_input_sequence_before_deactivation = 0;
		const bool deactivated = mode_before == kFlyDeactivatedModeId ||
			mode_after == kFlyDeactivatedModeId;
		AcquireSRWLockExclusive(&m_input_lock);
		if (deactivated)
		{
			if (!m_local_fly_deactivation_seen)
			{
				local_owner_before_deactivation =
					m_local_input.fly_controlled != 0;
				remote_owner_before_deactivation = m_remote_input.fly_controlled != 0 &&
					m_remote_input.fly_transform_sequence != 0;
				local_sequence_before_deactivation =
					m_local_input.fly_transform_sequence;
				remote_sequence_before_deactivation =
					m_remote_input.fly_transform_sequence;
				remote_input_sequence_before_deactivation =
					m_remote_input.transform_sequence;
				if (local_owner_before_deactivation)
				{
					// This process was the owner, so its native transition is the only
					// transition that can publish an exit to the peer. The sequence bump
					// makes the zero-owner state beat a live snapshot sent earlier this frame.
					ClearLocalFlyOwnershipLocked();
					local_exit_input_sequence = m_local_input.transform_sequence;
					m_local_fly_active_seen = false;
					m_local_mooch_exit_key_down = false;
					local_exit_started = true;
				}
				else if (remote_owner_before_deactivation)
				{
					// Native receiver state is process-local. It must not revoke a peer
					// owner; that peer's next sequenced packet remains authoritative.
					receiver_deactivation_observed = true;
				}
			}
			m_local_fly_deactivation_seen = true;
		}
		else
		{
			m_local_fly_deactivation_seen = false;
			if (m_local_input.fly_controlled != 0)
			{
			// The mode can be 0x34 at the beginning of its first tick and 0x33 at
			// the end of that same tick.  Observing either side catches the real
			// entry without treating the subsequent idle/follow state as an exit.
				if (mode_before == kFlyOrbitModeId ||
					mode_after == kFlyOrbitModeId)
				{
					active_seen = !m_local_fly_active_seen;
					m_local_fly_active_seen = true;
				}
			}
		}
		ReleaseSRWLockExclusive(&m_input_lock);
		if (local_exit_started)
		{
			CoopRuntime::Instance().Log(
				"[fly-lifecycle] local Fly_Deactivated modes=0x%08X->0x%08X input_seq=%u previous_fly_seq=%u; published ordered zero-owner exit\r\n",
				mode_before, mode_after,
				local_exit_input_sequence, local_sequence_before_deactivation);
		}
		if (receiver_deactivation_observed)
		{
			CoopRuntime::Instance().Log(
				"[fly-lifecycle] receiver Fly_Deactivated modes=0x%08X->0x%08X remote_input_seq=%u remote_fly_seq=%u; retained peer ownership until an ordered zero-owner packet\r\n",
				mode_before, mode_after,
				remote_input_sequence_before_deactivation,
				remote_sequence_before_deactivation);
		}
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
		protocol::InitializeFixedPacket(packet, protocol::PacketKind::Input);
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

	void CoopNetGame::SendQueuedFlyAbilityPackets()
	{
		protocol::FlyAbilityPacket packets[kFlyAbilityQueueCapacity] = {};
		std::uint32_t count = 0;
		AcquireSRWLockExclusive(&m_fly_ability_lock);
		while (m_outgoing_fly_ability_count != 0 &&
			count < kFlyAbilityQueueCapacity)
		{
			packets[count] =
				m_outgoing_fly_abilities[m_outgoing_fly_ability_head].packet;
			m_outgoing_fly_abilities[m_outgoing_fly_ability_head] = {};
			m_outgoing_fly_ability_head =
				(m_outgoing_fly_ability_head + 1) % kFlyAbilityQueueCapacity;
			--m_outgoing_fly_ability_count;
			++count;
		}
		ReleaseSRWLockExclusive(&m_fly_ability_lock);

		for (std::uint32_t index = 0; index < count; ++index)
		{
			const protocol::FlyAbilityPacket& packet = packets[index];
			if (IsClient() && SteamOClient && SteamOClient->IsConnected())
			{
				SteamOClient->SendRaw(&packet, sizeof(packet),
					k_nSteamNetworkingSend_Reliable);
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
					for (const HSteamNetConnection connection :
						server->GetPlayers())
					{
						server->SendRaw(connection, &packet, sizeof(packet),
							k_nSteamNetworkingSend_Reliable);
					}
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
		ChatOverlay::Instance().NetworkTick();
		SendQueuedFlyAbilityPackets();
		const DWORD now = GetTickCount();
		if (static_cast<DWORD>(now - m_last_send_tick) < kInputSendIntervalMs)
			return;
		m_last_send_tick = now;
		SendLocalInput();
	}

	void CoopNetGame::GameTick()
	{
		// The socket worker never touches a Win32 control. Deliver queued peer
		// lines here, after the stock P1 tick, even when a session just ended.
		ChatOverlay::Instance().GameTick();
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

	bool CoopNetGame::GetRemotePlayerModeSnapshot(
		std::uint32_t& transform_sequence, std::uint32_t& player_mode) const
	{
		CoopInput remote = {};
		if (!GetRemoteInput(remote))
			return false;
		transform_sequence = remote.transform_sequence;
		player_mode = remote.player_mode;
		return true;
	}

	bool CoopNetGame::GetRemoteAimRaySnapshot(float origin[3],
		float direction[3], std::uint32_t& transform_sequence) const
	{
		if (!origin || !direction)
			return false;
		CoopInput remote = {};
		if (!GetRemoteInput(remote))
			return false;
		memcpy(origin, remote.aim_origin, 3 * sizeof(float));
		memcpy(direction, remote.aim_direction, 3 * sizeof(float));
		transform_sequence = remote.transform_sequence;
		return true;
	}

	void CoopNetGame::PublishLocalPlayerTransform(const void* player)
	{
		if (!player)
			return;
		CoopInput snapshot = {};
		const retail::EntityRef player_ref = { retail::ToAddress(player) };
		retail::EntityView player_view(player_ref);
		retail::Transform player_transform = {};
		if (!player_view.ReadTransform(player_transform) ||
			!IsFiniteRetailTransform(player_transform))
			return;
		memcpy(snapshot.position, &player_transform.position, sizeof(snapshot.position));
		memcpy(snapshot.rotation, &player_transform.rotation, sizeof(snapshot.rotation));

		std::uint32_t selected_weapon_type = 0xFFFFFFFFu;
		retail::HandlerRef handler_ref = {};
		if (player_view.Handler(handler_ref))
			retail::HandlerView(handler_ref).SelectedWeaponType(selected_weapon_type);

		AcquireSRWLockExclusive(&m_input_lock);
		memcpy(m_local_input.position, snapshot.position,
			sizeof(snapshot.position));
		memcpy(m_local_input.rotation, snapshot.rotation,
			sizeof(snapshot.rotation));
		m_local_input.transform_sequence =
			NextNonZeroSequence(m_local_transform_sequence);
		m_local_input.selected_weapon_type = selected_weapon_type;

		if (selected_weapon_type != m_last_local_weapon_type)
		{
			m_last_local_weapon_type = selected_weapon_type;
			++m_local_weapon_sequence;
		}
		m_local_input.weapon_sequence = m_local_weapon_sequence;
		ReleaseSRWLockExclusive(&m_input_lock);
	}

	void CoopNetGame::PublishLocalPlayerMode(std::uint32_t mode)
	{
		AcquireSRWLockExclusive(&m_input_lock);
		m_local_input.player_mode = mode;
		ReleaseSRWLockExclusive(&m_input_lock);
	}

	void CoopNetGame::PublishLocalFlyTransform(const void* fly)
	{
		bool controlled = IsLocalFlyControlled();
		retail::Transform transform = {};
		if (controlled)
		{
			if (!fly)
				controlled = false;
			else
			{
				const retail::EntityRef fly_ref = { retail::ToAddress(fly) };
				controlled = retail::EntityView(fly_ref).ReadTransform(transform) &&
					IsFiniteRetailTransform(transform);
			}
		}

		AcquireSRWLockExclusive(&m_input_lock);
		// The owner can be released while the native controller is ticking.  Never
		// resurrect it from this late transform write.
		if (controlled && m_local_input.fly_controlled != 0)
		{
			memcpy(m_local_input.fly_position, &transform.position,
				sizeof(m_local_input.fly_position));
			memcpy(m_local_input.fly_rotation, &transform.rotation,
				sizeof(m_local_input.fly_rotation));
			m_local_input.fly_transform_sequence =
				NextNonZeroSequence(m_local_fly_transform_sequence);
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

	void CoopNetGame::PublishLocalFlyAimRay()
	{
		if (!IsLocalFlyControlled())
			return;

		// Unlike Default, Fly_Active can run without passing through the normal
		// Default-mode capture hook. Read the same process-global XGamePad used by
		// the local native Fly tick and retain its current world ray in the existing
		// input packet ABI.
		retail::GamePadRef primary_gamepad = {};
		if (retail::PrimaryGamePadStore().Read(primary_gamepad) && primary_gamepad)
			CaptureLocalAimRay(retail::ToPointer(primary_gamepad.value));
	}

	void CoopNetGame::PublishLocalAbrAimRay()
	{
		retail::GamePadRef primary_gamepad = {};
		if (retail::PrimaryGamePadStore().Read(primary_gamepad) && primary_gamepad)
			CaptureLocalAimRay(retail::ToPointer(primary_gamepad.value));
	}

	bool CoopNetGame::ApplyRemotePlayerTransform(void* player2)
	{
		m_remote_player_frame_transform_valid = false;
		m_remote_player_frame_entity = nullptr;
		m_remote_player_frame_sequence = 0;
		if (!player2 || !IsRemoteInputActiveOnThisThread() ||
			m_active_remote_input.transform_sequence == 0 ||
			IsVehicleMotorActiveForRemoteP2(m_active_remote_input))
		{
			return false;
		}

		retail::Transform remote_transform = {};
		if (!DecodeFiniteWireTransform(m_active_remote_input.position,
			m_active_remote_input.rotation, remote_transform))
		{
			return false;
		}
		const retail::Vec4& remote_position = remote_transform.position;
		const retail::Vec4& remote_rotation = remote_transform.rotation;

		const retail::EntityRef player2_ref = { retail::ToAddress(player2) };
		retail::EntityView player2_view(player2_ref);
		retail::Transform transform = {};
		if (!player2_view.ReadTransform(transform))
		{
			CoopRuntime::Instance().Log(
				"[net-transform-error] could not read remote P2 transform target\r\n");
			return false;
		}

		if (!IsFiniteRetailTransform(transform))
		{
			if (!player2_view.WriteTransform(remote_transform))
			{
				CoopRuntime::Instance().Log(
					"[net-transform-error] could not repair non-finite remote P2 transform\r\n");
				return false;
			}
			CoopRuntime::Instance().Log(
				"[net-transform-recovery] replaced non-finite P2 transform seq=%u\r\n",
				m_active_remote_input.transform_sequence);
			m_remote_player_frame_transform = remote_transform;
			m_remote_player_frame_entity = player2;
			m_remote_player_frame_sequence =
				m_active_remote_input.transform_sequence;
			m_remote_player_frame_transform_valid = true;
			return true;
		}

		const DWORD now = GetTickCount();
		DWORD elapsed_ms = m_last_remote_transform_apply_tick == 0 ? 16 :
			now - m_last_remote_transform_apply_tick;
		m_last_remote_transform_apply_tick = now;
		if (elapsed_ms > 50)
			elapsed_ms = 50;
		const float delta_seconds = static_cast<float>(elapsed_ms) * 0.001f;
		const float dx = remote_position.x - transform.position.x;
		const float dy = remote_position.y - transform.position.y;
		const float dz = remote_position.z - transform.position.z;
		const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
		float speed = distance;
		if (speed < 9.0f)
			speed = 9.0f;
		else if (speed > 13.0f)
			speed = 13.0f;
		float position_factor = speed * 0.7f * delta_seconds;
		if (position_factor > 1.0f)
			position_factor = 1.0f;

		transform.position.x += (remote_position.x - transform.position.x) * position_factor;
		transform.position.y += (remote_position.y - transform.position.y) * position_factor;
		transform.position.z += (remote_position.z - transform.position.z) * position_factor;
		transform.position.w = remote_position.w;

		transform.rotation.x += (remote_rotation.x - transform.rotation.x) * position_factor;
		transform.rotation.y += (remote_rotation.y - transform.rotation.y) * position_factor;
		transform.rotation.z += (remote_rotation.z - transform.rotation.z) * position_factor;
		transform.rotation.w = remote_rotation.w;
		if (!player2_view.WriteTransform(transform))
		{
			CoopRuntime::Instance().Log(
				"[net-transform-error] could not write remote P2 transform target\r\n");
			return false;
		}
		m_remote_player_frame_transform = transform;
		m_remote_player_frame_entity = player2;
		m_remote_player_frame_sequence =
			m_active_remote_input.transform_sequence;
		m_remote_player_frame_transform_valid = true;
		if (!m_logged_remote_transform)
		{
			CoopRuntime::Instance().Log(
				"[net-transform] remote target correction active for P2\r\n");
			m_logged_remote_transform = true;
		}
		return true;
	}

	bool CoopNetGame::ReapplyRemotePlayerFrameTransform(void* player2)
	{
		if (!player2 || !m_remote_player_frame_transform_valid ||
			m_remote_player_frame_entity != player2 ||
			m_remote_player_frame_sequence == 0)
		{
			return false;
		}
		const retail::EntityRef player2_ref = { retail::ToAddress(player2) };
		if (!retail::EntityView(player2_ref).WriteTransform(
			m_remote_player_frame_transform))
		{
			CoopRuntime::Instance().Log(
				"[net-transform-error] could not reapply prepared remote P2 transform\r\n");
			return false;
		}
		return true;
	}

	bool CoopNetGame::ApplyRemoteAbrTransform(void* player2)
	{
		if (!player2)
			return false;

		CoopInput remote = {};
		if (!GetRemoteInput(remote) || remote.player_mode != kAbrModeId ||
			remote.transform_sequence == 0)
		{
			return false;
		}

		retail::Transform transform = {};
		if (!DecodeFiniteWireTransform(remote.position, remote.rotation, transform))
			return false;

		const retail::EntityRef player2_ref = { retail::ToAddress(player2) };
		retail::EntityView player2_view(player2_ref);
		if (!player2_view.WriteTransform(transform))
		{
			CoopRuntime::Instance().Log(
				"[abr-transform] could not write remote P2/RDV root seq=%u\r\n",
				remote.transform_sequence);
			return false;
		}

		if (!m_logged_remote_abr_transform)
		{
			CoopRuntime::Instance().Log(
				"[abr-transform] peer ABR root transform active for P2/RDV\r\n");
			m_logged_remote_abr_transform = true;
		}
		return true;
	}

	bool CoopNetGame::IsVehicleMotorActiveForRemoteP2(
		const CoopInput& remote) const
	{
		if (remote.player_mode == kAbrModeId)
			return true;

		retail::EntitySlotRepository players;
		retail::EntitySlotBinding remote_player = {};
		std::uint32_t remote_mode = 0;
		// The receiver can enter its native vehicle mode a frame before the next
		// packet says so. P2's own controller is therefore the first authority
		// here; never run ordinary-player correction through a live RDV controller.
		if (players.GetBinding(retail::EntitySlot::RemoteP2, remote_player) &&
			retail::ControllerView(remote_player.controller).CurrentMode(remote_mode) &&
			remote_mode == kAbrModeId)
		{
			return true;
		}

		// The peer can cross the ABR boundary a packet ahead of this process.  The
		// local vehicle controller is therefore an independent stop condition: an
		// on-foot recovery must not touch the vehicle passenger while it is active.
		retail::EntitySlotBinding local_player = {};
		std::uint32_t local_mode = 0;
		return players.GetBinding(retail::EntitySlot::LocalP1, local_player) &&
			retail::ControllerView(local_player.controller).CurrentMode(local_mode) &&
			local_mode == kAbrModeId;
	}

	bool CoopNetGame::ApplyRemoteFlyTransform(void* fly)
	{
		if (!fly)
			return false;
		CoopInput remote = {};
		if (!IsRemoteFlyControlled() || !GetRemoteInput(remote) ||
			remote.fly_controlled == 0 ||
			remote.fly_transform_sequence == 0)
		{
			return false;
		}
		retail::Transform transform = {};
		memcpy(&transform.position, remote.fly_position,
			sizeof(remote.fly_position));
		memcpy(&transform.rotation, remote.fly_rotation,
			sizeof(remote.fly_rotation));
		// fly_rotation is one complete native representation. Do not splice a
		// process-global camera yaw into one component: it uses a different contract
		// and visibly skewed the receiver's Mooch body and laser direction.
		// OnRemotePacket rejects malformed snapshots before they reach the state
		// buffer. Keep this local barrier as well: this is a direct retail root
		// write and must never receive a NaN/Inf transform through a future path.
		if (!IsFiniteRetailTransform(transform))
			return false;
		const retail::EntityRef fly_ref = { retail::ToAddress(fly) };
		if (retail::EntityView(fly_ref).WriteTransform(transform))
			return true;

		CoopRuntime::Instance().Log(
			"[fly-sync-error] could not apply remote Mooch transform\r\n");
		return false;
	}

	void __fastcall CoopNetGame::HookWeaponAmmoConsume(void* weapon_record,
		void*)
	{
		Instance().HandleWeaponAmmoConsume(weapon_record);
	}

	void __fastcall CoopNetGame::HookHealthComponentSet(void* component, void*,
		float requested_value, std::uint32_t slot, bool notify)
	{
		Instance().HandleHealthComponentSet(component, requested_value, slot, notify,
			reinterpret_cast<void*>(_ReturnAddress()));
	}

	void __fastcall CoopNetGame::HookHealthComponentAdd(void* component, void*,
		float delta, std::uint32_t slot)
	{
		Instance().HandleHealthComponentAdd(component, delta, slot,
			reinterpret_cast<void*>(_ReturnAddress()));
	}

	void __fastcall CoopNetGame::HookHealthComponentSubtract(void* component, void*,
		float amount, std::uint32_t slot)
	{
		Instance().HandleHealthComponentSubtract(component, amount, slot,
			reinterpret_cast<void*>(_ReturnAddress()));
	}

	void __fastcall CoopNetGame::HookTriggerSpawnFromDefinition(void* trigger,

		void*)
	{
		Instance().HandleTriggerSpawnFromDefinition(trigger);
	}

	void* __cdecl CoopNetGame::HookTriggerFactory(std::uint32_t family,
		std::uint32_t subtype, void* output)
	{
		return Instance().HandleTriggerFactory(family, subtype, output);
	}

	int __fastcall CoopNetGame::HookTriggerEvent(void* trigger, void*,
		int event_code)
	{
		return Instance().HandleTriggerEvent(trigger, event_code);
	}

	int __fastcall CoopNetGame::HookGlobalEventForwarder(void* receiver, void*,
		void* source, int event_code)
	{
		return Instance().HandleGlobalEventForwarder(receiver, source, event_code);
	}

	int __cdecl CoopNetGame::HookObjectEventRelay(void* source, int event_code)
	{
		return Instance().HandleObjectEventRelay(source, event_code,
			reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
	}

	int __fastcall CoopNetGame::HookObjectEventForwarder(void* receiver, void*,
		void* object, int event_code)
	{
		return Instance().HandleObjectEventForwarder(receiver, object, event_code,
			reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
	}

	bool __fastcall CoopNetGame::HookNativeSaveLoad(void* manager, void*,
		std::uint32_t slot)
	{
		CoopNetGame& game = Instance();
		if (!game.m_original_native_save_load)
			return false;
		if (retail::ToAddress(manager) == gforce::kLoadSaveManager &&
			game.IsHost() && game.HasRemotePeer() &&
			slot < gforce::kVisibleSaveSlotCount)
		{
			CoopRuntime::Instance().Log(
				"[save-sync] host native Load Game intercepted slot=%u\r\n",
				slot);
			SaveSync::Instance().OnHostLoadGame(slot);
		}
		return game.m_original_native_save_load(manager, slot);
	}

	void CoopNetGame::HandleTriggerSpawnFromDefinition(void* trigger)
	{
		if (!m_original_trigger_spawn)
			return;

		// A client that physically activates a map trigger must run the retail
		// dispatcher/spawn path locally as well.  Its entity starts unassigned and
		// is bound to the host's later WorldSpawn by trigger signature.  Route 3 is
		// still not replayed on the client, so the host response cannot create a
		// second local entity.  A WorldSpawn-armed direct fallback remains valid for
		// a trigger that was activated only by the host.
		retail::TriggerIdentity pre_identity = {};
		const retail::TriggerRef pre_trigger_ref = { retail::ToAddress(trigger) };
		const bool have_pre_identity = trigger &&
			retail::TriggerView(pre_trigger_ref).Identity(pre_identity);
		const bool pre_is_npc_or_monster = have_pre_identity &&
			(pre_identity.family == kMonsterTriggerFamily ||
				pre_identity.family == kNpcTriggerFamily);
		if (pre_is_npc_or_monster && IsClient() && HasRemotePeer())
		{
			const bool host_armed =
				WorldSync::Instance().BeginExpectedClientReplicaSpawn(trigger,
					pre_identity.family, pre_identity.subtype);
			WorldSync::Instance().RecordTriggerTemplate(trigger,
				pre_identity.family, pre_identity.subtype);
			if (!host_armed)
			{
				CoopRuntime::Instance().Log(
					"[world-spawn-local] client allowed native spawn "
					"trigger=%p family=%08X subtype=%08X def=%d; awaiting host id\r\n",
					trigger, pre_identity.family, pre_identity.subtype,
					pre_identity.definition_id);
			}
		}

		__try
		{
			m_original_trigger_spawn(trigger);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log(
				"[world] native trigger spawn fault trigger=%p\r\n", trigger);
			return;
		}
		if (!trigger)
			return;

		const retail::TriggerRef trigger_ref = { retail::ToAddress(trigger) };
		const retail::TriggerView trigger_view(trigger_ref);
		retail::TriggerIdentity identity = {};
		if (!trigger_view.Identity(identity))
		{
			CoopRuntime::Instance().Log(
				"[world] could not read spawned trigger identity trigger=%p\r\n",
				trigger);
			return;
		}
		const std::uint32_t family = identity.family;
		const std::uint32_t subtype = identity.subtype;
		const std::int32_t definition_id = identity.definition_id;
		WorldSync::Instance().RecordTriggerTemplate(trigger, family, subtype);

		const bool is_npc_or_monster = family == kMonsterTriggerFamily ||
			family == kNpcTriggerFamily;
		if (!is_npc_or_monster)
			return;

		retail::EntityRef live_entity_ref = {};
		const retail::EntityRegistryView registry;
		const bool walked = registry.VisitLiveEntities(
			kEntityRegistryWalkSafetyLimit,
			[&trigger_ref, &live_entity_ref](retail::EntityRef entity_ref)
			{
				retail::TriggerRef entity_trigger = {};
				if (!retail::EntityView(entity_ref).Trigger(entity_trigger) ||
					entity_trigger != trigger_ref)
				{
					return true;
				}
				live_entity_ref = entity_ref;
				return false;
			});
		if (!walked)
		{
			CoopRuntime::Instance().Log(
				"[world] native NPC/monster spawn registration fault\r\n");
			return;
		}
		void* const live_entity = retail::ToPointer(live_entity_ref.value);

		retail::EntityRef spawned_object_ref = {};
		trigger_view.SpawnedEntity(spawned_object_ref);
		retail::Transform trigger_transform = {};
		trigger_view.ReadTransform(trigger_transform);
		retail::Transform entity_transform = {};
		if (live_entity)
		{
			const retail::EntityRef entity_ref = { retail::ToAddress(live_entity) };
			retail::EntityView(entity_ref).ReadTransform(entity_transform);
		}

		const char* const role = IsHost() ? "host" :
			(IsClient() ? "client" : "none");
		static volatile LONG s_world_spawn_trace_sequence = 0;
		const unsigned sequence = static_cast<unsigned>(
			InterlockedIncrement(&s_world_spawn_trace_sequence));
		CoopRuntime::Instance().Log(
			"[world-spawn-trace] seq=%u role=%s peer=%d trigger=%p "
			"family=0x%08X subtype=0x%08X def=%d object=%p live=%p "
			"trigger_pos=(%.2f,%.2f,%.2f) entity_pos=(%.2f,%.2f,%.2f)\r\n",
			sequence, role, HasRemotePeer() ? 1 : 0, trigger, family, subtype,
			definition_id, retail::ToPointer(spawned_object_ref.value), live_entity,
			trigger_transform.position.x, trigger_transform.position.y,
			trigger_transform.position.z, entity_transform.position.x,
			entity_transform.position.y, entity_transform.position.z);
		if (live_entity)
		{
			WorldSync::Instance().RecordNativeSpawn(trigger, live_entity, family,
				subtype, definition_id);
		}
	}

	void* CoopNetGame::HandleTriggerFactory(std::uint32_t family,
		std::uint32_t subtype, void* output)
	{
		if (!m_original_trigger_factory)
			return nullptr;

		void* trigger = nullptr;
		__try
		{
			trigger = m_original_trigger_factory(family, subtype, output);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log(
				"[world] native trigger factory fault family=0x%08X subtype=0x%08X output=%p\r\n",
				family, subtype, output);
			return nullptr;
		}
		WorldSync::Instance().RecordTriggerTemplate(trigger, family, subtype);
		return trigger;
	}

	int CoopNetGame::HandleGlobalEventForwarder(void* receiver, void* source,
		int event_code)
	{
		if (!m_original_global_event_forwarder)
			return 0;
		int result = 0;
		__try
		{
			result = m_original_global_event_forwarder(receiver, source, event_code);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log(
				"[global-event] native forwarder fault receiver=%p source=%p event=0x%08X\r\n",
				receiver, source, static_cast<unsigned>(event_code));
			return result;
		}

		// This is an event edge, not an input poll. Restrict the diagnostic to the
		// contextual namespace so ordinary timer/state notifications remain silent.
		if ((static_cast<std::uint32_t>(event_code) & 0xFF000000u) != 0x41000000u)
			return result;

		retail::EntitySlotRepository players;
		retail::EntityRef player1 = {};
		retail::Transform player_transform = {};
		const bool have_player_position = players.Get(retail::EntitySlot::LocalP1,
			player1) && retail::EntityView(player1).ReadTransform(player_transform) &&
			IsFiniteRetailTransform(player_transform);
		if (have_player_position)
		{
			CoopRuntime::Instance().Log(
				"[global-event] receiver=%p source=%p event=0x%08X result=%d p1_pos=(%.2f,%.2f,%.2f)\r\n",
				receiver, source, static_cast<unsigned>(event_code), result,
				player_transform.position.x, player_transform.position.y,
				player_transform.position.z);
		}
		else
		{
			CoopRuntime::Instance().Log(
				"[global-event] receiver=%p source=%p event=0x%08X result=%d p1_pos=unavailable\r\n",
				receiver, source, static_cast<unsigned>(event_code), result);
		}
		return result;
	}

	int CoopNetGame::HandleObjectEventRelay(void* source, int event_code,
		std::uintptr_t caller_return_address)
	{
		if (!m_original_object_event_relay)
			return 0;
		// A relay entered by another native object route will be recreated while the
		// peer replays its parent. Only independently returned calls become packets.
		const bool is_outermost_route = !IsObjectEventRouteNested();
		int result = 0;
		++g_object_event_route_depth;
		__try
		{
			result = m_original_object_event_relay(source, event_code);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			--g_object_event_route_depth;
			CoopRuntime::Instance().Log(
				"[object-event] route=relay-41E890 native fault source=%p event=0x%08X\r\n",
				source, static_cast<unsigned>(event_code));
			return result;
		}
		--g_object_event_route_depth;
		std::uintptr_t source_vtable = 0;
		std::uint32_t source_state_flags = 0;
		bool source_snapshot_available = false;
		__try
		{
			if (source)
			{
				source_vtable = *reinterpret_cast<std::uintptr_t*>(source);
				source_state_flags = *reinterpret_cast<std::uint32_t*>(
					reinterpret_cast<BYTE*>(source) + 0x110);
				source_snapshot_available = true;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			source_snapshot_available = false;
		}
		if ((static_cast<std::uint32_t>(event_code) & 0xFF000000u) ==
			0x41000000u && ClaimObjectDiagnosticTrace(1u,
				static_cast<std::uint32_t>(event_code), caller_return_address,
				source_vtable))
		{
			if (source_snapshot_available)
			{
				char source_class_name[96] = {};
				const char* const source_class =
					TryDescribeObjectRttiName(source_vtable, source_class_name,
						sizeof(source_class_name)) ? source_class_name : "unknown";
				CoopRuntime::Instance().Log(
					"[object-event] route=relay-41E890 caller=0x%08X source=%p class=%s vtable=0x%08X state_flags=0x%08X event=0x%08X result=%d\r\n",
					static_cast<unsigned>(caller_return_address), source,
					source_class, static_cast<unsigned>(source_vtable), source_state_flags,
					static_cast<unsigned>(event_code), result);
			}
			else
			{
				CoopRuntime::Instance().Log(
					"[object-event] route=relay-41E890 caller=0x%08X source=%p identity=unavailable event=0x%08X result=%d\r\n",
					static_cast<unsigned>(caller_return_address), source,
					static_cast<unsigned>(event_code), result);
			}
		}
		if (result != 0 && is_outermost_route && source_snapshot_available &&
			!IsRemoteObjectEventReplayActive() &&
			(static_cast<std::uint32_t>(event_code) & 0xFF000000u) ==
				0x41000000u)
		{
			WorldSync::Instance().QueueProgressionRally(source,
				static_cast<std::uint32_t>(source_vtable), event_code);
			// This route accepts only registered non-entity map templates. Nested
			// relays and `sub_46D6F0` are part of this exact call and are recreated
			// by the peer's relay; neither is queued a second time.
			WorldSync::Instance().QueueObjectEvent(source,
				static_cast<std::uint32_t>(source_vtable), event_code,
				protocol::kWorldObjectEventRouteRelay);
		}
		return result;
	}

	int CoopNetGame::HandleObjectEventForwarder(void* receiver, void* object,
		int event_code,
		std::uintptr_t caller_return_address)
	{
		if (!m_original_object_event_forwarder)
			return 0;
		const bool is_outermost_route = !IsObjectEventRouteNested();
		const retail::TriggerView counter_view(
			retail::TriggerRef{ retail::ToAddress(object) });
		retail::TriggerCounterState counter_before = {};
		const bool have_counter_before = counter_view.ReadCounterState(counter_before);
		int result = 0;
		++g_object_event_route_depth;
		__try
		{
			result = m_original_object_event_forwarder(receiver, object, event_code);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			--g_object_event_route_depth;
			CoopRuntime::Instance().Log(
				"[object-event] route=forwarder-46D6F0 native fault object=%p event=0x%08X\r\n",
				object, static_cast<unsigned>(event_code));
			return result;
		}
		--g_object_event_route_depth;
		retail::TriggerCounterState counter_after = {};
		if (have_counter_before && counter_view.ReadCounterState(counter_after))
		{
			CoopRuntime::Instance().Log(
				"[world-counter] origin=%s caller=%08X object=%p event=%08X "
				"value=%u->%u threshold=%d state=%08X->%08X result=%d\r\n",
				IsRemoteObjectEventReplayActive() ? "peer-replay" : "native",
				static_cast<unsigned>(caller_return_address), object,
				static_cast<unsigned>(event_code),
				static_cast<unsigned>(counter_before.value),
				static_cast<unsigned>(counter_after.value), counter_after.threshold,
				counter_before.state_flags, counter_after.state_flags, result);
		}
		std::uintptr_t object_vtable = 0;
		std::uint32_t object_state_flags = 0;
		bool object_snapshot_available = false;
		__try
		{
			if (object)
			{
				object_vtable = *reinterpret_cast<std::uintptr_t*>(object);
				object_state_flags = *reinterpret_cast<std::uint32_t*>(
					reinterpret_cast<BYTE*>(object) + 0x110);
				object_snapshot_available = true;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			object_snapshot_available = false;
		}
		if ((static_cast<std::uint32_t>(event_code) & 0xFF000000u) ==
			0x41000000u && ClaimObjectDiagnosticTrace(2u,
				static_cast<std::uint32_t>(event_code), caller_return_address,
				object_vtable))
		{
			if (object_snapshot_available)
			{
				char object_class_name[96] = {};
				const char* const object_class =
					TryDescribeObjectRttiName(object_vtable, object_class_name,
						sizeof(object_class_name)) ? object_class_name : "unknown";
				CoopRuntime::Instance().Log(
					"[object-event] route=forwarder-46D6F0 caller=0x%08X receiver=%p object=%p class=%s vtable=0x%08X state_flags=0x%08X event=0x%08X result=%d\r\n",
					static_cast<unsigned>(caller_return_address), receiver, object,
					object_class, static_cast<unsigned>(object_vtable), object_state_flags,
					static_cast<unsigned>(event_code), result);
			}
			else
			{
				CoopRuntime::Instance().Log(
					"[object-event] route=forwarder-46D6F0 caller=0x%08X receiver=%p object=%p identity=unavailable event=0x%08X result=%d\r\n",
					static_cast<unsigned>(caller_return_address), receiver, object,
					static_cast<unsigned>(event_code), result);
			}
		}
		if (result != 0 && object_snapshot_available &&
			!IsRemoteObjectEventReplayActive() &&
			is_outermost_route &&
			(static_cast<std::uint32_t>(event_code) & 0xFF000000u) ==
				0x41000000u)
		{
			WorldSync::Instance().QueueProgressionRally(object,
				static_cast<std::uint32_t>(object_vtable), event_code);
			if (IsCanonicalObjectEventReceiver(receiver))
			{
				WorldSync::Instance().QueueObjectEvent(object,
					static_cast<std::uint32_t>(object_vtable), event_code,
					protocol::kWorldObjectEventRouteForwarder);
			}
			else
			{
				CoopRuntime::Instance().Log(
					"[world-object] direct forwarder skipped noncanonical receiver=%p object=%p event=%08X\r\n",
					receiver, object, static_cast<unsigned>(event_code));
			}
		}
		return result;
	}

	int CoopNetGame::HandleTriggerEvent(void* trigger, int event_code)
	{
		if (!m_original_trigger_event || !trigger)
			return 0;
		const bool local_fly_event = IsLocalFlyControlled() &&
			(static_cast<std::uint32_t>(event_code) & 0xFFFF0000u) == 0x41080000u;
		const bool fly_shadow_event = IsFlyNativeAbilityPassActiveOnThisThread();
		const char* const event_source = IsRemoteObjectEventReplayActive() ?
			"peer-replay" : fly_shadow_event ? "fly-shadow" :
			(local_fly_event ? "fly-local" : "game");
		const retail::TriggerRef pre_trigger_ref = { retail::ToAddress(trigger) };
		const retail::TriggerView pre_trigger_view(pre_trigger_ref);
		retail::TriggerIdentity pre_identity = {};
		const bool pre_identity_available = pre_trigger_view.Identity(pre_identity);
		const bool pre_is_npc_or_monster = pre_identity_available &&
			(pre_identity.family == kMonsterTriggerFamily ||
				pre_identity.family == kNpcTriggerFamily);
		const bool contextual_entity_event = pre_is_npc_or_monster &&
			(static_cast<std::uint32_t>(event_code) & 0xFF000000u) ==
				0x41000000u;
		// A client sends the host a request for the canonical world id, but it also
		// continues through its own retail dispatcher. The resulting local entity is
		// later linked by signature; the host's route-3 notice is deliberately not
		// replayed here, so it cannot become a second client spawn.
		if (contextual_entity_event && IsClient() && HasRemotePeer())
		{
			std::uint32_t trigger_vtable = 0;
			if (TryReadObjectVtable(trigger, trigger_vtable) &&
				WorldSync::Instance().QueueObjectEvent(trigger, trigger_vtable,
					event_code,
					protocol::kWorldObjectEventRouteEntityTriggerRequest))
			{
				CoopRuntime::Instance().Log(
					"[world-entity-trigger] client request queued trigger=%p event=%08X; native dispatcher continues\r\n",
					trigger, static_cast<unsigned>(event_code));
			}
		}
		int result = 0;
		__try
		{
			result = m_original_trigger_event(trigger, event_code);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log(
				"[world-trigger-event] native dispatcher fault event=%d\r\n",
				event_code);
			return result;
		}

		WorldSync::Instance().RecordTriggerEvent(trigger, event_code);
		if (result != 0 && !IsRemoteObjectEventReplayActive())
		{
			std::uint32_t trigger_vtable = 0;
			if (TryReadObjectVtable(trigger, trigger_vtable))
			{
				WorldSync::Instance().QueueProgressionRally(trigger,
					trigger_vtable, event_code);
			}
		}
		const retail::TriggerRef trigger_ref = { retail::ToAddress(trigger) };
		const retail::TriggerView trigger_view(trigger_ref);
		retail::TriggerIdentity identity = {};
		if (!trigger_view.Identity(identity))
		{
			CoopRuntime::Instance().Log(
				"[trigger-activation] source=%s trigger=%p event=0x%08X result=%d identity=unavailable\r\n",
				event_source, trigger, static_cast<unsigned>(event_code), result);
			return result;
		}

		std::uint32_t flags = 0;
		const bool have_flags = trigger_view.Flags(flags);
		retail::Transform transform = {};
		const bool have_transform = trigger_view.ReadTransform(transform) &&
			IsFiniteRetailTransform(transform);
		if (have_transform)
		{
			CoopRuntime::Instance().Log(
				"[trigger-activation] source=%s trigger=%p event=0x%08X result=%d family=%08X subtype=%08X definition=%d flags=%08X flags_ok=%u pos=(%.2f,%.2f,%.2f)\r\n",
				event_source, trigger, static_cast<unsigned>(event_code), result,
				identity.family, identity.subtype, identity.definition_id, flags,
				have_flags ? 1u : 0u, transform.position.x, transform.position.y,
				transform.position.z);
		}
		else
		{
			CoopRuntime::Instance().Log(
				"[trigger-activation] source=%s trigger=%p event=0x%08X result=%d family=%08X subtype=%08X definition=%d flags=%08X flags_ok=%u pos=unavailable\r\n",
				event_source, trigger, static_cast<unsigned>(event_code), result,
				identity.family, identity.subtype, identity.definition_id, flags,
				have_flags ? 1u : 0u);
		}

		// Observation is useful before a client connects; packet queueing and
		// HP authority still require an actual peer.
		if (!IsHost() && !IsClient())
			return result;
		if (!HasRemotePeer())
			return result;

		const bool is_npc_or_monster = identity.family == kMonsterTriggerFamily ||
			identity.family == kNpcTriggerFamily;
		if (!is_npc_or_monster)
		{
			WorldSync::TriggerKey key = {};
			key.family = identity.family;
			key.subtype = identity.subtype;
			key.definition_id = identity.definition_id;
			key.occurrence = WorldSync::Instance().LocalOccurrence(trigger);
			WorldSync::Instance().QueueTriggerEvent(key, event_code, result);
		}
		else
		{
			// The host runs an entity trigger once, then sends the exact trigger
			// fingerprint and event to the client. A client-originated activation also
			// runs locally; its entity remains unassigned until WorldSpawn links it.
			if (IsHost())
			{
				std::uint32_t trigger_vtable = 0;
				if (TryReadObjectVtable(trigger, trigger_vtable))
				{
					WorldSync::Instance().QueueObjectEvent(trigger, trigger_vtable,
						event_code,
						protocol::kWorldObjectEventRouteEntityTriggerActivation);
				}
			}
			void* const linked_entity = WorldSync::Instance().EntityOfTrigger(trigger);
			if (linked_entity)
				WorldSync::Instance().ReportLocalDamage(linked_entity, event_code);
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

	bool CoopNetGame::DispatchWorldTriggerEvent(void* trigger, int event_code)
	{
		if (!trigger || !m_original_trigger_event)
			return false;
		HandleTriggerEvent(trigger, event_code);
		return true;
	}

	bool CoopNetGame::ReplayTriggerEvent(std::uint32_t family,
		std::uint32_t subtype, std::int32_t definition_id,
		std::uint32_t occurrence,
		int event_code)
	{
		(void)occurrence;
		if (family == kMonsterTriggerFamily || family == kNpcTriggerFamily)
		{
			CoopRuntime::Instance().Log(
				"[world-trigger-event] entity dispatcher replay rejected family=%08X subtype=%08X def=%d event=%d\r\n",
				family, subtype, definition_id, event_code);
			return true;
		}
		// The client replays a host trigger event on its own matching template.
		// It is intentionally not used for NPC/monster children; those entities
		// synchronize through WorldSpawn/WorldSnapshot/WorldDamage instead.
		void* const template_trigger = WorldSync::Instance().FindTemplateTrigger(
			family, subtype, definition_id);
		if (!template_trigger)
			return false;
		__try
		{
			if (m_original_trigger_event)
			{
				m_original_trigger_event(template_trigger, event_code);
				return true;
			}
			return SpawnWorldFromTrigger(template_trigger);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log(
				"[world-trigger-event] native replay fault family=%08X subtype=%08X definition=%d event=%d\r\n",
				family, subtype, definition_id, event_code);
		}
		return false;
	}

	bool CoopNetGame::ReplayObjectEvent(void* source, int event_code,
		std::uint32_t route)
	{
		if (!source)
			return false;
		const bool relay_route = route == protocol::kWorldObjectEventRouteRelay;
		void* receiver = nullptr;
		if (relay_route)
		{
			if (!m_original_object_event_relay)
				return false;
		}
		else if (route == protocol::kWorldObjectEventRouteForwarder)
		{
			if (!m_original_object_event_forwarder ||
				!GetObjectEventReceiver(receiver))
			{
				return false;
			}
		}
		else
		{
			return false;
		}
		int result = 0;
		++g_remote_object_event_replay_depth;
		++g_object_event_route_depth;
		__try
		{
			if (relay_route)
				result = m_original_object_event_relay(source, event_code);
			else
				result = m_original_object_event_forwarder(receiver, source,
					event_code);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			--g_object_event_route_depth;
			--g_remote_object_event_replay_depth;
			CoopRuntime::Instance().Log(
				"[world-object] native route fault route=%u source=%p event=%08X\r\n",
				route,
				source, static_cast<unsigned>(event_code));
			return false;
		}
		--g_object_event_route_depth;
		--g_remote_object_event_replay_depth;
		CoopRuntime::Instance().Log(
			"[world-object] native route result route=%u source=%p event=%08X result=%d\r\n",
			route, source, static_cast<unsigned>(event_code), result);
		// Match the sender's admission rule: a zero retail result is a rejected
		// object action, not a successfully applied remote event.
		return result != 0;
	}

	void CoopNetGame::ApplyRemoteDamage(void* trigger, std::uint32_t amount,
		std::uint32_t world_id, int event_code)
	{
		(void)amount;
		(void)world_id;
		if (!trigger || !m_original_trigger_event)
			return;
		// Replay the original trigger event for local scripted side effects.  HP
	// itself is changed by WorldSync through the confirmed handler field.
		__try
		{
			m_original_trigger_event(trigger, event_code);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			CoopRuntime::Instance().Log(
				"[world-damage] native event fault trigger=%p id=%u event=%d\r\n",
				trigger, world_id, event_code);
		}
	}

	void CoopNetGame::HandleHealthComponentSet(void* component,
		float requested_value, std::uint32_t slot, bool notify, void* caller)
	{
		if (!m_original_health_component_set)
			return;

		const PlayerHealthOwner owner_kind = IdentifyPlayerHealthOwner(component);
		const char* const owner = PlayerHealthOwnerName(owner_kind);
		float before = 0.0f;
		const bool tracked = owner_kind != PlayerHealthOwner::None &&
			ReadHealthComponentSlot(component, slot, before);

		// Keep the shared health component untouched. Death suppression belongs at
		// the P2 controller's death-mode transition, not in this generic setter.
		m_original_health_component_set(component, requested_value, slot, notify);
		if (!tracked)
			return;

		float after = before;
		if (!ReadHealthComponentSlot(component, slot, after))
			return;
		CoopRuntime::Instance().Log(
			"[p2-damage-receiver] op=set caller=%p owner=%s component=%p slot=%u before=%.2f requested=%.2f after=%.2f notify=%u\r\n",
			caller, owner, component, static_cast<unsigned>(slot), before,
			requested_value, after, notify ? 1u : 0u);
	}

	void CoopNetGame::HandleHealthComponentAdd(void* component, float delta,
		std::uint32_t slot, void* caller)
	{
		if (!m_original_health_component_add)
			return;

		const PlayerHealthOwner owner_kind = IdentifyPlayerHealthOwner(component);
		const char* const owner = PlayerHealthOwnerName(owner_kind);
		float before = 0.0f;
		const bool tracked = owner_kind != PlayerHealthOwner::None &&
			ReadHealthComponentSlot(component, slot, before);

		m_original_health_component_add(component, delta, slot);
		if (!tracked)
			return;

		float after = before;
		if (!ReadHealthComponentSlot(component, slot, after))
			return;
		CoopRuntime::Instance().Log(
			"[p2-damage-receiver] op=add caller=%p owner=%s component=%p slot=%u before=%.2f delta=%.2f after=%.2f\r\n",
			caller, owner, component, static_cast<unsigned>(slot), before, delta,
			after);
	}

	void CoopNetGame::HandleHealthComponentSubtract(void* component,
		float amount, std::uint32_t slot, void* caller)
	{
		if (!m_original_health_component_subtract)
			return;

		const PlayerHealthOwner owner_kind = IdentifyPlayerHealthOwner(component);
		const char* const owner = PlayerHealthOwnerName(owner_kind);
		float before = 0.0f;
		const bool receiver_is_player2 = owner_kind == PlayerHealthOwner::RemoteP2;
		const bool tracked = owner_kind != PlayerHealthOwner::None &&
			ReadHealthComponentSlot(component, slot, before);
		std::uint32_t world_id = 0;
		void* world_entity = nullptr;
		float world_before = 0.0f;
		const bool tracked_world = owner_kind == PlayerHealthOwner::None &&
			WorldSync::Instance().DescribeTrackedHealthComponent(component, world_id,
				world_entity) && ReadHealthComponentSlot(component, slot, world_before);

		// The player health component is shared by the local P1 gameplay state. P2 is
		// only a replicated remote body, so preserve its hit path but do not subtract
		// the hit from the local P1 pool. P1 and all non-player components retain the
		// original amount.
		const float applied_amount = receiver_is_player2 ? 0.0f : amount;
		m_original_health_component_subtract(component, applied_amount, slot);
		if (tracked_world)
		{
			float world_after = world_before;
			if (ReadHealthComponentSlot(component, slot, world_after))
			{
				CoopRuntime::Instance().Log(
					"[world-native-health] role=%s op=subtract caller=%p id=%u entity=%p slot=%u before=%.2f requested=%.2f applied=%.2f after=%.2f\r\n",
					IsHost() ? "host" : "client", caller, world_id, world_entity,
					static_cast<unsigned>(slot), world_before, amount, applied_amount,
					world_after);
			}
		}
		if (!tracked)
			return;

		float after = before;
		if (!ReadHealthComponentSlot(component, slot, after))
			return;
		CoopRuntime::Instance().Log(
			"[p2-damage-receiver] op=subtract caller=%p owner=%s component=%p slot=%u before=%.2f requested=%.2f applied=%.2f after=%.2f\r\n",
			caller, owner, component, static_cast<unsigned>(slot), before, amount,
			applied_amount, after);
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

		retail::WeaponAmmoItemRef weapon_ref = {};
		weapon_ref.value = retail::ToAddress(weapon_record);
		retail::WeaponAmmoItemView weapon_view(weapon_ref);
		std::uint32_t p2_rounds_before = 0;
		std::uint32_t p1_pool_before = 0;
		std::uint32_t ammo_id = 0xFFFFFFFFu;
		const bool have_p2_rounds = weapon_view.RoundCount(p2_rounds_before);
		const bool have_ammo_id = weapon_view.AmmoId(ammo_id);
		retail::AmmoPoolEntryRef ammo_entry_ref = {};
		bool have_p1_pool_amount = false;
		if (have_ammo_id && ammo_id != 0xFFFFFFFFu)
		{
			// The native resolver remains SEH-guarded inside NativeGameApi; ordinary
			// feature code sees only an opaque entry through the retail view.
			if (retail::NativeGameApi::ResolveAmmoPoolEntry(ammo_id,
				ammo_entry_ref) && ammo_entry_ref)
			{
				const retail::AmmoPoolEntryView ammo_entry_view(ammo_entry_ref);
				have_p1_pool_amount = ammo_entry_view.CurrentAmount(p1_pool_before);
			}
		}

		m_original_weapon_ammo_consume(weapon_record);

		// Preserve only values that were actually read before the stock call. The
		// former raw path restored zero after a failed read, which could turn a
		// transient invalid pointer into an artificial empty magazine or P1 HUD pool.
		const bool p2_rounds_restored = have_p2_rounds &&
			weapon_view.SetRoundCount(p2_rounds_before);
		bool p1_pool_restored = false;
		if (have_p1_pool_amount)
		{
			const retail::AmmoPoolEntryView ammo_entry_view(ammo_entry_ref);
			p1_pool_restored = ammo_entry_view.SetCurrentAmount(p1_pool_before);
		}
		if (!have_p2_rounds)
		{
			CoopRuntime::Instance().Log(
				"[p2-ammo-error] unable to snapshot P2 local round counter; skipped restore\r\n");
		}
		else if (!p2_rounds_restored)
		{
			CoopRuntime::Instance().Log(
				"[p2-ammo-error] unable to restore P2 local round counter\r\n");
		}
		if (have_p1_pool_amount && !p1_pool_restored)
		{
			CoopRuntime::Instance().Log(
				"[p2-ammo-error] unable to restore P1 shared ammo-pool amount\r\n");
		}
		if (p2_rounds_restored && !m_logged_remote_p2_ammo_restore)
		{
			if (p1_pool_restored)
			{
				CoopRuntime::Instance().Log(
					"[p2-ammo] stock consume restored P2=%u; P1 pool ammo=0x%X current=%u\r\n",
					p2_rounds_before, ammo_id, p1_pool_before);
			}
			else
			{
				CoopRuntime::Instance().Log(
					"[p2-ammo] stock consume restored P2=%u; no readable shared pool entry\r\n",
					p2_rounds_before);
			}
			m_logged_remote_p2_ammo_restore = true;
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
		const uint32_t fire_action_index = kFireActionId - kFirstKeyboardActionId;
		if (have_remote && (m_remote_press_edge[fire_action_index] ||
			m_remote_release_edge[fire_action_index]))
		{
			CoopRuntime::Instance().Log(
				"[remote-fire-input] press=%u release=%u held=%u transform=%u weapon=0x%08X fly=%u\r\n",
				m_remote_press_edge[fire_action_index] ? 1u : 0u,
				m_remote_release_edge[fire_action_index] ? 1u : 0u,
				GetActiveRemoteAction(kFireActionId) ? 1u : 0u,
				m_active_remote_input.transform_sequence,
				m_active_remote_input.selected_weapon_type,
				m_active_remote_input.fly_controlled);
		}

		m_remote_input_thread_id = GetCurrentThreadId();
		InterlockedExchange(&m_remote_input_active, 1);
	}

	void CoopNetGame::BeginRemoteAbrFireInput()
	{
		m_abr_attack_predicate_seen_in_scope = false;
		ZeroMemory(&m_active_remote_input, sizeof(m_active_remote_input));
		const bool have_remote = GetRemoteInput(m_active_remote_input);
		ZeroMemory(m_remote_press_edge, sizeof(m_remote_press_edge));
		ZeroMemory(m_remote_release_edge, sizeof(m_remote_release_edge));

		const std::uint32_t fire_index =
			kAbrFireActionId - kFirstKeyboardActionId;
		if (have_remote)
		{
			// Consume every counter while ABR is active so actions performed inside
			// the vehicle cannot replay after Darwin returns. Only Fire is exposed
			// during this vehicle tick.
			for (std::uint32_t index = 0; index < kCoopActionCount; ++index)
			{
				const std::uint8_t press =
					m_active_remote_input.action_press_seq[index];
				const std::uint8_t release =
					m_active_remote_input.action_release_seq[index];
				if (index == fire_index)
				{
					m_remote_press_edge[index] =
						press != m_prev_remote_press_seq[index];
					m_remote_release_edge[index] =
						release != m_prev_remote_release_seq[index];
				}
				m_prev_remote_press_seq[index] = press;
				m_prev_remote_release_seq[index] = release;
			}
		}

		const std::uint32_t fire_word = fire_index / 32u;
		const std::uint32_t fire_bit = 1u << (fire_index % 32u);
		const bool fire_held = have_remote &&
			(m_active_remote_input.action_down[fire_word] & fire_bit) != 0;
		ZeroMemory(m_active_remote_input.action_down,
			sizeof(m_active_remote_input.action_down));
		if (fire_held)
			m_active_remote_input.action_down[fire_word] = fire_bit;
		ZeroMemory(m_active_remote_input.virtual_keys,
			sizeof(m_active_remote_input.virtual_keys));
		ZeroMemory(m_active_remote_input.analog_axis,
			sizeof(m_active_remote_input.analog_axis));
		m_active_remote_input.fly_controlled = 0;
		m_active_remote_input.fly_transform_sequence = 0;

		m_remote_action_held[fire_index] = fire_held;
		if (fire_held && m_remote_hold_start_tick[fire_index] == 0)
			m_remote_hold_start_tick[fire_index] = GetTickCount();
		else if (!fire_held)
			m_remote_hold_start_tick[fire_index] = 0;

		m_remote_input_thread_id = GetCurrentThreadId();
		InterlockedExchange(&m_remote_input_active, 1);
		// ABR's native weapon helper reads XGamePad +0x2774/+0x2780 directly.
		// Apply the peer ray around the whole P2 ABR controller tick, not only the
		// fallback call: the normal dispatcher may now reach the fire block first.
		m_remote_abr_aim_ray_applied = false;
		m_remote_abr_aim_input_manager = {};
		m_remote_abr_saved_aim_ray = {};
		if (have_remote &&
			retail::PrimaryGamePadStore().Read(
				m_remote_abr_aim_input_manager) &&
			m_remote_abr_aim_input_manager)
		{
			m_remote_abr_aim_ray_applied = ApplyActiveRemoteAimRay(
				retail::ToPointer(m_remote_abr_aim_input_manager.value),
				m_remote_abr_saved_aim_ray);
		}
		if (have_remote && (m_remote_press_edge[fire_index] ||
			m_remote_release_edge[fire_index]))
		{
			CoopRuntime::Instance().Log(
				"[abr-fire] remote logical edge press=%u release=%u held=%u ray=%u origin=(%.2f,%.2f,%.2f) direction=(%.3f,%.3f,%.3f)\r\n",
				m_remote_press_edge[fire_index] ? 1u : 0u,
				m_remote_release_edge[fire_index] ? 1u : 0u,
				fire_held ? 1u : 0u,
				m_remote_abr_aim_ray_applied ? 1u : 0u,
				m_active_remote_input.aim_origin[0],
				m_active_remote_input.aim_origin[1],
				m_active_remote_input.aim_origin[2],
				m_active_remote_input.aim_direction[0],
				m_active_remote_input.aim_direction[1],
				m_active_remote_input.aim_direction[2]);
		}
	}

	bool CoopNetGame::RunRemoteAbrFireFallback(void* controller)
	{
		if (!controller || !IsRemoteInputActiveOnThisThread() ||
			!HasActiveRemotePressedEdge(kAbrFireActionId) ||
			m_active_remote_input.player_mode != kAbrModeId ||
			m_abr_attack_predicate_seen_in_scope)
		{
			return false;
		}

		retail::EntitySlotRepository players;
		retail::EntitySlotBinding remote_p2 = {};
		const retail::ControllerRef controller_ref = {
			retail::ToAddress(controller)
		};
		retail::ModeRef abr_mode = {};
		if (!players.GetBinding(retail::EntitySlot::RemoteP2, remote_p2) ||
			remote_p2.controller != controller_ref ||
			!retail::ControllerView(controller_ref).RegisteredMode(
				kAbrModeId, abr_mode))
		{
			return false;
		}

		retail::GamePadRef input_manager = {};
		retail::AimRay saved_ray = {};
		const bool have_input =
			retail::PrimaryGamePadStore().Read(input_manager) && input_manager;
		const bool ray_applied = have_input && ApplyActiveRemoteAimRay(
			retail::ToPointer(input_manager.value), saved_ray);
		CoopRuntime::Instance().Log(
			"[abr-fire] P2 dispatcher skipped outer RDV update; running exact registered mode once ray=%u\r\n",
			ray_applied ? 1u : 0u);
		const bool completed = retail::NativeGameApi::RunGPigRdvModeUpdate(
			abr_mode);
		if (ray_applied)
			RestoreAimRay(retail::ToPointer(input_manager.value), saved_ray);
		CoopRuntime::Instance().Log(
			"[abr-fire] fallback outer RDV update completed=%u predicate=%u\r\n",
			completed ? 1u : 0u,
			m_abr_attack_predicate_seen_in_scope ? 1u : 0u);
		return completed && m_abr_attack_predicate_seen_in_scope;
	}

	void CoopNetGame::EndRemoteInput()
	{
		// This edge is meaningful only while the stock P2 controller is consuming
		// its one coherent remote snapshot. Never let an unconsumed recovery leak
		// into a later local, Fly, menu or load input query.
		m_remote_ledge_release_edge_armed = false;
		m_remote_ledge_release_edge_consumed = false;
		if (m_remote_abr_aim_ray_applied && m_remote_abr_aim_input_manager)
		{
			RestoreAimRay(retail::ToPointer(
				m_remote_abr_aim_input_manager.value),
				m_remote_abr_saved_aim_ray);
		}
		m_remote_abr_aim_ray_applied = false;
		m_remote_abr_aim_input_manager = {};
		m_remote_abr_saved_aim_ray = {};
		InterlockedExchange(&m_remote_input_active, 0);
		m_remote_input_thread_id = 0;
		RestoreKeyboardState();
		ZeroMemory(&m_active_remote_input, sizeof(m_active_remote_input));
		m_active_remote_scan_codes = {};
	}

	void CoopNetGame::ResetForWorldLoad()
	{
		m_remote_p2_weapon_record = nullptr;
		m_abr_attack_predicate_seen_in_scope = false;
		m_remote_abr_aim_ray_applied = false;
		m_remote_abr_aim_input_manager = {};
		m_remote_abr_saved_aim_ray = {};
		m_logged_remote_p2_ammo_restore = false;
		m_remote_gamepad = nullptr;
		m_remote_gamepad_unavailable = false;
		m_logged_remote_gamepad = false;
		m_last_remote_transform_apply_tick = 0;
		AcquireSRWLockExclusive(&m_input_lock);
		m_last_invalid_input_trace_tick = 0;
		m_last_remote_fly_deactivation_suppression_tick = 0;
		ZeroMemory(&m_remote_input, sizeof(m_remote_input));
		ZeroMemory(&m_active_remote_input, sizeof(m_active_remote_input));
		ZeroMemory(&m_local_input, sizeof(m_local_input));
		m_last_accepted_remote_input_sequence = 0;
		m_pending_remote_fly_zero_owner_input_sequence = 0;
		m_local_fly_deactivation_seen = false;
		ReleaseSRWLockExclusive(&m_input_lock);
		ClearFlyAbilityQueues();
		// The prior world's route-item pointers are invalid after a load. Do not
		// attempt to clear their flags; drop both one-tick pulse records instead.
		ZeroMemory(m_remote_fly_laser_route_items,
			sizeof(m_remote_fly_laser_route_items));
		ZeroMemory(m_remote_fly_laser_item_ids,
			sizeof(m_remote_fly_laser_item_ids));
		m_remote_fly_laser_pulse_active = false;
		m_remote_ledge_release_edge_armed = false;
		m_remote_ledge_release_edge_consumed = false;
		ZeroMemory(m_debug_fly_laser_route_items,
			sizeof(m_debug_fly_laser_route_items));
		ZeroMemory(m_debug_fly_laser_item_ids,
			sizeof(m_debug_fly_laser_item_ids));
		m_debug_fly_laser_pulse_active = false;
		ZeroMemory(m_prev_remote_press_seq, sizeof(m_prev_remote_press_seq));
		ZeroMemory(m_prev_remote_release_seq, sizeof(m_prev_remote_release_seq));
		ZeroMemory(m_prev_remote_fly_raw_press_seq,
			sizeof(m_prev_remote_fly_raw_press_seq));
		ZeroMemory(m_prev_remote_fly_raw_release_seq,
			sizeof(m_prev_remote_fly_raw_release_seq));
		ZeroMemory(m_remote_press_edge, sizeof(m_remote_press_edge));
		ZeroMemory(m_remote_release_edge, sizeof(m_remote_release_edge));
		ZeroMemory(m_remote_action_held, sizeof(m_remote_action_held));
		ZeroMemory(m_remote_hold_start_tick, sizeof(m_remote_hold_start_tick));
		InterlockedExchange(&m_remote_input_active, 0);
		m_remote_input_thread_id = 0;
	}

	void CoopNetGame::ArmRemoteP2AmmoOwner(void* player2)
	{
		m_remote_p2_weapon_record = nullptr;
		if (!player2 || !IsRemoteInputActiveOnThisThread())
			return;

		const retail::EntityRef player2_ref = { retail::ToAddress(player2) };
		retail::HandlerRef handler_ref = {};
		if (!retail::EntityView(player2_ref).Handler(handler_ref))
			return;
		retail::InventoryRef inventory_ref = {};
		if (!retail::HandlerView(handler_ref).Inventory(inventory_ref))
			return;

		// The stock ammo HUD passes the inventory pointer stored in the handler to
		// 0x5933E0. `InventoryRef` preserves that ownership distinction and avoids
		// accidentally passing the address of the handler field itself.
		std::uint32_t selected_weapon_type = 0xFFFFFFFFu;
		retail::HandlerView(handler_ref).SelectedWeaponType(selected_weapon_type);

		std::uint32_t item_id = 0xFFFFFFFFu;
		retail::WeaponAmmoItemRef weapon_record = {};
		if (retail::NativeGameApi::CurrentWeaponId(handler_ref, item_id) &&
			item_id != 0xFFFFFFFFu)
		{
			retail::NativeGameApi::ResolveWeaponRecord(inventory_ref, item_id,
				weapon_record);
		}
		if (!weapon_record)
		{
			// During draw the native current-item getter can still report melee, while
			// the handler names the selected gun. Use the same type -> item mapping as
			// the stock ammo HUD, then resolve it through P2's inventory boundary.
			if (selected_weapon_type != 0xFFFFFFFFu &&
				retail::NativeGameApi::WeaponTypeToItemId(selected_weapon_type, item_id) &&
				item_id != 0xFFFFFFFFu)
			{
				retail::NativeGameApi::ResolveWeaponRecord(inventory_ref, item_id,
					weapon_record);
			}
		}
		if (weapon_record)
			m_remote_p2_weapon_record = retail::ToPointer(weapon_record.value);
	}


	void CoopNetGame::BuildRemoteScanCodeState()
	{
		m_active_remote_scan_codes = {};
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
				m_active_remote_scan_codes.bytes[scan_code] = 0x80;
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

		retail::KeyboardStateStore keyboard_state_store;
		retail::KeyboardStateBuffers buffers = {};
		if (!keyboard_state_store.ReadBuffers(buffers) ||
			!keyboard_state_store.Snapshot(buffers, m_saved_keyboard_state,
				m_saved_keyboard_state_secondary))
			return;

		// Save the resolved addresses before the first replacement write, so a
		// failed second write can restore whichever retail byte block changed.
		m_keyboard_state_buffers = buffers;
		m_keyboard_state_swapped = true;
		if (!keyboard_state_store.Replace(m_keyboard_state_buffers,
			m_active_remote_scan_codes))
		{
			RestoreKeyboardState();
			return;
		}

		// The action edge/hold path (0x488DC0) reads the second DirectInput
		// array. Replacing only +0x04 lets some actions through but leaves GPig
		// locomotion in idle.
		if (!m_logged_keyboard_state_swap)
		{
			CoopRuntime::Instance().Log(
				"[netgame] DirectInput keyboard snapshot override active for P2\r\n");
			m_logged_keyboard_state_swap = true;
		}
	}

	void CoopNetGame::RestoreKeyboardState()
	{
		if (!m_keyboard_state_swapped)
			return;

		const retail::KeyboardStateBuffers buffers = m_keyboard_state_buffers;
		m_keyboard_state_buffers = {};
		m_keyboard_state_swapped = false;
		retail::KeyboardStateStore().Restore(buffers, m_saved_keyboard_state,
			m_saved_keyboard_state_secondary);
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
		void*, std::uint32_t device, std::uint32_t action, std::uint32_t flags,
		std::uintptr_t)
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
			// The remote snapshot must not make Darwin fire while the peer owns
			// Mooch. The receiver keeps its own controller/camera on P1.
			if (action == kFireActionId && IsRemoteFlyControlledForInputQuery())
				return false;
			return GetActiveRemoteAction(action);
		}

		if (is_keyboard_action && IsMoochAction(action) &&
			IsRemoteFlyControlledForInputQuery())
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
			return action == kFireActionId && IsRemoteFlyControlledForInputQuery() ? true :
				!GetActiveRemoteAction(action);
		}
		if (is_keyboard_action && IsMoochAction(action) &&
			IsRemoteFlyControlledForInputQuery())
		{
			return true;
		}

		return m_original_input_action_up_query(input_manager, device, action,
			flags);
	}

	bool __fastcall CoopNetGame::HandleInputPressedQuery(void* input_manager,
		void*, std::uint32_t device, std::uint32_t action, std::uint32_t flags,
		std::uintptr_t caller_return_address)
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
			if (action == kFireActionId && IsRemoteFlyControlledForInputQuery())
				return false;
			if (action == kLedgeReleaseActionId &&
				m_remote_ledge_release_edge_armed)
			{
				// Do not fabricate a scan code or call a Ledge state directly.
				// This merely makes the checked logical press-edge visible during
				// the one stock P2 controller tick that armed it.
				m_remote_ledge_release_edge_consumed = true;
				return true;
			}
			if (IsMirrorSuppressedAction(action))
				return false;
			const uint32_t action_index = action - kFirstKeyboardActionId;
			const bool pressed = m_remote_press_edge[action_index];
			if (pressed && action == kAbrFireActionId &&
				caller_return_address == kAbrFirePressedQueryReturn)
			{
				CoopRuntime::Instance().Log(
					"[abr-fire] native ABR pressed query accepted device=%u flags=0x%08X\r\n",
					device, flags);
			}
			return pressed;
		}
		if (is_keyboard_action && IsMoochAction(action) &&
			IsRemoteFlyControlledForInputQuery())
		{
			// Block remote Darwin from entering Mooch, but let the shared fly's
			// exact native exit query consume this machine's local Mooch action.
			if (caller_return_address != kFlyExitActionQueryReturn)
				return false;
			const bool result = m_original_input_pressed_query(input_manager, device,
				action, flags);
			if (result)
			{
				AcquireSRWLockExclusive(&m_input_lock);
				ClearFlyInputLocked(m_remote_input);
				ClearFlyInputLocked(m_active_remote_input);
				ReleaseSRWLockExclusive(&m_input_lock);
				CoopRuntime::Instance().Log(
					"[fly] local Mooch action forced exit from remotely owned Mooch\r\n");
			}
			return result;
		}

		const bool result = m_original_input_pressed_query(input_manager, device,
			action, flags);
		if (is_keyboard_action && !is_remote_thread && result)
		{
			CaptureLocalPress(action);

			// The two downstream event diagnostics intentionally do not cover every
			// map object.  Record the native rising edge before the object's own
			// callback chooses its route.  The caller is the useful part here: it
			// identifies the exact retail interaction/weapon code that consumed this
			// press, without guessing an object pointer or sending anything to P2.
			if (ClaimLocalInputEdgeTrace(action, caller_return_address, false))
			{
				retail::EntitySlotRepository players;
				retail::EntityRef player1 = {};
				retail::Transform player_transform = {};
				const bool have_player_position = players.Get(retail::EntitySlot::LocalP1,
					player1) && retail::EntityView(player1).ReadTransform(player_transform) &&
					IsFiniteRetailTransform(player_transform);
				if (have_player_position)
				{
					CoopRuntime::Instance().Log(
						"[input-edge-local] kind=logical action=0x%08X caller=0x%08X device=%u flags=0x%08X p1_pos=(%.2f,%.2f,%.2f)\r\n",
						action, static_cast<unsigned>(caller_return_address), device, flags,
						player_transform.position.x, player_transform.position.y,
						player_transform.position.z);
				}
				else
				{
					CoopRuntime::Instance().Log(
						"[input-edge-local] kind=logical action=0x%08X caller=0x%08X device=%u flags=0x%08X p1_pos=unavailable\r\n",
						action, static_cast<unsigned>(caller_return_address), device, flags);
				}
			}
		}
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
			if (action == kFireActionId && IsRemoteFlyControlledForInputQuery())
				return false;
			if (IsMirrorSuppressedAction(action))
				return false;
			const uint32_t action_index = action - kFirstKeyboardActionId;
			return m_remote_release_edge[action_index];
		}
		if (is_keyboard_action && IsMoochAction(action) &&
			IsRemoteFlyControlledForInputQuery())
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
			if (action == kFireActionId && IsRemoteFlyControlledForInputQuery())
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
			if (action == kFireActionId && IsRemoteFlyControlledForInputQuery())
				return false;
			return GetActiveRemoteHold(action, threshold);
		}

		return m_original_input_aim_hold_query(input_manager, device, action, flags,
			threshold);
	}

	bool __fastcall CoopNetGame::HandleInputRawPressedQuery(void* input_manager,
		void*, void* device, std::uint32_t action, std::uint32_t flags, bool record,
		std::uintptr_t caller_return_address)
	{
		if (!m_original_input_raw_pressed_query)
			return false;

		// 0x40080029 also appears in P1 WeaponTaser code. Only the exact Fly_Active
		// return address is eligible here. A presentation shadow pass is deliberately
		// aim-only: it may inject its explicit reliable laser edge, but never consume
		// an arbitrary remote raw edge (magnet/carry and movement remain unsynced).
		const bool native_pass = IsFlyNativeAbilityPassActiveOnThisThread();
		if (native_pass && IsFlyActiveRawPressedQuery(action,
			caller_return_address))
		{
			const int raw_index = FindFlyRawActionIndex(action);
			const LONG raw_bit = raw_index >= 0 ?
				static_cast<LONG>(1u << static_cast<std::uint32_t>(raw_index)) : 0;
			if (action == kFlyDualLaserRawActionId && raw_bit != 0 &&
				(InterlockedCompareExchange(&m_fly_native_synthetic_press_mask,
					0, 0) & raw_bit) != 0)
			{
				InterlockedAnd(&m_fly_native_synthetic_press_mask, ~raw_bit);
				return true;
			}
			return false;
		}
		if (IsFlyActiveRawPressedQuery(action, caller_return_address) &&
			!IsLocalFlyControlled() && IsRemoteFlyControlled())
		{
			return false;
		}

		const bool result = m_original_input_raw_pressed_query(input_manager,
			device, action, flags, record);
		if (result)
		{
			// Fly actions use the raw input route, not the logical action route
			// above.  Keep an upstream local trace here as well so a tested button
			// can be tied to its real consumer even when no TriggerEventDispatcher
			// or GlobalEventForwarder event follows it.  Scoped F1/receiver passes
			// are synthetic and deliberately omitted from this observation.
			const bool is_known_fly_dual_laser_query =
				action == kFlyDualLaserRawActionId &&
				caller_return_address == kFlyDualLaserRawPressedQueryReturn;
			if (!native_pass && !IsRemoteInputActiveOnThisThread() &&
				!is_known_fly_dual_laser_query &&
				ClaimLocalInputEdgeTrace(action, caller_return_address, true))
			{
				retail::EntitySlotRepository players;
				retail::EntityRef player1 = {};
				retail::Transform player_transform = {};
				const bool have_player_position = players.Get(retail::EntitySlot::LocalP1,
					player1) && retail::EntityView(player1).ReadTransform(player_transform) &&
					IsFiniteRetailTransform(player_transform);
				if (have_player_position)
				{
					CoopRuntime::Instance().Log(
						"[input-edge-local] kind=raw action=0x%08X caller=0x%08X device=%p flags=0x%08X record=%u p1_pos=(%.2f,%.2f,%.2f)\r\n",
						action, static_cast<unsigned>(caller_return_address), device, flags,
						record ? 1u : 0u, player_transform.position.x,
						player_transform.position.y, player_transform.position.z);
				}
				else
				{
					CoopRuntime::Instance().Log(
						"[input-edge-local] kind=raw action=0x%08X caller=0x%08X device=%p flags=0x%08X record=%u p1_pos=unavailable\r\n",
						action, static_cast<unsigned>(caller_return_address), device, flags,
						record ? 1u : 0u);
				}
			}
			CaptureLocalFlyRaw(action, true, true, false);
			if (!native_pass && action == kFlyDualLaserRawActionId &&
				caller_return_address == kFlyDualLaserRawPressedQueryReturn &&
				IsLocalFlyControlled())
			{
				QueueLocalFlyDualLaserEvent(input_manager);
			}
		}
		return result;
	}

	bool __fastcall CoopNetGame::HandleInputRawReleasedQuery(void* input_manager,
		void*, void* device, std::uint32_t action, std::uint32_t flags, bool record,
		std::uintptr_t)
	{
		if (!m_original_input_raw_released_query)
			return false;

		const bool result = m_original_input_raw_released_query(input_manager,
			device, action, flags, record);
		if (result)
			CaptureLocalFlyRaw(action, false, false, true);
		return result;
	}

	bool __fastcall CoopNetGame::HandleInputRawHeldQuery(void* input_manager,
		void*, void* device, std::uint32_t action, std::uint32_t flags, bool record,
		std::uintptr_t caller_return_address)
	{
		if (!m_original_input_raw_held_query)
			return false;

		// A native presentation pass never inherits held Fly actions. Its one
		// supported action is the explicit reliable dual-laser press above; routing
		// carry/magnet or locomotion through this pass would make the receiver a
		// second owner instead of a visual/motor mirror.
		const bool native_pass = IsFlyNativeAbilityPassActiveOnThisThread();
		if (native_pass && IsFlyActiveRawHeldQuery(action,
			caller_return_address))
		{
			return false;
		}
		if (IsFlyActiveRawHeldQuery(action, caller_return_address) &&
			!IsLocalFlyControlled() && IsRemoteFlyControlled())
		{
			return false;
		}

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
			if (action == kFireActionId && IsRemoteFlyControlledForInputQuery())
				return false;
			return GetActiveRemoteHold(action, threshold);
		}

		const bool result = m_original_input_threshold_query(input_manager, device,
			action, threshold, flags);
		// 0x488DC0 is likewise a time/threshold gate.  Never overwrite the
		// packet's held level with its result; a threshold passing is not a key-down.
		return result;
	}

	float CoopNetGame::GetRemoteAnalogAxis(std::uint32_t axis) const
	{
		return axis < kCoopInputAnalogAxisCount ?
			m_active_remote_input.analog_axis[axis] : 0.0f;
	}

	float __fastcall CoopNetGame::HandleInputAxisQuery(void* input_manager,
		void*, std::uint32_t device, std::uint32_t axis, std::uint32_t flags)
	{
		if (!m_original_input_axis_query)
			return 0.0f;

		const bool remote_input_active = IsRemoteInputActiveOnThisThread();
		if (IsFlyNativeAbilityPassActiveOnThisThread() &&
			axis < kCoopInputAnalogAxisCount)
		{
			// The shadow pass supplies camera yaw/pitch and an aim ray, which are the
			// Fly Aim motor inputs. Do not replay axes: root movement is replicated
			// separately and must not be simulated a second time on this process.
			return 0.0f;
		}
		if (remote_input_active && axis < kCoopInputAnalogAxisCount &&
			(axis < 2 || m_active_remote_input.fly_controlled != 0))
		{
			// Normal GPig paths use axes 0/1. Fly_Active additionally consumes
			// axes 2/3, so a remote-owned Fly must see the full packet snapshot
			// rather than this process's physical pad.
			return GetRemoteAnalogAxis(axis);
		}

		const float value = m_original_input_axis_query(input_manager, device,
			axis, flags);
		// Device zero is the normal P1 capture route.  Fly_Active may ask its
		// registered device instead, so local Fly ownership deliberately captures
		// all four returned axes regardless of that device selector.
		if (!remote_input_active && axis < kCoopInputAnalogAxisCount &&
			(device == 0 || IsLocalFlyControlled()))
		{
			CaptureLocalAnalogAxis(axis, value);
		}
		return value;
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
		// P2 and a remote-owned Fly both run inside a short packet snapshot scope.
		// Local P1 and a locally owned Fly keep the real shared camera handler.
		if (IsRemoteInputActiveOnThisThread())
		{
			float remote_yaw = 0.0f;
			if (GetActiveRemoteCameraYaw(remote_yaw))
			{
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
		// handler in the process.  A remote P2/remote Fly must never drive it, and
		// after P1 has handed control to locally owned Mooch its Default mode must
		// not re-centre the same handler on Darwin every frame. Mooch owns its own
		// stock camera path; no fly yaw or position is manufactured here.
		if (IsRemoteInputActiveOnThisThread() || IsLocalFlyControlled())
			return;
		m_original_gpig_camera_update(mode);
	}

	bool CoopNetGame::InstallActionQueryHook()
	{
		if (m_action_query_hooked)
			return true;
		if (!InstallJmpHookRaw(kInputActionQuery, kExpectedInputActionQuery,
			sizeof(kExpectedInputActionQuery),
			reinterpret_cast<void*>(&HookInputActionQuery),
			m_original_input_action_query_bytes, &m_input_action_trampoline,
			"packet-backed action-query"))
		{
			return false;
		}
		m_original_input_action_query =
			reinterpret_cast<InputActionQueryFn>(m_input_action_trampoline);
		m_action_query_hooked = true;
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
		if (!InstallJmpHookRaw(kInputAxisQuery, kExpectedInputAxisQuery,
			sizeof(kExpectedInputAxisQuery),
			reinterpret_cast<void*>(&HookInputAxisQuery),
			m_original_input_axis_query_bytes, &m_input_axis_trampoline,
			"packet-backed motor-axis"))
		{
			return false;
		}
		m_original_input_axis_query =
			reinterpret_cast<InputAxisQueryFn>(m_input_axis_trampoline);
		m_axis_query_hooked = true;
		return true;
	}
	bool CoopNetGame::InstallThresholdQueryHook()
	{
		if (m_threshold_query_hooked)
			return true;
		if (!InstallJmpHookRaw(kInputThresholdQuery, kExpectedInputThresholdQuery,
			sizeof(kExpectedInputThresholdQuery),
			reinterpret_cast<void*>(&HookInputThresholdQuery),
			m_original_input_threshold_query_bytes, &m_input_threshold_trampoline,
			"packet-backed threshold-query"))
		{
			return false;
		}
		m_original_input_threshold_query =
			reinterpret_cast<InputThresholdQueryFn>(m_input_threshold_trampoline);
		m_threshold_query_hooked = true;
		return true;
	}
	void CoopNetGame::RemoveActionQueryHook()
	{
		if (!m_action_query_hooked)
			return;
		RemoveJmpHookRaw(kInputActionQuery, m_original_input_action_query_bytes,
			sizeof(m_original_input_action_query_bytes), &m_input_action_trampoline);
		m_original_input_action_query = nullptr;
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
		m_original_input_action_up_query = nullptr;
		m_action_up_query_hooked = false;
	}

	void CoopNetGame::RemoveThresholdQueryHook()
	{
		if (!m_threshold_query_hooked)
			return;
		RemoveJmpHookRaw(kInputThresholdQuery, m_original_input_threshold_query_bytes,
			sizeof(m_original_input_threshold_query_bytes),
			&m_input_threshold_trampoline);
		m_original_input_threshold_query = nullptr;
		m_threshold_query_hooked = false;
	}
	void CoopNetGame::RemoveAxisQueryHook()
	{
		if (!m_axis_query_hooked)
			return;
		RemoveJmpHookRaw(kInputAxisQuery, m_original_input_axis_query_bytes,
			sizeof(m_original_input_axis_query_bytes), &m_input_axis_trampoline);
		m_original_input_axis_query = nullptr;
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

		BYTE* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, relocate_len + 5,
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
		BYTE patch[5] = { 0xE9, 0, 0, 0, 0 };
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
		*trampoline_ptr = nullptr;
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
		m_original_input_pressed_query = nullptr;
		m_pressed_query_hooked = false;
	}

	bool CoopNetGame::InstallAbrAttackPredicateHook()
	{
		if (m_abr_attack_predicate_hooked)
			return true;
		if (!InstallJmpHookRaw(kAbrAttackPredicate,
			kExpectedAbrAttackPredicate, sizeof(kExpectedAbrAttackPredicate),
			reinterpret_cast<void*>(&HookAbrAttackPredicate),
			m_original_abr_attack_predicate_bytes,
			&m_abr_attack_predicate_trampoline,
			"RemoteP2 ABR attack predicate"))
		{
			return false;
		}
		m_original_abr_attack_predicate =
			reinterpret_cast<AbrAttackPredicateFn>(
				m_abr_attack_predicate_trampoline);
		m_abr_attack_predicate_hooked = true;
		return true;
	}

	void CoopNetGame::RemoveAbrAttackPredicateHook()
	{
		if (!m_abr_attack_predicate_hooked)
			return;
		RemoveJmpHookRaw(kAbrAttackPredicate,
			m_original_abr_attack_predicate_bytes,
			sizeof(m_original_abr_attack_predicate_bytes),
			&m_abr_attack_predicate_trampoline);
		m_original_abr_attack_predicate = nullptr;
		m_abr_attack_predicate_hooked = false;
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
		m_original_input_released_query = nullptr;
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
		m_original_input_hold_duration_query = nullptr;
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
		m_original_input_aim_hold_query = nullptr;
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
		m_original_input_raw_pressed_query = nullptr;
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
		m_original_input_raw_released_query = nullptr;
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
		m_original_input_raw_held_query = nullptr;
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
		m_original_camera_yaw = nullptr;
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
		m_original_gpig_camera_update = nullptr;
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
		m_original_default_mode_update = nullptr;
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
		m_original_fire_handler = nullptr;
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
		m_original_weapon_ammo_consume = nullptr;
		m_weapon_ammo_consume_hooked = false;
	}

	bool CoopNetGame::InstallHealthComponentSetHook()
	{
		if (m_health_component_set_hooked)
			return true;
		if (!InstallJmpHookRaw(kHealthComponentSet, kExpectedHealthComponentSet,
			sizeof(kExpectedHealthComponentSet),
			reinterpret_cast<void*>(&HookHealthComponentSet),
			m_original_health_component_set_bytes,
			&m_health_component_set_trampoline, "P1/P2 health-set trace"))
		{
			return false;
		}
		m_original_health_component_set = reinterpret_cast<HealthComponentSetFn>(
			m_health_component_set_trampoline);
		m_health_component_set_hooked = true;
		return true;
	}

	void CoopNetGame::RemoveHealthComponentSetHook()
	{
		if (!m_health_component_set_hooked)
			return;
		RemoveJmpHookRaw(kHealthComponentSet, m_original_health_component_set_bytes,
			sizeof(m_original_health_component_set_bytes),
			&m_health_component_set_trampoline);
		m_original_health_component_set = nullptr;
		m_health_component_set_hooked = false;
	}

	bool CoopNetGame::InstallHealthComponentAddHook()
	{
		if (m_health_component_add_hooked)
			return true;
		if (!InstallJmpHookRaw(kHealthComponentAdd, kExpectedHealthComponentAdd,
			sizeof(kExpectedHealthComponentAdd),
			reinterpret_cast<void*>(&HookHealthComponentAdd),
			m_original_health_component_add_bytes,
			&m_health_component_add_trampoline, "P1/P2 health-add trace"))
		{
			return false;
		}
		m_original_health_component_add = reinterpret_cast<HealthComponentAddFn>(
			m_health_component_add_trampoline);
		m_health_component_add_hooked = true;
		return true;
	}

	void CoopNetGame::RemoveHealthComponentAddHook()
	{
		if (!m_health_component_add_hooked)
			return;
		RemoveJmpHookRaw(kHealthComponentAdd, m_original_health_component_add_bytes,
			sizeof(m_original_health_component_add_bytes),
			&m_health_component_add_trampoline);
		m_original_health_component_add = nullptr;
		m_health_component_add_hooked = false;
	}

	bool CoopNetGame::InstallHealthComponentSubtractHook()
	{
		if (m_health_component_subtract_hooked)
			return true;
		if (!InstallJmpHookRaw(kHealthComponentSubtract,
			kExpectedHealthComponentSubtract,
			sizeof(kExpectedHealthComponentSubtract),
			reinterpret_cast<void*>(&HookHealthComponentSubtract),
			m_original_health_component_subtract_bytes,
			&m_health_component_subtract_trampoline, "P1/P2 health-subtract trace"))
		{
			return false;
		}
		m_original_health_component_subtract =
			reinterpret_cast<HealthComponentSubtractFn>(
				m_health_component_subtract_trampoline);
		m_health_component_subtract_hooked = true;
		return true;
	}

	void CoopNetGame::RemoveHealthComponentSubtractHook()
	{
		if (!m_health_component_subtract_hooked)
			return;
		RemoveJmpHookRaw(kHealthComponentSubtract,
			m_original_health_component_subtract_bytes,
			sizeof(m_original_health_component_subtract_bytes),
			&m_health_component_subtract_trampoline);
		m_original_health_component_subtract = nullptr;
		m_health_component_subtract_hooked = false;
	}

	bool CoopNetGame::InstallTriggerSpawnHook()

	{
		if (m_trigger_spawn_hooked)
			return true;
		if (!InstallJmpHookRaw(kTriggerSpawnFromDefinition,
			kExpectedTriggerSpawnFromDefinition,
			sizeof(kExpectedTriggerSpawnFromDefinition),
			reinterpret_cast<void*>(&HookTriggerSpawnFromDefinition),
			m_original_trigger_spawn_bytes, &m_trigger_spawn_trampoline,
			"NPC/monster spawn registration"))
		{
			return false;
		}
		m_original_trigger_spawn = reinterpret_cast<TriggerSpawnFromDefinitionFn>(
			m_trigger_spawn_trampoline);
		m_trigger_spawn_hooked = true;
		return true;
	}

	void CoopNetGame::RemoveTriggerSpawnHook()
	{
		if (!m_trigger_spawn_hooked)
			return;
		RemoveJmpHookRaw(kTriggerSpawnFromDefinition, m_original_trigger_spawn_bytes,
			sizeof(m_original_trigger_spawn_bytes), &m_trigger_spawn_trampoline);
		m_original_trigger_spawn = nullptr;
		m_trigger_spawn_hooked = false;
	}

	bool CoopNetGame::InstallTriggerFactoryHook()
	{
		if (m_trigger_factory_hooked)
			return true;
		if (!InstallJmpHookRaw(kTriggerFactory, kExpectedTriggerFactory,
			sizeof(kExpectedTriggerFactory),
			reinterpret_cast<void*>(&HookTriggerFactory),
			m_original_trigger_factory_bytes, &m_trigger_factory_trampoline,
			"NPC/monster trigger registration"))
		{
			return false;
		}
		m_original_trigger_factory = reinterpret_cast<TriggerFactoryFn>(
			m_trigger_factory_trampoline);
		m_trigger_factory_hooked = true;
		return true;
	}

	void CoopNetGame::RemoveTriggerFactoryHook()
	{
		if (!m_trigger_factory_hooked)
			return;
		RemoveJmpHookRaw(kTriggerFactory, m_original_trigger_factory_bytes,
			sizeof(m_original_trigger_factory_bytes), &m_trigger_factory_trampoline);
		m_original_trigger_factory = nullptr;
		m_trigger_factory_hooked = false;
	}

	bool CoopNetGame::InstallTriggerEventHook()
	{
		if (m_trigger_event_hooked)
			return true;

		if (!InstallJmpHookRaw(kTriggerEventDispatcher,
			kExpectedTriggerEventDispatcher,
			sizeof(kExpectedTriggerEventDispatcher),
			reinterpret_cast<void*>(&HookTriggerEvent),
			m_original_trigger_event_bytes, &m_trigger_event_trampoline,
			"NPC/monster trigger event"))
		{
			return false;
		}
		m_original_trigger_event = reinterpret_cast<TriggerEventFn>(
			m_trigger_event_trampoline);
		m_trigger_event_hooked = true;

		return true;
	}

	bool CoopNetGame::InstallGlobalEventForwarderHook()
	{
		if (m_global_event_forwarder_hooked)
			return true;
		if (!InstallJmpHookRaw(kGlobalEventForwarder,
			kExpectedGlobalEventForwarder,
			sizeof(kExpectedGlobalEventForwarder),
			reinterpret_cast<void*>(&HookGlobalEventForwarder),
			m_original_global_event_forwarder_bytes,
			&m_global_event_forwarder_trampoline, "global event diagnostic"))
		{
			return false;
		}
		m_original_global_event_forwarder =
			reinterpret_cast<GlobalEventForwarderFn>(
				m_global_event_forwarder_trampoline);
		m_global_event_forwarder_hooked = true;
		return true;
	}

	bool CoopNetGame::InstallObjectEventRelayHook()
	{
		if (m_object_event_relay_hooked)
			return true;
		if (!InstallJmpHookRaw(kObjectEventRelay, kExpectedObjectEventRelay,
			sizeof(kExpectedObjectEventRelay),
			reinterpret_cast<void*>(&HookObjectEventRelay),
			m_original_object_event_relay_bytes,
			&m_object_event_relay_trampoline, "object event relay trace"))
		{
			return false;
		}
		m_original_object_event_relay = reinterpret_cast<ObjectEventRelayFn>(
			m_object_event_relay_trampoline);
		m_object_event_relay_hooked = true;
		return true;
	}

	bool CoopNetGame::InstallObjectEventForwarderHook()
	{
		if (m_object_event_forwarder_hooked)
			return true;
		if (!InstallJmpHookRaw(kObjectEventForwarder,
			kExpectedObjectEventForwarder, sizeof(kExpectedObjectEventForwarder),
			reinterpret_cast<void*>(&HookObjectEventForwarder),
			m_original_object_event_forwarder_bytes,
			&m_object_event_forwarder_trampoline, "object event forwarder trace"))
		{
			return false;
		}
		m_original_object_event_forwarder =
			reinterpret_cast<ObjectEventForwarderFn>(
				m_object_event_forwarder_trampoline);
		m_object_event_forwarder_hooked = true;
		return true;
	}


	bool CoopNetGame::InstallLoadGameHook()
	{
		if (m_load_game_hooked)
			return true;
		// Two retail menu routes call the same stock loader (0x5ED3F0 and
		// 0x5EDC20). Intercept its checked entry so an already connected
		// client receives every host-selected DATA<n> load.
		if (!InstallJmpHookRaw(gforce::kBeginNativeSaveLoad,
			gforce::kExpectedBeginNativeSaveLoad,
			sizeof(m_original_native_save_load_bytes),
			reinterpret_cast<void*>(&HookNativeSaveLoad),
			m_original_native_save_load_bytes,
			&m_native_save_load_trampoline, "native Load Game broadcast"))
			return false;
		m_original_native_save_load =
			reinterpret_cast<NativeSaveLoadFn>(m_native_save_load_trampoline);
		m_load_game_hooked = true;
		CoopRuntime::Instance().Log(
			"[save-sync] native Load Game entry hooked at 0x%08X\r\n",
			static_cast<unsigned>(gforce::kBeginNativeSaveLoad));
		return true;
	}

	void CoopNetGame::RemoveLoadGameHook()
	{
		if (!m_load_game_hooked)
			return;
		RemoveJmpHookRaw(gforce::kBeginNativeSaveLoad,
			m_original_native_save_load_bytes,
			sizeof(m_original_native_save_load_bytes),
			&m_native_save_load_trampoline);
		m_original_native_save_load = nullptr;
		m_load_game_hooked = false;
	}

	void CoopNetGame::RemoveTriggerEventHook()
	{
		if (!m_trigger_event_hooked)
			return;
		RemoveJmpHookRaw(kTriggerEventDispatcher, m_original_trigger_event_bytes,
			sizeof(m_original_trigger_event_bytes), &m_trigger_event_trampoline);
		m_original_trigger_event = nullptr;
		m_trigger_event_hooked = false;
	}

	void CoopNetGame::RemoveGlobalEventForwarderHook()
	{
		if (!m_global_event_forwarder_hooked)
			return;
		RemoveJmpHookRaw(kGlobalEventForwarder,
			m_original_global_event_forwarder_bytes,
			sizeof(m_original_global_event_forwarder_bytes),
			&m_global_event_forwarder_trampoline);
		m_original_global_event_forwarder = nullptr;
		m_global_event_forwarder_hooked = false;
	}

	void CoopNetGame::RemoveObjectEventRelayHook()
	{
		if (!m_object_event_relay_hooked)
			return;
		RemoveJmpHookRaw(kObjectEventRelay, m_original_object_event_relay_bytes,
			sizeof(m_original_object_event_relay_bytes),
			&m_object_event_relay_trampoline);
		m_original_object_event_relay = nullptr;
		m_object_event_relay_hooked = false;
	}

	void CoopNetGame::RemoveObjectEventForwarderHook()
	{
		if (!m_object_event_forwarder_hooked)
			return;
		RemoveJmpHookRaw(kObjectEventForwarder,
			m_original_object_event_forwarder_bytes,
			sizeof(m_original_object_event_forwarder_bytes),
			&m_object_event_forwarder_trampoline);
		m_original_object_event_forwarder = nullptr;
		m_object_event_forwarder_hooked = false;
	}


	bool CoopNetGame::InstallStateMachineSelectStateHook()
	{
		if (m_state_machine_select_state_hooked)
			return true;
		if (!InstallJmpHookRaw(kSelectMode, kExpectedStateMachineSelectState,
			sizeof(kExpectedStateMachineSelectState),
			reinterpret_cast<void*>(&HookStateMachineSelectState),
			m_original_state_machine_select_state_bytes,
			&m_state_machine_select_state_trampoline,
			"remote Mooch lifecycle guard"))
		{
			return false;
		}
		m_original_state_machine_select_state =
			reinterpret_cast<StateMachineSelectStateFn>(
				m_state_machine_select_state_trampoline);
		m_state_machine_select_state_hooked = true;
		return true;
	}

	void CoopNetGame::RemoveStateMachineSelectStateHook()
	{
		if (!m_state_machine_select_state_hooked)
			return;
		RemoveJmpHookRaw(kSelectMode, m_original_state_machine_select_state_bytes,
			sizeof(m_original_state_machine_select_state_bytes),
			&m_state_machine_select_state_trampoline);
		m_original_state_machine_select_state = nullptr;
		m_state_machine_select_state_hooked = false;
	}

	bool CoopNetGame::InstallInputHook()
	{
		if (m_input_hooked)
			return true;
		CoopRuntime::Instance().Log("[netgame] installing input hooks...\r\n");
		BYTE* image = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
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
					InstallAbrAttackPredicateHook() &&
					InstallReleasedQueryHook() && InstallHoldDurationQueryHook() &&
					InstallAimHoldQueryHook() && InstallRawPressedQueryHook() &&
					InstallRawReleasedQueryHook() && InstallRawHeldQueryHook() &&
					InstallCameraYawHook() &&
					InstallGPigCameraUpdateHook() &&
					InstallDefaultModeUpdateHook() && InstallFireHandlerHook() &&
					InstallWeaponAmmoConsumeHook();
				if (!InstallStateMachineSelectStateHook())
				{
					CoopRuntime::Instance().Log(
						"[fly-lifecycle] receiver Deactivated guard unavailable\r\n");
				}
				if (!InstallHealthComponentSetHook() || !InstallHealthComponentAddHook() ||
					!InstallHealthComponentSubtractHook())
				{
					CoopRuntime::Instance().Log(
						"[p2-damage-receiver] health trace unavailable; no gameplay behavior changed\r\n");
				}
				if (!InstallTriggerFactoryHook() ||
					!InstallTriggerSpawnHook() ||
					!InstallTriggerEventHook())

				{
					CoopRuntime::Instance().Log(
						"[world-sync] trigger hooks unavailable; NPC/monster registration is disabled\r\n");
				}
				if (!InstallGlobalEventForwarderHook())
				{
					CoopRuntime::Instance().Log(
						"[global-event] diagnostic hook unavailable; button/object callbacks remain untraced\r\n");
				}
				const bool object_event_relay = InstallObjectEventRelayHook();
				const bool object_event_forwarder = InstallObjectEventForwarderHook();
				if (object_event_relay && object_event_forwarder)
				{
					CoopRuntime::Instance().Log(
						"[object-trace] relay/forwarder hooks installed\r\n");
				}
				else
				{
					CoopRuntime::Instance().Log(
						"[object-trace] relay or forwarder hook unavailable; see byte-mismatch line\r\n");
				}
				if (!InstallLoadGameHook())
					CoopRuntime::Instance().Log(
						"[save-sync] host Load Game synchronization unavailable\r\n");
				return input_hooks;
			}
		}
		CoopRuntime::Instance().Log(
			"[netgame-error] GetAsyncKeyState import was not found\r\n");
		return false;
	}

	void CoopNetGame::RemoveInputHook()
	{
		RemoveStateMachineSelectStateHook();
		RemoveLoadGameHook();
		RemoveHealthComponentSubtractHook();
		RemoveHealthComponentAddHook();
		RemoveHealthComponentSetHook();
		RemoveObjectEventForwarderHook();
		RemoveObjectEventRelayHook();
		RemoveGlobalEventForwarderHook();
		RemoveTriggerEventHook();

		RemoveTriggerSpawnHook();
		RemoveTriggerFactoryHook();
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
		RemoveAbrAttackPredicateHook();
		RemovePressedQueryHook();
		RemoveAxisQueryHook();
		RemoveThresholdQueryHook();
		RemoveActionUpQueryHook();
		RemoveActionQueryHook();

		if (!m_input_hooked || !m_async_key_state_iat_slot || !m_original_get_async_key_state)
			return;

		if (*m_async_key_state_iat_slot == reinterpret_cast<ULONG_PTR>(&HookGetAsyncKeyState))
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
		m_async_key_state_iat_slot = nullptr;
		m_original_get_async_key_state = nullptr;
	}


	int CoopNetGame::MapForcedGameLanguage(int original)
	{
		const char* forced = CoopRuntime::Instance().Config().audio_language;
		if (forced[0] == '\0' || _stricmp(forced, "AUT") == 0 ||
			_stricmp(forced, "OFF") == 0)
		{
			return original;
		}
		struct LanguageMapEntry final
		{
			const char* code;
			int value;
		};
		static const LanguageMapEntry table[] = {
			{ "USA", 0x00 }, { "FRE", 0x06 }, { "GER", 0x07 },
			{ "ITA", 0x08 }, { "SPA", 0x0B }, { "RUS", 0x0F },
			{ "DUT", 0x04 }, { "CZE", 0x12 }, { "POL", 0x13 },
			{ "BRA", 0x17 },
		};
		for (size_t i = 0; i < _countof(table); ++i)
		{
			if (_stricmp(forced, table[i].code) == 0)
				return table[i].value;
		}
		return original;
	}

	void __fastcall CoopNetGame::HookLanguageSelect(void* manager, void*,
		std::uint32_t language, std::uint32_t arg)
	{
		CoopNetGame& game = CoopNetGame::Instance();
		const int forced = MapForcedGameLanguage(
			static_cast<int>(language));
		if (forced != static_cast<int>(language) &&
			!game.m_logged_lang_override)
		{
			game.m_logged_lang_override = true;
			CoopRuntime::Instance().Log(
				"[language] game language overridden: 0x%02X -> 0x%02X\r\n",
				language, static_cast<unsigned>(forced));
		}
		game.m_original_language_select(manager,
			static_cast<std::uint32_t>(forced), arg);
	}

	bool CoopNetGame::InstallLanguageSelectHook()
	{
		if (m_language_select_hooked)
			return true;
		if (!InstallJmpHookRaw(kLanguageSelect, kExpectedLanguageSelect,
			sizeof(kExpectedLanguageSelect),
			reinterpret_cast<void*>(&HookLanguageSelect),
			m_original_language_select_bytes, &m_language_select_trampoline,
			"game language selector"))
		{
			return false;
		}
		m_original_language_select =
			reinterpret_cast<LanguageSelectFn>(m_language_select_trampoline);
		m_language_select_hooked = true;
		return true;
	}

	void CoopNetGame::RemoveLanguageSelectHook()
	{
		if (!m_language_select_hooked)
			return;
		RemoveJmpHookRaw(kLanguageSelect, m_original_language_select_bytes,
			sizeof(m_original_language_select_bytes),
			&m_language_select_trampoline);
		m_original_language_select = nullptr;
		m_language_select_hooked = false;
	}

}
