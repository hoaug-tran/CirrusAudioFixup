// CS35L41 embedded resource schema; borrowed immutable assets.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct FirmwareResource {
    uint32_t subsystemVendor;
    uint32_t subsystemDevice;
    uint32_t spkid;
    const char* fwName;
    const char* binName;
    const uint8_t* wmfw;
    size_t wmfwSize;
    const uint8_t* bin;
    size_t binSize;
    const uint8_t* binRight;
    size_t binRightSize;
    bool isDummy;
    // Optional companions belong to the same board/speaker entry as its BIN.
    // Absence on one channel uses the Linux default, never the other channel.
    const uint8_t* tuningLeft{nullptr};
    size_t tuningLeftSize{0};
    const uint8_t* tuningRight{nullptr};
    size_t tuningRightSize{0};
    const char* tuningLeftName{nullptr};
    const char* tuningRightName{nullptr};
};
