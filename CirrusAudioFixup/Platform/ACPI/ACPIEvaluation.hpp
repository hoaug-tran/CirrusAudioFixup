#pragma once
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <stddef.h>
#include <stdint.h>

class KernelPatcher;
namespace cirrus::transport {
using ACPIPropertyEvaluator = IOReturn (*)(IOACPIPlatformDevice*, OSObject**);
inline ACPIPropertyEvaluator nativePropertyEvaluator{nullptr};
inline const char* nativePropertySetupState{"CALLBACK_NOT_REACHED"};
inline IOReturn evaluateNativeProperties(IOACPIPlatformDevice* device, OSObject** result) {
    if (!device || !result || *result)
        return kIOReturnBadArgument;
    const auto evaluator = __atomic_load_n(&nativePropertyEvaluator, __ATOMIC_ACQUIRE);
    device->setProperty("Cirrus_ACPI_Interpreter_State", evaluator ? "READY" : "UNAVAILABLE");
    device->setProperty("Cirrus_ACPI_Interpreter_Setup", __atomic_load_n(&nativePropertySetupState, __ATOMIC_ACQUIRE));
    const auto status = evaluator ? evaluator(device, result) : kIOReturnNotReady;
    device->setProperty("Cirrus_ACPI_Interpreter_Return", uint64_t(uint32_t(status)), 32);
    return status;
}
void configureACPIEvaluation(KernelPatcher& patcher, size_t index, uintptr_t address, size_t size);
}
