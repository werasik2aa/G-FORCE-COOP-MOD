// Standalone numeric regression cases; no game process or native calls.
#include "../remote_root_math.h"
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdio>

int main()
{
    using coop::presentation::BlendAngle;
    using coop::presentation::BlendPosition;
    constexpr float pi = 3.14159265358979323846f;

    // Crossing the branch cut should stay near the back-facing direction,
    // not turn through zero/front as ordinary scalar lerp does.
    const float turn = BlendAngle(pi - 0.02f, -pi + 0.02f, 0.5f);
    assert(std::fabs(std::fabs(turn) - pi) < 0.0001f);
    assert(std::fabs(BlendAngle(0.0f, 0.8f, 0.25f) - 0.2f) < 0.0001f);
    assert(std::fabs(BlendAngle(-pi + 0.02f, pi - 0.02f, 0.5f)) > 3.1f);

    // Finite wire endpoints remain finite even when their float difference
    // overflows; halfway between opposite extremes is still zero.
    assert(BlendPosition(-FLT_MAX, FLT_MAX, 0.5f) == 0.0f);
    assert(std::isfinite(BlendAngle(-FLT_MAX, FLT_MAX, 0.5f)));
    assert(BlendPosition(2.0f, 10.0f, 0.0f) == 2.0f);
    assert(BlendPosition(2.0f, 10.0f, 1.0f) == 10.0f);

    // A persistent native downward displacement must not become the next
    // interpolation baseline. The retained presentation converges to owner Y.
    float retained = -1.0f;
    float physics_feedback = -1.0f;
    for (int frame = 0; frame < 120; ++frame)
    {
        retained = BlendPosition(retained, 0.0f, 0.1f);
        physics_feedback = BlendPosition(physics_feedback - 0.05f, 0.0f, 0.1f);
    }
    assert(std::fabs(retained) < 0.00001f);
    assert(physics_feedback < -0.4f);
    std::puts("REMOTE_ROOT_MATH_OK: branch-cut turning, finite extremes, drift convergence");
}
