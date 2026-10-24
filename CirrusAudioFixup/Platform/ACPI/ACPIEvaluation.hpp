// Read firmware properties without losing namespace references in OS conversion.
#pragma once
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <stddef.h>
#include <stdint.h>

class KernelPatcher;
namespace cirrus::transport {
using ACPIPropertyEvaluator = IOReturn (*)(IOACPIPlatformDevice*, OSObject**);
inline ACPIPropertyEvaluator nativePropertyEvaluator{nullptr};
inline IOReturn evaluateNativeProperties(IOACPIPlatformDevice* device, OSObject** result) {
    if (!device || !result || *result)
        return kIOReturnBadArgument;
    const auto evaluator = __atomic_load_n(&nativePropertyEvaluator, __ATOMIC_ACQUIRE);
    return evaluator ? evaluator(device, result) : kIOReturnUnsupported;
}
void configureACPIEvaluation(KernelPatcher& patcher, size_t index, uintptr_t address, size_t size);
}
