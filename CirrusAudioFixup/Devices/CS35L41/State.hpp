// CS35L41 live silicon, firmware and playback state. No IOService or provider
// ownership belongs here. Embedded firmware views are borrowed and never freed.
// Each backend instance owns this state; platform ports and immutable firmware
// views remain borrowed until shutdown and backend destruction complete.
#pragma once

#include "Core/AmplifierLifecycle.hpp"
#include "Devices/CS35L41/Playback.hpp"
#include "Diagnostics/DiagnosticTypes.hpp"

namespace cirrus::devices::cs35l41 {

struct State {
    cirrus::core::BringUpDisposition bringUpDisposition{cirrus::core::BringUpDisposition::Continue};
    cirrus::devices::cs35l41::PlaybackTransition playbackTransition{};
    cirrus::devices::cs35l41::ShutdownTransition shutdownTransition{};
    bool clockWaitAfterCleanup{false};
    bool present{false};
    uint32_t deviceId{0};
    uint32_t revisionId{0};

    // Borrowed views into embedded resources; these pointers are never freed.
    const uint8_t* wmfwData{nullptr};
    size_t wmfwSize{0};
    const uint8_t* binData{nullptr};
    size_t binSize{0};
    bool firmwareValidated{false};
    bool firmwareIdentityPending{false};
    uint64_t firmwareIdentityDeadlineMs{0};
    // Reset by resource discovery on every cold start and wake. This is the
    // board/channel tuning gain, independent of the host volume slider.
    uint32_t tuningPcmGain{17};
    uint32_t finalCrc{0};

    unsigned int monitorCount{0};
    bool initialized{false};
    bool dspAlive{false};

    uint32_t lastIrq1Status1{0};
    uint32_t lastIrq1Status3{0};
    uint32_t lastPowerManagementStatus{0};
    uint32_t lastMailbox2{0};
    uint32_t lastStreamArbitrationError{0};
    uint32_t lastClockDetect{0};
    uint32_t lastStreamArbitrationControl{0};

    uint32_t lastTimestamp{0};
    bool playbackActive{false};
    bool playbackStartCancelled{false};
    uint32_t playbackStableCount{0};

    struct InterestingControl {
        char name[64]{};
        uint32_t address{0};
    } diagnosticControls[14]{};
    uint32_t diagnosticControlCount{0};
    uint32_t firmwareIdVersion{0};
    uint32_t monitorLogCountdown{0};
    uint32_t haloStateRegister{0};
    uint32_t haloHeartbeatRegister{0};
    // Faults survive normal polling. Only the isolated PLL recovery path may
    // clear this flag; first-failure evidence remains available afterwards.
    bool playbackFaulted{false};
    uint32_t cleanupAttempts{0};
    uint32_t pllRetryCooldown{0};
    uint32_t pllRecoveryAttempts{0};
    bool pllRecoveryPending{false};
    cirrus::diagnostics::DiagnosticState diagnostic{};
};

}
