// Resolve controller cs-gpios without embedding board IDs or GPIO numbers.
// The SPI owner keeps GPIO providers/maps alive and serializes every frame/PM.
#pragma once
#include "Diagnostics/ACPIObjectEvidence.hpp"
#include "Platform/ACPI/ACPIDeviceProperties.hpp"
#include "Platform/ACPI/ACPIGPIOResources.hpp"
#include "Transport/SPI/Intel/IntelGPIOOutput.hpp"
#include "Transport/SPI/Intel/IntelGPIOInput.hpp"

namespace cirrus::transport::intel {
class IntelSPIChipSelects {
public:
    static constexpr size_t kCapacity = 16;
    using Trace = void (*)(IOService*, const char*, IOReturn);
    IntelSPIChipSelects() = default;
    IntelSPIChipSelects(const IntelSPIChipSelects&) = delete;
    IntelSPIChipSelects& operator=(const IntelSPIChipSelects&) = delete;
    ~IntelSPIChipSelects() { shutdown(); }
    IOReturn bind(IOACPIPlatformDevice* controller, IOService* owner, Trace trace = nullptr, bool captureReset = false) {
        if (!controller || !owner || mOwner)
            return kIOReturnBadArgument;
        mOwner = owner;
        mTrace = trace;
        ACPIDeviceProperties properties;
        report("CS_DSD_BEGIN");
        auto result = properties.load(controller, owner);
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
            if (!provider) {
                source->release();
                return decoded ? kIOReturnNotFound : kIOReturnUnsupported;
            }
            result = bindLine(line, provider, gpio.pin);
            provider->release();
            if (result == kIOReturnSuccess && captureReset)
                observeReset(source);
            source->release();
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
            const char* names[] = {"Revision", "Padbar", "Ownership", "LockTx", "Config", "Offset", "Written", "Readback",
                                   "Logical0_Written", "Logical0_Readback", "Logical1_Written", "Logical1_Readback"};
            const uint32_t values[] = {e.revision, e.padbar, e.ownership, e.lockTx, e.config, e.offset, e.written, e.readback,
                                       e.logical0Written, e.logical0Readback, e.logical1Written, e.logical1Readback};
            for (unsigned j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
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
    // Startup/discovery-only input read through the already owned provider.
    // It cannot acquire output permissions or change pad configuration.
    IOReturn readGPIO(IOACPIPlatformDevice* source, const char* property, size_t index, uint32_t& value) {
        value = 0;
        auto result = validatePower();
        if (result != kIOReturnSuccess || !source || !property || index >= kCapacity)
            return result != kIOReturnSuccess ? result : kIOReturnBadArgument;
        ACPIDeviceProperties properties;
        result = properties.load(source, mOwner);
        if (result != kIOReturnSuccess)
            return result;
        auto* tuples = OSDynamicCast(OSArray, properties.get(property));
        if (!tuples || tuples->getCount() % 4 || tuples->getCount() > kCapacity * 4 || index >= tuples->getCount() / 4)
            return kIOReturnUnsupported;
        const unsigned i = unsigned(index * 4);
        auto* resource = OSDynamicCast(OSNumber, tuples->getObject(i + 1));
        auto* pin = OSDynamicCast(OSNumber, tuples->getObject(i + 2));
        auto* low = OSDynamicCast(OSNumber, tuples->getObject(i + 3));
        if (!resource || !pin || !low || resource->unsigned64BitValue() > 0xffff || pin->unsigned64BitValue() > 0xffff ||
            low->unsigned64BitValue() > 1)
            return kIOReturnUnsupported;
        auto* device = resolveACPIReference(source, tuples->getObject(i));
        OSObject* crs = nullptr;
        const auto crsResult = device ? device->evaluateObject("_CRS", &crs) : kIOReturnNotFound;
        auto* bytes = OSDynamicCast(OSData, crs);
        ACPIGPIOResource gpio{};
        const bool decoded = crsResult == kIOReturnSuccess && bytes &&
            parseACPIGPIOResource(static_cast<const uint8_t*>(bytes->getBytesNoCopy()), bytes->getLength(),
                                  resource->unsigned32BitValue(), pin->unsigned32BitValue(), gpio, false);
        auto* provider = decoded ? resolveACPIDevice(device, gpio.source) : nullptr;
        OSSafeReleaseNULL(crs);
        OSSafeReleaseNULL(device);
        if (!provider)
            return kIOReturnUnsupported;
        Line* mapped = nullptr;
        IntelGPIOPad pad{};
        result = ownedCommunity(provider, gpio.pin, mapped, pad);
        IntelGPIOInputEvidence evidence;
        evidence.reason = "COMMUNITY_UNAVAILABLE";
        if (result == kIOReturnSuccess)
            result = readIntelGPIOInput(*mapped, mapped->map->getLength(), pad, low->unsigned32BitValue() != 0, value, evidence);
        provider->release();
        if (mOwner) {
            mOwner->setProperty("Cirrus_GPIO_Input_Property", property);
            mOwner->setProperty("Cirrus_GPIO_Input_Index", uint64_t(index), 32);
            mOwner->setProperty("Cirrus_GPIO_Input_Pin", uint64_t(gpio.pin), 32);
            mOwner->setProperty("Cirrus_GPIO_Input_Reason", evidence.reason);
            mOwner->setProperty("Cirrus_GPIO_Input_Config", uint64_t(evidence.config), 32);
            mOwner->setProperty("Cirrus_GPIO_Input_Ownership", uint64_t(evidence.ownership), 32);
            mOwner->setProperty("Cirrus_GPIO_Input_Memory_Index", uint64_t(pad.memoryIndex), 32);
            mOwner->setProperty("Cirrus_GPIO_Input_Return", uint64_t(uint32_t(result)), 32);
            if (result == kIOReturnSuccess)
                mOwner->setProperty("Cirrus_GPIO_Input_Value", uint64_t(value), 32);
        }
        return result;
    }
    // Prepare every firmware reference before changing any level. Shared pins
    // are acquired once; a GPIO group cannot borrow a chip-select pad. Other
    // communities may be mapped only through an already owned GPIO provider.
    IOReturn prepareGPIOGroup(IOACPIPlatformDevice* source, const char* property) {
        clearGPIOGroup();
        if (!source || !property || !mOwner)
            return kIOReturnBadArgument;
        const auto reject = [&](IOReturn result) {
            clearGPIOGroup();
            return result;
        };
        auto result = validatePower();
        if (result != kIOReturnSuccess)
            return result;
        ACPIDeviceProperties properties;
        result = properties.load(source, mOwner);
        if (result != kIOReturnSuccess)
            return result;
        auto* tuples = OSDynamicCast(OSArray, properties.get(property));
        if (!tuples || !tuples->getCount() || tuples->getCount() % 4 || tuples->getCount() > kCapacity * 4)
            return kIOReturnUnsupported;
        for (unsigned i = 0; i < tuples->getCount(); i += 4) {
            auto* resource = OSDynamicCast(OSNumber, tuples->getObject(i + 1));
            auto* pin = OSDynamicCast(OSNumber, tuples->getObject(i + 2));
            auto* low = OSDynamicCast(OSNumber, tuples->getObject(i + 3));
            if (!resource || !pin || !low || resource->unsigned64BitValue() > 0xffff || pin->unsigned64BitValue() > 0xffff ||
                low->unsigned64BitValue() > 1)
                return reject(kIOReturnUnsupported);
            auto* device = resolveACPIReference(source, tuples->getObject(i));
            OSObject* crs = nullptr;
            const auto crsResult = device ? device->evaluateObject("_CRS", &crs) : kIOReturnNotFound;
            auto* bytes = OSDynamicCast(OSData, crs);
            ACPIGPIOResource gpio{};
            const bool decoded = crsResult == kIOReturnSuccess && bytes &&
                                 parseACPIGPIOResource(static_cast<const uint8_t*>(bytes->getBytesNoCopy()), bytes->getLength(),
                                                       resource->unsigned32BitValue(), pin->unsigned32BitValue(), gpio);
            auto* provider = decoded ? resolveACPIDevice(device, gpio.source) : nullptr;
            OSSafeReleaseNULL(crs);
            OSSafeReleaseNULL(device);
            if (!provider)
                return reject(kIOReturnUnsupported);
            bool collision = false;
            for (size_t j = 0; j < mCount; ++j) {
                auto& line = mLines[j];
                if (line.provider != provider || !line.gpio)
                    continue;
                collision |= line.pin == gpio.pin;
            }
            Line* mapped = nullptr;
            IntelGPIOPad pad{};
            result = collision ? kIOReturnNotPermitted : ownedCommunity(provider, gpio.pin, mapped, pad);
            provider->release();
            if (result != kIOReturnSuccess)
                return reject(result);
            bool duplicate = false;
            for (size_t j = 0; j < mGPIOCount; ++j)
                if (mGPIO[j].line->provider == mapped->provider && mGPIO[j].pin == gpio.pin) {
                    if (mGPIO[j].activeLow != bool(low->unsigned32BitValue()))
                        return reject(kIOReturnBadArgument);
                    duplicate = true;
                }
            if (duplicate)
                continue;
            auto& output = mGPIO[mGPIOCount++];
            output.line = mapped;
            output.pin = gpio.pin;
            output.activeLow = low->unsigned32BitValue() != 0;
            // Resolve again for the chosen map if another line was visited.
            if (!lookupIntelGPIOPad(mapped->identity, gpio.pin, pad))
                return reject(kIOReturnUnsupported);
            result = output.output.bind(*mapped, mapped->map->getLength(), pad, output.activeLow);
            mOwner->setProperty("Cirrus_SPI_GPIO_Group_Prepare_Pin", uint64_t(gpio.pin), 32);
            mOwner->setProperty("Cirrus_SPI_GPIO_Group_Prepare_Return", uint64_t(uint32_t(result)), 32);
            mOwner->setProperty("Cirrus_SPI_GPIO_Group_Prepare_Reason", output.output.evidence().reason);
            if (result != kIOReturnSuccess)
                return reject(result);
        }
        mOwner->setProperty("Cirrus_SPI_GPIO_Group_Unique_Pins", uint64_t(mGPIOCount), 32);
        mOwner->setProperty("Cirrus_SPI_GPIO_Group_Property", property);
        return kIOReturnSuccess;
    }
    void publishGPIOGroupEvidence() const {
        if (!mOwner)
            return;
        mOwner->setProperty("Cirrus_SPI_GPIO_Group_Observation", "PAD_TX_LATCH_READBACK_NOT_EXTERNAL_WAVEFORM");
        for (size_t i = 0; i < mGPIOCount; ++i) {
            const auto& e = mGPIO[i].output.evidence();
            char key[80]{};
            const char* fields[]{"Pin", "Offset", "Config", "Written", "Readback", "Logical0_Written", "Logical0_Readback",
                                 "Logical1_Written", "Logical1_Readback", "Revision", "Padbar", "Ownership", "LockTx"};
            const uint32_t values[]{mGPIO[i].pin, e.offset, e.config, e.written, e.readback, e.logical0Written, e.logical0Readback,
                                    e.logical1Written, e.logical1Readback, e.revision, e.padbar, e.ownership, e.lockTx};
            for (unsigned j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
                snprintf(key, sizeof(key), "Cirrus_SPI_GPIO_Group_%u_%s", unsigned(i), fields[j]);
                mOwner->setProperty(key, uint64_t(values[j]), 32);
            }
            snprintf(key, sizeof(key), "Cirrus_SPI_GPIO_Group_%u_Reason", unsigned(i));
            mOwner->setProperty(key, e.reason);
        }
    }
    IOReturn writeGPIOGroup(bool logicalLevel) {
        if (!mGPIOCount)
            return kIOReturnNotReady;
        for (size_t i = 0; i < mGPIOCount; ++i) {
            const auto result = mGPIO[i].output.validate();
            if (result != kIOReturnSuccess)
                return result;
        }
        for (size_t i = 0; i < mGPIOCount; ++i) {
            const auto result = mGPIO[i].output.setActive(logicalLevel);
            if (result != kIOReturnSuccess)
                return result;
        }
        return kIOReturnSuccess;
    }
    void shutdown() {
        clearGPIOGroup();
        // Release additional community maps before closing their provider.
        for (size_t i = mCommunityCount; i > 0; --i) {
            auto& line = mCommunities[i - 1];
            OSSafeReleaseNULL(line.map);
            OSSafeReleaseNULL(line.provider);
        }
        mCommunityCount = 0;
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
    void clearGPIOGroup() {
        for (size_t i = 0; i < mGPIOCount; ++i)
            mGPIO[i].output.unbind();
        mGPIOCount = 0;
    }
    // Observe firmware-described reset pads through the existing exclusive
    // GPIO provider lease. Additional community maps perform no output writes
    // or provider opens, including in read-only mode.
    void observeReset(IOACPIPlatformDevice* source) {
        mOwner->setProperty("Cirrus_SPI_Reset_Observation", "PAD_TX_LATCH_READBACK_NOT_EXTERNAL_WAVEFORM");
        ACPIDeviceProperties properties;
        const auto result = properties.load(source, mOwner);
        mOwner->setProperty("Cirrus_SPI_Reset_Properties_Return", uint64_t(uint32_t(result)), 32);
        mOwner->setProperty("Cirrus_SPI_Reset_Properties_Reason", properties.reason());
        mOwner->setProperty("Cirrus_SPI_Reset_Properties_Source", properties.source());
        mOwner->setProperty("Cirrus_SPI_Reset_Observed_When", "CHIP_SELECT_BIND_BEFORE_RESET");
        auto* tuples = OSDynamicCast(OSArray, properties.get("reset-gpios"));
        mOwner->setProperty("Cirrus_SPI_Reset_Status", "PROPERTIES_UNAVAILABLE");
        if (result != kIOReturnSuccess)
            return;
        if (!tuples || !tuples->getCount() || tuples->getCount() % 4 || tuples->getCount() > kCapacity * 4) {
            mOwner->setProperty("Cirrus_SPI_Reset_Status", "MISSING_OR_INVALID_REFERENCE");
            return;
        }
        for (unsigned i = 0; i < tuples->getCount(); i += 4) {
            char key[80]{};
            const auto publish = [&](const char* field, uint32_t value) {
                snprintf(key, sizeof(key), "Cirrus_SPI_Reset_%u_%s", i / 4, field);
                mOwner->setProperty(key, uint64_t(value), 32);
            };
            auto* resource = OSDynamicCast(OSNumber, tuples->getObject(i + 1));
            auto* pin = OSDynamicCast(OSNumber, tuples->getObject(i + 2));
            auto* low = OSDynamicCast(OSNumber, tuples->getObject(i + 3));
            if (!resource || !pin || !low || resource->unsigned64BitValue() > 0xffff || pin->unsigned64BitValue() > 0xffff ||
                low->unsigned64BitValue() > 1) {
                mOwner->setProperty("Cirrus_SPI_Reset_Status", "INVALID_REFERENCE");
                return;
            }
            auto* device = resolveACPIReference(source, tuples->getObject(i));
            OSObject* crs = nullptr;
            const auto crsResult = device ? device->evaluateObject("_CRS", &crs) : kIOReturnNotFound;
            auto* bytes = OSDynamicCast(OSData, crs);
            ACPIGPIOResource gpio{};
            const bool decoded = crsResult == kIOReturnSuccess && bytes &&
                                 parseACPIGPIOResource(static_cast<const uint8_t*>(bytes->getBytesNoCopy()), bytes->getLength(),
                                                       resource->unsigned32BitValue(), pin->unsigned32BitValue(), gpio);
            auto* provider = decoded ? resolveACPIDevice(device, gpio.source) : nullptr;
            OSSafeReleaseNULL(crs);
            OSSafeReleaseNULL(device);
            publish("CRS_Return", uint32_t(crsResult));
            const char* state = "NO_OWNED_COMMUNITY";
            if (provider) {
                publish("Pin", gpio.pin);
                publish("ActiveLow", low->unsigned32BitValue());
                Line* line = nullptr;
                IntelGPIOPad pad{};
                const auto mapping = ownedCommunity(provider, gpio.pin, line, pad);
                publish("Map_Return", uint32_t(mapping));
                if (mapping == kIOReturnSuccess) {
                    publish("Memory_Index", pad.memoryIndex);
                    IntelGPIOOutput observer;
                    const auto observed = observer.bind(*line, line->map->getLength(), pad, low->unsigned32BitValue() != 0);
                    const auto& evidence = observer.evidence();
                    publish("Return", uint32_t(observed));
                    publish("Config", evidence.config);
                    publish("Offset", evidence.offset);
                    publish("Ownership", evidence.ownership);
                    publish("LockTx", evidence.lockTx);
                    state = evidence.reason;
                    if (observed == kIOReturnSuccess) {
                        publish("PhysicalLevel", evidence.config & 1U);
                        publish("LogicalLevel", (evidence.config & 1U) ^ low->unsigned32BitValue());
                        state = "OBSERVED_WITHOUT_WRITES";
                    }
                } else if (mapping == kIOReturnNoResources)
                    state = "COMMUNITY_MAP_FAILED";
                provider->release();
            }
            snprintf(key, sizeof(key), "Cirrus_SPI_Reset_%u_Status", i / 4);
            mOwner->setProperty(key, state);
        }
        mOwner->setProperty("Cirrus_SPI_Reset_Status", "REFERENCE_SCAN_COMPLETE");
    }
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
        uint8_t memoryIndex{0};
        char identity[16]{};
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
    // Reuse the physical identity and exclusive lease established by CS bind.
    // Never derive a register layout from the amplifier or a board name.
    IOReturn ownedCommunity(IOACPIPlatformDevice* provider, uint16_t pin, Line*& output, IntelGPIOPad& pad) {
        output = nullptr;
        const Line* lease = nullptr;
        for (size_t i = 0; i < mCount; ++i) {
            auto& line = mLines[i];
            if (line.provider != provider || !line.gpio || !lookupIntelGPIOPad(line.identity, pin, pad))
                continue;
            lease = &line;
            if (line.memoryIndex == pad.memoryIndex) {
                output = &line;
                return kIOReturnSuccess;
            }
        }
        if (!lease)
            return kIOReturnUnsupported;
        for (size_t i = 0; i < mCommunityCount; ++i) {
            auto& line = mCommunities[i];
            if (line.provider == provider && line.memoryIndex == pad.memoryIndex) {
                output = &line;
                return kIOReturnSuccess;
            }
        }
        if (mCommunityCount == kCapacity)
            return kIOReturnNoResources;
        // Firmware power checks occur outside the SPI transaction lock.
        uint32_t power = 0;
        if ((provider->getDeviceStatus() & 3) != 3 ||
            (provider->validateObject("_PSC") == kIOReturnSuccess &&
             (provider->evaluateInteger("_PSC", &power) != kIOReturnSuccess || power)))
            return kIOReturnNotReady;
        auto* map = provider->mapDeviceMemoryWithIndex(pad.memoryIndex);
        if (!map || !map->getVirtualAddress()) {
            OSSafeReleaseNULL(map);
            return kIOReturnNoResources;
        }
        auto& line = mCommunities[mCommunityCount++];
        line.provider = provider;
        provider->retain();
        line.map = map;
        line.memoryIndex = pad.memoryIndex;
        snprintf(line.identity, sizeof(line.identity), "%s", lease->identity);
        output = &line;
        return kIOReturnSuccess;
    }
    IOReturn bindLine(Line& line, IOACPIPlatformDevice* provider, uint16_t pin) {
        OSObject* object = nullptr;
        auto result = provider->evaluateObject("_HID", &object);
        auto* hid = OSDynamicCast(OSString, object);
        IntelGPIOPad pad{};
        mOwner->setProperty("Cirrus_SPI_GPIO_HID", hid ? hid->getCStringNoCopy() : "UNAVAILABLE");
        mOwner->setProperty("Cirrus_SPI_GPIO_HID_Return", uint64_t(uint32_t(result)), 32);
        ACPIDeviceProperties native;
        const auto nativeResult = native.load(provider, mOwner);
        mOwner->setProperty("Cirrus_SPI_GPIO_Properties_Return", uint64_t(uint32_t(nativeResult)), 32);
        mOwner->setProperty("Cirrus_SPI_GPIO_Properties_Reason", native.reason());
        mOwner->setProperty("Cirrus_SPI_GPIO_Properties_Source", native.source());
        mOwner->setProperty("Cirrus_SPI_GPIO_DSD_Validate_Return", uint64_t(uint32_t(native.validationReturn())), 32);
        mOwner->setProperty("Cirrus_SPI_GPIO_DSD_Evaluate_Return", uint64_t(uint32_t(native.evaluationReturn())), 32);
        mOwner->setProperty("Cirrus_SPI_GPIO_DSD_Adapter_Return", uint64_t(uint32_t(native.adapterReturn())), 32);
        mOwner->setProperty("Cirrus_SPI_GPIO_DSD_Native_Object_Type", native.nativeObjectType());
        diagnostics::ACPIObjectEvidence::publish(mOwner, "Cirrus_SPI_GPIO_DSD", native.object());
        // GPIO metadata is optional when firmware exposes an unmodified,
        // supported physical HID. Unlike SPI cs-gpios, a GPIO controller does
        // not need a property package to describe its register layout. Preserve
        // errors for malformed objects/adapters and for compatibility HIDs.
        IntelGPIOPad nativePad{};
        const bool nativeIdentity = result == kIOReturnSuccess && hid &&
                                    lookupIntelGPIOPad(hid->getCStringNoCopy(), pin, nativePad);
        const bool absentMetadata = native.missingNativeMetadata();
        if (nativeResult != kIOReturnSuccess && !(nativeIdentity && absentMetadata)) {
            OSSafeReleaseNULL(object);
            return nativeResult;
        }
        auto* physical = nativeResult == kIOReturnSuccess ? OSDynamicCast(OSString, native.get("cirrus,gpio-controller-hid")) : nullptr;
        // Explicit firmware metadata may preserve physical identity when the
        // compatibility HID was changed for another client. Never alias HIDs.
        const char* identity = physical ? physical->getCStringNoCopy() : hid ? hid->getCStringNoCopy() : nullptr;
        mOwner->setProperty("Cirrus_SPI_GPIO_Identity_Source", physical ? "FIRMWARE_METADATA" : "ACPI_HID");
        mOwner->setProperty("Cirrus_SPI_GPIO_Physical_HID", identity ? identity : "UNAVAILABLE");
        const bool supported = result == kIOReturnSuccess && hid && lookupIntelGPIOPad(identity, pin, pad);
        if (supported) {
            snprintf(line.identity, sizeof(line.identity), "%s", identity);
            line.memoryIndex = pad.memoryIndex;
        }
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
    Line mCommunities[kCapacity];
    size_t mCommunityCount{0};
    struct GPIOOutput {
        IntelGPIOOutput output;
        Line* line{nullptr};
        uint16_t pin{0};
        bool activeLow{false};
    };
    GPIOOutput mGPIO[kCapacity];
    size_t mGPIOCount{0};
    size_t mCount{0};
    IOService* mOwner{nullptr};
    Trace mTrace{nullptr};
};
}
