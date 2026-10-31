#pragma once
#include "Platform/ACPI/ACPIEvaluation.hpp"
#include "Platform/ACPI/ACPIPath.hpp"
#include "Support/KernelDeviceCast.hpp"
#include <IOKit/IORegistryEntry.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

#ifndef kIOReturnLockedWrite
#define kIOReturnLockedWrite static_cast<IOReturn>(0xe00002c4U)
#endif

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
    ~ACPIDeviceProperties() {
        OSSafeReleaseNULL(mFallback);
        OSSafeReleaseNULL(mObject);
    }
    const char* reason() const { return mReason; }
    IOReturn validationReturn() const { return mValidationReturn; }
    IOReturn evaluationReturn() const { return mEvaluationReturn; }
    unsigned failureIndex() const { return mFailureIndex; }
    OSObject* object() const { return mObject; }
    bool hasNativeProperties() const { return mProperties != nullptr; }
    // Treat interpreter unavailability or missing handle as absent DSD metadata,
    // allowing verified board profile fallback without forcing a custom SSDT.
    bool missingNativeMetadata() const {
        return !mObject && (mValidationReturn == kIOReturnNotFound || mValidationReturn == kIOReturnError ||
                            mValidationReturn == kIOReturnUnsupported || mValidationReturn == kIOReturnLockedWrite ||
                            mEvaluationReturn == kIOReturnNotFound || mEvaluationReturn == kIOReturnError ||
                            mEvaluationReturn == kIOReturnUnsupported || mEvaluationReturn == kIOReturnLockedWrite ||
                            (mReason && (!strcmp(mReason, "DSD_INTERPRETER_UNAVAILABLE") ||
                                         !strcmp(mReason, "DSD_INTERPRETER_HANDLE_NOT_FOUND"))));
    }
    const char* source() const { return mSource; }
    IOReturn referenceReturn() const { return mReferenceReturn; }
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
    IOReturn load(IOACPIPlatformDevice* device, IOService* owner = nullptr) {
        if (!device || mLoaded)
            return fail("INVALID_LOAD_ARGUMENT", kIOReturnBadArgument);
        mLoaded = true;
        const auto native = loadNative(device);
        if (!owner)
            return native;
        auto* nodes = OSDynamicCast(OSArray, owner->getProperty("Cirrus_Board_Metadata"));
        if (!nodes)
            return native;
        char path[256]{}, canonical[256]{};
        int length = sizeof(path);
        if (nodes->getCount() > 16 || !device->getPath(path, &length, IORegistryEntry::getPlane("IOACPIPlane")) ||
            !canonicalACPIPath(path, canonical, sizeof(canonical)))
            return fail("BOARD_METADATA_INVALID", kIOReturnUnsupported);
        OSArray* selected = nullptr;
        for (unsigned i = 0; i < nodes->getCount(); ++i) {
            auto* node = OSDynamicCast(OSArray, nodes->getObject(i));
            auto* name = node && node->getCount() == 2 ? OSDynamicCast(OSString, node->getObject(0)) : nullptr;
            auto* entries = node && node->getCount() == 2 ? OSDynamicCast(OSArray, node->getObject(1)) : nullptr;
            if (!name || !entries || entries->getCount() > 128)
                return fail("BOARD_METADATA_INVALID", kIOReturnUnsupported);
            if (name->isEqualTo(canonical)) {
                if (selected)
                    return fail("BOARD_METADATA_AMBIGUOUS", kIOReturnUnsupported);
                selected = entries;
            }
        }
        if (!selected)
            return native;
        if (native != kIOReturnSuccess && !missingNativeMetadata())
            return native;
        for (unsigned i = 0; i < selected->getCount(); ++i) {
            auto* pair = OSDynamicCast(OSArray, selected->getObject(i));
            auto* key = pair && pair->getCount() == 2 ? OSDynamicCast(OSString, pair->getObject(0)) : nullptr;
            if (!key)
                return fail("BOARD_METADATA_INVALID", kIOReturnUnsupported);
            for (unsigned j = 0; j < i; ++j) {
                auto* earlier = OSDynamicCast(OSArray, selected->getObject(j));
                auto* earlierKey = OSDynamicCast(OSString, earlier->getObject(0));
                if (key->isEqualTo(earlierKey))
                    return fail("BOARD_METADATA_AMBIGUOUS", kIOReturnUnsupported);
            }
        }
        mFallback = selected;
        mFallback->retain();
        mSource = mProperties ? "FIRMWARE_AND_BOARD_TABLE" : "BOARD_TABLE";
        mReason = "BOARD_METADATA_READY";
        return kIOReturnSuccess;
    }
private:
    IOReturn loadNative(IOACPIPlatformDevice* device) {
        mValidationReturn = device->validateObject("_DSD");
        if (mValidationReturn == kIOReturnNotFound) {
            mReason = "DSD_ABSENT";
            return kIOReturnSuccess;
        }
        if (mValidationReturn != kIOReturnSuccess)
            return fail("DSD_VALIDATION_FAILED", mValidationReturn);
        mEvaluationReturn = device->evaluateObject("_DSD", &mObject);
        mNativeObjectType = objectType(mObject);
        if (!mObject && (mEvaluationReturn == kIOReturnSuccess || mEvaluationReturn == kIOReturnUnsupported)) {
            mReferenceReturn = evaluateNativeProperties(device, &mObject);
            if (mReferenceReturn != kIOReturnSuccess)
                return fail(mReferenceReturn == kIOReturnNotReady ? "DSD_INTERPRETER_UNAVAILABLE"
                            : mReferenceReturn == kIOReturnIOError ? "DSD_INTERPRETER_EVALUATION_FAILED"
                            : mReferenceReturn == kIOReturnNotFound ? "DSD_INTERPRETER_HANDLE_NOT_FOUND"
                            : mReferenceReturn == kIOReturnNoMemory ? "DSD_INTERPRETER_ALLOCATION_FAILED"
                            : mReferenceReturn == kIOReturnBadArgument ? "DSD_INTERPRETER_RESULT_INVALID"
                            : "DSD_REFERENCE_CONVERSION_FAILED", mReferenceReturn);
            mSource = "_DSD_INTERPRETER";
        } else if (mEvaluationReturn != kIOReturnSuccess)
            return fail("DSD_EVALUATION_FAILED", mEvaluationReturn);
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
        mReason = mProperties ? "DSD_READY" : "DSD_PROPERTIES_UUID_ABSENT";
        return kIOReturnSuccess;
    }

public:
    OSObject* get(const char* name) const {
        if (!name)
            return nullptr;
        OSArray* sources[]{mProperties, mFallback};
        for (auto* entries : sources)
            for (unsigned i = 0; entries && i < entries->getCount(); ++i) {
                auto* pair = OSDynamicCast(OSArray, entries->getObject(i));
                auto* key = pair && pair->getCount() == 2 ? OSDynamicCast(OSString, pair->getObject(0)) : nullptr;
                if (!key)
                    return nullptr;
                if (key->isEqualTo(name))
                    return pair->getObject(1);
            }
        return nullptr;
    }

private:
    IOReturn fail(const char* reason, IOReturn result) {
        mReason = reason;
        mProperties = nullptr;
        OSSafeReleaseNULL(mFallback);
        return result;
    }
    OSObject* mObject{nullptr};
    OSArray* mProperties{nullptr};
    OSArray* mFallback{nullptr};
    bool mLoaded{false};
    const char* mReason{"NOT_LOADED"};
    const char* mSource{"_DSD"};
    const char* mNativeObjectType{"NOT_EVALUATED"};
    IOReturn mReferenceReturn{kIOReturnNotReady};
    IOReturn mValidationReturn{kIOReturnNotReady}, mEvaluationReturn{kIOReturnNotReady};
    unsigned mFailureIndex{0};
};
}
