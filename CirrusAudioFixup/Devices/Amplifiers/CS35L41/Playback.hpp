//
// Playback.hpp
// CS35L41 muted preparation and playback commit.
// Each advance performs one bounded phase and returns to the workloop.
// The caller owns route cancellation, stereo coordination and rollback.
// See LICENSE for distribution terms.
//

#pragma once

#include "Core/RegisterIO.hpp"
#include "Devices/Amplifiers/CS35L41/Hardware/Registers.hpp"
#include "Devices/Amplifiers/CS35L41/PowerConfiguration.hpp"
#include "Diagnostics/DiagnosticTypes.hpp"

namespace cirrus::devices::amplifiers::cs35l41 {

enum class PlaybackPhase : uint8_t { Idle, Resume, Power, Pup, Output, Pll, Prepared, Active, Failed };
enum class PlaybackProgress : uint8_t { Pending, Prepared, Failed };
enum class ShutdownPhase : uint8_t { Idle, Pdn, Pause, Finish, Complete, Failed };

struct ShutdownTransition {
    ShutdownPhase phase{ShutdownPhase::Idle};
    uint64_t deadlineMs{0};
    bool ok{true};
    bool pdnDone{false};
    bool haltDsp{false};
    diagnostics::DiagnosticFailure failure{diagnostics::DIAG_OK};
    uint32_t failureRegister{0};
    uint32_t actual{0};

    bool pending() const { return phase != ShutdownPhase::Idle && phase != ShutdownPhase::Complete && phase != ShutdownPhase::Failed; }
};

struct PlaybackTransition {
    uint64_t prepareStartedMs{0};
    PlaybackPhase phase{PlaybackPhase::Idle};
    uint64_t deadlineMs{0};
    uint32_t generation{0};
    uint32_t streamTag{0};
    uint32_t streamFormat{0};
    diagnostics::DiagnosticFailure failure{diagnostics::DIAG_OK};
    uint32_t failureRegister{0};
    uint32_t expected{0};
    uint32_t actual{0};

    bool pending() const { return phase != PlaybackPhase::Idle && phase != PlaybackPhase::Active && phase != PlaybackPhase::Failed; }
};

// Absolute monotonic deadlines are supplied by the service. Retry counts
// cannot account for a slow addressed transfer or a delayed timer callback.
class Playback final {
    static uint64_t sampleTime(uint64_t fallback, uint64_t (*clock)()) { return clock ? clock() : fallback; }
    static PlaybackProgress fail(PlaybackTransition& state, diagnostics::DiagnosticFailure failure, uint32_t reg = 0, uint32_t expected = 0,
                                 uint32_t actual = 0) {
        state.phase = PlaybackPhase::Failed;
        state.failure = failure;
        state.failureRegister = reg;
        state.expected = expected;
        state.actual = actual;
        return PlaybackProgress::Failed;
    }

    static bool closeKeys(core::RegisterIO& io) {
        // Attempt both lock writes even when the first transfer fails.
        bool first = io.write(0x40, 0xCC);
        bool second = io.write(0x40, 0x33);
        return first && second;
    }

    static PlaybackProgress mailbox(core::RegisterIO& io, PlaybackTransition& state, uint64_t now, uint64_t (*clock)()) {
        uint32_t status = 0;
        if (!io.read(0x13004, &status))
            return fail(state, diagnostics::DIAG_DSP_MAILBOX, 0x13004);
        state.actual = status;
        // Like Linux polling, observe acknowledgement before deciding timeout.
        // A delayed workloop or the other stereo endpoint may consume the wait
        // window while the DSP has already completed the command.
        if (status == 0xFFFFFFFFU || status == 0x00FFFFFFU)
            return fail(state, diagnostics::DIAG_DSP_MAILBOX, 0x13004, 0, status);
        if (status == registers::kStatusMailboxRunning)
            return PlaybackProgress::Prepared;
        if (sampleTime(now, clock) >= state.deadlineMs)
            return fail(state, diagnostics::DIAG_DSP_MAILBOX, 0x13004, 0, status);
        return PlaybackProgress::Pending;
    }

public:
    // Runtime shutdown yields for PDN and DSP acknowledgements. The service
    // retains a synchronous quiesce path for IOKit PM acknowledgement and
    // final teardown, where a pending timer cannot outlive the provider.
    static PlaybackProgress shutdown(core::RegisterIO& io, ShutdownTransition& state, uint64_t now, bool dspAlive, uint32_t,
                                     uint64_t (*clock)() = nullptr, const PowerConfiguration& power = kLegacyPowerConfiguration) {
        using namespace diagnostics;
        auto failed = [&](DiagnosticFailure failure, uint32_t reg, uint32_t actual = 0) {
            state.ok = false;
            if (state.failure == DIAG_OK) {
                state.failure = failure;
                state.failureRegister = reg;
                state.actual = actual;
            }
        };
        auto write = [&](uint32_t reg, uint32_t value) {
            if (!io.write(reg, value))
                failed(DIAG_IDLE_ROLLBACK, reg);
        };
        auto update = [&](uint32_t reg, uint32_t mask, uint32_t value) {
            if (!io.updateBits(reg, mask, value))
                failed(DIAG_IDLE_ROLLBACK, reg);
        };
        auto protectedWrite = [&](uint32_t reg, uint32_t value) {
            bool unlocked = io.write(0x40, 0x55) && io.write(0x40, 0xAA);
            if (unlocked)
                write(reg, value);
            bool locked = closeKeys(io);
            if (!unlocked || !locked)
                failed(DIAG_IDLE_ROLLBACK, 0x40);
        };
        switch (state.phase) {
        case ShutdownPhase::Idle: {
            write(0x6C04, 0);
            write(0x6000, 0xA678);
            uint32_t before = 0;
            bool known = io.read(0x2014, &before);
            if (!known)
                failed(DIAG_IDLE_ROLLBACK, 0x2014);
            bool needPdn = !known || (before & 1);
            if (!power.internal())
                protectedWrite(0x7438, 0x00585941);
            if (needPdn)
                write(0x10010, 0x00800000);
            update(0x2014, 1, 0);
            if (!power.internal())
                protectedWrite(0x742C, 9);
            state.pdnDone = !needPdn;
            state.deadlineMs = sampleTime(now, clock) + 100;
            state.phase = ShutdownPhase::Pdn;
            return PlaybackProgress::Pending;
        }
        case ShutdownPhase::Pdn: {
            if (!state.pdnDone) {
                uint32_t irq = 0;
                if (!io.read(0x10010, &irq)) {
                    failed(DIAG_IDLE_ROLLBACK, 0x10010);
                } else if (irq & 0x00800000) {
                    state.pdnDone = true;
                    write(0x10010, 0x00800000);
                } else if (sampleTime(now, clock) < state.deadlineMs) {
                    return PlaybackProgress::Pending;
                } else {
                    failed(DIAG_POWER_DOWN_TIMEOUT, 0x10010, irq);
                }
            }
            if (!state.pdnDone && state.failure == DIAG_OK)
                failed(DIAG_POWER_DOWN_TIMEOUT, 0x10010);
            if (!power.internal()) {
                protectedWrite(0x7438, 0x00580941);
                write(0x11008, 1);
            }
            update(0x2018, 1, 0);
            if (dspAlive) {
                write(0x13020, registers::kCmdMailboxPause);
                state.phase = ShutdownPhase::Pause;
                state.deadlineMs = sampleTime(now, clock) + 5;
            } else {
                state.phase = ShutdownPhase::Finish;
            }
            return PlaybackProgress::Pending;
        }
        case ShutdownPhase::Pause: {
            uint32_t status = 0;
            uint32_t expected = registers::kStatusMailboxPaused;
            bool readable = io.read(0x13004, &status);
            bool error = status == 0xFFFFFFFFU || status == 0x00FFFFFFU;
            if (readable && !error && status != expected && sampleTime(now, clock) < state.deadlineMs)
                return PlaybackProgress::Pending;
            bool acknowledged = readable && !error && status == expected;
            if (!acknowledged)
                failed(DIAG_DSP_MAILBOX, 0x13004, status);
            state.haltDsp = !acknowledged;
            state.phase = ShutdownPhase::Finish;
            return PlaybackProgress::Pending;
        }
        case ShutdownPhase::Finish: {
            update(0x2018, 0x3000 | registers::kMaskBoostEnable, 0);
            uint32_t pwr1 = 0, pwr2 = 0, volume = 0, gain = 0;
            bool readable = io.read(0x2014, &pwr1) && io.read(0x2018, &pwr2) && io.read(0x6000, &volume) && io.read(0x6C04, &gain);
            bool safe = state.ok && state.pdnDone && readable && !(pwr1 & 1) && !(pwr2 & (0x3001 | registers::kMaskBoostEnable)) &&
                        volume == 0xA678 && gain == 0;
            if (!safe) {
                failed(DIAG_IDLE_ROLLBACK, 0x2018, pwr2);
                state.phase = ShutdownPhase::Failed;
                return PlaybackProgress::Failed;
            }
            state.phase = ShutdownPhase::Complete;
            return PlaybackProgress::Prepared;
        }
        case ShutdownPhase::Complete:
            return PlaybackProgress::Prepared;
        default:
            return PlaybackProgress::Failed;
        }
    }

    static PlaybackProgress advance(core::RegisterIO& io, PlaybackTransition& state, uint64_t now, bool dsp, uint32_t firmwareVersion,
                                    uint64_t (*clock)() = nullptr, const PowerConfiguration& power = kLegacyPowerConfiguration) {
        using namespace diagnostics;
        switch (state.phase) {
        case PlaybackPhase::Idle: {
            state.prepareStartedMs = sampleTime(now, clock);
            // Keep digital output muted until the service commits the pair.
            const uint32_t common[][2] = {{0x6000, 0xA678},
                                          {0x6C04, 0},
                                          {0x2C04, 0x430},
                                          {0x2C08, 3},
                                          {0x2C0C, 3},
                                          {0x4800, dsp ? 0x10001U : 0x10000U},
                                          {0x4804, 0x21},
                                          {0x4808, 0x20200200},
                                          {0x480C, dsp ? 3U : 2U},
                                          {0x4830, 0x18},
                                          {0x4840, 0x18},
                                          {0x4C00, dsp ? 0x32U : 8U},
                                          {0x4C20, 0x18},
                                          {0x4C24, 0x19},
                                          {0x4C28, dsp ? 0x28U : 0U},
                                          {0x4C2C, dsp ? 0x29U : 0U},
                                          {0x4C40, 8},
                                          {0x4C44, dsp ? 8U : 9U},
                                          {0x4C48, 0x18},
                                          {0x4C4C, 0x19},
                                          {0x4C50, dsp ? power.dspSupplyMonitorSource() : 0x20U},
                                          {0x4C54, dsp ? 0x29U : 0x21U}};
            for (const auto& item : common)
                if (!io.write(item[0], item[1]))
                    return fail(state, DIAG_PLAYBACK_INVARIANT, item[0], item[1]);
            if (dsp && (!io.write(0x4C58, 0x3A) || !io.write(0x4C5C, 0x3B) || !io.updateBits(0x2018, 0x3000, 0x3000)))
                return fail(state, DIAG_PLAYBACK_INVARIANT);
            if (dsp) {
                if (!io.write(0x13020, registers::kCmdMailboxResume))
                    return fail(state, DIAG_DSP_MAILBOX, 0x13020, registers::kCmdMailboxResume);
                state.phase = PlaybackPhase::Resume;
                state.deadlineMs = sampleTime(now, clock) + 5;
            } else {
                state.phase = PlaybackPhase::Power;
            }
            return PlaybackProgress::Pending;
        }
        case PlaybackPhase::Resume: {
            auto result = mailbox(io, state, now, clock);
            if (result == PlaybackProgress::Prepared) {
                state.phase = PlaybackPhase::Power;
                return PlaybackProgress::Pending;
            }
            return result;
        }
        case PlaybackPhase::Power: {
            if (!io.updateBits(0x2018, 1 | registers::kMaskBoostEnable, power.activePower(false)) ||
                (!power.internal() && !io.write(0x11008, 0x8001)) || !io.write(0x10010, 0x01000000))
                return fail(state, DIAG_PLAYBACK_INVARIANT);
            bool ok = power.internal() ? io.updateBits(0x2014, 1, 1) :
                      io.write(0x40, 0x55) && io.write(0x40, 0xAA) && io.write(0x742C, 0x0F) && io.write(0x742C, 0x79) &&
                      io.write(0x7438, 0x00585941) && io.updateBits(0x2014, 1, 1);
            bool locked = power.internal() || closeKeys(io);
            if (!ok || !locked)
                return fail(state, DIAG_PLAYBACK_INVARIANT);
            state.phase = PlaybackPhase::Pup;
            state.deadlineMs = sampleTime(now, clock) + 100;
            return PlaybackProgress::Pending;
        }
        case PlaybackPhase::Pup: {
            uint32_t irq = 0;
            if (!io.read(0x10010, &irq))
                return fail(state, DIAG_PLAYBACK_INVARIANT, 0x10010);
            state.actual = irq;
            if (irq & registers::kMaskProtectionFault)
                return fail(state, DIAG_AMP_PROTECTION, 0x10010, 0, irq);
            if (!(irq & 0x01000000)) {
                if (sampleTime(now, clock) >= state.deadlineMs)
                    return fail(state, DIAG_POWER_UP_TIMEOUT, 0x10010, 0x01000000, irq);
                return PlaybackProgress::Pending;
            }
            if (!io.write(0x10010, 0x01000000))
                return fail(state, DIAG_PLAYBACK_INVARIANT, 0x10010);
            if (dsp && firmwareVersion > 0x001C00) {
                if (!io.write(0x13020, registers::kCmdMailboxSpeakerOutputEnable))
                    return fail(state, DIAG_DSP_MAILBOX, 0x13020);
                state.phase = PlaybackPhase::Output;
                state.deadlineMs = sampleTime(now, clock) + 5;
            } else {
                bool ok = power.internal() || (io.write(0x40, 0x55) && io.write(0x40, 0xAA) && io.write(0x742C, 0xF9) && io.write(0x7438, 0x00580941));
                bool locked = power.internal() || closeKeys(io);
                if (!ok || !locked)
                    return fail(state, DIAG_PLAYBACK_INVARIANT);
                state.phase = PlaybackPhase::Pll;
                state.deadlineMs = sampleTime(now, clock) + 20;
            }
            return PlaybackProgress::Pending;
        }
        case PlaybackPhase::Output: {
            auto result = mailbox(io, state, now, clock);
            if (result == PlaybackProgress::Prepared) {
                state.phase = PlaybackPhase::Pll;
                state.deadlineMs = sampleTime(now, clock) + 20;
                return PlaybackProgress::Pending;
            }
            return result;
        }
        case PlaybackPhase::Pll: {
            uint32_t pll = 0;
            if (!io.read(0x10098, &pll))
                return fail(state, DIAG_PLAYBACK_INVARIANT, 0x10098);
            state.actual = pll;
            if (!(pll & 2)) {
                if (sampleTime(now, clock) >= state.deadlineMs)
                    return fail(state, DIAG_PLL_UNLOCKED, 0x10098, 2, pll);
                return PlaybackProgress::Pending;
            }
            uint32_t pwr1 = 0, pwr2 = 0, dac = 0, asp = 0;
            if (!io.read(0x2014, &pwr1) || !io.read(0x2018, &pwr2) || !io.read(0x4C00, &dac) || !io.read(0x4800, &asp) || !(pwr1 & 1) ||
                (pwr2 & (0x3001 | registers::kMaskBoostEnable)) != power.activePower(dsp) || dac != (dsp ? 0x32U : 8U) ||
                asp != (dsp ? 0x10001U : 0x10000U))
                return fail(state, DIAG_PLAYBACK_INVARIANT);
            state.phase = PlaybackPhase::Prepared;
            state.deadlineMs = sampleTime(now, clock) + 100;
            return PlaybackProgress::Prepared;
        }
        case PlaybackPhase::Prepared:
            if (now >= state.deadlineMs)
                return fail(state, diagnostics::DIAG_PLAYBACK_INVARIANT);
            return PlaybackProgress::Prepared;
        case PlaybackPhase::Failed:
            return PlaybackProgress::Failed;
        default:
            return PlaybackProgress::Pending;
        }
    }

    // The caller must recheck route generation, stream identity, protection
    // and readiness of every required peer immediately before this operation.
    static bool commit(core::RegisterIO& io, PlaybackTransition& state, uint32_t gain, bool dsp,
                       const PowerConfiguration& power = kLegacyPowerConfiguration) {
        if (state.phase != PlaybackPhase::Prepared)
            return false;
        uint32_t volume = 0, actualGain = 0, pwr1 = 0, pwr2 = 0;
        uint32_t pll = 0, irq = 0, dac = 0, asp = 0, mailboxStatus = 0;
        auto powerReady = [&]() {
            return io.read(0x2014, &pwr1) && io.read(0x2018, &pwr2) && (pwr1 & 1) &&
                   (pwr2 & (0x3001 | registers::kMaskBoostEnable)) == power.activePower(dsp);
        };
        // A peer may wait in Prepared while clocks or power change. Read
        // current invariants again immediately before its own unmute write.
        if (!powerReady() || !io.read(0x10098, &pll) || !(pll & 2) || !io.read(0x10010, &irq) || (irq & registers::kMaskProtectionFault) ||
            !io.read(0x4C00, &dac) || dac != (dsp ? 0x32U : 8U) || !io.read(0x4800, &asp) || asp != (dsp ? 0x10001U : 0x10000U) ||
            (dsp && (!io.read(0x13004, &mailboxStatus) || mailboxStatus != registers::kStatusMailboxRunning))) {
            fail(state, (irq & registers::kMaskProtectionFault) ? diagnostics::DIAG_AMP_PROTECTION : diagnostics::DIAG_PLAYBACK_INVARIANT);
            return false;
        }
        const uint32_t signalPath[][2] = {{0x4804, 0x21},
                                          {0x4808, 0x20200200},
                                          {0x4830, 0x18},
                                          {0x4840, 0x18},
                                          {0x4C40, 8},
                                          {0x4C44, dsp ? 8U : 9U},
                                          {0x4C48, 0x18},
                                          {0x4C4C, 0x19},
                                          {0x4C50, dsp ? power.dspSupplyMonitorSource() : 0x20U},
                                          {0x4C54, dsp ? 0x29U : 0x21U},
                                          {0x4C28, dsp ? 0x28U : 0U},
                                          {0x4C2C, dsp ? 0x29U : 0U},
                                          {0x480C, dsp ? 3U : 2U}};
        for (unsigned i = 0; i < sizeof(signalPath) / sizeof(signalPath[0]); ++i) {
            uint32_t actual = 0;
            if (!io.read(signalPath[i][0], &actual) || actual != signalPath[i][1]) {
                fail(state, diagnostics::DIAG_PLAYBACK_INVARIANT, signalPath[i][0], signalPath[i][1], actual);
                return false;
            }
        }
        if (!io.write(0x6C04, gain) || !io.write(0x6000, registers::kPlaybackDigitalVolume) || !powerReady() || !io.read(0x6000, &volume) ||
            !io.read(0x6C04, &actualGain) || volume != registers::kPlaybackDigitalVolume || actualGain != gain) {
            fail(state, diagnostics::DIAG_PLAYBACK_INVARIANT);
            return false;
        }
        state.phase = PlaybackPhase::Active;
        return true;
    }
};

}
