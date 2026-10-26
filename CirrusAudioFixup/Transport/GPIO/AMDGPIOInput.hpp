// AMD pinctrl-amd.c/h, Linux v6.12: resource 0, pin * 4, PIN_STS_OFF=16.
// Read firmware-configured inputs only; never change direction, mux or IRQ state.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
namespace cirrus::transport::amd {
inline bool supportsGPIOInput(const char* hid) {
    return hid && (!strcmp(hid, "AMD0030") || !strcmp(hid, "AMDI0030") || !strcmp(hid, "AMDI0031"));
}
inline bool readGPIOInput(const volatile uint32_t* registers, size_t bytes, uint16_t pin, bool activeLow,
                          uint32_t& value, const char*& reason) {
    reason = "GPIO_INPUT_BOUNDS_INVALID";
    const size_t offset = size_t(pin) * 4;
    if (!registers || (reinterpret_cast<uintptr_t>(registers) & 3) || offset > bytes || bytes - offset < 4 ||
        offset == 0xfc || offset == 0x2f8 || offset == 0x2fc)
        return false;
    const uint32_t config = registers[pin];
    reason = "GPIO_REGISTER_UNAVAILABLE";
    if (config == UINT32_MAX)
        return false;
    reason = "GPIO_PIN_IS_OUTPUT";
    if (config & (1U << 23))
        return false;
    value = ((config >> 16) & 1U) ^ uint32_t(activeLow);
    reason = "GPIO_INPUT_READ_WITHOUT_WRITES";
    return true;
}
}
