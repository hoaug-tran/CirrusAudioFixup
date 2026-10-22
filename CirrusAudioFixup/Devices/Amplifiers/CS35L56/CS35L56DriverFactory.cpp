// Construct and destroy drivers with owned CS35L56 state.
#include "Devices/Amplifiers/CS35L56/CS35L56DriverFactory.hpp"
#include "Devices/Amplifiers/CS35L56/Board/CS35L56BoardDiscovery.hpp"
#include "Devices/Amplifiers/CS35L56/CS35L56Driver.hpp"
namespace cirrus::devices::amplifiers::cs35l56 {
core::AmplifierLifecycleHooks* createDriver(const platform::AmplifierDriverContext& context) {
    if (!context.host || !context.host->supportsResources() || !context.board.permits(context.actualBoard) ||
        context.bindingIndex >= context.board.count)
        return nullptr;
    const auto& binding = context.board.amplifiers[context.bindingIndex];
    if (binding.siliconId != registers::kValDeviceId || binding.channelIndex > 1 || !context.endpoint.sameDevice(binding.endpoint))
        return nullptr;
    return new CS35L56Driver(context);
}
void destroyDriver(core::AmplifierLifecycleHooks* driver) { delete driver; }
void inspectDevice(const platform::AmplifierDriverContext& context) {
    if (context.host && context.board.amplifiers && context.bindingIndex < context.board.count) {
        CS35L56Driver driver(context);
        driver.inspect();
    }
}
bool validDebugPhase(const char* requested) {
    if (!requested)
        return false;
    const char* phases[] = {"probe", "firmware", "dsp"};
    for (const char* phase : phases)
        if (!strcmp(requested, phase))
            return true;
    return false;
}
platform::DiscoveredBoard* discoverBoard(const platform::BoardDiscoveryContext& context, const char*& reason) {
    return resolveBoardConfiguration(context, reason);
}
}
