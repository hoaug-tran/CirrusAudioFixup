#pragma once

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace cirrus::platform::hda {

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

    bool syncCodec(HDAStreamState& state);

    static bool supportedFormat(uint16_t format);

private:
    HDAController() = default;

    IOService* getAudioController();
};

}
