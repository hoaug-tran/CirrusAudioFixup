#pragma once
// IOKit host for registered amplifier and communication drivers.
// Hardware readiness is established by register and DSP checks. HDA stream
// observation cannot confirm the downstream audio route or audible output.
// See LICENSE for distribution terms.
#include "Service/CirrusAudioFixup.hpp"
#include "Diagnostics/ACPIObjectEvidence.hpp"
#include "Diagnostics/FailureNames.hpp"
#include "Support/CPUIdentity.hpp"
#include "Platform/ACPI/ACPIPlatformProperties.hpp"

#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>

#define super IOService
using namespace cirrus::diagnostics;

#if defined(KERNEL)
    namespace {
    // Publish one coherent event on the registry root so failed service creation
    // remains observable after IOKit frees the temporary driver instance.
    inline void publishMatchingEvent(const char* stage, IOService* provider, IOReturn result, const char* retainedProperty = nullptr) {
        if (!gCirrusDebug)
            return;
        auto* event = OSDictionary::withCapacity(4);
        if (!event)
            return;
        auto* text = OSString::withCString(stage);
        auto* code = OSNumber::withNumber(uint64_t(uint32_t(result)), 32);
        if (text) {
            event->setObject("Stage", text);
            text->release();
        }
        if (code) {
            event->setObject("Return", code);
            code->release();
        }
        if (provider) {
            auto* name = OSString::withCString(provider->getName());
            auto* id = OSNumber::withNumber(provider->getRegistryEntryID(), 64);
            if (name) {
                event->setObject("Provider", name);
                name->release();
            }
            if (id) {
                event->setObject("ProviderID", id);
                id->release();
            }
        }
        auto* root = IORegistryEntry::getRegistryRoot();
        if (root) {
            root->setProperty("Cirrus_Last_Service_Lifecycle", event);
            if (retainedProperty)
                root->setProperty(retainedProperty, event);
            if (result != kIOReturnSuccess)
                root->setProperty("Cirrus_Last_Service_Failure", event);
        }
        event->release();
    }
    inline void publishBootCheckpoint(IOService* service, const char* stage, IOReturn result = kIOReturnSuccess) {
        if (!gCirrusDebug || !service)
            return;
        auto* previous = OSDynamicCast(OSNumber, service->getProperty("Cirrus_Boot_Checkpoint_Count"));
        const uint32_t count = previous ? previous->unsigned32BitValue() : 0;
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        service->setProperty("Cirrus_Boot_Checkpoint_Last", stage);
        service->setProperty("Cirrus_Boot_Checkpoint_Return", uint64_t(uint32_t(result)), 32);
        if (count < 96) {
            char key[80]{}, value[160]{};
            snprintf(key, sizeof(key), "Cirrus_Boot_Checkpoint_%03u", count);
            snprintf(value, sizeof(value), "uptime_us=%llu stage=%s ret=0x%08x", static_cast<unsigned long long>(ns / 1000), stage,
                     uint32_t(result));
            service->setProperty(key, value);
            service->setProperty("Cirrus_Boot_Checkpoint_Count", uint64_t(count + 1), 32);
        } else
            service->setProperty("Cirrus_Boot_Checkpoint_Status", "PARTIAL_LIMIT");
        if (result != kIOReturnSuccess && !service->getProperty("Cirrus_Boot_First_Error")) {
            service->setProperty("Cirrus_Boot_First_Error", stage);
            service->setProperty("Cirrus_Boot_First_Return", uint64_t(uint32_t(result)), 32);
        }
    }
    inline void retainFailedServiceEvidence(IOService * service) {
        if (!gCirrusDebug)
            return;
        auto* properties = service->dictionaryWithProperties();
        if (!properties)
            return;
        auto* evidence = OSDictionary::withCapacity(properties->getCount());
        auto* iterator = OSCollectionIterator::withCollection(properties);
        if (evidence && iterator) {
            while (auto* key = OSDynamicCast(OSString, iterator->getNextObject())) {
                if (strncmp(key->getCStringNoCopy(), "Cirrus", 6) == 0)
                    evidence->setObject(key, properties->getObject(key));
            }
            auto* root = IORegistryEntry::getRegistryRoot();
            if (root)
                root->setProperty("Cirrus_Failed_Service_Evidence", evidence);
        }
        OSSafeReleaseNULL(iterator);
        OSSafeReleaseNULL(evidence);
        properties->release();
    }
}
#endif

class CirrusAudioFixup;

class ServiceRegisterIO : public cirrus::core::RegisterIO {
private:
    CirrusAudioFixup* mService;
    AmplifierState& mAmplifier;
    cirrus::diagnostics::TraceSource mSource;

public:
    ServiceRegisterIO(CirrusAudioFixup* fixup, AmplifierState& amp,
                      cirrus::diagnostics::TraceSource source = cirrus::diagnostics::TRACE_PROBE)
        : mService(fixup), mAmplifier(amp), mSource(source) {}

    int32_t lastError() const override { return mService->mLastTransferReturn; }

    bool read(uint32_t address, uint32_t* value) override { return mService->readRegister(mAmplifier, address, value, mSource); }

    bool write(uint32_t address, uint32_t value) override { return mService->writeRegister(mAmplifier, address, value, mSource); }

    bool readWithTrace(uint32_t r, uint32_t* v, TraceSource source) override { return mService->readRegister(mAmplifier, r, v, source); }
    bool writeWithTrace(uint32_t r, uint32_t v, TraceSource source) override { return mService->writeRegister(mAmplifier, r, v, source); }
    bool bulkReadWithTrace(uint32_t r, uint8_t* p, size_t n, TraceSource source) override {
        return mService->bulkRead(mAmplifier, r, p, n, source);
    }
    bool bulkWriteWithTrace(uint32_t r, const uint8_t* p, size_t n, TraceSource source) override {
        return mService->bulkWrite(mAmplifier, r, p, n, source);
    }
    bool readForDiagnostic(uint32_t address, uint32_t* value) override {
        return mService->readRegister(mAmplifier, address, value, cirrus::diagnostics::TRACE_DUMP);
    }

    bool bulkReadForDiagnostic(uint32_t address, uint8_t* data, size_t length) override {
        return mService->bulkRead(mAmplifier, address, data, length, cirrus::diagnostics::TRACE_DUMP);
    }

    bool updateBits(uint32_t address, uint32_t mask, uint32_t value) override {
        return mService->updateRegisterBits(mAmplifier, address, mask, value, mSource);
    }

    bool pollBit(uint32_t address, uint32_t mask, uint32_t expected, uint32_t timeoutMs) override {
        return mService->pollRegisterBit(mAmplifier, address, mask, expected, timeoutMs, cirrus::diagnostics::TRACE_PROBE);
    }

    bool bulkRead(uint32_t address, uint8_t* data, size_t length) override {
        return mService->bulkRead(mAmplifier, address, data, length, cirrus::diagnostics::TRACE_FIRMWARE);
    }

    bool bulkWrite(uint32_t address, const uint8_t* data, size_t length) override {
        return mService->bulkWrite(mAmplifier, address, data, length, cirrus::diagnostics::TRACE_FIRMWARE);
    }
};

inline uint64_t playbackTimeMilliseconds() {
    uint64_t nanos = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nanos);
    return nanos / 1000000;
}
