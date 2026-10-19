// Owned ACPI device-properties package and namespace reference resolution.
#pragma once
#include "Support/KernelDeviceCast.hpp"
#include "Transport/ACPIPath.hpp"
#include <IOKit/IORegistryEntry.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

namespace cirrus::transport {
inline IOACPIPlatformDevice* resolveACPIDevice(IOACPIPlatformDevice* device, const char* source) {
    const auto* plane = IORegistryEntry::getPlane("IOACPIPlane");
    if (!device || !source || !plane)
        return nullptr;
    IORegistryEntry* scope = device;
    do {
        char path[256]{}, canonical[256]{}, wanted[256]{};
        int length = sizeof(path);
        if (!scope->getPath(path, &length, plane) || !canonicalACPIPath(path, canonical, sizeof(canonical)) ||
            !resolveACPIPath(canonical, source, wanted, sizeof(wanted)))
            return nullptr;
        auto* it = IORegistryIterator::iterateOver(IORegistryEntry::getRegistryRoot(), plane, kIORegistryIterateRecursively);
        if (!it)
            return nullptr;
        IOACPIPlatformDevice* found = nullptr;
        bool duplicate = false;
        while (auto* object = it->getNextObject()) {
            auto* candidate = support::acpiDeviceCast(object);
            length = sizeof(path);
            if (candidate && candidate->getPath(path, &length, plane) && canonicalACPIPath(path, canonical, sizeof(canonical)) &&
                !strcmp(canonical, wanted)) {
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
inline IOACPIPlatformDevice* resolveACPIReference(IOACPIPlatformDevice* scope, OSObject* object) {
    if (auto* device = support::acpiDeviceCast(object)) {
        device->retain();
        return device;
    }
    auto* path = OSDynamicCast(OSString, object);
    if (!path)
        return nullptr;
    const char* text = path->getCStringNoCopy();
    char canonical[256]{};
    if (!strncmp(text, "IOACPIPlane:", 12) || *text == '/')
        return canonicalACPIPath(text, canonical, sizeof(canonical)) ? resolveACPIDevice(scope, canonical) : nullptr;
    return resolveACPIDevice(scope, text);
}
class ACPIDeviceProperties {
public:
    ACPIDeviceProperties() = default;
    ACPIDeviceProperties(const ACPIDeviceProperties&) = delete;
    ACPIDeviceProperties& operator=(const ACPIDeviceProperties&) = delete;
    ~ACPIDeviceProperties() { OSSafeReleaseNULL(mObject); }
    const char* reason() const { return mReason; }
    IOReturn validationReturn() const { return mValidationReturn; }
    IOReturn evaluationReturn() const { return mEvaluationReturn; }
    unsigned failureIndex() const { return mFailureIndex; }
    OSObject* object() const { return mObject; }
    const char* source() const { return mSource; }
    IOReturn adapterReturn() const { return mAdapterReturn; }
    const char* nativeObjectType() const { return mNativeObjectType; }
    static const char* objectType(OSObject* object) {
        if (!object)
            return "NONE";
        if (OSDynamicCast(OSArray, object))
            return "OSArray";
        if (OSDynamicCast(OSData, object))
            return "OSData";
        if (OSDynamicCast(OSString, object))
            return "OSString";
        if (OSDynamicCast(OSNumber, object))
            return "OSNumber";
        if (support::acpiDeviceCast(object))
            return "IOACPIPlatformDevice";
        return "OTHER";
    }
    IOReturn load(IOACPIPlatformDevice* device) {
        if (!device || mObject)
            return fail("INVALID_LOAD_ARGUMENT", kIOReturnBadArgument);
        mValidationReturn = device->validateObject("_DSD");
        // Apple may report generic Error rather than NotFound for a missing
        // object. Only an explicit, schema-validated adapter can recover that
        // condition; never reinterpret it as native-only CS wiring.
        if (mValidationReturn != kIOReturnSuccess) {
            const auto adapter = loadAdapter(device);
            if (mValidationReturn == kIOReturnNotFound && adapter == kIOReturnNotFound) {
                mReason = "DSD_ABSENT";
                return kIOReturnSuccess;
            }
            if (adapter != kIOReturnSuccess)
                return fail("DSD_VALIDATION_FAILED", mValidationReturn);
        } else {
            mEvaluationReturn = device->evaluateObject("_DSD", &mObject);
            mNativeObjectType = objectType(mObject);
            // Some macOS ACPI conversions discard reference-bearing packages.
            // CSDP is an optional firmware adapter using the same UUID/schema,
            // version 1, with references represented by namespace strings.
            // It never overrides a successfully returned native object.
            if ((mEvaluationReturn == kIOReturnSuccess && !mObject) || mEvaluationReturn == kIOReturnUnsupported) {
                const auto adapter = loadAdapter(device);
                if (adapter != kIOReturnSuccess)
                    return fail(mEvaluationReturn == kIOReturnSuccess ? "DSD_EMPTY_RESULT" : "DSD_EVALUATION_FAILED",
                                mEvaluationReturn == kIOReturnSuccess ? kIOReturnUnsupported : mEvaluationReturn);
            } else if (mEvaluationReturn != kIOReturnSuccess)
                return fail("DSD_EVALUATION_FAILED", mEvaluationReturn);
        }
        auto* root = OSDynamicCast(OSArray, mObject);
        if (!root)
            return fail("DSD_ROOT_TYPE", kIOReturnUnsupported);
        if (root->getCount() > 64 || (root->getCount() & 1))
            return fail("DSD_ROOT_SIZE", kIOReturnUnsupported);
        static constexpr uint8_t uuid[] = {0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d, 0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01};
        for (unsigned i = 0; i < root->getCount(); i += 2) {
            mFailureIndex = i;
            auto* id = OSDynamicCast(OSData, root->getObject(i));
            if (!id || id->getLength() != sizeof(uuid))
                return fail("DSD_UUID_TYPE_OR_SIZE", kIOReturnUnsupported);
            if (memcmp(id->getBytesNoCopy(), uuid, sizeof(uuid)))
                continue;
            if (mProperties)
                return fail("DSD_DUPLICATE_UUID", kIOReturnUnsupported);
            mProperties = OSDynamicCast(OSArray, root->getObject(i + 1));
            if (!mProperties || mProperties->getCount() > 128)
                return fail("DSD_PROPERTIES_TYPE_OR_SIZE", kIOReturnUnsupported);
            for (unsigned j = 0; j < mProperties->getCount(); ++j) {
                mFailureIndex = j;
                auto* pair = OSDynamicCast(OSArray, mProperties->getObject(j));
                auto* key = pair && pair->getCount() == 2 ? OSDynamicCast(OSString, pair->getObject(0)) : nullptr;
                if (!key)
                    return fail("DSD_PROPERTY_PAIR_OR_KEY", kIOReturnUnsupported);
                for (unsigned k = 0; k < j; ++k) {
                    auto* previous = OSDynamicCast(OSArray, mProperties->getObject(k));
                    auto* previousKey = previous && previous->getCount() == 2 ? OSDynamicCast(OSString, previous->getObject(0)) : nullptr;
                    if (!previousKey)
                        return fail("DSD_PROPERTY_PAIR_OR_KEY", kIOReturnUnsupported);
                    if (key->isEqualTo(previousKey))
                        return fail("DSD_DUPLICATE_KEY", kIOReturnUnsupported);
                }
            }
        }
        if (!strcmp(mSource, "CSDP")) {
            auto* version = OSDynamicCast(OSNumber, get("cirrus,acpi-adapter-version"));
            if (!version || version->unsigned64BitValue() != 1)
                return fail("ADAPTER_VERSION_UNSUPPORTED", kIOReturnUnsupported);
        }
        mReason = mProperties ? "DSD_READY" : "DSD_PROPERTIES_UUID_ABSENT";
        return kIOReturnSuccess;
    }
    OSObject* get(const char* name) const {
        if (!name)
            return nullptr;
        for (unsigned i = 0; mProperties && i < mProperties->getCount(); ++i) {
            auto* pair = OSDynamicCast(OSArray, mProperties->getObject(i));
            auto* key = pair && pair->getCount() == 2 ? OSDynamicCast(OSString, pair->getObject(0)) : nullptr;
            if (!key)
                return nullptr;
            if (key->isEqualTo(name))
                return pair->getObject(1);
        }
        return nullptr;
    }

private:
    IOReturn loadAdapter(IOACPIPlatformDevice* device) {
        mAdapterReturn = device->validateObject("CSDP");
        if (mAdapterReturn != kIOReturnSuccess)
            return mAdapterReturn;
        OSSafeReleaseNULL(mObject);
        mSource = "CSDP";
        mAdapterReturn = device->evaluateObject("CSDP", &mObject);
        return mAdapterReturn;
    }
    IOReturn fail(const char* reason, IOReturn result) {
        mReason = reason;
        mProperties = nullptr;
        return result;
    }
    OSObject* mObject{nullptr};
    OSArray* mProperties{nullptr};
    const char* mReason{"NOT_LOADED"};
    const char* mSource{"_DSD"};
    const char* mNativeObjectType{"NOT_EVALUATED"};
    IOReturn mAdapterReturn{kIOReturnNotReady};
    IOReturn mValidationReturn{kIOReturnNotReady}, mEvaluationReturn{kIOReturnNotReady};
    unsigned mFailureIndex{0};
};
}
