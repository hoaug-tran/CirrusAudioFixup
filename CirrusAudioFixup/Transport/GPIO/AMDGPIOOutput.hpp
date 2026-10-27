// AMD pinctrl-amd.c/h Linux v6.12: output-enable bit23, level bit22.
// Preserve configuration and never acknowledge W1C interrupt/wake status.
#pragma once
#include "Transport/GPIO/AMDGPIOInput.hpp"
namespace cirrus::transport::amd {
template<class Window, class Delay>
bool pulseGPIOReset(Window& window, size_t bytes, uint16_t pin, bool activeLow, Delay delay, const char*& reason) {
    reason = "RESET_MEMORY_UNAVAILABLE";
    if (!validGPIOOffset(bytes, pin))
        return false;
    const uint32_t offset = uint32_t(pin) * 4;
    const uint32_t config = window.read32(offset);
    reason = "RESET_REGISTER_UNAVAILABLE";
    if (config == UINT32_MAX)
        return false;
    const uint32_t levelMask = (1U << 23) | (1U << 22);
    const uint32_t base = (config & ~((1U << 28) | (1U << 29) | (1U << 22))) | (1U << 23);
    const uint32_t asserted = base | (activeLow ? 1U << 22 : 0);
    const uint32_t released = base | (activeLow ? 0 : 1U << 22);
    window.write32(offset, asserted);
    const bool held = (window.read32(offset) & levelMask) == (asserted & levelMask);
    delay(2);
    // Always release a shared reset even after an assertion/readback failure.
    window.write32(offset, released);
    const bool high = (window.read32(offset) & levelMask) == (released & levelMask);
    delay(2);
    reason = held && high ? "RESET_PULSE_COMPLETE" : "RESET_READBACK_FAILED";
    return held && high;
}
}
