// CS35L41 module factory: no service cast, helper callbacks or borrowed chip state.
#include "Devices/Amplifiers/CS35L41/CS35L41DriverFactory.hpp"
#include "Devices/Amplifiers/CS35L41/CS35L41Driver.hpp"
#include "Devices/Amplifiers/CS35L41/Hardware/Registers.hpp"
namespace cirrus::devices::amplifiers::cs35l41 {
core::AmplifierLifecycleHooks* createDriver(const platform::AmplifierDriverContext& c) {
    if (!c.host || !c.host->supportsResources() || !c.board.amplifiers || c.bindingIndex >= c.board.count ||
        !c.board.permits(c.actualBoard))
        return nullptr;
    const auto& binding = c.board.amplifiers[c.bindingIndex];
    if (binding.siliconId != registers::kValDeviceId || !c.endpoint.sameDevice(binding.endpoint) ||
        binding.channelIndex > uint32_t(core::AudioChannel::Right) ||
        !powerConfiguration(binding.driverConfiguration, binding.driverConfigurationSize, binding.driverConfigurationKind))
        return nullptr;
    return new CS35L41Driver(c);
}
void destroyDriver(core::AmplifierLifecycleHooks* driver) { delete driver; }
void inspectDevice(const platform::AmplifierDriverContext& c) {
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
