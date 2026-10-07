#pragma once

#include <cmath>

namespace coop
{
namespace presentation
{
    inline float BlendPosition(float current, float target, float factor)
    {
        // Use double for the difference: finite wire endpoints can otherwise
        // overflow a float subtraction before the final write barrier.
        return static_cast<float>(static_cast<double>(current) +
            (static_cast<double>(target) - current) * factor);
    }

    inline float BlendAngle(float current, float target, float factor)
    {
        constexpr double two_pi = 6.28318530717958647692;
        const double delta = std::remainder(
            static_cast<double>(target) - current, two_pi);
        return static_cast<float>(std::remainder(
            static_cast<double>(current) + delta * factor, two_pi));
    }
}
}
