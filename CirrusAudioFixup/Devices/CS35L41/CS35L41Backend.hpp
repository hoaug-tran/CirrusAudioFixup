// CS35L41 lifecycle factory and the staged platform contract.
// Calls are serialized by the owner. The platform, state, register port and
// context resources outlive the backend through shutdown and diagnostics.
// The state pointer and endpoint/channel/model identity stay fixed while bound.
// This bridge still delegates silicon operations pending helper migration;
// it is an internal source interface, not a VoodooI2C provider ABI.
#pragma once

#include "Core/AudioTypes.hpp"
#include "Devices/CS35L41/State.hpp"
#include "Platform/AmplifierBackendRegistry.hpp"
#include "Platform/AmplifierHost.hpp"

namespace cirrus::devices::cs35l41 {

enum class Operation {
    InitializeCodec,
    ApplyErrata,
    ConfigurePLL,
    ConfigureASP,
    ConfigureGPIO,
    ConfigureHardware,
    StopDSP,
    PowerUp,
    VerifyIdle,
    VerifyDSP
};

inline constexpr const char* kPlatformContract = "cs35l41-service-bridge-v1";

// Temporary silicon helper bridge; ordinary backends need only AmplifierHost.
class Platform : public platform::AmplifierHost {
public:
    virtual ~Platform() = default;
    virtual State* state() const = 0;
    virtual core::BusEndpoint endpoint() const = 0;
    virtual core::AudioChannel channel() const = 0;
    virtual core::AmplifierModel model() const = 0;
    virtual bool perform(Operation) = 0;
    virtual bool bypassDSP() const = 0;
    virtual bool stopAfterDebugStage(const char*) = 0;
    virtual bool firmwarePhaseHalted() const = 0;
    virtual void discoverFirmware() = 0;
    virtual void initializeFirmware() = 0;
    virtual void logASPSnapshot() = 0;
    virtual IOReturn commitPlayback() = 0;
    virtual bool stopPlayback() = 0;
    virtual void captureDiagnostics() = 0;
    virtual bool configureInterrupt(bool enabled) = 0;
    virtual bool checkProtection() = 0;
    virtual void monitor(const core::PlaybackContext&) = 0;
    virtual bool validatePlayback(const core::PlaybackContext&) = 0;
    virtual void cancelPlayback() = 0;
};

// platformContext must point to Platform (including any base-pointer adjustment).
// Null state or mismatched endpoint/channel rejects construction without I/O.
core::AmplifierLifecycleHooks* createBackend(const platform::AmplifierBackendContext&);
void destroyBackend(core::AmplifierLifecycleHooks*);
}
