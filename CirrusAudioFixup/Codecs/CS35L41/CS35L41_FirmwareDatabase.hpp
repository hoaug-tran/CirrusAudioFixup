#ifndef CS35L41_FirmwareDatabase_hpp
#define CS35L41_FirmwareDatabase_hpp

#include "../../CirrusAudioFixup.hpp"

// Dummy payloads for testing Phase 5A lookup
static const uint8_t dummy_wmfw[] = { 0x57, 0x4D, 0x46, 0x57, 0x00, 0x00 }; // "WMFW"
static const uint8_t dummy_bin[] = { 0x01, 0x02, 0x03, 0x04 };

static const FirmwareResource firmwareTable[] = {
    // SSID: 10431438, spkid: 0
    {
        0x1043, 0x1438, 0,
        "cs35l41-dsp1-spk-prot-10431438.wmfw",
        "cs35l41-dsp1-spk-prot-10431438-spkid0.bin", // generic bin base name
        dummy_wmfw, sizeof(dummy_wmfw),
        dummy_bin, sizeof(dummy_bin)
    },
    // Lenovo 17aa:3800
    {
        0x17aa, 0x3800, 0,
        "cs35l41-dsp1-spk-prot-17aa3800.wmfw",
        "cs35l41-dsp1-spk-prot-17aa3800-spkid0.bin",
        dummy_wmfw, sizeof(dummy_wmfw),
        dummy_bin, sizeof(dummy_bin)
    }
};

static const size_t firmwareTableSize = sizeof(firmwareTable) / sizeof(firmwareTable[0]);

#endif /* CS35L41_FirmwareDatabase_hpp */
