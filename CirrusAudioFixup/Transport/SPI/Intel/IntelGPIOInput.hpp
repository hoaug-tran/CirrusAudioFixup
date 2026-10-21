// Read an existing host-owned GPIO input. Never change mux, direction, locks,
// output state or interrupt routing to infer a speaker identity.
#pragma once
#include "Transport/SPI/Intel/IntelGPIOOutput.hpp"
namespace cirrus::transport::intel {
struct IntelGPIOInputEvidence {
    const char* reason{"LAYOUT_REJECTED"};
    uint32_t revision{0}, padbar{0}, ownership{0}, config{0}, offset{0};
};
inline IOReturn readIntelGPIOInput(IntelGPIOWindow& window, size_t bytes, const IntelGPIOPad& pad, bool activeLow, uint32_t& value,
                                  IntelGPIOInputEvidence& evidence) {
    value = 0;
    evidence = {};
    if (bytes < 0x200 || pad.bit >= 32 || 0xa4U + uint32_t(pad.ownershipWord) * 4 > bytes)
        return kIOReturnBadArgument;
    const uint32_t revision = window.read32(0), padbar = window.read32(0x0c);
    evidence.revision = revision;
    evidence.padbar = padbar;
    if (revision == 0xffffffffU || revision >> 16 < 0x94 || !padbar || (padbar & 3) || padbar > bytes ||
        uint64_t(pad.pad) * 16 + 4 > bytes - padbar)
        return kIOReturnUnsupported;
    const uint32_t own = window.read32(0xa0 + uint32_t(pad.ownershipWord) * 4);
    const uint32_t config = window.read32(padbar + uint32_t(pad.pad) * 16);
    evidence.ownership = own;
    evidence.config = config;
    evidence.offset = padbar + uint32_t(pad.pad) * 16;
    if (own == 0xffffffffU || config == 0xffffffffU) {
        evidence.reason = "REGISTER_UNAVAILABLE";
        return kIOReturnNoDevice;
    }
    if (own & (0xfU << ((pad.bit % 8) * 4))) {
        evidence.reason = "PAD_NOT_HOST_OWNED";
        return kIOReturnNotPermitted;
    }
    if ((config & ((0xfU << 10) | (1U << 9))) || !(config & (1U << 8))) {
        evidence.reason = "PAD_NOT_GPIO_INPUT";
        return kIOReturnNotPermitted;
    }
    value = ((config >> 1) & 1U) ^ uint32_t(activeLow);
    evidence.reason = "INPUT_READ_WITHOUT_WRITES";
    return kIOReturnSuccess;
}
}
