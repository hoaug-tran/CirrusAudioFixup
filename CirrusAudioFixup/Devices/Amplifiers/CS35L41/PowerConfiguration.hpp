// CS35L41 power topology, separate from bus and subsystem identity.
// Boost coefficients/register facts follow Cirrus Logic's cs35l41-lib.c and
// cs35l41.h in Linux v6.12 (Copyright 2017-2021 Cirrus Logic, Inc.).
#pragma once
#include "Core/RegisterIO.hpp"
#include <string.h>

namespace cirrus::devices::amplifiers::cs35l41 {
enum class BoostTopology : uint8_t { ExternalWithoutSwitch, Internal };
struct PowerConfiguration {
    BoostTopology topology{BoostTopology::ExternalWithoutSwitch};
    uint32_t inductorNanohenry{0}, peakMilliamp{0}, capacitorMicrofarad{0};
    bool internal() const { return topology == BoostTopology::Internal; }
    uint32_t activePower(bool dsp) const { return (dsp ? 0x3001U : 1U) | (internal() ? 0x20U : 0U); }
    bool valid() const {
        if (topology == BoostTopology::ExternalWithoutSwitch)
            return !inductorNanohenry && !peakMilliamp && !capacitorMicrofarad;
        return internal() &&
               (inductorNanohenry == 1000 || inductorNanohenry == 1200 || inductorNanohenry == 1500 || inductorNanohenry == 2200) &&
               peakMilliamp >= 1600 && peakMilliamp <= 4500 && capacitorMicrofarad;
    }
    bool configure(core::RegisterIO& io) const {
        if (!valid())
            return false;
        if (!internal())
            return true;
        uint32_t globalPower = 0;
        if (!io.read(0x2014, &globalPower) || (globalPower & 1))
            return false;
        const unsigned inductor = inductorNanohenry == 1000 ? 0 : inductorNanohenry == 1200 ? 1 : inductorNanohenry == 1500 ? 2 : 3;
        const unsigned capacitor = capacitorMicrofarad < 20     ? 0
                                   : capacitorMicrofarad <= 50  ? 1
                                   : capacitorMicrofarad <= 100 ? 2
                                   : capacitorMicrofarad <= 200 ? 3
                                                                : 4;
        static constexpr uint8_t k1[][5] = {
            {0x24, 0x32, 0x32, 0x4f, 0x57}, {0x24, 0x32, 0x32, 0x4f, 0x57}, {0x40, 0x32, 0x32, 0x4f, 0x57}, {0x40, 0x32, 0x32, 0x4f, 0x57}};
        static constexpr uint8_t k2[][5] = {
            {0x24, 0x49, 0x66, 0xa3, 0xea}, {0x24, 0x49, 0x66, 0xa3, 0xea}, {0x48, 0x49, 0x66, 0xa3, 0xea}, {0x48, 0x49, 0x66, 0xa3, 0xea}};
        static constexpr uint8_t slope[] = {0x75, 0x6b, 0x3b, 0x28};
        const uint32_t coefficients = uint32_t(k1[inductor][capacitor]) | uint32_t(k2[inductor][capacitor]) << 8;
        const uint32_t slopeValue = uint32_t(slope[inductor]) << 8 | inductor;
        const uint32_t peak = (peakMilliamp - 1600) / 50 + 0x10;
        if (!io.updateBits(0x3810, 0xffff, coefficients) || !io.updateBits(0x3814, 0xff03, slopeValue) ||
            !io.updateBits(0x3808, 0x7f, peak))
            return false;
        uint32_t actual = 0;
        return io.read(0x3810, &actual) && (actual & 0xffff) == coefficients && io.read(0x3814, &actual) &&
               (actual & 0xff03) == slopeValue && io.read(0x3808, &actual) && (actual & 0x7f) == peak;
    }
};
inline constexpr PowerConfiguration kLegacyPowerConfiguration{};
inline constexpr const char* kPowerConfigurationKind = "cs35l41-power-v1";
inline const PowerConfiguration* powerConfiguration(const void* data, size_t size, const char* kind) {
    if (!data && !size && !kind)
        return &kLegacyPowerConfiguration;
    if (!data || size != sizeof(PowerConfiguration) || !kind || strcmp(kind, kPowerConfigurationKind))
        return nullptr;
    const auto* configuration = static_cast<const PowerConfiguration*>(data);
    return configuration->valid() ? configuration : nullptr;
}
}
