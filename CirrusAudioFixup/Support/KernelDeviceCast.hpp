// Resolve external driver types through their registered kernel metaclasses.
// Imported pointer identity must not misclassify ACPI or discard a PCI device.
#pragma once
#include <IOKit/IORegistryEntry.h>
#if defined(KERNEL)
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/pci/IOPCIDevice.h>
#endif

class IOACPIPlatformDevice;
class IOPCIDevice;

namespace cirrus::support {
template <typename Entry> IOACPIPlatformDevice* acpiDeviceCast(Entry* entry) {
#if defined(KERNEL)
    // metaCast checks the registered class hierarchy, including subclasses;
    // this is not a cast based on an IORegistry name or a published property.
    return entry ? static_cast<IOACPIPlatformDevice*>(entry->metaCast("IOACPIPlatformDevice")) : nullptr;
#else
    return OSDynamicCast(IOACPIPlatformDevice, entry);
#endif
}
template <typename Entry> IOPCIDevice* pciDeviceCast(Entry* entry) {
#if defined(KERNEL)
    return entry ? static_cast<IOPCIDevice*>(entry->metaCast("IOPCIDevice")) : nullptr;
#else
    return OSDynamicCast(IOPCIDevice, entry);
#endif
}

}
