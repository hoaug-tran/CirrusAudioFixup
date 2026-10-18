// Resolve controller cs-gpios without embedding board IDs or GPIO numbers.
// The SPI owner keeps GPIO providers/maps alive and serializes every frame/PM.
#pragma once
#include "Diagnostics/ACPIObjectEvidence.hpp"
#include "Transport/ACPIDeviceProperties.hpp"
#include "Transport/ACPIGPIOResources.hpp"
#include "Transport/Intel/IntelGPIOOutput.hpp"

namespace cirrus::transport::intel {
class IntelSPIChipSelects {
public:
    static constexpr size_t kCapacity = 16;
    using Trace = void (*)(IOService*, const char*, IOReturn);
    IntelSPIChipSelects() = default;
    IntelSPIChipSelects(const IntelSPIChipSelects&) = delete;
    IntelSPIChipSelects& operator=(const IntelSPIChipSelects&) = delete;
    ~IntelSPIChipSelects() { shutdown(); }
    IOReturn bind(IOACPIPlatformDevice* controller, IOService* owner, Trace trace = nullptr) {
        if (!controller || !owner || mOwner)
            return kIOReturnBadArgument;
        mOwner = owner;
        mTrace = trace;
        ACPIDeviceProperties properties;
        report("CS_DSD_BEGIN");
        auto result = properties.load(controller);
        diagnostics::ACPIObjectEvidence::publish(owner, "Cirrus_SPI_DSD", properties.object());
        owner->setProperty("Cirrus_Intel_SPI_CS_DSD_Reason", properties.reason());
        owner->setProperty("Cirrus_Intel_SPI_CS_DSD_Source", properties.source());
        owner->setProperty("Cirrus_Intel_SPI_CS_DSD_Native_Object_Type", properties.nativeObjectType());
        owner->setProperty("Cirrus_Intel_SPI_CS_DSD_Adapter_Return", uint64_t(uint32_t(properties.adapterReturn())), 32);
        owner->setProperty("Cirrus_Intel_SPI_CS_DSD_Validate_Return", uint64_t(uint32_t(properties.validationReturn())), 32);
        owner->setProperty("Cirrus_Intel_SPI_CS_DSD_Evaluate_Return", uint64_t(uint32_t(properties.evaluationReturn())), 32);
        owner->setProperty("Cirrus_Intel_SPI_CS_DSD_Object_Type", ACPIDeviceProperties::objectType(properties.object()));
        owner->setProperty("Cirrus_Intel_SPI_CS_DSD_Failure_Index", uint64_t(properties.failureIndex()), 32);
        // Preserve only bounded shape evidence, not arbitrary firmware objects
        // or live device references, in the service-failure snapshot.
        auto* root = OSDynamicCast(OSArray, properties.object());
        if (root) {
            owner->setProperty("Cirrus_Intel_SPI_CS_DSD_Root_Count", uint64_t(root->getCount()), 32);
            if (root->getCount()) {
                auto* uuid = root->getObject(0);
                owner->setProperty("Cirrus_Intel_SPI_CS_DSD_UUID_Type", ACPIDeviceProperties::objectType(uuid));
                auto* bytes = OSDynamicCast(OSData, uuid);
                if (bytes && bytes->getLength() <= 16)
                    owner->setProperty("Cirrus_Intel_SPI_CS_DSD_UUID", bytes);
            }
        }
        report("CS_DSD_END", result);
        if (result != kIOReturnSuccess)
            return result;
        auto* object = properties.get("cs-gpios");
        owner->setProperty("Cirrus_Intel_SPI_CS_Value_Type", ACPIDeviceProperties::objectType(object));
        if (!object)
            return kIOReturnSuccess;
        auto* values = OSDynamicCast(OSArray, object);
        if (values)
            owner->setProperty("Cirrus_Intel_SPI_CS_Value_Count", uint64_t(values->getCount()), 32);
        if (!values || !values->getCount() || values->getCount() > kCapacity * 4)
            return kIOReturnUnsupported;
        for (unsigned i = 0; i < values->getCount();) {
            owner->setProperty("Cirrus_Intel_SPI_CS_Value_Index", uint64_t(i), 32);
            owner->setProperty("Cirrus_Intel_SPI_CS_Entry_Type", ACPIDeviceProperties::objectType(values->getObject(i)));
            if (mCount == kCapacity)
                return kIOReturnUnsupported;
            auto& line = mLines[mCount++];
            auto* native = OSDynamicCast(OSNumber, values->getObject(i));
            if (native) {
                if (native->unsigned64BitValue())
                    return kIOReturnUnsupported;
                ++i;
                continue;
            }
            if (values->getCount() - i < 4)
                return kIOReturnUnsupported;
            auto* resource = OSDynamicCast(OSNumber, values->getObject(i + 1));
            auto* pin = OSDynamicCast(OSNumber, values->getObject(i + 2));
            auto* low = OSDynamicCast(OSNumber, values->getObject(i + 3));
            if (!resource || !pin || !low || resource->unsigned64BitValue() > 0xffff || pin->unsigned64BitValue() > 0xffff ||
                low->unsigned64BitValue() > 1)
                return kIOReturnUnsupported;
            owner->setProperty("Cirrus_SPI_CS_Resource_Index", resource->unsigned64BitValue(), 64);
            owner->setProperty("Cirrus_SPI_CS_Pin_Index", pin->unsigned64BitValue(), 64);
            owner->setProperty("Cirrus_SPI_CS_Active_Low", low->unsigned64BitValue(), 64);
            // SPI endpoint polarity is active-low; ACPI's GPIO flag is not a
            // replacement for SpiSerialBus polarity (Linux spi-cs-high policy).
            auto* source = resolveACPIReference(controller, values->getObject(i));
            if (!source)
                return kIOReturnNotFound;
            OSObject* crs = nullptr;
            report("CS_CRS_BEGIN");
            result = source->evaluateObject("_CRS", &crs);
            diagnostics::ACPIObjectEvidence::publish(owner, "Cirrus_SPI_CS_CRS", crs);
            report("CS_CRS_END", result);
            auto* bytes = OSDynamicCast(OSData, crs);
            ACPIGPIOResource gpio{};
            const bool decoded = result == kIOReturnSuccess && bytes &&
                                 parseACPIGPIOResource(static_cast<const uint8_t*>(bytes->getBytesNoCopy()), bytes->getLength(),
                                                       resource->unsigned32BitValue(), pin->unsigned32BitValue(), gpio);
            OSSafeReleaseNULL(crs);
            auto* provider = decoded ? resolveACPIDevice(source, gpio.source) : nullptr;
            owner->setProperty("Cirrus_SPI_CS_CRS_Decoded", uint64_t(decoded), 32);
            if (decoded) {
                owner->setProperty("Cirrus_SPI_CS_GPIO_Source", gpio.source);
                owner->setProperty("Cirrus_SPI_CS_GPIO_Pin", uint64_t(gpio.pin), 32);
            }
            source->release();
            if (!provider)
                return decoded ? kIOReturnNotFound : kIOReturnUnsupported;
            result = bindLine(line, provider, gpio.pin);
            provider->release();
            if (result != kIOReturnSuccess) {
                publishEvidence();
                return result;
            }
            i += 4;
        }
        // Verify the complete topology before changing any output. Every
        // bound CS is set inactive; reset pins are never borrowed as CS.
        report("CS_INACTIVE_BEGIN");
        for (size_t i = 0; i < mCount; ++i)
            if (mLines[i].gpio && (result = mLines[i].output.setActive(false)) != kIOReturnSuccess) {
                report("CS_INACTIVE_END", result);
                publishEvidence();
                return result;
            }
        report("CS_INACTIVE_END");
        publishEvidence();
        return kIOReturnSuccess;
    }
    void publishEvidence() const {
        if (!mOwner)
            return;
        for (size_t i = 0; i < mCount; ++i) {
            const auto& e = mLines[i].output.evidence();
            char key[96]{};
            snprintf(key, sizeof(key), "Cirrus_SPI_GPIO_%u_Reason", unsigned(i));
            mOwner->setProperty(key, e.reason);
            const char* names[] = {"Revision", "Padbar", "Ownership", "LockTx", "Config", "Offset", "Written", "Readback"};
            const uint32_t values[] = {e.revision, e.padbar, e.ownership, e.lockTx, e.config, e.offset, e.written, e.readback};
            for (unsigned j = 0; j < 8; ++j) {
                snprintf(key, sizeof(key), "Cirrus_SPI_GPIO_%u_%s", unsigned(i), names[j]);
                mOwner->setProperty(key, uint64_t(values[j]), 32);
            }
        }
    }
    size_t count() const { return mCount; }
    bool usesGPIO(size_t index) const { return index < mCount && mLines[index].gpio; }
    uint16_t pin(size_t index) const { return index < mCount ? mLines[index].pin : 0; }
    SPIChipSelect* select(uint32_t index) { return index < mCount && mLines[index].gpio ? &mLines[index].output : nullptr; }
    // Firmware evaluation may sleep or acquire the ACPI interpreter lock.
    // The owner calls this before taking its SPI transaction lock.
    IOReturn validatePower() const {
        for (size_t i = 0; i < mCount; ++i) {
            if (!mLines[i].gpio)
                continue;
            uint32_t power = 0;
            auto* provider = mLines[i].provider;
            if ((provider->getDeviceStatus() & 3) != 3 || (provider->validateObject("_PSC") == kIOReturnSuccess &&
                                                           (provider->evaluateInteger("_PSC", &power) != kIOReturnSuccess || power)))
                return kIOReturnNotReady;
        }
        return kIOReturnSuccess;
    }
    // Only register reads: safe inside the serialized transaction region.
    IOReturn validateOutputs() const {
        for (size_t i = 0; i < mCount; ++i) {
            if (!mLines[i].gpio)
                continue;
            const auto result = mLines[i].output.validate();
            if (result != kIOReturnSuccess)
                return result;
        }
        return kIOReturnSuccess;
    }
    IOReturn validate() const {
        const auto result = validatePower();
        return result == kIOReturnSuccess ? validateOutputs() : result;
    }
    void shutdown() {
        for (size_t i = mCount; i > 0; --i) {
            auto& line = mLines[i - 1];
            line.output.unbind();
            OSSafeReleaseNULL(line.map);
            // One exclusive open per provider, even with multiple CS lines.
            if (line.opened)
                line.provider->close(mOwner);
            OSSafeReleaseNULL(line.provider);
            line.gpio = line.opened = false;
        }
        mCount = 0;
        mOwner = nullptr;
        mTrace = nullptr;
    }

private:
    void report(const char* stage, IOReturn result = kIOReturnSuccess) const {
        if (mOwner) {
            mOwner->setProperty("Cirrus_Intel_SPI_CS_Stage", stage);
            mOwner->setProperty("Cirrus_Intel_SPI_CS_Stage_Return", uint64_t(uint32_t(result)), 32);
        }
        if (mTrace)
            mTrace(mOwner, stage, result);
    }
    struct Line final : IntelGPIOWindow {
        IOACPIPlatformDevice* provider{nullptr};
        IOMemoryMap* map{nullptr};
        IntelGPIOOutput output;
        uint16_t pin{0};
        bool gpio{false}, opened{false};
        uint32_t read32(uint32_t offset) override {
            const auto value = *reinterpret_cast<volatile uint32_t*>(map->getVirtualAddress() + offset);
            OSSynchronizeIO();
            return value;
        }
        void write32(uint32_t offset, uint32_t value) override {
            *reinterpret_cast<volatile uint32_t*>(map->getVirtualAddress() + offset) = value;
            OSSynchronizeIO();
        }
    };
    IOReturn bindLine(Line& line, IOACPIPlatformDevice* provider, uint16_t pin) {
        OSObject* object = nullptr;
        auto result = provider->evaluateObject("_HID", &object);
        auto* hid = OSDynamicCast(OSString, object);
        IntelGPIOPad pad{};
        mOwner->setProperty("Cirrus_SPI_GPIO_HID", hid ? hid->getCStringNoCopy() : "UNAVAILABLE");
        mOwner->setProperty("Cirrus_SPI_GPIO_HID_Return", uint64_t(uint32_t(result)), 32);
        ACPIDeviceProperties native;
        const auto nativeResult = native.load(provider);
        mOwner->setProperty("Cirrus_SPI_GPIO_Properties_Return", uint64_t(uint32_t(nativeResult)), 32);
        mOwner->setProperty("Cirrus_SPI_GPIO_Properties_Reason", native.reason());
        mOwner->setProperty("Cirrus_SPI_GPIO_Properties_Source", native.source());
        diagnostics::ACPIObjectEvidence::publish(mOwner, "Cirrus_SPI_GPIO_DSD", native.object());
        if (nativeResult != kIOReturnSuccess) {
            OSSafeReleaseNULL(object);
            return nativeResult;
        }
        auto* physical = nativeResult == kIOReturnSuccess ? OSDynamicCast(OSString, native.get("cirrus,gpio-controller-hid")) : nullptr;
        // Explicit firmware metadata may preserve physical identity when the
        // compatibility HID was changed for another client. Never alias HIDs.
        const char* identity = physical ? physical->getCStringNoCopy() : hid ? hid->getCStringNoCopy() : nullptr;
        mOwner->setProperty("Cirrus_SPI_GPIO_Physical_HID", identity ? identity : "UNAVAILABLE");
        const bool supported = result == kIOReturnSuccess && hid && lookupIntelGPIOPad(identity, pin, pad);
        OSSafeReleaseNULL(object);
        const auto status = provider->getDeviceStatus();
        mOwner->setProperty("Cirrus_SPI_GPIO_Device_Status", uint64_t(status), 32);
        mOwner->setProperty("Cirrus_SPI_GPIO_Profile_Supported", uint64_t(supported), 32);
        if (!supported || (status & 3) != 3)
            return kIOReturnUnsupported;
        mOwner->setProperty("Cirrus_SPI_GPIO_Memory_Index", uint64_t(pad.memoryIndex), 32);
        mOwner->setProperty("Cirrus_SPI_GPIO_Pad", uint64_t(pad.pad), 32);
        mOwner->setProperty("Cirrus_SPI_GPIO_Group", uint64_t(pad.group), 32);
        uint32_t power = 0;
        if (provider->validateObject("_PSC") == kIOReturnSuccess &&
            (provider->evaluateInteger("_PSC", &power) != kIOReturnSuccess || power))
            return kIOReturnNotReady;
        bool alreadyOpen = false;
        for (size_t i = 0; i + 1 < mCount; ++i) {
            const auto& previous = mLines[i];
            if (previous.provider == provider) {
                if (previous.pin == pin)
                    return kIOReturnBadArgument;
                alreadyOpen = true;
            }
        }
        line.provider = provider;
        provider->retain();
        if (!alreadyOpen) {
            mOwner->setProperty("Cirrus_SPI_GPIO_Already_Open", uint64_t(provider->isOpen()), 32);
            if (provider->isOpen() || !provider->open(mOwner))
                return kIOReturnExclusiveAccess;
            line.opened = true;
        }
        line.pin = pin;
        report("GPIO_MAP_BEGIN");
        line.map = provider->mapDeviceMemoryWithIndex(pad.memoryIndex);
        report("GPIO_MAP_END", line.map ? kIOReturnSuccess : kIOReturnNoResources);
        if (!line.map || !line.map->getVirtualAddress())
            return kIOReturnNoResources;
        report("GPIO_MMIO_VALIDATE_BEGIN");
        result = line.output.bind(line, line.map->getLength(), pad, true);
        report("GPIO_MMIO_VALIDATE_END", result);
        if (result == kIOReturnSuccess)
            line.gpio = true;
        return result;
    }
    Line mLines[kCapacity];
    size_t mCount{0};
    IOService* mOwner{nullptr};
    Trace mTrace{nullptr};
};
}
