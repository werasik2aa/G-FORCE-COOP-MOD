#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <type_traits>

#include "retail_types.h"

namespace coop
{
namespace retail
{
    // This is the sole boundary for direct reads/writes of partially reverse-
    // engineered retail objects. Feature code receives typed values and never
    // needs to perform SEH around a known field offset itself.
    template<typename T>
    bool TryRead(Address address, T& out)
    {
        static_assert(std::is_trivially_copyable<T>::value,
            "Retail memory access is limited to trivially-copyable values");
        if (address == 0)
            return false;

        __try
        {
            std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(T));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    template<typename T>
    bool TryWrite(Address address, const T& value)
    {
        static_assert(std::is_trivially_copyable<T>::value,
            "Retail memory access is limited to trivially-copyable values");
        if (address == 0)
            return false;

        __try
        {
            std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline bool TryReadAddress(Address address, Address& out)
    {
        return TryRead(address, out);
    }

    inline bool TryReadPointer(Address address, EntityRef& out)
    {
        return TryReadAddress(address, out.value) && static_cast<bool>(out);
    }

    inline bool TryReadPointer(Address address, HandlerRef& out)
    {
        return TryReadAddress(address, out.value) && static_cast<bool>(out);
    }

    inline bool TryReadPointer(Address address, ControllerRef& out)
    {
        return TryReadAddress(address, out.value) && static_cast<bool>(out);
    }

    inline bool TryReadPointer(Address address, TriggerRef& out)
    {
        return TryReadAddress(address, out.value) && static_cast<bool>(out);
    }

    inline bool TryReadPointer(Address address, CameraHandlerRef& out)
    {
        return TryReadAddress(address, out.value) && static_cast<bool>(out);
    }
}
}
