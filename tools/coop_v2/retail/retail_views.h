#pragma once

#include <cstddef>
#include <cstdint>

#include "../gforce_constants.h"
#include "retail_memory.h"

namespace coop
{
namespace retail
{
    class InputManagerView final
    {
    public:
        explicit InputManagerView(InputManagerRef manager) : manager_(manager) {}

        bool ReadAimRay(AimRay& out) const
        {
            return TryRead(AddOffset(manager_.value,
                gforce::kInputAimOriginOffset), out.origin) &&
                TryRead(AddOffset(manager_.value,
                    gforce::kInputAimDirectionOffset), out.direction);
        }

        bool WriteAimRay(const AimRay& value) const
        {
            return TryWrite(AddOffset(manager_.value,
                gforce::kInputAimOriginOffset), value.origin) &&
                TryWrite(AddOffset(manager_.value,
                    gforce::kInputAimDirectionOffset), value.direction);
        }

        InputManagerRef ref() const { return manager_; }

    private:
        InputManagerRef manager_;
    };

    // Confirmed XAText fields from its resolver at 0x490900. These fields are
    // only used to cache the one private co-op label; they are not a full text
    // widget definition.
    class LocalizedTextView final
    {
    public:
        explicit LocalizedTextView(LocalizedTextRef text) : text_(text) {}

        bool ResourceId(std::uint32_t& out) const
        {
            return text_ && TryRead(AddOffset(text_.value, 0x08u), out);
        }

        bool IsResolvedFor(std::uint8_t language, bool& out) const
        {
            std::uint8_t resolved = 0;
            std::uint8_t cached_language = 0;
            if (!text_ || !TryRead(AddOffset(text_.value, 0x04u), resolved) ||
                !TryRead(AddOffset(text_.value, 0x0Cu), cached_language))
                return false;
            out = resolved != 0 && cached_language == language;
            return true;
        }

        bool MarkResolvedFor(std::uint8_t language) const
        {
            const std::uint8_t resolved = 1;
            return text_ && TryWrite(AddOffset(text_.value, 0x0Cu), language) &&
                TryWrite(AddOffset(text_.value, 0x04u), resolved);
        }

        bool Invalidate() const
        {
            const std::uint8_t unresolved = 0;
            return text_ && TryWrite(AddOffset(text_.value, 0x04u), unresolved);
        }

    private:
        LocalizedTextRef text_;
    };

    class LoadSaveManagerView final
    {
    public:
        explicit LoadSaveManagerView(LoadSaveManagerRef manager) : manager_(manager) {}

        bool SelectedSlot(std::uint32_t& out) const
        {
            return manager_ && TryRead(AddOffset(manager_.value,
                gforce::kLoadSaveSelectedSlotOffset), out);
        }

        LoadSaveManagerRef ref() const { return manager_; }

    private:
        LoadSaveManagerRef manager_;
    };

    // The P1 XGamePad pointer is process-global. P2 must substitute its private
    // pad only inside one scoped stock update and restore the precise prior
    // pointer afterwards; this view intentionally exposes no pad fields.
    class PrimaryGamePadStore final
    {
    public:
        bool Read(GamePadRef& out) const
        {
            out = {};
            return TryReadAddress(gforce::kPrimaryGamePad, out.value);
        }

        bool Replace(GamePadRef replacement, GamePadRef& previous) const
        {
            if (!replacement || !Read(previous))
                return false;
            return TryWrite(gforce::kPrimaryGamePad, replacement.value);
        }

        bool Restore(GamePadRef previous) const
        {
            return TryWrite(gforce::kPrimaryGamePad, previous.value);
        }
    };

    // Owns the only raw access to the two DirectInput byte arrays used during a
    // scoped remote-P2 controller tick.  It is deliberately a buffer API, not
    // a guessed representation of the surrounding retail input-owner object.
    class KeyboardStateStore final
    {
    public:
        bool ReadBuffers(KeyboardStateBuffers& out) const
        {
            out = {};
            Address owner = 0;
            if (!TryReadAddress(gforce::kKeyboardStateOwner, owner) || owner == 0)
                return false;

            out.primary = AddOffset(owner, gforce::kKeyboardStateBytesOffset);
            out.secondary = AddOffset(owner,
                gforce::kKeyboardStateSecondaryBytesOffset);
            return static_cast<bool>(out);
        }

        bool Snapshot(const KeyboardStateBuffers& buffers,
            KeyboardStateSnapshot& primary, KeyboardStateSnapshot& secondary) const
        {
            primary = {};
            secondary = {};
            return buffers && TryRead(buffers.primary, primary) &&
                TryRead(buffers.secondary, secondary);
        }

        bool Replace(const KeyboardStateBuffers& buffers,
            const KeyboardStateSnapshot& replacement) const
        {
            return buffers && TryWrite(buffers.primary, replacement) &&
                TryWrite(buffers.secondary, replacement);
        }

        bool Restore(const KeyboardStateBuffers& buffers,
            const KeyboardStateSnapshot& primary,
            const KeyboardStateSnapshot& secondary) const
        {
            if (!buffers)
                return false;

            // Attempt both writes even when the first one has become invalid:
            // leaving the second array overridden would otherwise retain input
            // after the scoped P2 update ends.
            const bool primary_restored = TryWrite(buffers.primary, primary);
            const bool secondary_restored = TryWrite(buffers.secondary, secondary);
            return primary_restored && secondary_restored;
        }
    };

    class WeaponAmmoItemView final
    {
    public:
        explicit WeaponAmmoItemView(WeaponAmmoItemRef item) : item_(item) {}

        bool RoundCount(std::uint32_t& out) const
        {
            return TryRead(AddOffset(item_.value,
                gforce::kWeaponRecordRoundCountOffset), out);
        }

        bool SetRoundCount(std::uint32_t value) const
        {
            return TryWrite(AddOffset(item_.value,
                gforce::kWeaponRecordRoundCountOffset), value);
        }

        bool AmmoId(std::uint32_t& out) const
        {
            return TryRead(AddOffset(item_.value,
                gforce::kWeaponRecordAmmoIdOffset), out);
        }

        WeaponAmmoItemRef ref() const { return item_; }

    private:
        WeaponAmmoItemRef item_;
    };

    class FlyDualLaserRouteItemView final
    {
    public:
        explicit FlyDualLaserRouteItemView(FlyDualLaserRouteItemRef item)
            : item_(item) {}

        bool ItemId(std::uint32_t& out) const
        {
            return TryRead(AddOffset(item_.value,
                gforce::kFlyDualLaserRouteItemIdOffset), out);
        }

        bool SetEffectActive(bool active) const
        {
            return TryWrite(AddOffset(item_.value,
                gforce::kFlyDualLaserRouteItemActiveOffset),
                static_cast<std::uint32_t>(active ? 1u : 0u));
        }

        bool EffectActive(bool& out) const
        {
            std::uint32_t value = 0;
            if (!TryRead(AddOffset(item_.value,
                gforce::kFlyDualLaserRouteItemActiveOffset), value))
            {
                return false;
            }
            out = value != 0;
            return true;
        }

        FlyDualLaserRouteItemRef ref() const { return item_; }

    private:
        FlyDualLaserRouteItemRef item_;
    };

    class AmmoPoolEntryView final
    {
    public:
        explicit AmmoPoolEntryView(AmmoPoolEntryRef entry) : entry_(entry) {}

        bool CurrentAmount(std::uint32_t& out) const
        {
            return TryRead(AddOffset(entry_.value,
                gforce::kAmmoEntryCurrentOffset), out);
        }

        bool SetCurrentAmount(std::uint32_t value) const
        {
            return TryWrite(AddOffset(entry_.value,
                gforce::kAmmoEntryCurrentOffset), value);
        }

        AmmoPoolEntryRef ref() const { return entry_; }

    private:
        AmmoPoolEntryRef entry_;
    };

    class HandlerView final
    {
    public:
        explicit HandlerView(HandlerRef handler) : handler_(handler) {}

        bool Controller(ControllerRef& out) const
        {
            return TryReadPointer(AddOffset(handler_.value,
                gforce::kHandlerControllerOffset), out);
        }

        bool Inventory(InventoryRef& out) const
        {
            return TryReadPointer(AddOffset(handler_.value,
                gforce::kHandlerInventoryOffset), out);
        }

        // This byte is read by the exact Fly_Active dual-laser path immediately
        // before it chooses the two inventory item ids. Its broader meaning is
        // intentionally left unmodelled.
        bool FlyDualLaserUsesAlternateItemSet(bool& out) const
        {
            std::uint8_t value = 0;
            if (!TryRead(AddOffset(handler_.value,
                gforce::kHandlerFlyDualLaserItemSetOffset), value))
            {
                return false;
            }
            out = value != 0;
            return true;
        }

        bool SelectedWeaponType(std::uint32_t& out) const
        {
            return TryRead(AddOffset(handler_.value,
                gforce::kHandlerSelectedWeaponTypeOffset), out);
        }

        bool MotorSystem(MotorSystemRef& out) const
        {
            out.value = handler_ ? AddOffset(handler_.value,
                gforce::kHandlerMotorSystemOffset) : 0;
            return static_cast<bool>(out);
        }

        // Fly_Idle::Update reaches this exact handler-owned state table, then
        // reads byte +0x53 from the entry selected by the engine-owned index.
        // It is deliberately a narrow operation rather than a guessed model of
        // either the table or a complete Fly state object.
        bool SetFlyControlActive(std::uint32_t state_index, bool active) const
        {
            Address state_table = 0;
            Address state = 0;
            return TryReadAddress(AddOffset(handler_.value,
                gforce::kHandlerFlyStateTableOffset), state_table) &&
                state_table != 0 &&
                TryReadAddress(AddOffset(state_table,
                    state_index * sizeof(Address)), state) &&
                state != 0 &&
                TryWrite(AddOffset(state, gforce::kFlyControlActiveOffset),
                    static_cast<std::uint8_t>(active ? 1u : 0u));
        }

        bool FlyControlActive(std::uint32_t state_index, bool& out) const
        {
            Address state_table = 0;
            Address state = 0;
            std::uint8_t value = 0;
            if (!TryReadAddress(AddOffset(handler_.value,
                gforce::kHandlerFlyStateTableOffset), state_table) ||
                state_table == 0 ||
                !TryReadAddress(AddOffset(state_table,
                    state_index * sizeof(Address)), state) ||
                state == 0 ||
                !TryRead(AddOffset(state, gforce::kFlyControlActiveOffset), value))
            {
                return false;
            }
            out = value != 0;
            return true;
        }

        // The same handler-owned state table contains the 96-byte FlyFly motion
        // entry selected by the engine-owned Fly runtime index. Expose only its
        // address for FlyFlyMotionStateView; this is not a generic task factory.
        bool FlyFlyMotionState(std::uint32_t state_index,
            MotorTaskRef& out) const
        {
            out = {};
            Address state_table = 0;
            return TryReadAddress(AddOffset(handler_.value,
                gforce::kHandlerFlyStateTableOffset), state_table) &&
                state_table != 0 &&
                TryReadAddress(AddOffset(state_table,
                    state_index * sizeof(Address)), out.value) && out;
        }

        bool Health(float& out) const
        {
            if (!TryRead(AddOffset(handler_.value,
                gforce::kHandlerHealthOffset), out))
            {
                return false;
            }
            return out == out && out >= 0.0f && out <= 100000.0f;
        }

        bool SetHealth(float value) const
        {
            if (!(value == value && value >= 0.0f && value <= 100000.0f))
                return false;
            return TryWrite(AddOffset(handler_.value,
                gforce::kHandlerHealthOffset), value);
        }

        bool HealthComponent(HealthComponentRef& out) const
        {
            out.value = handler_ ? AddOffset(handler_.value,
                gforce::kHandlerHealthOffset - sizeof(float)) : 0;
            return static_cast<bool>(out);
        }

        HandlerRef ref() const { return handler_; }

    private:
        HandlerRef handler_;
    };

    class EntityView final
    {
    public:
        explicit EntityView(EntityRef entity) : entity_(entity) {}

        bool Handler(HandlerRef& out) const
        {
            return TryReadPointer(AddOffset(entity_.value,
                gforce::kEntityHandlerOffset), out);
        }

        bool Trigger(TriggerRef& out) const
        {
            return TryReadPointer(AddOffset(entity_.value,
                gforce::kEntityTriggerOffset), out);
        }

        bool ReadTransform(Transform& out) const
        {
            return TryRead(AddOffset(entity_.value, gforce::kEntityPositionOffset),
                out.position) &&
                TryRead(AddOffset(entity_.value, gforce::kEntityRotationOffset),
                    out.rotation);
        }

        bool WriteTransform(const Transform& value) const
        {
            return TryWrite(AddOffset(entity_.value, gforce::kEntityPositionOffset),
                value.position) &&
                TryWrite(AddOffset(entity_.value, gforce::kEntityRotationOffset),
                    value.rotation) &&
                InvalidateTransformCache();
        }

        // Direct root writes bypass retail's normal setter.  Clear the same
        // validity byte that retail clears so subsequent rendering, attachment,
        // and aim reads rebuild the cached root matrix from this transform.
        bool InvalidateTransformCache() const
        {
            const std::uint8_t invalid = 0;
            return TryWrite(AddOffset(entity_.value,
                gforce::kEntityTransformCacheValidOffset), invalid);
        }

        bool ReadHealth(float& out) const
        {
            HandlerRef handler = {};
            return Handler(handler) && HandlerView(handler).Health(out);
        }

        bool SetHealth(float value) const
        {
            HandlerRef handler = {};
            return Handler(handler) && HandlerView(handler).SetHealth(value);
        }

        EntityRef ref() const { return entity_; }

    private:
        EntityRef entity_;
    };

    // The retail process owns one global registry with two intrusive lists.
    // This view deliberately exposes traversal only: the list node's complete
    // layout, ownership and mutation API are not known.  The visitor returns
    // true to continue, false to stop a successfully-read walk early.  The
    // next node is fetched before the visitor runs, matching the stock-style
    // callers that may change world bookkeeping from inside the callback.
    class EntityRegistryView final
    {
    public:
        template<typename Visitor>
        bool VisitLiveEntities(std::size_t per_list_limit, Visitor visitor) const
        {
            Address registry = 0;
            if (!TryReadAddress(gforce::kEntityRegistry, registry))
                return false;
            if (registry == 0)
                return true;

            const std::size_t list_offsets[] = {
                gforce::kEntityRegistryMonsterListOffset,
                gforce::kEntityRegistryNpcListOffset
            };
            for (std::size_t list_index = 0;
                list_index != _countof(list_offsets); ++list_index)
            {
                Address node = 0;
                if (!TryReadAddress(AddOffset(registry,
                    list_offsets[list_index]), node))
                {
                    return false;
                }
                for (std::size_t visited = 0;
                    node != 0 && visited != per_list_limit; ++visited)
                {
                    Address entity_address = 0;
                    Address next_node = 0;
                    if (!TryReadAddress(AddOffset(node,
                        gforce::kIntrusiveListValueOffset), entity_address) ||
                        !TryReadAddress(AddOffset(node,
                            gforce::kIntrusiveListNextOffset), next_node))
                    {
                        return false;
                    }
                    node = next_node;
                    if (entity_address == 0)
                        continue;

                    EntityRef entity = {};
                    entity.value = entity_address;
                    if (!visitor(entity))
                        return true;
                }
            }
            return true;
        }
    };

    class MotorSystemView final
    {
    public:
        explicit MotorSystemView(MotorSystemRef system) : system_(system) {}

        bool ResourceCount(std::uint32_t& out) const
        {
            return TryRead(AddOffset(system_.value,
                gforce::kMotorSystemResourceCountOffset), out);
        }

        bool ResourceTable(Address& out) const
        {
            return TryReadAddress(AddOffset(system_.value,
                gforce::kMotorSystemResourceTableOffset), out) && out != 0;
        }

        bool ResourceAt(std::uint32_t index, MotorResourceRef& out) const
        {
            out = {};
            std::uint32_t count = 0;
            Address table = 0;
            return ResourceCount(count) && index < count && ResourceTable(table) &&
                TryReadPointer(AddOffset(table, index * sizeof(Address)), out);
        }

        bool TaskStateTable(Address& out) const
        {
            return TryReadAddress(AddOffset(system_.value,
                gforce::kMotorSystemTaskStateTableOffset), out) && out != 0;
        }

        // A null task is a valid readable state-table entry: it tells the
        // caller to use the stock lazy factory.  Therefore this does not use
        // TryReadPointer, whose contract intentionally rejects null values.
        bool TaskAt(std::uint32_t index, MotorTaskRef& out) const
        {
            out = {};
            Address table = 0;
            if (index >= gforce::kMotorSystemTaskStateSafetyLimit ||
                !TaskStateTable(table))
            {
                return false;
            }
            return TryReadAddress(AddOffset(table, index * sizeof(Address)),
                out.value);
        }

        MotorSystemRef ref() const { return system_; }

    private:
        MotorSystemRef system_;
    };

    // A task-state-table entry used by XFlyFlyMode. The known fields are only
    // the current/target angles and its direction-active byte recovered from
    // XFlyFlyMode_Move/Update; no complete retail object layout is implied.
    class FlyFlyMotionStateView final
    {
    public:
        explicit FlyFlyMotionStateView(MotorTaskRef state) : state_(state) {}

        bool SetImmediateDirection(float yaw, float pitch) const
        {
            if (!state_ || !(yaw > -1000.0f && yaw < 1000.0f) ||
                !(pitch > -1.7f && pitch < 1.7f))
            {
                return false;
            }

            // Match the settled state produced by the stock smoother, then let
            // the stock terminal helper derive and submit its LookAt target.
            const std::uint8_t active = 1u;
            return TryWrite(AddOffset(state_.value,
                gforce::kFlyFlyTargetYawOffset), yaw) &&
                TryWrite(AddOffset(state_.value,
                    gforce::kFlyFlyCurrentYawOffset), yaw) &&
                TryWrite(AddOffset(state_.value,
                    gforce::kFlyFlyTargetPitchOffset), pitch) &&
                TryWrite(AddOffset(state_.value,
                    gforce::kFlyFlyCurrentPitchOffset), pitch) &&
                TryWrite(AddOffset(state_.value,
                    gforce::kFlyFlyDirectionActiveOffset), active);
        }

    private:
        MotorTaskRef state_;
    };

    class MotorResourceView final
    {
    public:
        explicit MotorResourceView(MotorResourceRef resource) : resource_(resource) {}

        bool VTable(Address& out) const
        {
            return TryReadAddress(resource_.value, out) && out != 0;
        }

        MotorResourceRef ref() const { return resource_; }

    private:
        MotorResourceRef resource_;
    };

    class MotorTaskView final
    {
    public:
        explicit MotorTaskView(MotorTaskRef task) : task_(task) {}

        bool VTable(Address& out) const
        {
            return TryReadAddress(task_.value, out) && out != 0;
        }

        bool RdvEnabled(std::uint8_t& out) const
        {
            return TryRead(AddOffset(task_.value,
                gforce::kGPigRdvTaskEnabledOffset), out);
        }

        MotorTaskRef ref() const { return task_; }

    private:
        MotorTaskRef task_;
    };

    class SpawnContextView final
    {
    public:
        explicit SpawnContextView(SpawnContextRef context) : context_(context) {}

        bool Flags(std::uint32_t& out) const
        {
            return TryRead(AddOffset(context_.value,
                gforce::kGPigSpawnContextFlagsOffset), out);
        }

        bool ActiveEntity(EntityRef& out) const
        {
            return TryReadPointer(AddOffset(context_.value,
                gforce::kGPigSpawnContextEntityOffset), out);
        }

        SpawnContextRef ref() const { return context_; }

    private:
        SpawnContextRef context_;
    };

    inline bool ReadGPigRdvTaskStateIndex(std::uint32_t& out)
    {
        return TryRead(gforce::kGPigRdvTaskStateIndex, out);
    }

    inline bool ReadFlyActiveStateIndex(std::uint32_t& out)
    {
        return TryRead(gforce::kFlyActiveStateIndex, out);
    }

    class CameraHandlerView final
    {
    public:
        explicit CameraHandlerView(CameraHandlerRef handler) : handler_(handler) {}

        bool TargetControllerId(std::uint32_t& out) const
        {
            return TryRead(AddOffset(handler_.value,
                gforce::kCameraTargetControllerOffset +
                gforce::kCameraTargetIdOffset), out);
        }

        // The state machine is inline in the one camera handler, rather than a
        // separately allocated object.  Native state-machine calls still stay
        // at the feature boundary because their calling/lifetime contract is
        // not modelled as a C++ class here.
        bool StateMachineAddress(Address& out) const
        {
            out = handler_ ? AddOffset(handler_.value,
                gforce::kCameraStateMachineOffset) : 0;
            return out != 0;
        }

        bool ReadAimAssist(CameraAimAssistState& out) const
        {
            return TryRead(AddOffset(handler_.value,
                gforce::kCameraAimAssistOffset), out);
        }

        bool WriteAimAssist(const CameraAimAssistState& value) const
        {
            return TryWrite(AddOffset(handler_.value,
                gforce::kCameraAimAssistOffset), value);
        }

        bool ReadAimYawState(CameraAimYawState& out) const
        {
            return TryRead(AddOffset(handler_.value,
                gforce::kCameraAimYawStateOffset), out);
        }

        bool WriteAimYawState(const CameraAimYawState& value) const
        {
            return TryWrite(AddOffset(handler_.value,
                gforce::kCameraAimYawStateOffset), value);
        }

        bool ReadFlyTransientState(CameraFlyTransientState& out) const
        {
            return TryRead(AddOffset(handler_.value,
                gforce::kCameraFlyTransientOffset), out);
        }

        bool WriteFlyTransientState(const CameraFlyTransientState& value) const
        {
            return TryWrite(AddOffset(handler_.value,
                gforce::kCameraFlyTransientOffset), value);
        }

        CameraHandlerRef ref() const { return handler_; }

    private:
        CameraHandlerRef handler_;
    };

    class CameraStateView final
    {
    public:
        explicit CameraStateView(CameraStateRef state) : state_(state) {}

        bool FollowTurn(float& out) const
        {
            return TryRead(AddOffset(state_.value,
                gforce::kCameraStateTurnOffset), out);
        }

        bool SetFollowTurn(float value) const
        {
            return TryWrite(AddOffset(state_.value,
                gforce::kCameraStateTurnOffset), value);
        }

        CameraStateRef ref() const { return state_; }

    private:
        CameraStateRef state_;
    };

    class ModeView final
    {
    public:
        explicit ModeView(ModeRef mode) : mode_(mode) {}

        bool Controller(ControllerRef& out) const
        {
            return TryReadPointer(AddOffset(mode_.value,
                gforce::kModeControllerOffset), out);
        }

        bool VTable(Address& out) const
        {
            return TryReadAddress(mode_.value, out) && out != 0;
        }

        bool Id(ModeId& out) const
        {
            return TryRead(AddOffset(mode_.value, gforce::kModeIdOffset), out);
        }

        bool ConflictMask(std::uint32_t& out) const
        {
            return TryRead(AddOffset(mode_.value,
                gforce::kModeConflictMaskOffset), out);
        }

        bool SetConflictMask(std::uint32_t value) const
        {
            return TryWrite(AddOffset(mode_.value,
                gforce::kModeConflictMaskOffset), value);
        }

        bool Update(Address& out) const
        {
            out = 0;
            Address vtable = 0;
            return VTable(vtable) &&
                TryReadAddress(AddOffset(vtable,
                    gforce::kModeUpdateVtableOffset), out) && out != 0;
        }

        bool FlyActiveEntered(std::uint8_t& out) const
        {
            return TryRead(AddOffset(mode_.value,
                gforce::kFlyActiveModeEnteredOffset), out);
        }

        bool SetFlyActiveEntered(std::uint8_t value) const
        {
            return TryWrite(AddOffset(mode_.value,
                gforce::kFlyActiveModeEnteredOffset), value);
        }

        ModeRef ref() const { return mode_; }

    private:
        ModeRef mode_;
    };

    class GPigAttachmentStateView final
    {
    public:
        explicit GPigAttachmentStateView(GPigAttachmentStateRef state) : state_(state) {}

        bool OwnerEntity(EntityRef& out) const
        {
            out = {};
            return state_ && TryReadPointer(AddOffset(state_.value,
                gforce::kGPigAttachmentStateOwnerEntityOffset), out);
        }

    private:
        GPigAttachmentStateRef state_;
    };

    // Compact registry exposed by the common native StateMachine_SelectState
    // dispatcher.  It is deliberately distinct from ControllerView: nested
    // motors share this registry shape but are not player controllers.
    class StateMachineView final
    {
    public:
        explicit StateMachineView(StateMachineRef state_machine) :
            state_machine_(state_machine) {}

        bool CurrentState(ModeRef& out) const
        {
            out = {};
            return state_machine_ && TryReadPointer(AddOffset(state_machine_.value,
                gforce::kStateMachineCurrentModeOffset), out);
        }

        // Classifies a nested machine by its complete registered-state set,
        // rather than treating a reused numeric mode ID as a class identity.
        bool ContainsRegisteredVTable(Address expected_vtable) const
        {
            if (!state_machine_ || expected_vtable == 0)
                return false;

            std::uint32_t count = 0;
            Address table = 0;
            if (!TryRead(AddOffset(state_machine_.value,
                gforce::kStateMachineModeCountOffset), count) ||
                count == 0 || count > gforce::kStateMachineModeSafetyLimit ||
                !TryReadAddress(AddOffset(state_machine_.value,
                    gforce::kStateMachineModeTableOffset), table) ||
                table == 0)
            {
                return false;
            }

            for (std::uint32_t index = 0; index < count; ++index)
            {
                ModeRef candidate = {};
                Address candidate_vtable = 0;
                if (TryReadPointer(AddOffset(table,
                    index * sizeof(Address)), candidate) &&
                    ModeView(candidate).VTable(candidate_vtable) &&
                    candidate_vtable == expected_vtable)
                {
                    return true;
                }
            }
            return false;
        }

        StateMachineRef ref() const { return state_machine_; }

    private:
        StateMachineRef state_machine_;
    };

    class ControllerView final
    {
    public:
        explicit ControllerView(ControllerRef controller) : controller_(controller) {}

        bool Owner(HandlerRef& out) const
        {
            return TryReadPointer(AddOffset(controller_.value,
                gforce::kControllerOwnerOffset), out);
        }

        bool Mode(ModeRef& out) const
        {
            return TryReadPointer(AddOffset(controller_.value,
                gforce::kControllerModeOffset), out);
        }

        bool CurrentMode(ModeId& out) const
        {
            ModeRef mode = {};
            return Mode(mode) && ModeView(mode).Id(out);
        }

        // The controller owns a compact registry at +0x10/+0x14.  This lookup
        // is bounded and validates the mode ID before returning a borrowed
        // process-local object; it never changes the current mode.
        bool RegisteredMode(ModeId mode_id, ModeRef& out) const
        {
            out = {};
            if (!controller_)
                return false;

            std::uint32_t count = 0;
            Address table = 0;
            if (!TryRead(AddOffset(controller_.value,
                gforce::kControllerModeCountOffset), count) ||
                count == 0 || count > gforce::kControllerModeSafetyLimit ||
                !TryReadAddress(AddOffset(controller_.value,
                    gforce::kControllerModeTableOffset), table) ||
                table == 0)
            {
                return false;
            }

            for (std::uint32_t index = 0; index < count; ++index)
            {
                ModeRef candidate = {};
                if (!TryReadPointer(AddOffset(table,
                    index * sizeof(Address)), candidate))
                {
                    continue;
                }
                ModeId candidate_id = 0;
                if (ModeView(candidate).Id(candidate_id) &&
                    candidate_id == mode_id)
                {
                    out = candidate;
                    return true;
                }
            }
            return false;
        }

        // Retail state dispatcher at 0x004B7050 finds a registered state by
        // mode ID, calls the old state's Exit vtable slot, installs the new
        // state and calls its Enter slot. Keep this direct native call at the
        // retail boundary; feature code must not cast arbitrary controllers.
        bool SelectMode(ModeId mode_id, bool force_reselect = false) const
        {
            if (!controller_)
                return false;

            using SelectModeFn = bool(__thiscall*)(void*, ModeId, bool);
            const SelectModeFn select_mode =
                reinterpret_cast<SelectModeFn>(gforce::kSelectMode);
            __try
            {
                return select_mode(ToPointer(controller_.value), mode_id,
                    force_reselect);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        ControllerRef ref() const { return controller_; }

    private:
        ControllerRef controller_;
    };

    // This is the narrow direct-call boundary for confirmed retail entry points.
    // Feature code passes opaque refs/values and never forms a function pointer
    // from a retail VA in the middle of P2, weapon or vehicle logic. A `true`
    // return means only that the native call returned without an SEH fault; a
    // nullable native result is returned through its out parameter.
    class NativeGameApi final
    {
    public:
        static bool ConstructGamePad(void* storage, GamePadRef& out)
        {
            out = {};
            if (!storage)
                return false;
            using ConstructGamePadFn = void* (__thiscall*)(void*);
            const ConstructGamePadFn construct =
                reinterpret_cast<ConstructGamePadFn>(gforce::kXGamePadCtor);
            __try
            {
                out.value = ToAddress(construct(storage));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Mirrors XHudMenuMain::BuildMainMenu's native construction sequence:
        // allocate XAMemFunc_1<XHudMenuMain, void, uint>, construct its four
        // confirmed fields, then call the stock button factory. The returned
        // pane owns the callback by the retail UI lifetime; this function never
        // frees it through a foreign allocator.
        static bool CreateMainMenuButton(HudMenuMainRef menu,
            std::uint32_t label_resource, std::uint32_t callback_argument,
            void* callback_entry, HudPaneRef& out)
        {
            struct MenuUintCallback final
            {
                std::uint32_t vtable;
                std::uint32_t argument;
                void* object;
                void* entry;
            };
            static_assert(sizeof(MenuUintCallback) == 16u,
                "XAMemFunc_1<XHudMenuMain, void, uint> has four x86 words");

            out = {};
            if (!menu || !callback_entry)
                return false;
            using AllocateFn = void* (__cdecl*)(std::size_t, std::uint32_t);
            using CreateButtonFn = void* (__thiscall*)(void*, std::uint32_t,
                void*, std::uint32_t);
            const AllocateFn allocate = reinterpret_cast<AllocateFn>(
                gforce::kGameAllocator);
            const CreateButtonFn create_button = reinterpret_cast<CreateButtonFn>(
                gforce::kMenuCreateButton);
            __try
            {
                MenuUintCallback* callback = static_cast<MenuUintCallback*>(
                    allocate(sizeof(MenuUintCallback), 0));
                if (!callback)
                    return false;
                callback->vtable = gforce::kRetailMenuUintCallbackVtable;
                callback->argument = callback_argument;
                callback->object = ToPointer(menu.value);
                callback->entry = callback_entry;
                out.value = ToAddress(create_button(ToPointer(menu.value),
                    label_resource, callback, 0));
                return static_cast<bool>(out);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                out = {};
                return false;
            }
        }

        static bool AddMenuChild(HudPaneRef parent, HudPaneRef child)
        {
            if (!parent || !child)
                return false;
            using AddChildFn = void(__thiscall*)(void*, void*);
            const AddChildFn add_child = reinterpret_cast<AddChildFn>(
                gforce::kMenuAddChild);
            __try
            {
                add_child(ToPointer(parent.value), ToPointer(child.value));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The literal path is deliberately ASCII: the retail helper widens
        // bytes directly instead of decoding UTF-8. The temporary EXWString is
        // released by the stock ref-counting routine after assignment.
        static bool AssignGameStringFromAnsi(GameStringRef destination,
            const char* text)
        {
            if (!destination || !text || !*text)
                return false;
            using AssignAnsiFn = void* (__thiscall*)(void*, const char*);
            using AssignFn = void* (__thiscall*)(void*, void*);
            using ReleaseFn = void* (__thiscall*)(void*);
            const AssignAnsiFn assign_ansi = reinterpret_cast<AssignAnsiFn>(
                gforce::kGameStringAssignAnsi);
            const AssignFn assign = reinterpret_cast<AssignFn>(
                gforce::kGameStringAssign);
            const ReleaseFn release = reinterpret_cast<ReleaseFn>(
                gforce::kGameStringRelease);
            Address temporary = 0;
            bool assigned = false;
            __try
            {
                assign_ansi(&temporary, text);
                if (temporary != 0)
                {
                    assign(ToPointer(destination.value), &temporary);
                    assigned = true;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                assigned = false;
            }
            if (temporary != 0)
            {
                __try
                {
                    release(&temporary);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    assigned = false;
                }
            }
            return assigned;
        }

        static bool NativeLoadEntryMatchesExpected()
        {
            std::uint8_t actual[sizeof(gforce::kExpectedBeginNativeSaveLoad)] = {};
            for (std::size_t index = 0; index < sizeof(actual); ++index)
            {
                if (!TryRead(gforce::kBeginNativeSaveLoad + index, actual[index]))
                    return false;
            }
            return std::memcmp(actual, gforce::kExpectedBeginNativeSaveLoad,
                sizeof(actual)) == 0;
        }

        // `accepted` is the stock return value. The bool result separates a
        // checked call fault from a normal native refusal.
        static bool BeginNativeLoad(LoadSaveManagerRef manager,
            std::uint32_t slot, bool& accepted)
        {
            accepted = false;
            if (!manager)
                return false;
            using BeginNativeLoadFn = bool(__thiscall*)(void*, std::uint32_t);
            const BeginNativeLoadFn begin_load =
                reinterpret_cast<BeginNativeLoadFn>(gforce::kBeginNativeSaveLoad);
            __try
            {
                accepted = begin_load(ToPointer(manager.value), slot);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        static bool SpawnGPig(const Vec4* position, const Vec4* rotation,
            std::uint32_t gpig_id, SpawnContextRef context, EntityRef& out)
        {
            using SpawnGPigFn = void* (__cdecl*)(const Vec4*, const Vec4*,
                std::uint32_t, void*);
            out = {};
            const SpawnGPigFn spawn = reinterpret_cast<SpawnGPigFn>(
                gforce::kSpawnGPig);
            __try
            {
                out.value = ToAddress(spawn(position, rotation, gpig_id,
                    ToPointer(context.value)));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        static bool WeaponTypeToItemId(std::uint32_t weapon_type,
            std::uint32_t& out)
        {
            using WeaponTypeToItemIdFn = std::uint32_t(__cdecl*)(std::uint32_t);
            const WeaponTypeToItemIdFn resolve =
                reinterpret_cast<WeaponTypeToItemIdFn>(
                    gforce::kWeaponTypeToItemId);
            __try
            {
                out = resolve(weapon_type);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        static bool SetSelectedWeaponType(HandlerRef handler,
            std::uint32_t weapon_type)
        {
            if (!handler)
                return false;
            using SetSelectedWeaponTypeFn = void(__thiscall*)(void*,
                std::uint32_t);
            const SetSelectedWeaponTypeFn set_selected =
                reinterpret_cast<SetSelectedWeaponTypeFn>(
                    gforce::kSetSelectedWeaponType);
            __try
            {
                set_selected(ToPointer(handler.value), weapon_type);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        static bool CurrentWeaponId(HandlerRef handler, std::uint32_t& out)
        {
            if (!handler)
                return false;
            using GetCurrentWeaponIdFn = std::uint32_t(__thiscall*)(void*);
            const GetCurrentWeaponIdFn get_current =
                reinterpret_cast<GetCurrentWeaponIdFn>(
                    gforce::kGetCurrentWeaponId);
            __try
            {
                out = get_current(ToPointer(handler.value));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The stock ammo HUD passes the Handler's inventory pointer directly
        // to this resolver. `InventoryRef` stays opaque: the result is only
        // consumed through WeaponAmmoItemView or compared by identity.
        static bool ResolveWeaponRecord(InventoryRef inventory,
            std::uint32_t item_id, WeaponAmmoItemRef& out)
        {
            out = {};
            if (!inventory || item_id == 0xFFFFFFFFu)
                return false;
            using ResolveWeaponRecordFn = void* (__thiscall*)(void*,
                std::uint32_t);
            const ResolveWeaponRecordFn resolve =
                reinterpret_cast<ResolveWeaponRecordFn>(
                    gforce::kResolveWeaponRecord);
            __try
            {
                out.value = ToAddress(resolve(ToPointer(inventory.value), item_id));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Mirrors the two lookup branches in Fly_Active's confirmed dual-laser
        // block. The first function returns an already-active route item for a
        // slot; the second finds and activates the item matching a concrete id.
        // Both calls remain limited to the four IDs and two slots used there.
        static bool FindFlyDualLaserRouteItem(InventoryRef inventory,
            std::uint32_t route_slot, FlyDualLaserRouteItemRef& out)
        {
            out = {};
            if (!inventory ||
                (route_slot != gforce::kFlyDualLaserFirstRouteSlot &&
                    route_slot != gforce::kFlyDualLaserSecondRouteSlot) ||
                !CodePrefixMatches(gforce::kFindFlyDualLaserRouteItem,
                    gforce::kExpectedFindFlyDualLaserRouteItem,
                    sizeof(gforce::kExpectedFindFlyDualLaserRouteItem)))
            {
                return false;
            }
            using FindFlyDualLaserRouteItemFn = void* (__thiscall*)(void*,
                std::uint32_t);
            const FindFlyDualLaserRouteItemFn find =
                reinterpret_cast<FindFlyDualLaserRouteItemFn>(
                    gforce::kFindFlyDualLaserRouteItem);
            __try
            {
                out.value = ToAddress(find(ToPointer(inventory.value),
                    route_slot));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                out = {};
                return false;
            }
        }

        static bool ResolveFlyDualLaserRouteItem(InventoryRef inventory,
            std::uint32_t item_id, FlyDualLaserRouteItemRef& out)
        {
            out = {};
            const bool known_item_id =
                item_id == gforce::kFlyDualLaserDefaultItemBase ||
                item_id == gforce::kFlyDualLaserDefaultItemBase + 1u ||
                item_id == gforce::kFlyDualLaserAlternateItemBase ||
                item_id == gforce::kFlyDualLaserAlternateItemBase + 1u;
            if (!inventory || !known_item_id ||
                !CodePrefixMatches(gforce::kResolveFlyDualLaserRouteItem,
                    gforce::kExpectedResolveFlyDualLaserRouteItem,
                    sizeof(gforce::kExpectedResolveFlyDualLaserRouteItem)))
            {
                return false;
            }
            using ResolveFlyDualLaserRouteItemFn = void* (__thiscall*)(void*,
                std::uint32_t);
            const ResolveFlyDualLaserRouteItemFn resolve =
                reinterpret_cast<ResolveFlyDualLaserRouteItemFn>(
                    gforce::kResolveFlyDualLaserRouteItem);
            __try
            {
                out.value = ToAddress(resolve(ToPointer(inventory.value),
                    item_id));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                out = {};
                return false;
            }
        }

        // This is the preparation immediately before Fly_Active writes its two
        // laser item flags.  It stays narrow: controller -> controller-local
        // presentation context -> XMotorTask_Aim, then the exact retail setter.
        // It never selects a mode, assigns an active entity, or touches camera/HUD
        // state. `target` is a world-space endpoint and the stock route supplies
        // a zero Vec4 as its second setter argument.
        static bool SetFlyDualLaserPresentationTarget(ControllerRef controller,
            const Vec4& target)
        {
            if (!controller ||
                !CodePrefixMatches(gforce::kGetFlyDualLaserPresentationContext,
                    gforce::kExpectedGetFlyDualLaserPresentationContext,
                    sizeof(gforce::kExpectedGetFlyDualLaserPresentationContext)) ||
                !CodePrefixMatches(gforce::kGetFlyDualLaserAimTask,
                    gforce::kExpectedGetFlyDualLaserAimTask,
                    sizeof(gforce::kExpectedGetFlyDualLaserAimTask)) ||
                !CodePrefixMatches(gforce::kSetFlyDualLaserAimTarget,
                    gforce::kExpectedSetFlyDualLaserAimTarget,
                    sizeof(gforce::kExpectedSetFlyDualLaserAimTarget)))
            {
                return false;
            }

            using GetPresentationContextFn = void* (__thiscall*)(void*);
            using GetAimTaskFn = void* (__thiscall*)(void*, bool);
            using SetAimTargetFn = void* (__thiscall*)(void*, const Vec4*,
                const Vec4*);
            const GetPresentationContextFn get_context =
                reinterpret_cast<GetPresentationContextFn>(
                    gforce::kGetFlyDualLaserPresentationContext);
            const GetAimTaskFn get_aim_task =
                reinterpret_cast<GetAimTaskFn>(gforce::kGetFlyDualLaserAimTask);
            const SetAimTargetFn set_target =
                reinterpret_cast<SetAimTargetFn>(gforce::kSetFlyDualLaserAimTarget);
            const Vec4 zero = {};
            __try
            {
                void* const context = get_context(ToPointer(controller.value));
                if (!context)
                    return false;
                void* const aim_task = get_aim_task(context, true);
                if (!aim_task)
                    return false;
                set_target(aim_task, &target, &zero);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Terminal portion of XFlyFlyMode_Move::Update. Its only observed work
        // is to turn the supplied FlyFly state angles into a world LookAt point
        // and submit that point to the controller-local motor task. Mode choice,
        // input, camera and entity ownership remain outside this call.
        static bool SubmitFlyFlyBodyDirection(ControllerRef controller,
            MotorTaskRef fly_state)
        {
            if (!controller || !fly_state ||
                !CodePrefixMatches(gforce::kSubmitFlyFlyBodyDirection,
                    gforce::kExpectedSubmitFlyFlyBodyDirection,
                    sizeof(gforce::kExpectedSubmitFlyFlyBodyDirection)))
            {
                return false;
            }

            using SubmitFlyFlyBodyDirectionFn = int (__thiscall*)(void*, void*);
            const SubmitFlyFlyBodyDirectionFn submit =
                reinterpret_cast<SubmitFlyFlyBodyDirectionFn>(
                    gforce::kSubmitFlyFlyBodyDirection);
            __try
            {
                (void)submit(ToPointer(controller.value),
                    ToPointer(fly_state.value));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Calls only the already-registered Fly_Active::Update body.  Enter/Exit
        // and controller mode selection stay outside this boundary, which is what
        // keeps a remote presentation from stealing the local camera or HUD.
        static bool RunFlyActiveUpdate(ModeRef mode)
        {
            if (!mode ||
                !CodePrefixMatches(gforce::kFlyActiveUpdate,
                    gforce::kExpectedFlyActiveUpdate,
                    sizeof(gforce::kExpectedFlyActiveUpdate)))
            {
                return false;
            }

            ModeId mode_id = 0;
            Address vtable = 0;
            Address update = 0;
            const ModeView mode_view(mode);
            if (!mode_view.Id(mode_id) || mode_id != gforce::kFlyActiveModeId ||
                !mode_view.VTable(vtable) || vtable != gforce::kFlyActiveVTable ||
                !mode_view.Update(update) ||
                update != gforce::kFlyActiveUpdate)
            {
                return false;
            }

            using FlyActiveUpdateFn = void* (__thiscall*)(void*);
            const FlyActiveUpdateFn run = reinterpret_cast<FlyActiveUpdateFn>(
                gforce::kFlyActiveUpdate);
            __try
            {
                (void)run(ToPointer(mode.value));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        static bool ResolveAmmoPoolEntry(std::uint32_t ammo_id,
            AmmoPoolEntryRef& out)
        {
            out = {};
            if (ammo_id == 0xFFFFFFFFu)
                return false;
            using ResolveAmmoPoolEntryFn = void* (__thiscall*)(void*,
                std::uint32_t);
            const ResolveAmmoPoolEntryFn resolve =
                reinterpret_cast<ResolveAmmoPoolEntryFn>(gforce::kResolveAmmoEntry);
            __try
            {
                out.value = ToAddress(resolve(ToPointer(gforce::kAmmoPool), ammo_id));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        static bool EnsureGPigRdvTask(MotorSystemRef motor_system, bool enabled,
            MotorTaskRef& out)
        {
            if (!motor_system)
                return false;
            using EnsureGPigRdvTaskFn = void* (__thiscall*)(void*, bool);
            const EnsureGPigRdvTaskFn ensure =
                reinterpret_cast<EnsureGPigRdvTaskFn>(
                    gforce::kEnsureGPigRdvTask);
            out = {};
            __try
            {
                out.value = ToAddress(ensure(ToPointer(motor_system.value),
                    enabled));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        static bool ConfigureGPigRdvTask(SpawnContextRef context,
            HandlerRef handler)
        {
            if (!context || !handler)
                return false;
            using ConfigureGPigRdvTaskFn = void(__thiscall*)(void*, void*);
            const ConfigureGPigRdvTaskFn configure =
                reinterpret_cast<ConfigureGPigRdvTaskFn>(
                    gforce::kConfigureGPigRdvTask);
            __try
            {
                configure(ToPointer(context.value), ToPointer(handler.value));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

    private:
        static bool CodePrefixMatches(Address address,
            const std::uint8_t* expected, std::size_t expected_size)
        {
            if (address == 0 || !expected || expected_size == 0)
                return false;
            for (std::size_t index = 0; index < expected_size; ++index)
            {
                std::uint8_t actual = 0;
                if (!TryRead(address + index, actual) ||
                    actual != expected[index])
                {
                    return false;
                }
            }
            return true;
        }

        NativeGameApi() = delete;
    };

    class HealthComponentView final
    {
    public:
        explicit HealthComponentView(HealthComponentRef component) : component_(component) {}

        // Only channels 0 and 1 are proven by the native mutator hooks.
        bool ReadSlot(std::uint32_t slot, float& out) const
        {
            return component_ && slot < 2u && TryRead(AddOffset(component_.value,
                sizeof(float) * (slot + 1u)), out);
        }

        HealthComponentRef ref() const { return component_; }

    private:
        HealthComponentRef component_;
    };

    struct TriggerCounterState final
    {
        std::uint8_t value;
        std::int32_t threshold;
        std::uint32_t state_flags;
    };

    class TriggerView final
    {
    public:
        explicit TriggerView(TriggerRef trigger) : trigger_(trigger) {}

        // Do not interpret these offsets on another trigger subtype.
        bool ReadCounterState(TriggerCounterState& out) const
        {
            Address vtable = 0;
            return trigger_ && TryRead(trigger_.value, vtable) &&
                vtable == gforce::kTriggerCounterVtable &&
                TryRead(AddOffset(trigger_.value,
                    gforce::kTriggerCounterValueOffset), out.value) &&
                TryRead(AddOffset(trigger_.value,
                    gforce::kTriggerCounterThresholdOffset), out.threshold) &&
                TryRead(AddOffset(trigger_.value,
                    gforce::kTriggerStateFlagsOffset), out.state_flags);
        }

        bool Identity(TriggerIdentity& out) const
        {
            return TryRead(AddOffset(trigger_.value, gforce::kTriggerFamilyOffset),
                out.family) &&
                TryRead(AddOffset(trigger_.value, gforce::kTriggerSubtypeOffset),
                    out.subtype) &&
                TryRead(AddOffset(trigger_.value, gforce::kTriggerSpawnIdOffset),
                    out.definition_id);
        }

        bool Flags(std::uint32_t& out) const
        {
            return TryRead(AddOffset(trigger_.value, gforce::kTriggerFlagsOffset),
                out);
        }

        bool ReadTransform(Transform& out) const
        {
            return TryRead(AddOffset(trigger_.value,
                gforce::kTriggerPositionOffset), out.position) &&
                TryRead(AddOffset(trigger_.value,
                    gforce::kTriggerRotationOffset), out.rotation);
        }

        bool SpawnedEntity(EntityRef& out) const
        {
            return TryReadPointer(AddOffset(trigger_.value,
                gforce::kTriggerSpawnedObjectOffset), out);
        }

        TriggerRef ref() const { return trigger_; }

    private:
        TriggerRef trigger_;
    };

    class EntitySlotRepository final
    {
    public:
        bool Get(EntitySlot slot, EntityRef& out) const
        {
            return GetByIndex(static_cast<std::uint32_t>(slot), out);
        }

        bool GetSelectable(EntitySlot slot, EntityRef& out) const
        {
            out = {};
            return IsSelectableGPigSlot(slot) && Get(slot, out);
        }

        EntityRef GetSelectable(EntitySlot slot) const
        {
            EntityRef out = {};
            GetSelectable(slot, out);
            return out;
        }

        bool GetByIndex(std::uint32_t index, EntityRef& out) const
        {
            out = {};
            if (index >= kRetailEntitySlotCount)
                return false;
            return TryReadPointer(gforce::kGPigEntityArray +
                index * sizeof(Address), out);
        }

        bool GetController(EntitySlot slot, ControllerRef& out) const
        {
            EntityRef entity = {};
            HandlerRef handler = {};
            return Get(slot, entity) && EntityView(entity).Handler(handler) &&
                HandlerView(handler).Controller(out);
        }

        bool GetHandler(EntitySlot slot, HandlerRef& out) const
        {
            EntityRef entity = {};
            return Get(slot, entity) && EntityView(entity).Handler(out);
        }

        bool GetBinding(EntitySlot slot, EntitySlotBinding& out) const
        {
            out = {};
            if (!IsKnownEntitySlot(slot))
                return false;
            out.slot = slot;
            return Get(slot, out.entity) &&
                EntityView(out.entity).Handler(out.handler) &&
                HandlerView(out.handler).Controller(out.controller);
        }

        // Resolves an already-ticking controller to a slot considered by the
        // two verified native selectors. It deliberately establishes no
        // co-op lifecycle or ownership for that slot.
        EntitySlot FindSelectableGPigSlotForController(
            ControllerRef controller) const
        {
            if (!controller)
                return EntitySlot::None;

            HandlerRef owner = {};
            if (!ControllerView(controller).Owner(owner))
                return EntitySlot::None;

            for (const EntitySlot slot : kSelectableGPigSlots)
            {
                EntitySlotBinding candidate = {};
                if (GetBinding(slot, candidate) && candidate.handler == owner)
                    return slot;
            }
            return EntitySlot::None;
        }
    };

    class ActiveEntityStore final
    {
    public:
        bool Set(EntityRef entity) const
        {
            return entity && TryWrite(gforce::kActiveEntityA, entity.value) &&
                TryWrite(gforce::kActiveEntityB, entity.value);
        }

        bool Read(EntityRef& first, EntityRef& second) const
        {
            // An empty native active-entity slot is still a successful read. It
            // matters during Mooch hand-off because this store must be able to
            // replace a null transient value with the live fly entity.
            return TryReadAddress(gforce::kActiveEntityA, first.value) &&
                TryReadAddress(gforce::kActiveEntityB, second.value);
        }

        bool Restore(EntityRef first, EntityRef second) const
        {
            return TryWrite(gforce::kActiveEntityA, first.value) &&
                TryWrite(gforce::kActiveEntityB, second.value);
        }
    };
}
}
