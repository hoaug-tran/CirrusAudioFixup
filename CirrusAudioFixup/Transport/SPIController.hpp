// Synchronous SPI controller contract. This is a project interface, not an
// existing Apple or VoodooSPI ABI. An owning controller driver must implement
// it before the register adapter can access physical hardware.
#pragma once

#include <IOKit/IOLib.h>
#include <stddef.h>
#include <stdint.h>

namespace cirrus::transport {

struct SPIEndpoint {
    uint32_t chipSelect;
    uint32_t clockHz;
    uint8_t mode;
};

class SPIController {
public:
    virtual ~SPIController() = default;

    // Limit includes header and payload. Zero means unavailable, not unlimited.
    virtual size_t maxTransferBytes() const = 0;

    // Full-duplex, 8-bit words, MSB first. Assert the selected CS once across
    // the entire frame, then deassert on success AND failure. Buffers are
    // borrowed until this sleepable call returns; rx may be null on writes.
    // Serialize the shared bus, bound waits, honor endpoint clock/mode and
    // reject transfers while stopped/suspended. Do not retain these buffers.
    virtual IOReturn exchange(const SPIEndpoint& endpoint, const uint8_t* tx,
                              uint8_t* rx, size_t length) = 0;
};

}
