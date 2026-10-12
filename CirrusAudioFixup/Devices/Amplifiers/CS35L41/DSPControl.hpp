// CS35L41 DSP halt/reset transaction. The owner serializes calls and supplies
// the existing microsecond delay; this helper owns no platform resources.
#pragma once

#include "Core/RegisterIO.hpp"
#include "Devices/Amplifiers/CS35L41/Hardware/Registers.hpp"
#include "Devices/Amplifiers/CS35L41/SiliconState.hpp"

namespace cirrus::devices::amplifiers::cs35l41 {

class DSPControl {
public:
    static bool halt(core::RegisterIO& io, SiliconState& state, void (*delayUs)(uint32_t)) {
        // Invalidate cached readiness before touching hardware. Always attempt
        // reset after halt failure; readback is useful only if both succeeded.
        if (!delayUs)
            return false;
        state.dspAlive = false;
        state.monitorCount = 0;
        const bool halted = io.updateBits(registers::kRegDsp1CcmCoreControl, registers::kValHaloCoreEnable, 0);
        const bool reset = io.updateBits(registers::kRegDsp1CoreSoftReset, 1, 1);
        delayUs(1000);
        uint32_t core = 0;
        return halted && reset && io.read(registers::kRegDsp1CcmCoreControl, &core) && !(core & registers::kValHaloCoreEnable);
    }
};

}
