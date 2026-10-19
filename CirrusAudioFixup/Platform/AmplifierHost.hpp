// Shared service facilities for independently implemented amplifier drivers.
// The host owns board authorization and diagnostic publication, not silicon state
// or register sequences. Calls are serialized; the host outlives driver teardown.
#pragma once

#include "Diagnostics/DiagnosticTypes.hpp"
#include "Platform/HDA/HDAStreamState.hpp"
#include <stddef.h>
class IOService;
class OSObject;

namespace cirrus::platform {

class AmplifierHost {
public:
    virtual ~AmplifierHost() = default;
    virtual bool authorized() const = 0;
    virtual const char* name() const = 0;
    virtual void setDiagnosticStage(diagnostics::DriverStage) = 0;
    virtual void markDiagnosticSuccess(diagnostics::DriverStage) = 0;
    virtual void recordDiagnosticFailure(diagnostics::DiagnosticFailure, uint32_t reg = 0, uint32_t expected = 0, uint32_t actual = 0,
                                         IOReturn result = kIOReturnSuccess, bool captureSnapshot = true) = 0;
    virtual const diagnostics::DiagnosticState& diagnostics() const {
        static const diagnostics::DiagnosticState empty{};
        return empty;
    }
    virtual bool supportsResources() const { return false; }
    virtual uint8_t endpointIndex() const { return 0; }
    virtual bool bootArgEnabled(const char*) const { return false; }
    virtual bool bootArgStrEquals(const char*, const char*) const { return false; }
    virtual bool parseBootArgument(const char*, void*, size_t) const { return false; }
    virtual bool stopAfterDebugStage(const char*) { return false; }
    virtual bool debugHalted() const { return false; }
    virtual void publish(const char*, const char*) {}
    virtual void publish(const char*, uint64_t, unsigned) {}
    virtual void publish(const char*, OSObject*) {}
    virtual void remove(const char*) {}
    virtual void publishVerdict() {}
    // Retained result, caller releases.
    virtual IOService* audioController() { return nullptr; }
    virtual hda::HDAStreamState streamObservation() const { return {}; }
    virtual bool hasRouteEvents() const { return false; }
    virtual bool routeUnchanged(uint32_t) const { return false; }
    virtual bool synchronizeRoute() { return false; }
    virtual void beginDiagnosticCapture() {}
    virtual void endDiagnosticCapture() {}
};

}
