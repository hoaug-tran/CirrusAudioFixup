// Use the running ACPICA interpreter; never inject or reimplement firmware AML.
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
    char path[256]{}, canonical[256]{};
    int length = sizeof(path);
    if (!device->getPath(path, &length, IORegistryEntry::getPlane("IOACPIPlane")) || !canonicalACPIPath(path, canonical, sizeof(canonical)))
        return kIOReturnBadArgument;
    void* handle = nullptr;
    if (getHandle(nullptr, canonical, &handle) || !handle)
        return kIOReturnNotFound;
    auto* storage = IOMalloc(kPropertyBufferSize);
    if (!storage)
        return kIOReturnNoMemory;
    acpi::Buffer buffer{kPropertyBufferSize, storage};
    const auto status = evaluateObject(handle, "_DSD", nullptr, &buffer);
    IOReturn ret = kIOReturnUnsupported;
    if (!status && buffer.pointer == storage && buffer.length >= sizeof(acpi::Object) && buffer.length <= kPropertyBufferSize) {
        acpi::ObjectConverter converter(storage, buffer.length, getName);
        *result = converter.copy(static_cast<const acpi::Object*>(storage));
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
    if (!handle || !name || !evaluate)
        return;
    getHandle = handle;
    getName = name;
    evaluateObject = evaluate;
    __atomic_store_n(&nativePropertyEvaluator, &evaluateProperties, __ATOMIC_RELEASE);
}
}
