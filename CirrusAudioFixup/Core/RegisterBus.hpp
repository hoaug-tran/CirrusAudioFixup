// Register framing is independent of physical controller and amplifier model.
// The owner serializes operation-level RMW, lifetime and power transitions.
#pragma once
#include <IOKit/IOLib.h>
#include <stddef.h>
#include <stdint.h>
namespace cirrus::core {
class RegisterBus {
public:
    virtual ~RegisterBus() = default;
    // Payload limit after accounting for the register header and bus limits.
    virtual size_t maxPayloadBytes(size_t headerBytes, bool read) const = 0;
    // One command/combined transaction: no STOP/CS release between header/RX.
    virtual IOReturn command(const uint8_t* tx, size_t txLength, uint8_t* rx, size_t rxLength) = 0;
};
class RegisterOperationLock {
public:
    virtual ~RegisterOperationLock() = default;
    virtual void lock() = 0;
    virtual void unlock() = 0;
};
}
