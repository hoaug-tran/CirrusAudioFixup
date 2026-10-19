// Register access contract shared by device code and host tests.
// Addresses and scalar values are in host byte order. The transport handles
// wire encoding; bulk buffers contain bytes in the device's transfer order.
// See LICENSE for distribution terms.
#pragma once

#include "Diagnostics/TraceTypes.hpp"
#include <stddef.h>
#include <stdint.h>

namespace cirrus::core {

class RegisterIO {
public:
    virtual ~RegisterIO() = default;

    // Native controller error when available; -1 is the legacy unspecified
    // failure sentinel. Read immediately on failure, before cleanup transfers.
    virtual int32_t lastError() const { return -1; }

    virtual bool read(uint32_t reg, uint32_t* value) = 0;

    // Drivers with precious/clear-on-read registers override diagnostic reads.
    virtual bool readForDiagnostic(uint32_t reg, uint32_t* value) { return read(reg, value); }
    virtual bool bulkReadForDiagnostic(uint32_t reg, uint8_t* data, size_t length) { return bulkRead(reg, data, length); }
    virtual bool write(uint32_t reg, uint32_t value) = 0;
    // Optional traced views keep operation provenance when an independent chip
    // driver borrows the service port. Native transports need no tracing policy.
    virtual bool readWithTrace(uint32_t reg, uint32_t* value, diagnostics::TraceSource source) {
        return source == diagnostics::TRACE_DUMP ? readForDiagnostic(reg, value) : read(reg, value);
    }
    virtual bool writeWithTrace(uint32_t reg, uint32_t value, diagnostics::TraceSource) { return write(reg, value); }
    virtual bool bulkReadWithTrace(uint32_t reg, uint8_t* data, size_t n, diagnostics::TraceSource source) {
        return source == diagnostics::TRACE_DUMP ? bulkReadForDiagnostic(reg, data, n) : bulkRead(reg, data, n);
    }
    virtual bool bulkWriteWithTrace(uint32_t reg, const uint8_t* data, size_t n, diagnostics::TraceSource) {
        return bulkWrite(reg, data, n);
    }

    virtual bool updateBits(uint32_t reg, uint32_t mask, uint32_t value) = 0;

    virtual bool bulkRead(uint32_t reg, uint8_t* data, size_t length) = 0;

    virtual bool bulkWrite(uint32_t reg, const uint8_t* data, size_t length) = 0;

    virtual bool pollBit(uint32_t reg, uint32_t mask, uint32_t targetVal, uint32_t timeoutMs) = 0;
};

}
