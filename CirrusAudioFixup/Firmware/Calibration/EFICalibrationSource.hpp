// Copy the vendor calibration property; callers validate its type and contents.
#pragma once
#include <IOKit/IOService.h>

namespace cirrus::firmware {
inline OSObject* copyEFICalibrationProperty() {
    IORegistryEntry* entries[]{IORegistryEntry::fromPath("IODeviceTree:/options"), IORegistryEntry::fromPath("IODeviceTree:/chosen")};
    // Preserve Linux cs-amp-lib vendor priority, including unqualified registry keys.
    const char* names[]{"53559579-8753-4f5c-9130-e82acfb8d893:SmartAmpCalibrationData", "SmartAmpCalibrationData",
                        "02f9af02-7734-4233-b43d-93fe5aa35db3:CirrusSmartAmpCalibrationData", "CirrusSmartAmpCalibrationData"};
    OSObject* property = nullptr;
    for (const auto* name : names) {
        for (auto* entry : entries)
            if (entry && (property = entry->copyProperty(name)))
                break;
        if (property)
            break;
    }
    for (auto* entry : entries)
        if (entry)
            entry->release();
    return property;
}
}
