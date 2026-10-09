//
// DiagnosticTracer.hpp
// Bounded I2C flight recorder.
// The lock protects trace indices and counters, not hardware sequencing.
// Record completed transfers here; never hold this lock across I2C or sleeps.
// See LICENSE for distribution terms.
//

#pragma once

#include "CirrusAudioFixup.hpp"

#include "Support/Logging.hpp"

#include <IOKit/IOLocks.h>

namespace cirrus::diagnostics {

class DiagnosticTracer {
public:
    static constexpr size_t kDefaultBufferSize = 1024;

    static void record(TraceEntry* buffer, size_t capacity, uint32_t& head, uint32_t& tail, TraceStats& stats, IOLock* lock,
                       TraceSource source, uint8_t ampIndex, bool isWrite, bool isBulk, uint32_t reg, uint32_t valOrLen, IOReturn ret) {
        if (!buffer || !lock || capacity == 0)
            return;

        IOLockLock(lock);
        if (ret == kIOReturnOffline) {
            stats.noackCount++;
        } else if (ret == kIOReturnTimeout) {
            stats.retries++;
        }

        if (isBulk) {
            if (ret == kIOReturnSuccess)
                stats.bulkSuccess++;
            else
                stats.bulkFail++;
        } else if (isWrite) {
            if (ret == kIOReturnSuccess)
                stats.writeSuccess++;
            else
                stats.writeFail++;
        } else {
            if (ret == kIOReturnSuccess)
                stats.readSuccess++;
            else
                stats.readFail++;
        }

        uint64_t time = 0;
        clock_get_uptime(&time);
        uint64_t timeMs = 0;
        absolutetime_to_nanoseconds(time, &timeMs);

        TraceEntry& entry = buffer[head];
        entry.timestamp = timeMs / 1000000;
        entry.amp = ampIndex;
        entry.isWrite = isWrite;
        entry.isBulk = isBulk;
        entry.reg = reg;
        entry.value = valOrLen;
        entry.ret = ret;
        entry.source = source;

        head = (head + 1) % capacity;
        if (head == tail)
            tail = (tail + 1) % capacity;

        IOLockUnlock(lock);
    }
};

}
