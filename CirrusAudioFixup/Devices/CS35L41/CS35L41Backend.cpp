// CS35L41 module factory: no service cast, helper callbacks or borrowed chip state.
#include "Devices/CS35L41/CS35L41Backend.hpp"
#include "Devices/CS35L41/CS35L41Driver.hpp"
#include "Devices/CS35L41/Hardware/Registers.hpp"
namespace cirrus::devices::cs35l41 {
core::AmplifierLifecycleHooks* createBackend(const platform::AmplifierBackendContext& c) {
    if (!c.host || !c.host->supportsResources() || !c.board.amplifiers || c.bindingIndex >= c.board.count ||
        !c.board.permits(c.actualBoard))
        return nullptr;
    const auto& binding = c.board.amplifiers[c.bindingIndex];
    if (binding.siliconId != registers::kValDeviceId || !c.endpoint.sameDevice(binding.endpoint) ||
        binding.channelIndex > uint32_t(core::AudioChannel::Right))
        return nullptr;
    return new CS35L41Driver(c);
}
void destroyBackend(core::AmplifierLifecycleHooks* backend) { delete backend; }
void inspectBackend(const platform::AmplifierBackendContext& c) {
    if (c.host && c.host->supportsResources() && c.board.amplifiers && c.bindingIndex < c.board.count) {
        CS35L41Driver driver(c);
        driver.inspect();
    }
}
bool validDebugPhase(const char* requested) {
    if (!requested)
        return false;
    const char* phases[] = {"probe", "otp", "errata", "clock", "asp", "gpio", "platform", "firmware", "dsp"};
    for (const char* phase : phases)
        if (!strcmp(requested, phase))
            return true;
    return false;
}
}
