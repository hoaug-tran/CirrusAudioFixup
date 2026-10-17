// Verified Linux topology and assets for HP ZBook Fury 16 G9, 103C89C6.
// Evidence and validation limits: docs/hp_spi_bringup.md.
#pragma once
#include "Core/AudioTypes.hpp"
#include "Devices/Amplifiers/CS35L41/PowerConfiguration.hpp"
#include "Platform/BoardConfiguration.hpp"
namespace cirrus::platform::boards {
inline constexpr devices::amplifiers::cs35l41::PowerConfiguration kHP89C6Power{devices::amplifiers::cs35l41::BoostTopology::Internal, 1000,
                                                                               4500, 24};
inline constexpr AmplifierBinding kHP89C6Amplifiers[] = {
    {{core::BusTransportType::SPI, "\\_SB.PC00.SPI0", 0, false, 0, 4000000, 0},
     0x35a40,
     uint32_t(core::AudioChannel::Right),
     0,
     "cs35l41-dsp1-spk-prot-103c89c6-r0.bin",
     "CSC3551",
     2,
     &kHP89C6Power,
     sizeof(kHP89C6Power),
     devices::amplifiers::cs35l41::kPowerConfigurationKind},
    {{core::BusTransportType::SPI, "\\_SB.PC00.SPI0", 0, false, 1, 4000000, 0},
     0x35a40,
     uint32_t(core::AudioChannel::Left),
     0,
     "cs35l41-dsp1-spk-prot-103c89c6-l0.bin",
     "CSC3551",
     2,
     &kHP89C6Power,
     sizeof(kHP89C6Power),
     devices::amplifiers::cs35l41::kPowerConfigurationKind},
};
inline constexpr BoardConfiguration kHP89C6Board{{0x103c, 0x89c6, "CSC3551"}, kHP89C6Amplifiers, 2, true, true, true, true};
}
