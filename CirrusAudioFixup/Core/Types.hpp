#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cirrus::core {

// Common PCM sample rates supported across Cirrus smart amplifiers
enum class SampleRate : uint32_t {
    Rate44k1 = 44100,
    Rate48k0 = 48000,
    Rate88k2 = 88200,
    Rate96k0 = 96000,
    Rate176k4 = 176400,
    Rate192k0 = 192000,
};

// PCM bit resolution per audio slot
enum class BitDepth : uint8_t {
    Bits16 = 16,
    Bits20 = 20,
    Bits24 = 24,
    Bits32 = 32,
};

// Logical speaker channel mapping across multi-amplifier configurations
enum class AudioChannel : uint8_t {
    Left = 0,
    Right = 1,
    LeftTweeter = 2,
    RightTweeter = 3,
    MaxChannels = 4,
};

// Power states for amplifier hardware lifecycle
enum class DevicePowerState : uint8_t {
    Off = 0,
    Sleep,
    Standby,
    Active,
};

// Supported physical bus interconnect types
enum class BusTransportType : uint8_t {
    I2C = 0,
    SPI,
    SoundWire,
};

// Cirrus Logic boosted smart amplifier silicon family
enum class CodecModel : uint16_t {
    Unknown = 0,
    CS35L41 = 0x3541,
    CS35L45 = 0x3545,
    CS35L51 = 0x3551,
    CS35L56 = 0x3556,
};

} // namespace cirrus::core
