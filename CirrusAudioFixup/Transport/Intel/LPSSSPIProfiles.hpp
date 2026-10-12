// Controller identities and clocks from Linux v6.12 intel-lpss-pci.c.
// PCI identity selects a register family, never amplifier wiring or tuning.
#pragma once
#include <stdint.h>
namespace cirrus::transport::intel {
enum class LPSSSPIFamily { SunrisePoint, Broxton, CannonLake };
struct LPSSSPIProfile {
    LPSSSPIFamily family;
    uint32_t rootClockHz;
    bool capabilities;
    bool forceClockGate;
    uint32_t selectMask;
};
inline bool lookupLPSSSPIProfile(uint16_t vendor, uint16_t device, LPSSSPIProfile& result) {
    if (vendor != 0x8086)
        return false;
    switch (device) {
    case 0x9d29:
    case 0x9d2a:
    case 0xa129:
    case 0xa12a:
    case 0xa2a9:
    case 0xa2aa:
    case 0xa3a9:
    case 0xa3aa:
        result = {LPSSSPIFamily::SunrisePoint, 120000000, false, false, 0};
        return true;
    case 0x0ac2:
    case 0x0ac4:
    case 0x0ac6:
    case 0x1ac2:
    case 0x1ac4:
    case 0x1ac6:
    case 0x31c2:
    case 0x31c4:
    case 0x31c6:
    case 0x4b2a:
    case 0x4b2b:
    case 0x4b37:
    case 0x5ac2:
    case 0x5ac4:
    case 0x5ac6:
    case 0x98aa:
        result = {LPSSSPIFamily::Broxton, 100000000, true, true, 0x300};
        return true;
    case 0x02aa:
    case 0x02ab:
    case 0x02fb:
    case 0x06aa:
    case 0x06ab:
    case 0x06fb:
    case 0x34aa:
    case 0x34ab:
    case 0x34fb:
    case 0x4daa:
    case 0x4dab:
    case 0x4dfb:
    case 0x9daa:
    case 0x9dab:
    case 0x9dfb:
    case 0xa0aa:
    case 0xa0ab:
    case 0xa0de:
    case 0xa0df:
    case 0xa0fb:
    case 0xa0fd:
    case 0xa0fe:
    case 0xa32a:
    case 0xa32b:
    case 0xa37b:
        result = {LPSSSPIFamily::CannonLake, 120000000, true, true, 0x300};
        return true;
    case 0x43aa:
    case 0x43ab:
    case 0x43fb:
    case 0x43fd:
    case 0x51aa:
    case 0x51ab:
    case 0x51fb:
    case 0x54aa:
    case 0x54ab:
    case 0x54fb:
    case 0x7727:
    case 0x7730:
    case 0x7746:
    case 0x7a2a:
    case 0x7a2b:
    case 0x7a79:
    case 0x7a7b:
    case 0x7aaa:
    case 0x7aab:
    case 0x7af9:
    case 0x7afb:
    case 0x7e27:
    case 0x7e30:
    case 0x7e46:
    case 0x7f2a:
    case 0x7f2b:
    case 0x7f5e:
    case 0x7f5f:
    case 0xa827:
    case 0xa830:
    case 0xa846:
    case 0xe327:
    case 0xe330:
    case 0xe346:
    case 0xe427:
    case 0xe430:
    case 0xe446:
        result = {LPSSSPIFamily::CannonLake, 100000000, true, true, 0x300};
        return true;
    default:
        return false;
    }
}
}
