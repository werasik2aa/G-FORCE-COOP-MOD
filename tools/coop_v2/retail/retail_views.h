#pragma once

#include <cstdint>

#include "../gforce_constants.h"
#include "retail_memory.h"

namespace coop
{
namespace retail
{
    class HandlerView final
    {
    public:
        explicit HandlerView(HandlerRef handler) : handler_(handler) {}

        bool Controller(ControllerRef& out) const
        {
            return TryReadPointer(AddOffset(handler_.value,
                gforce::kHandlerControllerOffset), out);
        }

        bool Inventory(Address& out) const
        {
            return TryReadAddress(AddOffset(handler_.value,
                gforce::kHandlerInventoryOffset), out) && out != 0;
        }

        bool SelectedWeaponType(std::uint32_t& out) const
        {
            return TryRead(AddOffset(handler_.value,
                gforce::kHandlerSelectedWeaponTypeOffset), out);
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
                    value.rotation);
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

    class ControllerView final
    {
    public:
        explicit ControllerView(ControllerRef controller) : controller_(controller) {}

        bool Owner(HandlerRef& out) const
        {
            return TryReadPointer(AddOffset(controller_.value,
                gforce::kControllerOwnerOffset), out);
        }

        bool CurrentMode(ModeId& out) const
        {
            Address mode = 0;
            if (!TryReadAddress(AddOffset(controller_.value,
                gforce::kControllerModeOffset), mode) || mode == 0)
            {
                return false;
            }
            return TryRead(AddOffset(mode, gforce::kModeIdOffset), out);
        }

        ControllerRef ref() const { return controller_; }

    private:
        ControllerRef controller_;
    };

    class TriggerView final
    {
    public:
        explicit TriggerView(TriggerRef trigger) : trigger_(trigger) {}

        bool Identity(TriggerIdentity& out) const
        {
            return TryRead(AddOffset(trigger_.value, gforce::kTriggerFamilyOffset),
                out.family) &&
                TryRead(AddOffset(trigger_.value, gforce::kTriggerSubtypeOffset),
                    out.subtype) &&
                TryRead(AddOffset(trigger_.value, gforce::kTriggerSpawnIdOffset),
                    out.definition_id);
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

    class PlayerRepository final
    {
    public:
        bool Get(PlayerSlot slot, EntityRef& out) const
        {
            const std::uint32_t index = static_cast<std::uint32_t>(slot);
            return TryReadPointer(gforce::kGPigEntityArray +
                index * sizeof(Address), out);
        }

        bool GetController(PlayerSlot slot, ControllerRef& out) const
        {
            EntityRef entity = {};
            HandlerRef handler = {};
            return Get(slot, entity) && EntityView(entity).Handler(handler) &&
                HandlerView(handler).Controller(out);
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
            return TryReadPointer(gforce::kActiveEntityA, first) &&
                TryReadPointer(gforce::kActiveEntityB, second);
        }
    };
}
}
