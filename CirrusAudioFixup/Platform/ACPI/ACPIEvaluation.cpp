#include "Platform/ACPI/ACPIEvaluation.hpp"
#include "Platform/ACPI/ACPIObjectConverter.hpp"
#include <Headers/kern_patcher.hpp>

namespace cirrus::transport {
namespace {
acpi::GetHandle getHandle{nullptr};
acpi::GetName getName{nullptr};
acpi::EvaluateObject evaluateObject{nullptr};
constexpr size_t kPropertyBufferSize = 65536;

IOReturn evaluateProperties(IOACPIPlatformDevice* device, OSObject** result) {
    device->setProperty("Cirrus_ACPI_Interpreter_Stage", "VALIDATE_PATH");
    char path[256]{}, canonical[256]{};
    int length = sizeof(path);
    if (!device->getPath(path, &length, IORegistryEntry::getPlane("IOACPIPlane")) || !canonicalACPIPath(path, canonical, sizeof(canonical)))
        return kIOReturnBadArgument;
    device->setProperty("Cirrus_ACPI_Interpreter_Stage", "HANDLE_LOOKUP");
    void* handle = nullptr;
    const auto handleStatus = getHandle(nullptr, canonical, &handle);
    device->setProperty("Cirrus_ACPI_Handle_Status", uint64_t(handleStatus), 32);
    if (handleStatus || !handle)
        return kIOReturnNotFound;
    auto* storage = IOMalloc(kPropertyBufferSize);
    if (!storage)
        return kIOReturnNoMemory;
    acpi::Buffer buffer{kPropertyBufferSize, storage};
    device->setProperty("Cirrus_ACPI_Interpreter_Stage", "EVALUATE_DSD");
    const auto status = evaluateObject(handle, "_DSD", nullptr, &buffer);
    device->setProperty("Cirrus_ACPI_Evaluate_Status", uint64_t(status), 32);
    device->setProperty("Cirrus_ACPI_Result_Length", uint64_t(buffer.length), 64);
    IOReturn ret = status ? kIOReturnIOError : kIOReturnBadArgument;
    if (!status && buffer.pointer == storage && buffer.length >= sizeof(acpi::Object) && buffer.length <= kPropertyBufferSize) {
        device->setProperty("Cirrus_ACPI_Interpreter_Stage", "CONVERT_RESULT");
        device->setProperty("Cirrus_ACPI_Result_Type", uint64_t(static_cast<const acpi::Object*>(storage)->type), 32);
        acpi::ObjectConverter converter(storage, buffer.length, getName);
        *result = converter.copy(static_cast<const acpi::Object*>(storage));
        device->setProperty("Cirrus_ACPI_Conversion_Reason", *result ? "READY" : converter.reason());
        device->setProperty("Cirrus_ACPI_Conversion_Type", uint64_t(converter.failureType()), 32);
        device->setProperty("Cirrus_ACPI_Reference_Name_Status", uint64_t(converter.nameStatus()), 32);
        ret = *result ? kIOReturnSuccess : kIOReturnUnsupported;
    }
    IOFree(storage, kPropertyBufferSize);
    return ret;
}
}
void configureACPIEvaluation(KernelPatcher& patcher, size_t index, uintptr_t start, size_t size) {
    if (__atomic_load_n(&nativePropertyEvaluator, __ATOMIC_ACQUIRE))
        return;
    auto resolve = [&](const char* symbol) {
        const auto address = patcher.solveSymbol<mach_vm_address_t>(index, symbol, start, size);
        patcher.clearError();
        return address;
    };
    const auto handle = reinterpret_cast<acpi::GetHandle>(resolve("_AcpiGetHandle"));
    const auto name = reinterpret_cast<acpi::GetName>(resolve("_AcpiGetName"));
    const auto evaluate = reinterpret_cast<acpi::EvaluateObject>(resolve("_AcpiEvaluateObject"));
    if (!handle || !name || !evaluate) {
        __atomic_store_n(&nativePropertySetupState, !handle ? "GET_HANDLE_SYMBOL_MISSING" :
                         !name ? "GET_NAME_SYMBOL_MISSING" : "EVALUATE_OBJECT_SYMBOL_MISSING", __ATOMIC_RELEASE);
        return;
    }
    getHandle = handle;
    getName = name;
    evaluateObject = evaluate;
    __atomic_store_n(&nativePropertySetupState, static_cast<const char*>("READY"), __ATOMIC_RELEASE);
    __atomic_store_n(&nativePropertyEvaluator, &evaluateProperties, __ATOMIC_RELEASE);
}
}
