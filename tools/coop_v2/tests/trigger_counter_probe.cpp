// Standalone Win32 test: no game process, hooks or native calls.
#include "../retail/retail_views.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    using namespace coop;
    alignas(std::uint32_t) unsigned char storage[0x180] = {};
    unsigned char original[sizeof(storage)] = {};
    const retail::Address vtable = gforce::kTriggerCounterVtable;
    const std::uint8_t value = 7;
    const std::int32_t threshold = 12;
    const std::uint32_t flags = 0x51;
    std::memcpy(storage, &vtable, sizeof(vtable));
    std::memcpy(storage + gforce::kTriggerCounterValueOffset, &value, sizeof(value));
    std::memcpy(storage + gforce::kTriggerCounterThresholdOffset,
        &threshold, sizeof(threshold));
    std::memcpy(storage + gforce::kTriggerStateFlagsOffset, &flags, sizeof(flags));
    std::memcpy(original, storage, sizeof(storage));

    const retail::TriggerView view(retail::TriggerRef{retail::ToAddress(storage)});
    retail::TriggerCounterState state = {};
    assert(view.ReadCounterState(state));
    assert(state.value == value && state.threshold == threshold);
    assert(state.state_flags == flags);
    assert(std::memcmp(storage, original, sizeof(storage)) == 0);

    const std::uint8_t max_value = 255;
    const std::int32_t negative_threshold = -1;
    std::memcpy(storage + gforce::kTriggerCounterValueOffset,
        &max_value, sizeof(max_value));
    std::memcpy(storage + gforce::kTriggerCounterThresholdOffset,
        &negative_threshold, sizeof(negative_threshold));
    assert(view.ReadCounterState(state));
    assert(state.value == 255 && state.threshold == -1);

    // Same allocation and offsets, different native class: must not classify.
    const retail::Address other_vtable = vtable + 4;
    std::memcpy(storage, &other_vtable, sizeof(other_vtable));
    assert(!view.ReadCounterState(state));
    assert(!retail::TriggerView(retail::TriggerRef{}).ReadCounterState(state));
    std::puts("TRIGGER_COUNTER_TEST_OK: exact class, fields, read-only, rejection");
}
