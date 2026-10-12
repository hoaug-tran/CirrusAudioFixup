// Shared service facilities for independently implemented amplifier backends.
// The host owns board authorization and diagnostic publication, not silicon state
// or register sequences. Calls are serialized; the host outlives backend teardown.
#pragma once

#include "Diagnostics/DiagnosticTypes.hpp"

namespace cirrus::platform {

class AmplifierHost {
public:
    virtual ~AmplifierHost() = default;
    virtual bool authorized() const = 0;
    virtual const char* name() const = 0;
    virtual void setDiagnosticStage(diagnostics::DriverStage) = 0;
    virtual void markDiagnosticSuccess(diagnostics::DriverStage) = 0;
    virtual void recordDiagnosticFailure(diagnostics::DiagnosticFailure, uint32_t reg = 0, uint32_t expected = 0, uint32_t actual = 0,
                                         IOReturn result = kIOReturnSuccess) = 0;
};

}
