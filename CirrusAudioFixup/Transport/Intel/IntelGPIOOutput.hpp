// Intel community GPIO output engine. Register facts: Linux v6.12
// pinctrl-intel.c (Intel Corporation) and pinctrl-alderlake.c (Andy Shevchenko).
// Only TX state is changed on an already configured host-owned GPIO output.
#pragma once
#include "Transport/SPIController.hpp"
#include <string.h>

namespace cirrus::transport::intel {
struct IntelGPIOPad {
    uint8_t memoryIndex, group, ownershipWord;
    uint16_t pad, bit;
};
inline bool lookupIntelGPIOPad(const char* hid, uint16_t gpio, IntelGPIOPad& output) {
    if (!hid || (strcmp(hid, "INTC1056") && strcmp(hid, "INTC1085")))
        return false;
    // GPIO numbering includes holes; pad numbering does not. Memory indices
    // follow the controller's ACPI resources, never fixed physical addresses.
    struct Group {
        uint16_t gpio, pin, count, communityBase;
        uint8_t memory, group, ownershipWord;
    };
    static constexpr Group groups[] = {
        {0, 0, 25, 0, 0, 0, 0},       {32, 25, 23, 0, 0, 1, 4},     {64, 48, 12, 0, 0, 2, 7},     {96, 60, 27, 0, 0, 3, 9},
        {128, 87, 8, 0, 0, 4, 13},    {160, 95, 24, 95, 1, 0, 0},   {192, 119, 8, 95, 1, 1, 3},   {224, 127, 24, 95, 1, 2, 4},
        {256, 160, 16, 151, 2, 1, 2}, {288, 176, 24, 151, 2, 2, 4}, {320, 200, 8, 200, 3, 0, 0},  {352, 208, 23, 200, 3, 1, 1},
        {384, 231, 15, 200, 3, 2, 4}, {416, 246, 24, 200, 3, 3, 6}, {448, 270, 25, 270, 4, 0, 0},
    };
    for (const auto& g : groups)
        if (gpio >= g.gpio && gpio - g.gpio < g.count) {
            const uint16_t bit = gpio - g.gpio;
            output = {g.memory, g.group, uint8_t(g.ownershipWord + bit / 8), uint16_t(g.pin + bit - g.communityBase), bit};
            return true;
        }
    return false;
}
class IntelGPIOWindow {
public:
    virtual ~IntelGPIOWindow() = default;
    virtual uint32_t read32(uint32_t offset) = 0;
    virtual void write32(uint32_t offset, uint32_t value) = 0;
};
class IntelGPIOOutput final : public SPIChipSelect {
public:
    IOReturn bind(IntelGPIOWindow& window, size_t bytes, const IntelGPIOPad& pad, bool activeLow) {
        if (mWindow || bytes < 0x200 || pad.bit >= 32 || 0x118U + uint32_t(pad.group) * 8 > bytes ||
            0xa4U + uint32_t(pad.ownershipWord) * 4 > bytes)
            return kIOReturnBadArgument;
        const uint32_t revision = window.read32(0);
        const uint32_t padbar = window.read32(0x0c);
        // Alder Lake's debounce-capable layout has four DWORDs per pad.
        if (revision == 0xffffffffU || revision >> 16 < 0x94 || !padbar || (padbar & 3) || padbar > bytes ||
            uint64_t(pad.pad) * 16 + 4 > bytes - padbar)
            return kIOReturnUnsupported;
        mWindow = &window;
        mPad = pad;
        mOffset = padbar + uint32_t(pad.pad) * 16;
        mActiveLow = activeLow;
        const auto result = validate();
        if (result != kIOReturnSuccess)
            mWindow = nullptr;
        return result;
    }
    IOReturn validate() const {
        if (!mWindow)
            return kIOReturnNotReady;
        const uint32_t own = mWindow->read32(0xa0 + uint32_t(mPad.ownershipWord) * 4);
        const uint32_t lockTx = mWindow->read32(0x114 + uint32_t(mPad.group) * 8);
        const uint32_t config = mWindow->read32(mOffset);
        if (own == 0xffffffffU || lockTx == 0xffffffffU || config == 0xffffffffU)
            return kIOReturnNoDevice;
        if ((own & (0xfU << ((mPad.bit % 8) * 4))) || (lockTx & (1U << mPad.bit)) || (config & ((0xfU << 10) | (1U << 8) | (0xfU << 17))))
            return kIOReturnNotPermitted;
        return kIOReturnSuccess;
    }
    IOReturn setActive(bool active) override {
        const auto result = validate();
        if (result != kIOReturnSuccess)
            return result;
        const uint32_t before = mWindow->read32(mOffset);
        if (before == 0xffffffffU)
            return kIOReturnNoDevice;
        const uint32_t after = (before & ~1U) | uint32_t(active != mActiveLow);
        mWindow->write32(mOffset, after);
        return (mWindow->read32(mOffset) & ~2U) == (after & ~2U) ? kIOReturnSuccess : kIOReturnIOError;
    }
    void unbind() { mWindow = nullptr; }

private:
    IntelGPIOWindow* mWindow{nullptr};
    IntelGPIOPad mPad{};
    uint32_t mOffset{0};
    bool mActiveLow{true};
};
}
