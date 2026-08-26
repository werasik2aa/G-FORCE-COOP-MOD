#pragma once

#include <cstddef>
#include <cstdint>

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

    struct Transform final
    {
        Vec4 position;
        Vec4 rotation;
    };

    static_assert(sizeof(Transform) == 8 * sizeof(float),
        "Transform must preserve two retail vec4 fields");

    enum class PlayerSlot : std::uint32_t
    {
        LocalP1 = 1,
        RemoteP2 = 2,
        AuxiliaryP3 = 3,
        Mooch = 4
    };

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

    struct ControllerRef final
    {
        Address value = 0;

        explicit operator bool() const { return value != 0; }
        bool operator==(const ControllerRef& other) const { return value == other.value; }
        bool operator!=(const ControllerRef& other) const { return !(*this == other); }
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
