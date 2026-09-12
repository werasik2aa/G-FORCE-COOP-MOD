#pragma once

#include <cstddef>
#include <cstdint>

#include "../gforce_constants.h"

namespace coop
{
namespace retail
{
    using Address = std::uintptr_t;
    using ModeId = std::uint32_t;
    using EventCode = std::int32_t;

    static_assert(sizeof(Address) == 4,
        "The retail ABI layer is intentionally limited to the x86 GForce.exe build");

    struct Vec3 final
    {
        float x;
        float y;
        float z;
    };

    struct Vec4 final
    {
        float x;
        float y;
        float z;
        float w;
    };

    static_assert(sizeof(Vec3) == 3 * sizeof(float),
        "Vec3 must remain a compact wire-compatible value type");
    static_assert(sizeof(Vec4) == 4 * sizeof(float),
        "Vec4 must remain the exact retail x86 value layout");

    // The two verified ray blocks stored by the native input manager.  This is
    // a process-local snapshot used only around the synchronous fire handler;
    // it is not a claim about the complete input-manager layout or a wire type.
    struct AimRay final
    {
        Vec3 origin;
        Vec3 direction;
    };

    static_assert(sizeof(AimRay) == 6 * sizeof(float),
        "AimRay must preserve the two compact retail vec3 fields");

    struct Transform final
    {
        Vec4 position;
        Vec4 rotation;
    };

    static_assert(sizeof(Transform) == 8 * sizeof(float),
        "Transform must preserve two retail vec4 fields");

    // This is the fixed six-entry retail entity table, not a claim that the
    // game supports six controllable players.  IDA shows native cleanup
    // iterating [0, 6), while two player-selection paths stop at slot 3.
    // Keep the unclassified entries explicit so feature code cannot casually
    // treat Mooch or slot 5 as a fourth/fifth player.
    enum class EntitySlot : std::uint16_t
    {
        Slot0 = 0,
        LocalP1 = 1,
        RemoteP2 = 2,
        AuxiliaryP3 = 3,
        Mooch = 4,
        Unclassified5 = 5,
        None = 0xFFFFu
    };

    constexpr std::uint32_t kRetailEntitySlotCount = 6u;

    constexpr bool IsKnownEntitySlot(EntitySlot slot)
    {
        return static_cast<std::uint32_t>(slot) < kRetailEntitySlotCount;
    }

    // These slots are consulted by two native selectors.  That proves they
    // are selectable GPig slots, not that every one has a complete local or
    // network-control lifecycle in the co-op DLL.
    constexpr bool IsSelectableGPigSlot(EntitySlot slot)
    {
        return slot == EntitySlot::LocalP1 ||
            slot == EntitySlot::RemoteP2 ||
            slot == EntitySlot::AuxiliaryP3;
    }

    // This is the native selector domain only. A caller that finds P3 here
    // has identified an existing controller; it has not gained permission to
    // spawn, tick, feed input to, or assign the shared camera to P3.
    constexpr EntitySlot kSelectableGPigSlots[] = {
        EntitySlot::LocalP1,
        EntitySlot::RemoteP2,
        EntitySlot::AuxiliaryP3
    };

    constexpr std::size_t kSelectableGPigSlotCount =
        sizeof(kSelectableGPigSlots) / sizeof(kSelectableGPigSlots[0]);
    static_assert(kSelectableGPigSlotCount == 3u,
        "The verified native GPig selector has exactly P1-P3 slots");

    struct EntityRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const EntityRef& other) const { return value == other.value; }
        bool operator!=(const EntityRef& other) const { return !(*this == other); }
    };

    struct HandlerRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const HandlerRef& other) const { return value == other.value; }
        bool operator!=(const HandlerRef& other) const { return !(*this == other); }
    };

    // This is an address inside a Handler, not an owning heap object.  It names
    // the confirmed native health component passed to its three mutators.
    struct HealthComponentRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const HealthComponentRef& other) const { return value == other.value; }
        bool operator!=(const HealthComponentRef& other) const { return !(*this == other); }
    };

    struct ControllerRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const ControllerRef& other) const { return value == other.value; }
        bool operator!=(const ControllerRef& other) const { return !(*this == other); }
    };

    // Opaque compact state-machine object passed to the common native
    // StateMachine_SelectState dispatcher.  It may be an outer controller or
    // a nested motor machine; callers must classify its registered states
    // before assigning it gameplay meaning.
    struct StateMachineRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const StateMachineRef& other) const { return value == other.value; }
        bool operator!=(const StateMachineRef& other) const { return !(*this == other); }
    };

    // A momentary resolved chain from one retail table slot to its controller.
    // It owns no game memory and grants no authority to tick that controller;
    // it just lets feature code state its required slot explicitly instead of
    // passing unrelated void* values through a P2-specific helper.
    struct EntitySlotBinding final
    {
        EntitySlot slot = EntitySlot::None;
        EntityRef entity = {};
        HandlerRef handler = {};
        ControllerRef controller = {};

        explicit operator bool() const
        {
            return entity && handler && controller;
        }
    };

    // Opaque native input-manager allocation.  Only its cached aim ray is
    // exposed through InputManagerView; no full class layout is assumed.
    struct InputManagerRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
    };

    // The retail input-owner layout is intentionally not modelled.  These are
    // only the two observed 256-byte DirectInput snapshots that the scoped P2
    // input override must save and restore together.
    struct KeyboardStateBuffers final
    {
        Address primary = 0;
        Address secondary = 0;

        explicit operator bool() const
        {
            return primary != 0 && secondary != 0;
        }
    };

    struct KeyboardStateSnapshot final
    {
        std::uint8_t bytes[gforce::kKeyboardStateBytes] = {};
    };

    static_assert(sizeof(KeyboardStateSnapshot) == gforce::kKeyboardStateBytes,
        "KeyboardStateSnapshot must preserve one retail DirectInput buffer");

    // Opaque retail UI objects. The co-op feature knows only the menu-factory,
    // AddChild and XAText callback boundaries below; none of these refs claims
    // a complete EngineX UI layout.
    struct HudMenuMainRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
    };

    struct HudPaneRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
    };

    struct LocalizedTextRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
    };

    // Borrowed storage for one native EXWString pointer. It is never a C++
    // string owned by the mod; the stock ref-counting helpers own its data.
    struct GameStringRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
    };

    // Opaque XGamePad allocation. The co-op code may construct a private pad
    // and temporarily swap the process-global pointer, but never relies on a
    // full XGamePad layout outside the retail boundary.
    struct GamePadRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const GamePadRef& other) const { return value == other.value; }
        bool operator!=(const GamePadRef& other) const { return value != other.value; }
    };

    // Opaque process-global load/save manager. Only the selected native slot
    // and its confirmed BeginLoad dispatcher are exposed below.
    struct LoadSaveManagerRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const LoadSaveManagerRef& other) const { return value == other.value; }
        bool operator!=(const LoadSaveManagerRef& other) const { return value != other.value; }
    };

    // Opaque inventory object owned by a Handler. Its layout is intentionally
    // unknown; the only approved operation is resolving an item through the
    // stock inventory lookup at the retail-call boundary.
    struct InventoryRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
    };

    // Opaque item selected by the confirmed two-item Mooch dual-laser route.
    // Only its native item id and momentary effect field are exposed below;
    // this does not describe a general inventory-record layout.
    struct FlyDualLaserRouteItemRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const FlyDualLaserRouteItemRef& other) const
        {
            return value == other.value;
        }
        bool operator!=(const FlyDualLaserRouteItemRef& other) const
        {
            return !(*this == other);
        }
    };

    // Opaque inventory record consumed by the stock WeaponAmmoItem timer. The
    // verified fields are exposed by WeaponAmmoItemView; this is not a full
    // item-handler layout.
    struct WeaponAmmoItemRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
    };

    // Opaque entry returned by the engine's shared ammo-pool resolver. Only
    // the current amount used by the HUD and stock consume path is known.
    struct AmmoPoolEntryRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
    };

    // Process-local controller-mode allocation. Only the vtable, mode ID and
    // conflict mask are confirmed; it is deliberately not modelled as a full
    // C++ class with guessed padding.
    struct ModeRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const ModeRef& other) const { return value == other.value; }
        bool operator!=(const ModeRef& other) const { return !(*this == other); }
    };

    // A concrete inner GPig Ledge/Climb state. Only its owner-GPig reference is
    // exposed; treating it as a complete state-machine layout would be wrong.
    struct GPigAttachmentStateRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
    };

    // This is the inline XMotorSystem region inside a GPig handler.  It is not
    // a separately allocated object and only the fields exposed by
    // MotorSystemView are known.
    struct MotorSystemRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const MotorSystemRef& other) const { return value == other.value; }
        bool operator!=(const MotorSystemRef& other) const { return !(*this == other); }
    };

    // Process-local entries reached through the motor system's resource and
    // task-state tables.  They deliberately expose no guessed full layouts.
    struct MotorResourceRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const MotorResourceRef& other) const { return value == other.value; }
        bool operator!=(const MotorResourceRef& other) const { return !(*this == other); }
    };

    struct MotorTaskRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const MotorTaskRef& other) const { return value == other.value; }
        bool operator!=(const MotorTaskRef& other) const { return !(*this == other); }
    };

    // Opaque native context forwarded by the stock GPig factory.  Only its
    // flags and linked active entity are exposed by SpawnContextView.
    struct SpawnContextRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const SpawnContextRef& other) const { return value == other.value; }
        bool operator!=(const SpawnContextRef& other) const { return !(*this == other); }
    };

    struct TriggerRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const TriggerRef& other) const { return value == other.value; }
        bool operator!=(const TriggerRef& other) const { return !(*this == other); }
    };

    struct CameraHandlerRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
    };

    // Native camera state objects are returned by the stock state-machine API.
    // Only the follow-turn float exposed by CameraStateView is known here.
    struct CameraStateRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
    };

    // The two blocks written by GPig Default's aim state machine.  They are
    // copied as values across a temporary remote-controller tick; neither is a
    // wire type or a complete camera-handler layout.
    struct CameraAimAssistState final
    {
        float value[2];
    };

    struct CameraAimYawState final
    {
        float value[6];
    };

    // Exact opaque snapshot of the Fly_Active camera-request/apply window.
    // This is a process-local retail layout, never a wire-format type.
    struct CameraFlyTransientState final
    {
        std::uint8_t value[gforce::kCameraFlyTransientBytes];
    };

    static_assert(sizeof(CameraAimAssistState) == 2 * sizeof(float),
        "Camera aim-assist snapshot must preserve two retail floats");
    static_assert(sizeof(CameraAimYawState) == 6 * sizeof(float),
        "Camera aim-yaw snapshot must preserve six retail floats");
    static_assert(sizeof(CameraFlyTransientState) ==
        gforce::kCameraFlyTransientBytes,
        "Fly camera snapshot must preserve the exact retail byte window");

    struct TriggerIdentity final
    {
        std::uint32_t family = 0;
        std::uint32_t subtype = 0;
        std::int32_t definition_id = -1;
    };

    inline Address AddOffset(Address address, std::size_t offset)
    {
        return address == 0 ? 0 : address + offset;
    }

    inline Address ToAddress(const void* pointer)
    {
        return reinterpret_cast<Address>(pointer);
    }

    inline void* ToPointer(Address address)
    {
        return reinterpret_cast<void*>(address);
    }
}
}
