#pragma once

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace cirrus::platform::hda {

// State encapsulated from the HDA stream observations
struct HDAStreamState {
    bool observed{false};
    bool streamActive{false};
    bool converterPrepared{false};
    bool topologyLogged{false};
    uint8_t lastDescriptor{0xFF};
    uint8_t lastStreamTag{0};
    uint16_t lastFormat{0};
    uint32_t missCount{0};
};

class HDAController final {
public:
    static HDAController& getInstance() {
        static HDAController instance;
        return instance;
    }

    // Probes the HDA controller and updates stream telemetry
    bool syncCodec(HDAStreamState& state);

    // Determines if a specific format is supported by the CS35L41 ASP
    static bool supportedFormat(uint16_t format);

private:
    HDAController() = default;

    // Searches the IORegistry for the active AppleHDA-attached PCI controller
    IOService* getAudioController();
};

} // namespace cirrus::platform::hda
