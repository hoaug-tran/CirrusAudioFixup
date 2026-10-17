// Native SPI attachment. Owner serializes clients and joins them before teardown.
// Board authorization is separate: discovery never enables amplifier writes.
#pragma once
#include "Core/BusEndpoint.hpp"
#include "Core/RegisterMap.hpp"
#include "Platform/AmplifierDriverRegistry.hpp"
#include "Support/KernelDeviceCast.hpp"
#include "Transport/ACPIPath.hpp"
#include "Transport/ACPISPIResources.hpp"
#include "Transport/SPIControllerDrivers.hpp"
#include "Transport/SPIRegisterBus.hpp"
#include <IOKit/IORegistryEntry.h>
namespace cirrus::transport {
class NativeSPIHost {
public:
    struct Failure {
        const char* reason{"NONE"};
        IOReturn result{kIOReturnSuccess};
        size_t index{0};
        uint32_t detail{0};
        char source[256]{};
    };
    const Failure& failure() const { return mFailure; }
    ~NativeSPIHost() { shutdown(); }
    bool bound() const { return mCount != 0; }
    size_t count() const { return mCount; }
    core::RegisterIO* registers(size_t i) const { return i < mCount ? mRegisters[i] : nullptr; }
    const ACPISPIResource* resource(size_t i) const { return i < mCount ? &mResources.endpoints[i] : nullptr; }
    core::BusEndpoint endpoint(size_t i) const {
        if (i >= mCount)
            return {};
        const auto& r = mResources.endpoints[i];
        return {core::BusTransportType::SPI, mPaths[i], 0, false, r.endpoint.chipSelect, r.endpoint.clockHz, r.endpoint.mode};
    }
    IOReturn bind(IOACPIPlatformDevice* amplifier, IOService* owner, const core::RegisterMapConfig& format,
                  NativeSPIControllerFactory factory = createNativeSPIController) {
        return bindImpl(amplifier, owner, &format, factory);
    }
    // Controller inventory without guessing an unsupported AMP's register format.
    // No register buses/maps or AMP transfers are created in this mode.
    IOReturn bindControllers(IOACPIPlatformDevice* amplifier, IOService* owner,
                             NativeSPIControllerFactory factory = createNativeSPIController) {
        return bindImpl(amplifier, owner, nullptr, factory);
    }

private:
    IOReturn bindImpl(IOACPIPlatformDevice* amplifier, IOService* owner, const core::RegisterMapConfig* format,
                      NativeSPIControllerFactory factory) {
        mFailure = {};
        if (bound() || !amplifier || !owner || !factory || (format && !core::RegisterMap::isValidConfig(*format)))
            return fail("INVALID_BIND_ARGUMENT", kIOReturnBadArgument);
        // _STA present/enabled. Do not turn on a firmware-disabled device.
        if ((amplifier->getDeviceStatus() & 3) != 3)
            return fail("ACPI_DEVICE_DISABLED", kIOReturnNotReady);
        OSObject* object = nullptr;
        IOReturn ret = amplifier->evaluateObject("_CRS", &object);
        auto* data = OSDynamicCast(OSData, object);
        if (ret != kIOReturnSuccess || !data) {
            OSSafeReleaseNULL(object);
            return fail("ACPI_CRS_UNAVAILABLE", ret == kIOReturnSuccess ? kIOReturnNotFound : ret);
        }
        auto result = parseACPISPIResources(static_cast<const uint8_t*>(data->getBytesNoCopy()), data->getLength(), mResources);
        object->release();
        if (result != ACPISPIResult::Success)
            return fail("ACPI_SPI_RESOURCE_REJECTED", kIOReturnUnsupported, 0, nullptr, uint32_t(result));
        for (size_t i = 0; i < mResources.count; ++i) {
            auto& r = mResources.endpoints[i];
            if (r.sourceIndex != 0) {
                fail("UNSUPPORTED_RESOURCE_SOURCE_INDEX", kIOReturnUnsupported, i, r.source, r.sourceIndex);
                shutdown();
                return mFailure.result;
            }
            // Multiple resources may share a controller, never the same CS.
            for (size_t j = 0; j < i; ++j)
                if (!strcmp(r.source, mResources.endpoints[j].source) &&
                    r.endpoint.chipSelect == mResources.endpoints[j].endpoint.chipSelect) {
                    fail("DUPLICATE_CHIP_SELECT", kIOReturnBadArgument, i, r.source, r.endpoint.chipSelect);
                    shutdown();
                    return mFailure.result;
                }
            IOACPIPlatformDevice* target = resolve(amplifier, r.source);
            if (!target) {
                fail("CONTROLLER_ACPI_PATH_UNRESOLVED_OR_AMBIGUOUS", kIOReturnNotFound, i, r.source);
                shutdown();
                return mFailure.result;
            }
            size_t slot = 0;
            while (slot < mControllers && mTargets[slot] != target)
                ++slot;
            for (size_t j = 0; j < i; ++j) {
                if (mSlots[j] == slot && r.endpoint.chipSelect == mResources.endpoints[j].endpoint.chipSelect) {
                    fail("DUPLICATE_CONTROLLER_CHIP_SELECT", kIOReturnBadArgument, i, r.source, r.endpoint.chipSelect);
                    target->release();
                    shutdown();
                    return mFailure.result;
                }
            }
            char registryPath[256]{};
            int length = sizeof(registryPath);
            auto* plane = IORegistryEntry::getPlane("IOACPIPlane");
            if (!target->getPath(registryPath, &length, plane) || !canonicalACPIPath(registryPath, mPaths[i], sizeof(mPaths[i]))) {
                fail("INVALID_CONTROLLER_ACPI_PATH", kIOReturnBadArgument, i, r.source);
                target->release();
                shutdown();
                return mFailure.result;
            }
            mSlots[i] = slot;
            if (slot == mControllers) {
                mTargets[slot] = target; // Adopt resolver retain, including failure path.
                ++mControllers;
                auto* pci = findPCI(target, owner, factory == createNativeSPIController);
                NativeSPIControllerFailure failure{};
                mOwners[slot] = factory(target, pci, owner, failure);
                if (pci)
                    pci->release();
                if (!mOwners[slot]) {
                    ret = failure.result == kIOReturnSuccess ? kIOReturnNotReady : failure.result;
                    fail(failure.reason, ret, i, r.source, failure.detail);
                    shutdown();
                    return ret;
                }
                mBus[slot] = &mOwners[slot]->controller();
            } else
                target->release();
            if (!format) {
                ++mCount;
                continue;
            }
            // No board write permission is inferred from _CRS or HID.
            mPorts[i] = new SPIRegisterBus(mBus[slot], r.endpoint);
            if (!mPorts[i]) {
                fail("REGISTER_BUS_ALLOCATION_FAILED", kIOReturnNoMemory, i, r.source);
                shutdown();
                return mFailure.result;
            }
            mRegisters[i] = new core::RegisterMap(*mPorts[i], *format, false);
            if (!mRegisters[i]) {
                fail("REGISTER_MAP_ALLOCATION_FAILED", kIOReturnNoMemory, i, r.source);
                shutdown();
                return mFailure.result;
            }
            ++mCount;
        }
        return kIOReturnSuccess;
    }

public:
    // Call before lending register ports to clients. Replaces the read-only
    // discovery maps only after every endpoint has a verified board/driver.
    IOReturn configureAuthorized(const platform::AmplifierDriverRegistry& registry, const platform::BoardConfiguration& board,
                                 const platform::BoardIdentity& actual, const char* deviceHid, bool allowWrites = true) {
        if (!bound() || mAuthorized || !deviceHid || !board.permits(actual) || board.count != mCount)
            return fail("BOARD_AUTHORIZATION_REJECTED", kIOReturnNotPermitted);
        mFailure = {};
        core::RegisterMap* prepared[ACPISPIResources::kMaxEndpoints]{};
        IOReturn result = kIOReturnSuccess;
        for (size_t i = 0; i < mCount; ++i) {
            const auto e = endpoint(i);
            const platform::AmplifierBinding* binding = nullptr;
            for (size_t j = 0; j < board.count; ++j)
                if (e.sameDevice(board.amplifiers[j].endpoint))
                    binding = &board.amplifiers[j];
            const char* expected = binding ? (binding->acpiHid ? binding->acpiHid : actual.acpiHid) : nullptr;
            if (!binding || !expected || strcmp(expected, deviceHid) || e.spiMode != binding->endpoint.spiMode ||
                e.spiClockHz > binding->endpoint.spiClockHz) {
                result = fail("BOARD_ENDPOINT_OR_HID_MISMATCH", kIOReturnNotPermitted, i, e.controllerPath);
                break;
            }
            const auto* definition = registry.find(deviceHid, binding->siliconId, core::BusTransportType::SPI);
            if (!definition) {
                result = fail("NO_REGISTERED_SPI_DRIVER", kIOReturnUnsupported, i, e.controllerPath, binding->siliconId);
                break;
            }
            if (!mPorts[i]) {
                result = fail("REGISTER_FORMAT_UNAVAILABLE", kIOReturnUnsupported, i, e.controllerPath);
                break;
            }
            prepared[i] = new core::RegisterMap(*mPorts[i], *definition->format(core::BusTransportType::SPI), allowWrites);
            if (!prepared[i]) {
                result = fail("AUTHORIZED_MAP_ALLOCATION_FAILED", kIOReturnNoMemory, i, e.controllerPath);
                break;
            }
        }
        if (result != kIOReturnSuccess) {
            for (auto* map : prepared)
                delete map;
            return result;
        }
        for (size_t i = 0; i < mCount; ++i) {
            delete mRegisters[i];
            mRegisters[i] = prepared[i];
        }
        mAuthorized = true;
        return kIOReturnSuccess;
    }
    void suspend() {
        for (size_t i = 0; i < mControllers; ++i)
            if (mOwners[i])
                mOwners[i]->suspend();
    }
    IOReturn resume() {
        mFailure = {};
        for (size_t i = 0; i < mControllers; ++i) {
            NativeSPIControllerFailure failure{};
            const auto result = mOwners[i]->resume(failure);
            if (result != kIOReturnSuccess) {
                fail(failure.reason, result, i, controllerSource(i), failure.detail);
                suspend();
                return result;
            }
        }
        return kIOReturnSuccess;
    }
    void shutdown() {
        for (size_t i = 0; i < ACPISPIResources::kMaxEndpoints; ++i) {
            delete mRegisters[i];
            mRegisters[i] = nullptr;
            delete mPorts[i];
            mPorts[i] = nullptr;
        }
        for (size_t i = 0; i < ACPISPIResources::kMaxEndpoints; ++i) {
            delete mOwners[i];
            mOwners[i] = nullptr;
            mBus[i] = nullptr;
            OSSafeReleaseNULL(mTargets[i]);
        }
        mCount = mControllers = 0;
        mResources = {};
        mAuthorized = false;
        memset(mPaths, 0, sizeof(mPaths));
        memset(mSlots, 0, sizeof(mSlots));
    }

private:
    NativeSPIHost(const NativeSPIHost&) = delete;
    NativeSPIHost& operator=(const NativeSPIHost&) = delete;

public:
    NativeSPIHost() = default;

private:
    static IOACPIPlatformDevice* resolve(IOACPIPlatformDevice* device, const char* source) {
        auto* plane = IORegistryEntry::getPlane("IOACPIPlane");
        if (!plane)
            return nullptr;
        IORegistryEntry* scope = device;
        do {
            char registryPath[256]{}, canonical[256]{}, wanted[256]{};
            int length = sizeof(registryPath);
            if (!scope->getPath(registryPath, &length, plane) || !canonicalACPIPath(registryPath, canonical, sizeof(canonical)) ||
                !resolveACPIPath(canonical, source, wanted, sizeof(wanted)))
                return nullptr;
            auto* it = IORegistryIterator::iterateOver(IORegistryEntry::getRegistryRoot(), plane, kIORegistryIterateRecursively);
            if (!it)
                return nullptr;
            IOACPIPlatformDevice* found = nullptr;
            bool duplicate = false;
            while (auto* entry = it->getNextObject()) {
                auto* candidate = cirrus::support::acpiDeviceCast(entry);
                char path[256]{}, normal[256]{};
                length = sizeof(path);
                if (candidate && candidate->getPath(path, &length, plane) && canonicalACPIPath(path, normal, sizeof(normal)) &&
                    !strcmp(normal, wanted)) {
                    if (found) {
                        duplicate = true;
                        break;
                    }
                    found = candidate;
                }
            }
            if (found && !duplicate)
                found->retain();
            it->release();
            if (duplicate)
                return nullptr;
            if (found)
                return found;
            if (*source == '\\' || *source == '^')
                break;
            scope = scope->getParentEntry(plane);
        } while (scope);
        return nullptr;
    }
    static IOPCIDevice* findPCI(IOACPIPlatformDevice* target, IOService* owner, bool allowWait = true) {
        auto* matching = IOService::serviceMatching("IOPCIDevice");
        auto* it = matching ? IOService::getMatchingServices(matching) : nullptr;
        if (matching)
            matching->release();
        unsigned visited = 0, castable = 0;
        IOPCIDevice* found = nullptr;
        bool duplicate = false;
        char targetPath[256]{}, targetName[256]{};
        int length = sizeof(targetPath);
        auto* plane = IORegistryEntry::getPlane("IOACPIPlane");
        const bool havePath =
            plane && target->getPath(targetPath, &length, plane) && canonicalACPIPath(targetPath, targetName, sizeof(targetName));
        while (auto* entry = it ? it->getNextObject() : nullptr) {
            ++visited;
            auto* pci = cirrus::support::pciDeviceCast(entry);
            if (pci)
                ++castable;
            bool matches = pci && pci->getProperty("acpi-device") == target;
            if (pci && !matches && havePath) {
                auto* path = OSDynamicCast(OSString, pci->getProperty("acpi-path"));
                char canonical[256]{};
                matches =
                    path && canonicalACPIPath(path->getCStringNoCopy(), canonical, sizeof(canonical)) && !strcmp(canonical, targetName);
            }
            if (matches) {
                if (found) {
                    duplicate = true;
                    break;
                }
                found = pci;
            }
        }
        if (found && !duplicate)
            found->retain();
        if (it)
            it->release();
#if defined(KERNEL)
        if (owner) {
            if (allowWait) {
                owner->setProperty("Cirrus_SPI_PCI_Initial_Visited", uint64_t(visited), 32);
                owner->setProperty("Cirrus_SPI_PCI_Initial_Castable", uint64_t(castable), 32);
            }
            owner->setProperty("Cirrus_SPI_PCI_Visited", uint64_t(visited), 32);
            owner->setProperty("Cirrus_SPI_PCI_Castable", uint64_t(castable), 32);
            owner->setProperty("Cirrus_SPI_PCI_Target_Path", targetPath);
            owner->setProperty("Cirrus_SPI_PCI_Lookup_Status", duplicate ? "AMBIGUOUS" : found ? "PRESENT" : "NOT_PUBLISHED_OR_UNMATCHED");
        }
        if (allowWait && !found && !duplicate && havePath && !nativeSPIControllerHasACPIContract(target)) {
            // ACPI nubs can match before PCI enumeration has published the
            // corresponding service. Wait only for this exact registry path,
            // with a finite deadline; never substitute another PCI device.
            auto* waitMatching = IOService::serviceMatching("IOPCIDevice");
            auto* properties = OSDictionary::withCapacity(1);
            auto* path = OSString::withCString(targetPath);
            if (waitMatching && properties && path) {
                properties->setObject("acpi-path", path);
                waitMatching->setObject("IOPropertyMatch", properties);
                owner->setProperty("Cirrus_SPI_PCI_Lookup_Status", "WAITING_PUBLICATION");
                auto* service = IOService::waitForMatchingService(waitMatching, 5000000000ULL);
                auto* waitedPCI = cirrus::support::pciDeviceCast(service);
                if (waitedPCI) {
                    // Recheck identity and uniqueness after publication rather
                    // than trusting whichever service woke the wait first.
                    auto* validated = findPCI(target, owner, false);
                    if (validated == waitedPCI)
                        found = validated;
                    else if (validated)
                        validated->release();
                }
                if (service)
                    service->release();
                owner->setProperty("Cirrus_SPI_PCI_Lookup_Status", found ? "PUBLISHED_AFTER_WAIT" : "WAIT_TIMEOUT_OR_ASSOCIATION_REJECTED");
            } else
                owner->setProperty("Cirrus_SPI_PCI_Lookup_Status", "WAIT_ALLOCATION_FAILED");
            OSSafeReleaseNULL(path);
            OSSafeReleaseNULL(properties);
            OSSafeReleaseNULL(waitMatching);
        }
#else
        (void)allowWait;
        (void)owner;
        (void)visited;
        (void)castable;
#endif
        return duplicate ? nullptr : found;
    }
    const char* controllerSource(size_t slot) const {
        for (size_t i = 0; i < mCount; ++i)
            if (mSlots[i] == slot)
                return mPaths[i];
        return "";
    }
    IOReturn fail(const char* reason, IOReturn result, size_t index = 0, const char* source = nullptr, uint32_t detail = 0) {
        mFailure = {};
        mFailure.reason = reason;
        mFailure.result = result;
        mFailure.index = index;
        mFailure.detail = detail;
        if (source) {
            size_t n = strlen(source);
            if (n >= sizeof(mFailure.source))
                n = sizeof(mFailure.source) - 1;
            memcpy(mFailure.source, source, n);
        }
        return result;
    }
    // Copy failure context before shutdown clears discovery paths/resources.
    // No hardware traffic or log allocation occurs during error reporting.
    Failure mFailure{};
    ACPISPIResources mResources{};
    size_t mCount{0}, mControllers{0};
    bool mAuthorized{false};
    IOACPIPlatformDevice* mTargets[ACPISPIResources::kMaxEndpoints]{};
    NativeSPIControllerOwner* mOwners[ACPISPIResources::kMaxEndpoints]{};
    SPIController* mBus[ACPISPIResources::kMaxEndpoints]{};
    core::RegisterMap* mRegisters[ACPISPIResources::kMaxEndpoints]{};
    SPIRegisterBus* mPorts[ACPISPIResources::kMaxEndpoints]{};
    size_t mSlots[ACPISPIResources::kMaxEndpoints]{};
    char mPaths[ACPISPIResources::kMaxEndpoints][256]{};
};
}
