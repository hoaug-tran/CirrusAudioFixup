//
// HDAController.hpp
// State observed by the service's HDA stream scanner.
// RUN, tag and format checks describe controller activity. They do not prove
// that PCM reaches the speaker pin or that the speakers produce sound.
// See LICENSE for distribution terms.
//

#pragma once

#include <stdint.h>

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

// Hardware access is owned by CirrusAudioFixup's serialized lifecycle.
// End of the HDA observation state.
}
