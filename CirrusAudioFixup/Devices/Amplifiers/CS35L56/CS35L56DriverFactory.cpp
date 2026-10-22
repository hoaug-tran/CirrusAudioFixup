// Construct the CS35L56 diagnostic driver. No lifecycle factory is registered.
#include "Devices/Amplifiers/CS35L56/CS35L56DriverFactory.hpp"
#include "Devices/Amplifiers/CS35L56/Board/CS35L56BoardDiscovery.hpp"
#include "Devices/Amplifiers/CS35L56/CS35L56Driver.hpp"
namespace cirrus::devices::amplifiers::cs35l56 {
void inspectDevice(const platform::AmplifierDriverContext& context) {
    if (context.host && context.board.amplifiers && context.bindingIndex < context.board.count) {
        CS35L56Driver driver(context);
        driver.inspect();
    }
}
bool validDebugPhase(const char* requested) {
    if (!requested)
        return false;
    const char* phases[] = {"probe"};
    for (const char* phase : phases)
        if (!strcmp(requested, phase))
            return true;
    return false;
}
platform::DiscoveredBoard* discoverBoard(const platform::BoardDiscoveryContext& context, const char*& reason) {
    return resolveBoardConfiguration(context, reason);
}
}
