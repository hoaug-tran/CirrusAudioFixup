#ifndef CS35L41_FirmwareDatabase_hpp
#define CS35L41_FirmwareDatabase_hpp

#include "../../CirrusAudioFixup.hpp"

// Dummy payloads for testing Phase 5A lookup
static const uint8_t dummy_wmfw[] = { 'W', 'M', 'F', 'W', 0x01, 0x02, 0x03, 0x04 }; // 8 bytes (aligned, with magic and version)
static const uint8_t dummy_bin[] = { 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04 }; // 8 bytes (aligned)

static const FirmwareResource firmwareTable[] = {
    // ASUS 1043:1438
    {
        0x1043, 0x1438, 0,
        "cs35l41-dsp1-spk-prot-10431438.wmfw",
        "cs35l41-dsp1-spk-prot-10431438-spkid0.bin",
        dummy_wmfw, sizeof(dummy_wmfw),
        dummy_bin, sizeof(dummy_bin),
        true
    },
    // Lenovo 17aa:3800
    {
        0x17aa, 0x3800, 0,
        "cs35l41-dsp1-spk-prot-17aa3800.wmfw",
        "cs35l41-dsp1-spk-prot-17aa3800-spkid0.bin",
        dummy_wmfw, sizeof(dummy_wmfw),
        dummy_bin, sizeof(dummy_bin),
        true
    },
    // Lenovo 17aa:382b (User's specific device)
    {
        0x17aa, 0x382b, 0,
        "cs35l41-dsp1-spk-prot-17aa382b.wmfw",
        "cs35l41-dsp1-spk-prot-17aa382b-spkid0.bin",
        dummy_wmfw, sizeof(dummy_wmfw),
        dummy_bin, sizeof(dummy_bin),
        true
    }
};

static const size_t firmwareTableSize = sizeof(firmwareTable) / sizeof(firmwareTable[0]);

#endif /* CS35L41_FirmwareDatabase_hpp */
