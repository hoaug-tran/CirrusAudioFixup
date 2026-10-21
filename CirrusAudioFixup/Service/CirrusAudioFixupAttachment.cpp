#include "Service/CirrusAudioFixupSupport.hpp"

bool CirrusAudioFixup::registerAmplifierDrivers() { return cirrus::devices::registerDrivers(mDriverRegistry); }

cirrus::transport::I2CController* CirrusAudioFixup::createI2CController() {
    if (!mProvider)
        return nullptr;
    if (mPlatformProfile && mPlatformProfile->i2cControllerFactory) {
        auto* controller = mPlatformProfile->i2cControllerFactory(mProvider);
        mAttachment.communication = controller ? "CREATED" : "FAILED";
        if (!controller)
            recordAttachmentFailure("COMMUNICATION", "I2C_FACTORY_REJECTED", kIOReturnNotReady);
        return controller;
    }
    if (!mProvider->metaCast("VoodooI2CDeviceNub")) {
        mAttachment.communication = "UNSUPPORTED";
        recordAttachmentFailure("COMMUNICATION", "I2C_PROVIDER_ABI_UNAVAILABLE", kIOReturnUnsupported);
        return nullptr;
    }
    auto* controller = new cirrus::transport::VoodooI2CController(mProvider);
    mAttachment.communication = controller ? "CREATED" : "FAILED";
    if (!controller)
        recordAttachmentFailure("COMMUNICATION", "I2C_CONTROLLER_ALLOCATION_FAILED", kIOReturnNoMemory);
    return controller;
}

bool CirrusAudioFixup::bindAmplifierDrivers() {
    if (mAmplifierManager.bound())
        return true;
    if (bootArgEnabled("-cirrusro"))
        return false;
    if (!mPlatformAuthorized || !mPlatformProfile || !mAmplifierCount || mAmplifierCount > kMaxAmplifiers)
        return false;
    cirrus::platform::BoardIdentity actual{uint16_t(mPlatformSubsystemId >> 16), uint16_t(mPlatformSubsystemId), mPlatformProfile->acpiHid};
    if (!mNativeSPI.bound()) {
        int length = sizeof(mRuntimeControllerPath);
        if (!mProvider->getPath(mRuntimeControllerPath, &length, gIOServicePlane))
            return false;
        for (size_t i = 0; i < mAmplifierCount; ++i)
            mAmplifiers[i].endpoint = {cirrus::core::BusTransportType::I2C, mRuntimeControllerPath, mAmplifiers[i].address};
        if (!mI2CController)
            mI2CController = createI2CController();
        if (!mI2CController)
            return false;
    }
    if (mPlatformProfile->boardConfiguration)
        mRuntimeBoard = *mPlatformProfile->boardConfiguration;
    else {
        int length = sizeof(mRuntimeControllerPath);
        if (!mProvider->getPath(mRuntimeControllerPath, &length, gIOServicePlane)) {
            releaseAmplifierDrivers();
            return false;
        }
        for (size_t i = 0; i < mAmplifierCount; ++i) {
            auto& amp = mAmplifiers[i];
            amp.endpoint = {cirrus::core::BusTransportType::I2C, mRuntimeControllerPath, amp.address};
            mRuntimeBindings[i] = {amp.endpoint, amp.deviceId, uint32_t(amp.channel), 1, amp.name, mPlatformProfile->acpiHid, 2};
        }
        // The legacy allowlist authorizes only its compiled reference routing,
        // reset/power and firmware-selection plan. No new board is inferred.
        mRuntimeBoard = {actual, mRuntimeBindings, mAmplifierCount, true, true, true};
    }
    const cirrus::platform::AmplifierDriverContext* contexts[kMaxAmplifiers]{};
    IOReturn ret = kIOReturnSuccess;
    for (size_t i = 0; i < mAmplifierCount; ++i) {
        auto& amp = mAmplifiers[i];
        size_t index = 0;
        while (index < mRuntimeBoard.count && !amp.endpoint.sameDevice(mRuntimeBoard.amplifiers[index].endpoint))
            ++index;
        if (index == mRuntimeBoard.count || !amp.present) {
            ret = kIOReturnNotPermitted;
            break;
        }
        const auto& binding = mRuntimeBoard.amplifiers[index];
        const auto* definition = mDriverRegistry.find(mPlatformProfile->acpiHid, binding.siliconId, amp.endpoint.transport);
        if (!definition) {
            amp.playbackFaulted = true;
            recordDiagnosticFailure(amp, DIAG_DEVICE_ID);
            ret = kIOReturnUnsupported;
            break;
        }
        if (amp.endpoint.transport == cirrus::core::BusTransportType::I2C) {
            delete mI2CMaps[i];
            mI2CMaps[i] = nullptr;
            delete mI2CBuses[i];
            mI2CBuses[i] = nullptr;
            amp.registerIO = nullptr;
            mI2CBuses[i] = new cirrus::transport::I2CRegisterBus(mI2CController, {amp.endpoint.i2cAddress, amp.endpoint.tenBitAddress});
            if (!mI2CBuses[i]) {
                ret = kIOReturnNoMemory;
                break;
            }
            mI2CMaps[i] = new cirrus::core::RegisterMap(*mI2CBuses[i], *definition->format(amp.endpoint.transport), true);
            if (!mI2CMaps[i]) {
                ret = kIOReturnNoMemory;
                break;
            }
            amp.registerIO = mI2CMaps[i];
            amp.endpointIndex = uint8_t(i);
        }
        mHostAdapters[i].bind(this, &amp);
        mRuntimePorts[i] = new ServiceRegisterIO(this, amp);
        if (!mRuntimePorts[i]) {
            ret = kIOReturnNoMemory;
            break;
        }
        if (mPlatformProfile->firmwareBundles) {
            if (index >= mPlatformProfile->firmwareBundleCount) {
                ret = kIOReturnNotPermitted;
                break;
            }
            mRuntimeFirmware[i] = mPlatformProfile->firmwareBundles[index];
        } else
            mRuntimeFirmware[i] = {{binding.siliconId, actual.subsystemVendor, actual.subsystemDevice, binding.speakerId,
                                    binding.channelIndex, binding.tuningResource},
                                   cirrus::firmware::FirmwareFormat::Deferred};
        contexts[i] = new cirrus::platform::AmplifierDriverContext{*mRuntimePorts[i],
                                                                   amp.endpoint,
                                                                   mRuntimeBoard,
                                                                   actual,
                                                                   index,
                                                                   mRuntimeFirmware[i],
                                                                   mPlatformProfile->acpiHid,
                                                                   nullptr,
                                                                   nullptr,
                                                                   static_cast<cirrus::platform::AmplifierHost*>(&mHostAdapters[i])};
        if (!contexts[i]) {
            ret = kIOReturnNoMemory;
            break;
        }
    }
    if (ret == kIOReturnSuccess) {
        ret = mAmplifierManager.bind(mDriverRegistry, contexts, mAmplifierCount);
        const auto& failure = mAmplifierManager.bindFailure();
        setProperty("Cirrus_Runtime_Bind_Reason", failure.reason);
        setProperty("Cirrus_Runtime_Bind_Index", uint64_t(uint32_t(failure.index)), 32);
        if (ret != kIOReturnSuccess)
            recordAttachmentFailure("BINDING", failure.reason, ret, uint32_t(failure.index), "AMP_SLOT");
        if (ret != kIOReturnSuccess)
            CIRRUS_ERR("driver binding rejected: index=%lu reason=%s ret=0x%08X", failure.index, failure.reason, ret);
    } else
        setProperty("Cirrus_Runtime_Bind_Reason", "PORT_OR_PROFILE_PREPARATION_FAILED");
    for (auto* context : contexts)
        delete context;
    setProperty("Cirrus_Runtime_Bind_Return", uint64_t(uint32_t(ret)), 32);
    if (ret != kIOReturnSuccess) {
        CIRRUS_ERR("amplifier runtime bind rejected: profile=%s endpoints=%lu ret=0x%08X", mPlatformProfile->name, mAmplifierCount, ret);
        releaseAmplifierDrivers();
        return false;
    }
    setProperty("Cirrus_Runtime", "REGISTERED_DRIVERS");
    return true;
}

void CirrusAudioFixup::releaseAmplifierDrivers() {
    const bool wasBound = mAmplifierManager.bound();
    IOReturn ret = mAmplifierManager.unbind();
    publishRuntimeStatus();
    if (wasBound) {
        mAttachment.amplifier = "DRIVERS_UNBOUND";
        publishAttachmentStatus();
    }
    setProperty("Cirrus_Runtime_Release_Return", uint64_t(uint32_t(ret)), 32);
    if (ret != kIOReturnSuccess)
        CIRRUS_ERR("amplifier runtime teardown incomplete: ret=0x%08X; retained failure evidence", ret);
    for (auto& platform : mHostAdapters)
        platform.clear();
    for (auto*& port : mRuntimePorts) {
        delete port;
        port = nullptr;
    }
    for (size_t i = 0; i < kMaxAmplifiers; ++i) {
        if (mI2CMaps[i])
            mAmplifiers[i].registerIO = nullptr;
        delete mI2CMaps[i];
        mI2CMaps[i] = nullptr;
        delete mI2CBuses[i];
        mI2CBuses[i] = nullptr;
    }
    delete mI2CController;
    mI2CController = nullptr;
}

bool CirrusAudioFixup::performPlatformHardwareReset() {
    if (bootArgEnabled("-cirrusro") || bootArgStrEquals("-cirrusphase", "probe")) {
        setProperty("Cirrus_Reset_Policy", bootArgEnabled("-cirrusro") ? "SKIPPED_READ_ONLY" : "SKIPPED_PROBE_PHASE");
        return true;
    }
    if (!mPlatformAuthorized) {
        setProperty("Cirrus_Reset_Policy", "BLOCKED_NOT_AUTHORIZED");
        return false;
    }
    setProperty("Cirrus_Reset_Policy", "AUTHORIZED");
    if (mNativeSPI.bound()) {
        setProperty("Cirrus_SPI_Reset_Sequence", "BEGIN");
        const auto result = mNativeSPI.prepareHardware(cirrus::support::acpiDeviceCast(mProvider));
        setProperty("Cirrus_SPI_Reset_Sequence_Return", uint64_t(uint32_t(result)), 32);
        setProperty("Cirrus_SPI_Reset_Sequence", result == kIOReturnSuccess ? "COMPLETE" : "FAILED");
        if (result != kIOReturnSuccess) {
            recordAttachmentFailure("RESET", mNativeSPI.failure().reason, result);
            mPlatformAuthorized = false;
            setProperty("Cirrus_Platform_Authorized", kOSBooleanFalse);
        }
        return result == kIOReturnSuccess;
    }
    const cirrus::platform::ResetControllerQuirk* quirkPtr = mPlatformProfile ? mPlatformProfile->resetQuirk : nullptr;
    if (quirkPtr) {
        const cirrus::platform::ResetControllerQuirk& quirk = *quirkPtr;
        OSDictionary* dict = IOService::nameMatching(quirk.controllerName);
        if (!dict)
            return false;

        OSIterator* iter = IOService::getMatchingServices(dict);
        dict->release();
        if (!iter)
            return false;

        IOService* controller = OSDynamicCast(IOService, iter->getNextObject());
        if (!controller) {
            iter->release();
            return false;
        }

        bool verified = false;
        if (controller->open(this)) {
            IOMemoryDescriptor* bmd = controller->getDeviceMemoryWithIndex(0);
            if (bmd && bmd->getLength() >= quirk.minMemoryLength) {
                if (bmd->prepare() == kIOReturnSuccess) {
                    IOMemoryMap* map = bmd->map();
                    if (map) {
                        volatile UInt32* base = (volatile UInt32*)map->getVirtualAddress();
                        if (base) {
                            UInt32 value = base[quirk.registerIndex];
                            setProperty("Cirrus_Platform_Reset_Old", value, 32);
                            value &= ~(1U << quirk.assertClearBit);
                            value |= (1U << quirk.outputEnableBit);
                            base[quirk.registerIndex] = value;
                            UInt32 verifyLow = base[quirk.registerIndex];
                            setProperty("Cirrus_Platform_Reset_VerifyLow", verifyLow, 32);

                            IOSleep(quirk.settleMs);

                            value = base[quirk.registerIndex];
                            value |= (1U << quirk.assertClearBit);
                            value |= (1U << quirk.outputEnableBit);
                            base[quirk.registerIndex] = value;
                            UInt32 verifyHigh = base[quirk.registerIndex];
                            setProperty("Cirrus_Platform_Reset_VerifyHigh", verifyHigh, 32);

                            verified =
                                ((verifyLow & (1U << quirk.assertClearBit)) == 0) && ((verifyHigh & (1U << quirk.assertClearBit)) != 0);
                            CIRRUS_LOG("platform hardware reset quirk %s %s", quirk.controllerName, verified ? "verified" : "not verified");
                        }
                        map->release();
                    }
                    bmd->complete();
                }
            }
            controller->close(this);
        }
        iter->release();
        return verified;
    }

    CIRRUS_LOG(
        "No platform hardware reset quirk configured for profile %s; continuing with firmware reset state and silicon software reset",
        mPlatformProfile ? mPlatformProfile->name : "unknown");
    return true;
}

void CirrusAudioFixup::resolvePlatformProfile() {
    const auto* previousProfile = mPlatformProfile;
    const uint32_t previousSubsystemId = mPlatformSubsystemId;
    const char* acpiHid = nullptr;
    OSObject* ownedHid = nullptr;
    if (auto* acpi = cirrus::support::acpiDeviceCast(mProvider)) {
        if (acpi->evaluateObject("_HID", &ownedHid) == kIOReturnSuccess)
            if (auto* hid = OSDynamicCast(OSString, ownedHid))
                acpiHid = hid->getCStringNoCopy();
    }
    if (mProvider && !acpiHid) {
        OSString* ioName = OSDynamicCast(OSString, mProvider->getProperty("IOName"));
        if (ioName)
            acpiHid = ioName->getCStringNoCopy();
        if (!acpiHid)
            acpiHid = mProvider->getName();
    }

    mPlatformProfile = cirrus::platform::findPlatformProfile(acpiHid);
    if (!mPlatformProfile) {
        mPlatformProfile = &cirrus::platform::defaultPlatformProfile();
        CIRRUS_LOG("No exact platform profile for ACPI HID %s; using conservative profile %s", acpiHid ? acpiHid : "unknown",
                   mPlatformProfile->name);
    }

    uint32_t pciSubsystemId = 0;
    IOService* controller = getAudioController();
    if (auto* pci =
#if defined(KERNEL)
            controller ? static_cast<IOPCIDevice*>(controller->metaCast("IOPCIDevice")) : nullptr
#else
            OSDynamicCast(IOPCIDevice, controller)
#endif
    )
        pciSubsystemId = (uint32_t(pci->configRead16(kIOPCIConfigSubSystemVendorID)) << 16) | pci->configRead16(kIOPCIConfigSubSystemID);
    if (controller)
        controller->release();
    if (const auto* exact = cirrus::platform::findPlatformProfile(acpiHid, pciSubsystemId))
        if (exact->identitySource == cirrus::platform::BoardIdentitySource::AudioPCI)
            mPlatformProfile = exact;
    uint32_t acpiSubsystemId = 0;
    if (auto* acpi = cirrus::support::acpiDeviceCast(mProvider)) {
        OSObject* object = nullptr;
        if (acpi->evaluateObject("_SUB", &object) == kIOReturnSuccess)
            if (auto* sub = OSDynamicCast(OSString, object))
                cirrus::platform::parseACPISubsystemId(sub->getCStringNoCopy(), acpiSubsystemId);
        OSSafeReleaseNULL(object);
    }
    if (acpiSubsystemId)
        if (const auto* exact = cirrus::platform::findPlatformProfile(acpiHid, acpiSubsystemId))
            if (exact->identitySource == cirrus::platform::BoardIdentitySource::ACPI)
                mPlatformProfile = exact;
    // Discovery results belong to this provider instance, never to a global
    // model profile. Preserve them while identity and borrowers remain valid.
    if (mDiscoveredBoard && acpiHid) {
        const cirrus::platform::BoardIdentity identity{uint16_t(acpiSubsystemId >> 16), uint16_t(acpiSubsystemId), acpiHid};
        if (mDiscoveredBoard->configuration.permits(identity))
            mPlatformProfile = &mDiscoveredProfile;
    }
    mPlatformSubsystemId =
        mPlatformProfile->identitySource == cirrus::platform::BoardIdentitySource::ACPI ? acpiSubsystemId : pciSubsystemId;
    mPlatformAuthorized = cirrus::platform::permitsHardwareInitialization(*mPlatformProfile, mPlatformSubsystemId);
    setProperty("Cirrus_Platform_ACPI_SSID", uint64_t(acpiSubsystemId), 32);
    setProperty("Cirrus_Platform_Identity_Source",
                mPlatformProfile->identitySource == cirrus::platform::BoardIdentitySource::ACPI ? "ACPI_SUB" : "AUDIO_PCI");
    if (!previousProfile || previousProfile != mPlatformProfile || previousSubsystemId != mPlatformSubsystemId) {
        setProperty("Cirrus_Platform_PCI_SSID", uint64_t(uint32_t(pciSubsystemId)), 32);
        setProperty("Cirrus_Platform_Authorized", mPlatformAuthorized ? kOSBooleanTrue : kOSBooleanFalse);
        setProperty("Cirrus_Board_Authorized", mPlatformAuthorized ? kOSBooleanTrue : kOSBooleanFalse);
        setProperty("Cirrus_Platform_Profile", mPlatformProfile->name);
        setProperty("Cirrus_Platform_ACPI_HID", acpiHid ? acpiHid : "unknown");
        CIRRUS_LOG("platform profile selected: %s (hid=%s, endpoints=%lu)", mPlatformProfile->name, acpiHid ? acpiHid : "unknown",
                   mPlatformProfile->endpointCount);
    }
    OSSafeReleaseNULL(ownedHid);
}

void CirrusAudioFixup::discoverPlatformConfiguration(IOACPIPlatformDevice* acpi,
                                                     const cirrus::platform::AmplifierDriverRegistration& definition) {
    cirrus::transport::ACPIDeviceProperties properties;
    const IOReturn propertyReturn = properties.load(acpi, this);
    setProperty("Cirrus_Platform_Properties_Return", uint64_t(uint32_t(propertyReturn)), 32);
    setProperty("Cirrus_Platform_Properties_Reason", properties.reason());
    setProperty("Cirrus_Platform_Properties_Source", properties.source());
    setProperty("Cirrus_Platform_DSD_Validate_Return", uint64_t(uint32_t(properties.validationReturn())), 32);
    setProperty("Cirrus_Platform_DSD_Evaluate_Return", uint64_t(uint32_t(properties.evaluationReturn())), 32);
    setProperty("Cirrus_Platform_DSD_Adapter_Return", uint64_t(uint32_t(properties.adapterReturn())), 32);
    if (gCirrusDebug)
        cirrus::diagnostics::ACPIObjectEvidence::publish(this, "Cirrus_Platform_DSD", properties.object());
    const char* reason = "ACPI_PROPERTIES_UNAVAILABLE";
    if (propertyReturn == kIOReturnSuccess && mPlatformProfile->identitySource == cirrus::platform::BoardIdentitySource::ACPI) {
        cirrus::transport::ACPIPlatformProperties source(properties);
        cirrus::core::BusEndpoint endpoints[kMaxAmplifiers]{};
        for (size_t i = 0; i < mNativeSPI.count(); ++i)
            endpoints[i] = mNativeSPI.endpoint(i);
        struct GPIOReader final : cirrus::platform::BoardDiscoveryContext::GPIOReader {
            cirrus::transport::NativeSPIHost& host;
            IOACPIPlatformDevice* device;
            GPIOReader(cirrus::transport::NativeSPIHost& h, IOACPIPlatformDevice* d) : host(h), device(d) {}
            bool read(const char* property, size_t index, uint32_t& value) override {
                return host.readGPIO(device, property, index, value) == kIOReturnSuccess;
            }
        } gpio(mNativeSPI, acpi);
        const cirrus::platform::BoardDiscoveryContext context{
            {uint16_t(mPlatformSubsystemId >> 16), uint16_t(mPlatformSubsystemId), mPlatformProfile->acpiHid},
            endpoints,
            mNativeSPI.count(),
            source, &gpio};
        mDiscoveredBoard = definition.discoverBoard(context, reason);
        if (mDiscoveredBoard && mDiscoveredBoard->configuration.permits(context.identity)) {
            mDiscoveredProfile = *mPlatformProfile;
            mDiscoveredProfile.name = "acpi-device-properties";
            mDiscoveredProfile.boardConfiguration = &mDiscoveredBoard->configuration;
            mDiscoveredProfile.endpointCount = mDiscoveredBoard->configuration.count;
            mDiscoveredProfile.allowAutomaticInitialization = true;
            mPlatformProfile = &mDiscoveredProfile;
            mPlatformAuthorized = true;
            setProperty("Cirrus_Platform_Profile", mPlatformProfile->name);
        } else {
            delete mDiscoveredBoard;
            mDiscoveredBoard = nullptr;
        }
    }
    setProperty("Cirrus_Platform_Discovery", reason);
}

bool CirrusAudioFixup::setupCommunication(IOService* provider) {
    auto* acpi = cirrus::support::acpiDeviceCast(provider);
#if defined(KERNEL)
    if (gCirrusDebug)
        setProperty("Cirrus_ACPI_Provider_Cast", acpi ? "REGISTERED_CLASS_MATCH" : "NOT_ACPI");
    if (!acpi && provider && strcmp(provider->getMetaClass()->getClassName(), "IOACPIPlatformDevice") == 0) {
        mAttachment.communication = "TYPE_REJECTED";
        recordAttachmentFailure("PROVIDER", "ACPI_METACLASS_CAST_FAILED", kIOReturnUnsupported);
        return false;
    }
#endif
    if (!acpi) {
        setProperty("Cirrus_Bus", "I2C");
        const auto* expected =
            mPlatformProfile->boardConfiguration && mPlatformProfile->boardConfiguration->amplifiers &&
                    mPlatformProfile->boardConfiguration->count
                ? mDriverRegistry.find(mPlatformProfile->acpiHid, mPlatformProfile->boardConfiguration->amplifiers[0].siliconId,
                                       cirrus::core::BusTransportType::I2C)
                : mDriverRegistry.findModel(mPlatformProfile->acpiHid, mPlatformProfile->amplifierModel,
                                            cirrus::core::BusTransportType::I2C);
        mAttachment.amplifier = expected ? "REGISTERED_NOT_PROBED" : "UNSUPPORTED";
        if (!mI2CController)
            mI2CController = createI2CController();
        if (!mI2CController)
            return false;
        if (!expected) {
            mPlatformAuthorized = false;
            setProperty("Cirrus_Platform_Authorized", kOSBooleanFalse);
            recordAttachmentFailure("AMPLIFIER", "NO_REGISTERED_DRIVER", kIOReturnUnsupported);
        }
        if (expected && !preflightI2CIdentity(expected))
            return false;
        publishAttachmentStatus();
        return true;
    }
    OSObject* resources = nullptr;
    CIRRUS_LOG("BOOT_FLOW ACPI_CRS_BEGIN");
    const IOReturn resourceReturn = acpi->evaluateObject("_CRS", &resources);
    CIRRUS_LOG("BOOT_FLOW ACPI_CRS_END ret=0x%08X", uint32_t(resourceReturn));
    auto* resourceData = OSDynamicCast(OSData, resources);
    cirrus::transport::ACPISPIResources parsed{};
    bool hasI2C = false;
    const auto resourceKind = resourceReturn == kIOReturnSuccess && resourceData
                                  ? cirrus::transport::parseACPISPIResources(static_cast<const uint8_t*>(resourceData->getBytesNoCopy()),
                                                                             resourceData->getLength(), parsed, &hasI2C)
                                  : cirrus::transport::ACPISPIResult::Malformed;
    if ((hasI2C || resourceKind == cirrus::transport::ACPISPIResult::Success) &&
        !mDriverRegistry.prepareBoardMetadata(
            {uint16_t(mPlatformSubsystemId >> 16), uint16_t(mPlatformSubsystemId), mPlatformProfile->acpiHid}, acpi, this,
            hasI2C ? cirrus::core::BusTransportType::I2C : cirrus::core::BusTransportType::SPI)) {
        OSSafeReleaseNULL(resources);
        recordAttachmentFailure("PLATFORM", "BOARD_METADATA_REJECTED", kIOReturnUnsupported);
        return false;
    }
#if defined(KERNEL)
    if (gCirrusDebug) {
        setProperty("Cirrus_ACPI_CRS_Return", uint64_t(uint32_t(resourceReturn)), 32);
        setProperty("Cirrus_ACPI_CRS_Object_Type", resources ? resources->getMetaClass()->getClassName() : "NONE");
        setProperty("Cirrus_ACPI_CRS_Parse_Result", uint64_t(resourceKind), 32);
        setProperty("Cirrus_ACPI_CRS_Has_I2C", hasI2C ? kOSBooleanTrue : kOSBooleanFalse);
        if (resourceData && resourceData->getLength() <= 65536)
            setProperty("Cirrus_ACPI_CRS_Data", resourceData);
    }
#endif
    OSSafeReleaseNULL(resources);
    if (resourceKind == cirrus::transport::ACPISPIResult::NoSPI && hasI2C) {
        setProperty("Cirrus_Bus", "I2C");
        const auto* expected =
            mPlatformProfile->boardConfiguration && mPlatformProfile->boardConfiguration->amplifiers &&
                    mPlatformProfile->boardConfiguration->count
                ? mDriverRegistry.find(mPlatformProfile->acpiHid, mPlatformProfile->boardConfiguration->amplifiers[0].siliconId,
                                       cirrus::core::BusTransportType::I2C)
                : mDriverRegistry.findModel(mPlatformProfile->acpiHid, mPlatformProfile->amplifierModel,
                                            cirrus::core::BusTransportType::I2C);
        mAttachment.amplifier = expected ? "REGISTERED_NOT_PROBED" : "UNSUPPORTED";
        if (mPlatformProfile->i2cControllerFactory) {
            if (!mI2CController)
                mI2CController = createI2CController();
            if (!mI2CController)
                return false;
            if (!expected) {
                mPlatformAuthorized = false;
                setProperty("Cirrus_Platform_Authorized", kOSBooleanFalse);
                recordAttachmentFailure("AMPLIFIER", "NO_REGISTERED_DRIVER", kIOReturnUnsupported);
            }
            if (expected && !preflightI2CIdentity(expected))
                return false;
            publishAttachmentStatus();
            return true;
        }
        mAttachment.communication = "UNSUPPORTED";
        recordAttachmentFailure("COMMUNICATION", "NATIVE_I2C_PROVIDER_CONTRACT_UNAVAILABLE", kIOReturnUnsupported);
        return false;
    }
    if (resourceKind != cirrus::transport::ACPISPIResult::Success || hasI2C) {
        setProperty("Cirrus_Bus", "ACPI_UNRESOLVED");
        mAttachment.communication = "RESOURCE_REJECTED";
        recordAttachmentFailure("RESOURCE", hasI2C ? "MIXED_I2C_SPI_RESOURCES" : "ACPI_SERIAL_RESOURCE_REJECTED",
                                resourceReturn != kIOReturnSuccess ? resourceReturn : kIOReturnUnsupported);
        return false;
    }
    setProperty("Cirrus_Bus", "SPI");
    const cirrus::platform::AmplifierDriverRegistration* selected = nullptr;
    if (mPlatformProfile->boardConfiguration && mPlatformProfile->boardConfiguration->amplifiers &&
        mPlatformProfile->boardConfiguration->count) {
        const auto& first = mPlatformProfile->boardConfiguration->amplifiers[0];
        selected = mDriverRegistry.find(mPlatformProfile->acpiHid, first.siliconId, cirrus::core::BusTransportType::SPI);
    } else
        selected =
            mDriverRegistry.findModel(mPlatformProfile->acpiHid, mPlatformProfile->amplifierModel, cirrus::core::BusTransportType::SPI);
    if (!selected) {
        mAttachment.amplifier = "UNSUPPORTED";
        const auto factory =
            mPlatformProfile->spiControllerFactory ? mPlatformProfile->spiControllerFactory : cirrus::transport::createNativeSPIController;
        const IOReturn ret = mNativeSPI.bound() ? mNativeSPI.continueBind() : mNativeSPI.bindControllers(acpi, this, factory);
        mAttachment.communication = ret == kIOReturnSuccess ? "READY" : "FAILED";
        if (ret != kIOReturnSuccess) {
            const auto& failure = mNativeSPI.failure();
            recordAttachmentFailure("COMMUNICATION", failure.reason, ret, uint32_t(failure.index));
            return false;
        }
        mPlatformAuthorized = false;
        setProperty("Cirrus_SPI_Status", "CONTROLLERS_ONLY_AMP_UNSUPPORTED");
        setProperty("Cirrus_Platform_Authorized", kOSBooleanFalse);
        recordAttachmentFailure("AMPLIFIER", "NO_REGISTERED_DRIVER", kIOReturnUnsupported);
        return true;
    }
    mAttachment.amplifier = "REGISTERED_NOT_PROBED";
    const auto format = *selected->format(cirrus::core::BusTransportType::SPI);
    CIRRUS_LOG("BOOT_FLOW SPI_BIND_BEGIN");
    IOReturn ret = mNativeSPI.bound() ? mNativeSPI.continueBind() : mNativeSPI.bind(acpi, this, format,
                                   mPlatformProfile->spiControllerFactory ? mPlatformProfile->spiControllerFactory
                                                                          : cirrus::transport::createNativeSPIController);
    CIRRUS_LOG("BOOT_FLOW SPI_BIND_END ret=0x%08X", uint32_t(ret));
    if (mNativeSPI.powerPending()) {
        setProperty("Cirrus_SPI_Status", "WAITING_POWER");
        mAttachment.communication = "WAITING_POWER";
        publishAttachmentStatus();
        return true;
    }
    const IOReturn controllerReturn = ret;
    bool writable = false;
    if (ret == kIOReturnSuccess && selected->discoverBoard && !mPlatformProfile->boardConfiguration && !mDiscoveredBoard)
        discoverPlatformConfiguration(acpi, *selected);
    if (ret == kIOReturnSuccess && mPlatformAuthorized && mPlatformProfile->boardConfiguration) {
        cirrus::platform::BoardIdentity actual{uint16_t(mPlatformSubsystemId >> 16), uint16_t(mPlatformSubsystemId),
                                               mPlatformProfile->acpiHid};
        ret = mNativeSPI.configureAuthorized(mDriverRegistry, *mPlatformProfile->boardConfiguration, actual, mPlatformProfile->acpiHid,
                                             !bootArgEnabled("-cirrusro"));
        writable = ret == kIOReturnSuccess && !bootArgEnabled("-cirrusro");
    }
    // Board acceptance and permission to write are different facts. Read-only
    // policy must not make a resolved board look unsupported in the dump.
    setProperty("Cirrus_Board_Authorized", ret == kIOReturnSuccess && mPlatformAuthorized && mPlatformProfile->boardConfiguration
                                               ? kOSBooleanTrue : kOSBooleanFalse);
    mPlatformAuthorized = writable;
    setProperty("Cirrus_Platform_Authorized", writable ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("Cirrus_SPI_Bind_Return", uint64_t(uint32_t(ret)), 32);
    setProperty("Cirrus_SPI_Status", ret == kIOReturnSuccess ? (writable ? "BOUND_AUTHORIZED" : "BOUND_READ_ONLY") : "BIND_FAILED");
    const auto& failure = mNativeSPI.failure();
    setProperty("Cirrus_SPI_Failure_Reason", failure.reason);
    setProperty("Cirrus_SPI_Failure_Index", uint64_t(uint32_t(failure.index)), 32);
    setProperty("Cirrus_SPI_Failure_Source", failure.source);
    setProperty("Cirrus_SPI_Failure_Detail", uint64_t(uint32_t(failure.detail)), 32);
    mAttachment.communication = controllerReturn == kIOReturnSuccess ? (writable ? "READY" : "READ_ONLY") : "FAILED";
    publishAttachmentStatus();
    if (ret != kIOReturnSuccess)
        recordAttachmentFailure(controllerReturn == kIOReturnSuccess ? "POLICY" : "COMMUNICATION", failure.reason, ret,
                                uint32_t(failure.index));
    if (ret != kIOReturnSuccess)
        CIRRUS_ERR("SPI bind rejected: reason=%s index=%lu source=%s detail=0x%08X ret=0x%08X", failure.reason, failure.index,
                   failure.source, failure.detail, ret);
    else
        CIRRUS_LOG("SPI bound: endpoints=%lu board=%s access=%s", mNativeSPI.count(), mPlatformProfile->name,
                   writable ? "AUTHORIZED_WRITES" : "READ_ONLY");
    return ret == kIOReturnSuccess;
}

size_t CirrusAudioFixup::detectNativeSPIAmplifiers() {
    mAmplifierCount = 0;
    size_t identitySamples = 0;
    mProbingAmplifiers = true;
    setProperty("Cirrus_SPI_Resource_Count", uint64_t(uint32_t(mNativeSPI.count())), 32);
    for (size_t i = 0; i < mNativeSPI.count() && i < kMaxAmplifiers; ++i) {
        const auto endpoint = mNativeSPI.endpoint(i);
        char key[64]{};
        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_ChipSelect", i);
        setProperty(key, uint64_t(uint32_t(endpoint.spiChipSelect)), 32);
        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_Controller", i);
        setProperty(key, endpoint.controllerPath);
        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_ClockHz", i);
        setProperty(key, uint64_t(uint32_t(endpoint.spiClockHz)), 32);
        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_Mode", i);
        setProperty(key, uint64_t(uint32_t(endpoint.spiMode)), 32);
        auto* io = mNativeSPI.registers(i);
        uint32_t identityRegister = 0, identityMask = 0xffffffffU;
        const cirrus::platform::AmplifierDriverRegistration* definition = nullptr;
        const cirrus::platform::AmplifierBinding* configuredBinding = nullptr;
        if (mPlatformProfile->boardConfiguration) {
            const auto& board = *mPlatformProfile->boardConfiguration;
            for (size_t j = 0; j < board.count; ++j)
                if (endpoint.sameDevice(board.amplifiers[j].endpoint)) {
                    configuredBinding = &board.amplifiers[j];
                    const char* hid = configuredBinding->acpiHid ? configuredBinding->acpiHid : mPlatformProfile->acpiHid;
                    if (hid && mPlatformProfile->acpiHid && !strcmp(hid, mPlatformProfile->acpiHid))
                        definition = mDriverRegistry.find(hid, configuredBinding->siliconId, cirrus::core::BusTransportType::SPI);
                    break;
                }
            if (!definition) {
                mAttachment.amplifier = "UNSUPPORTED";
                recordAttachmentFailure("AMPLIFIER", "NO_REGISTERED_DRIVER", kIOReturnUnsupported, uint32_t(i));
                CIRRUS_ERR("SPI endpoint has no matching registered driver: endpoint=%lu controller=%s CS=%u; no probe", i,
                           endpoint.controllerPath, endpoint.spiChipSelect);
                continue;
            }
        } else
            definition =
                mDriverRegistry.findModel(mPlatformProfile->acpiHid, mPlatformProfile->amplifierModel, cirrus::core::BusTransportType::SPI);
        if (!definition)
            continue;
        identityRegister = definition->identityRegister;
        identityMask = definition->identityMask;
        uint32_t id = 0;
        const bool readable = io && io->read(identityRegister, &id);
        const uint32_t rawId = id;
        recordTrace(TRACE_PROBE, uint8_t(i), false, false, identityRegister, readable ? rawId : 0,
                    io ? io->lastError() : kIOReturnNotReady);
        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_RawDeviceID", i);
        setProperty(key, uint64_t(uint32_t(rawId)), 32);
        id &= identityMask;
        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_Probe_Return", i);
        setProperty(key, uint64_t(uint32_t(io ? io->lastError() : kIOReturnNotReady)), 32);
        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_DeviceID", i);
        setProperty(key, uint64_t(uint32_t(id)), 32);
        if (!readable) {
            mAttachment.communication = "FAILED";
            recordAttachmentFailure("COMMUNICATION", "SPI_IDENTITY_TRANSFER_FAILED", io ? io->lastError() : kIOReturnNotReady, uint32_t(i));
            CIRRUS_ERR("SPI identity read failed: endpoint=%lu controller=%s CS=%u reg=0x%08X ret=0x%08X", i, endpoint.controllerPath,
                       endpoint.spiChipSelect, identityRegister, io ? io->lastError() : kIOReturnNotReady);
            continue;
        }
        // Both manifest and legacy discovery use module-owned identity policy.
        if (id != (configuredBinding ? configuredBinding->siliconId : definition->siliconId)) {
            mAttachment.communication = "READY";
            mAttachment.amplifier = "IDENTITY_MISMATCH";
            recordAttachmentFailure("AMPLIFIER", "AMP_IDENTITY_MISMATCH", kIOReturnNotFound, uint32_t(i));
            CIRRUS_ERR("SPI silicon unsupported/mismatched: endpoint=%lu controller=%s CS=%u DEVID=0x%08X; no initialization", i,
                       endpoint.controllerPath, endpoint.spiChipSelect, id);
            if (bootArgEnabled("-cirrusdbg")) {
                const uint32_t divisors[]{1, 4, 16};
                unsigned sample = 0, errors = 0;
                for (auto divisor : divisors) {
                    const uint32_t hz = endpoint.spiClockHz / divisor;
                    for (unsigned repeat = 0; hz && repeat < 3 && identitySamples < 32 && errors < 3; ++repeat) {
                        ++identitySamples;
                        uint32_t observed = 0;
                        const auto result = mNativeSPI.readDiagnostic(i, hz, *definition->format(cirrus::core::BusTransportType::SPI),
                                                                      identityRegister, observed);
                        char detail[128]{};
                        snprintf(detail, sizeof(detail), "hz=%u reg=0x%08x value=0x%08x ret=0x%08x", hz, identityRegister, observed,
                                 uint32_t(result));
                        snprintf(key, sizeof(key), "Cirrus_SPI_%lu_Identity_Sample_%u", i, sample++);
                        setProperty(key, detail);
                        if (result != kIOReturnSuccess)
                            ++errors;
                        if (result == kIOReturnSuccess &&
                            (observed & identityMask) == (configuredBinding ? configuredBinding->siliconId : definition->siliconId)) {
                            snprintf(key, sizeof(key), "Cirrus_SPI_%lu_Diagnostic_Match_Hz", i);
                            setProperty(key, uint64_t(hz), 32);
                        }
                    }
                }
                snprintf(key, sizeof(key), "Cirrus_SPI_%lu_Identity_Samples_Status", i);
                setProperty(key, errors >= 3 ? "STOPPED_AFTER_ERRORS" : sample == 9 ? "COMPLETE" : "PARTIAL_LIMIT");
            }
            continue;
        }
        auto& amp = mAmplifiers[mAmplifierCount++];
        amp = {};
        snprintf(amp.endpointLabel, sizeof(amp.endpointLabel), "spi%u", unsigned(i));
        amp.name = amp.endpointLabel;
        // SPI chip-select is never an I2C slave address.
        amp.address = 0xff;
        amp.channel = cirrus::core::AudioChannel::MaxChannels;
        amp.registerIO = io;
        amp.endpoint = endpoint;
        amp.endpointIndex = uint8_t(i);
        amp.model = definition->model;
        amp.present = true;
        amp.deviceId = id;
        if (mPlatformProfile->boardConfiguration) {
            const auto& board = *mPlatformProfile->boardConfiguration;
            for (size_t j = 0; j < board.count; ++j) {
                const auto& binding = board.amplifiers[j];
                if (endpoint.sameDevice(binding.endpoint) && id == binding.siliconId)
                    amp.channel = static_cast<cirrus::core::AudioChannel>(binding.channelIndex);
            }
        }
        CIRRUS_LOG("SPI endpoint=%lu controller=%s CS=%u clock=%u mode=%u DEVID=0x%08X", i, endpoint.controllerPath, endpoint.spiChipSelect,
                   endpoint.spiClockHz, endpoint.spiMode, id);
    }
    mProbingAmplifiers = false;
    if (mAmplifierCount) {
        mAttachment.amplifier = mAmplifierCount == expectedAmplifierCount() ? "DETECTED" : "PARTIAL";
        mAttachment.communication =
            !strcmp(mAttachment.latest.stage, "COMMUNICATION") && mAmplifierCount != expectedAmplifierCount() ? "PARTIAL" : "READY";
    }
    publishAttachmentStatus();
    setProperty("Cirrus_Detected_Amplifiers", uint64_t(uint32_t(mAmplifierCount)), 32);
    setProperty("Cirrus_Amp_Discovery", mAmplifierCount ? (mPlatformAuthorized ? "DETECTED_AUTHORIZED" : "DETECTED_READ_ONLY") : "NONE");
    return mAmplifierCount;
}

void CirrusAudioFixup::teardownCommunication() {
    releaseAmplifierDrivers();
    for (auto& amp : mAmplifiers)
        amp.registerIO = nullptr;
    mNativeSPI.shutdown();
    removeProperty("Cirrus_Board_Metadata");
    if (mPlatformProfile == &mDiscoveredProfile)
        mPlatformProfile = &cirrus::platform::defaultPlatformProfile();
    delete mDiscoveredBoard;
    mDiscoveredBoard = nullptr;
    mDiscoveredProfile = {};
}

void CirrusAudioFixup::bringUpAmplifiers() {
    CIRRUS_LOG("starting hardware initialization flow");
    if (bootArgEnabled("-cirrusro"))
        return;
    if (!mPlatformAuthorized || !mPlatformProfile || mAmplifierCount != expectedAmplifierCount()) {
#if defined(KERNEL)
        publishBootCheckpoint(this, "BRINGUP_POLICY_BLOCKED", kIOReturnNotPermitted);
#endif
        setProperty("Cirrus_Runtime_Block_Reason", !mPlatformAuthorized ? "BOARD_NOT_AUTHORIZED" : "INCOMPLETE_TOPOLOGY");
        CIRRUS_ERR("initialization blocked: authorized=%u detected=%lu expected=%lu; no amplifier initialization",
                   unsigned(mPlatformAuthorized), mAmplifierCount, expectedAmplifierCount());
        publishDriverVerdict();
        return;
    }
    for (size_t i = 0; i < mAmplifierCount; ++i) {
        if (!mAmplifiers[i].present) {
            publishDriverVerdict();
            return;
        }
    }

    if (stopAfterDebugStage("probe")) {
        publishDriverVerdict();
        return;
    }
    if (!bindAmplifierDrivers()) {
#if defined(KERNEL)
        publishBootCheckpoint(this, "DRIVER_BIND_FAILED", kIOReturnNotReady);
#endif
        publishDriverVerdict();
        return;
    }
    if (mAmplifierManager.stage() == cirrus::core::AmplifierStage::Detached) {
#if defined(KERNEL)
        publishBootCheckpoint(this, "AMPLIFIER_LIFECYCLE_BEGIN");
#endif
        IOReturn result = mAmplifierManager.bringUp();
        setProperty("Cirrus_Runtime_BringUp_Return", uint64_t(uint32_t(result)), 32);
#if defined(KERNEL)
        const auto stage = mAmplifierManager.stage();
        const bool pending = stage == cirrus::core::AmplifierStage::Waiting || stage == cirrus::core::AmplifierStage::DebugHalted;
        publishBootCheckpoint(this, pending ? "AMPLIFIER_LIFECYCLE_DEFERRED" : "AMPLIFIER_LIFECYCLE_END",
                              pending ? kIOReturnSuccess : result);
#endif
        if (result != kIOReturnSuccess)
            CIRRUS_LOG("amplifier bring-up paused/failed: stage=%u ret=0x%08X", unsigned(mAmplifierManager.stage()), result);
    }
    for (size_t i = 0; i < mAmplifierManager.count(); ++i) {
        const auto* status = mAmplifierManager.status(i);
        if (status) {
            mAmplifiers[i].initialized =
                status->stage == cirrus::core::AmplifierStage::Ready || status->stage == cirrus::core::AmplifierStage::Playing;
            if (status->stage == cirrus::core::AmplifierStage::Faulted)
                mAmplifiers[i].playbackFaulted = true;
            if (status->firstFailure != kIOReturnSuccess)
                CIRRUS_ERR("AMP driver failure: index=%lu stage=%u first=0x%08X cleanup=0x%08X", i, unsigned(status->firstFailureStage),
                           status->firstFailure, status->cleanupResult);
        }
    }

    publishDriverVerdict();
    CIRRUS_LOG("hardware initialization flow complete");
}

bool CirrusAudioFixup::stopAfterDebugStage(const char* stage) {
    if (!bootArgStrEquals("-cirrusphase", stage))
        return false;
    mDebugPhaseHalted = true;
    setProperty("Cirrus_Debug_Phase", stage);
    return true;
}

size_t CirrusAudioFixup::detectConfiguredI2CAmplifiers() {
    // Live drivers borrow endpoint state/ports. Rediscovery must not replace
    // them; an unbound rescan releases old discovery ports before opening anew.
    if (mAmplifierManager.bound())
        return mAmplifierCount;
    releaseAmplifierDrivers();
    mAmplifierCount = 0;
    if (!mPlatformAuthorized || !mPlatformProfile || !mPlatformProfile->boardConfiguration) {
        recordAttachmentFailure("POLICY", "BOARD_NOT_AUTHORIZED", kIOReturnNotPermitted);
        return 0;
    }
    const auto& board = *mPlatformProfile->boardConfiguration;
    if (!board.amplifiers) {
        recordAttachmentFailure("RESOURCE", "INVALID_BOARD_BINDINGS", kIOReturnBadArgument);
        return 0;
    }
    const auto* expected =
        board.count ? mDriverRegistry.find(mPlatformProfile->acpiHid, board.amplifiers[0].siliconId, cirrus::core::BusTransportType::I2C)
                    : nullptr;
    mAttachment.amplifier = expected ? "REGISTERED_NOT_PROBED" : "UNSUPPORTED";
    int length = sizeof(mRuntimeControllerPath);
    if (!mProvider->getPath(mRuntimeControllerPath, &length, gIOServicePlane))
        return 0;
    const bool readOnly = bootArgEnabled("-cirrusro");
    // Retain the same communication owner from read-only identity discovery
    // through runtime binding; do not double-open a provider/controller.
    if (!mI2CController)
        mI2CController = createI2CController();
    if (!mI2CController)
        return 0;
    for (size_t i = 0; i < board.count && mAmplifierCount < kMaxAmplifiers; ++i) {
        const auto& binding = board.amplifiers[i];
        const auto& endpoint = binding.endpoint;
        if (!endpoint.valid() || endpoint.transport != cirrus::core::BusTransportType::I2C || endpoint.tenBitAddress ||
            strcmp(endpoint.controllerPath, mRuntimeControllerPath)) {
            recordAttachmentFailure("RESOURCE", "I2C_ENDPOINT_UNMATCHED", kIOReturnUnsupported, uint32_t(i));
            CIRRUS_ERR("I2C endpoint unsupported/unmatched: index=%lu controller=%s address=0x%X tenBit=%u bus=%u", i,
                       endpoint.controllerPath, unsigned(endpoint.i2cAddress), unsigned(endpoint.tenBitAddress),
                       unsigned(endpoint.transport));
            continue;
        }
        const char* hid = binding.acpiHid ? binding.acpiHid : mPlatformProfile->acpiHid;
        const auto* driver = mDriverRegistry.find(hid, binding.siliconId, cirrus::core::BusTransportType::I2C);
        if (!hid || !mPlatformProfile->acpiHid || !driver || strcmp(hid, mPlatformProfile->acpiHid)) {
            mAttachment.amplifier = "UNSUPPORTED";
            recordAttachmentFailure("AMPLIFIER", "NO_REGISTERED_DRIVER", kIOReturnUnsupported, uint32_t(i));
            CIRRUS_ERR("I2C driver unsupported: endpoint=%lu hid=%s silicon=0x%08X", i, hid, binding.siliconId);
            continue;
        }
        cirrus::transport::I2CRegisterBus bus(mI2CController, {endpoint.i2cAddress, endpoint.tenBitAddress});
        cirrus::core::RegisterMap map(bus, *driver->format(cirrus::core::BusTransportType::I2C), false);
        uint32_t id = 0;
        bool readable = map.read(driver->identityRegister, &id);
        recordTrace(TRACE_PROBE, uint8_t(i), false, false, driver->identityRegister, readable ? id : 0, map.lastError());
        char key[64]{};
        snprintf(key, sizeof(key), "Cirrus_I2C_%lu_Probe_Return", i);
        setProperty(key, uint64_t(uint32_t(map.lastError())), 32);
        snprintf(key, sizeof(key), "Cirrus_I2C_%lu_DeviceID", i);
        setProperty(key, uint64_t(uint32_t(id)), 32);
        if (!readable) {
            mAttachment.communication = map.lastError() == kIOReturnUnsupported ? "UNSUPPORTED" : "FAILED";
            recordAttachmentFailure("COMMUNICATION", "I2C_IDENTITY_TRANSFER_FAILED", map.lastError(), uint32_t(i));
        } else
            mAttachment.communication = "READY";
        if (!readable || (id & driver->identityMask) != binding.siliconId) {
            if (readable) {
                mAttachment.amplifier = "IDENTITY_MISMATCH";
                recordAttachmentFailure("AMPLIFIER", "AMP_IDENTITY_MISMATCH", kIOReturnNotFound, uint32_t(i));
            }
            CIRRUS_ERR(
                "I2C identity rejected: endpoint=%lu controller=%s address=0x%X actual=0x%08X expected=0x%08X mask=0x%08X ret=0x%08X", i,
                endpoint.controllerPath, unsigned(endpoint.i2cAddress), id, binding.siliconId, driver->identityMask, map.lastError());
            continue;
        }
        auto& amp = mAmplifiers[mAmplifierCount];
        amp = {};
        snprintf(amp.endpointLabel, sizeof(amp.endpointLabel), "i2c%u", unsigned(i));
        amp.name = amp.endpointLabel;
        amp.endpoint = endpoint;
        amp.address = uint8_t(endpoint.i2cAddress);
        amp.endpointIndex = uint8_t(mAmplifierCount);
        amp.channel = static_cast<cirrus::core::AudioChannel>(binding.channelIndex);
        if (readOnly) {
            mI2CBuses[mAmplifierCount] =
                new cirrus::transport::I2CRegisterBus(mI2CController, {endpoint.i2cAddress, endpoint.tenBitAddress});
            if (!mI2CBuses[mAmplifierCount])
                break;
            mI2CMaps[mAmplifierCount] =
                new cirrus::core::RegisterMap(*mI2CBuses[mAmplifierCount], *driver->format(cirrus::core::BusTransportType::I2C), false);
            if (!mI2CMaps[mAmplifierCount])
                break;
            amp.registerIO = mI2CMaps[mAmplifierCount];
        }
        amp.model = driver->model;
        amp.deviceId = binding.siliconId;
        amp.present = true;
        ++mAmplifierCount;
    }
    if (mAmplifierCount) {
        mAttachment.amplifier = mAmplifierCount == expectedAmplifierCount() ? "DETECTED" : "PARTIAL";
        mAttachment.communication =
            !strcmp(mAttachment.latest.stage, "COMMUNICATION") && mAmplifierCount != expectedAmplifierCount() ? "PARTIAL" : "READY";
    }
    publishAttachmentStatus();
    setProperty("Cirrus_Detected_Amplifiers", uint64_t(uint32_t(mAmplifierCount)), 32);
    setProperty("Cirrus_Amp_Discovery", mAmplifierCount ? "DETECTED_CONFIGURED_I2C" : "NONE");
    return mAmplifierCount;
}

size_t CirrusAudioFixup::detectAmplifiers() {
    if (mAmplifierManager.bound())
        return mAmplifierCount;
    if (mPlatformProfile && mPlatformProfile->boardConfiguration)
        return detectConfiguredI2CAmplifiers();
    for (size_t i = 0; i < kMaxAmplifiers; ++i) {
        delete mI2CMaps[i];
        mI2CMaps[i] = nullptr;
        delete mI2CBuses[i];
        mI2CBuses[i] = nullptr;
        mAmplifiers[i] = {};
    }
    mAmplifierCount = 0;
    const auto& profile = mPlatformProfile ? *mPlatformProfile : cirrus::platform::defaultPlatformProfile();
    const auto* definition = mDriverRegistry.findModel(profile.acpiHid, profile.amplifierModel, cirrus::core::BusTransportType::I2C);
    if (!definition) {
        mAttachment.amplifier = "UNSUPPORTED";
        recordAttachmentFailure("AMPLIFIER", "NO_REGISTERED_DRIVER", kIOReturnUnsupported);
        return 0;
    }
    mAttachment.amplifier = "REGISTERED_NOT_PROBED";
    if (!profile.endpoints || !profile.endpointCount) {
        recordAttachmentFailure("RESOURCE", "INVALID_BOARD_BINDINGS", kIOReturnBadArgument);
        return 0;
    }
    if (!mPlatformAuthorized && !bootArgEnabled("-cirruslegacyprobe")) {
        recordAttachmentFailure("POLICY", "BOARD_NOT_AUTHORIZED", kIOReturnNotPermitted);
        return 0;
    }
    int pathLength = sizeof(mRuntimeControllerPath);
    if (!mProvider->getPath(mRuntimeControllerPath, &pathLength, gIOServicePlane))
        return 0;
    if (!mI2CController)
        mI2CController = createI2CController();
    if (!mI2CController)
        return 0;
    mAmplifierCount = 0;
    mProbingAmplifiers = true;
    for (size_t i = 0; i < profile.endpointCount && mAmplifierCount < kMaxAmplifiers; ++i) {
        const auto& candidate = profile.endpoints[i];
        const size_t index = mAmplifierCount;
        auto* bus = new cirrus::transport::I2CRegisterBus(mI2CController, {candidate.address, false});
        if (!bus)
            break;
        auto* map = new cirrus::core::RegisterMap(*bus, *definition->format(cirrus::core::BusTransportType::I2C), false);
        if (!map) {
            delete bus;
            break;
        }
        uint32_t id = 0;
        const bool readable = map->read(definition->identityRegister, &id);
        recordTrace(TRACE_PROBE, uint8_t(i), false, false, definition->identityRegister, readable ? id : 0, map->lastError());
        char key[64]{};
        snprintf(key, sizeof(key), "Cirrus_I2C_%lu_Probe_Return", i);
        setProperty(key, uint64_t(uint32_t(map->lastError())), 32);
        snprintf(key, sizeof(key), "Cirrus_I2C_%lu_DeviceID", i);
        setProperty(key, uint64_t(uint32_t(id)), 32);
        if (!readable || (id & definition->identityMask) != definition->siliconId) {
            if (!readable) {
                mAttachment.communication = map->lastError() == kIOReturnUnsupported ? "UNSUPPORTED" : "FAILED";
                recordAttachmentFailure("COMMUNICATION", "I2C_IDENTITY_TRANSFER_FAILED", map->lastError(), uint32_t(i));
            } else {
                mAttachment.communication = "READY";
                mAttachment.amplifier = "IDENTITY_MISMATCH";
                recordAttachmentFailure("AMPLIFIER", "AMP_IDENTITY_MISMATCH", kIOReturnNotFound, uint32_t(i));
            }
            delete map;
            delete bus;
            continue;
        }
        auto& amp = mAmplifiers[index];
        amp = {};
        amp.name = candidate.name;
        amp.address = candidate.address;
        amp.model = definition->model;
        amp.channel = candidate.channel;
        amp.present = true;
        amp.deviceId = id;
        amp.endpoint = {cirrus::core::BusTransportType::I2C, mRuntimeControllerPath, candidate.address};
        amp.endpointIndex = uint8_t(index);
        amp.registerIO = map;
        mI2CMaps[index] = map;
        mI2CBuses[index] = bus;
        ++mAmplifierCount;
    }
    mProbingAmplifiers = false;
    setProperty("Cirrus_Discovery_Fallback", kOSBooleanFalse);
    if (mAmplifierCount) {
        mAttachment.amplifier = mAmplifierCount == expectedAmplifierCount() ? "DETECTED" : "PARTIAL";
        mAttachment.communication =
            !strcmp(mAttachment.latest.stage, "COMMUNICATION") && mAmplifierCount != expectedAmplifierCount() ? "PARTIAL" : "READY";
    }
    publishAttachmentStatus();
    setProperty("Cirrus_Detected_Amplifiers", uint64_t(uint32_t(mAmplifierCount)), 32);
    setProperty("Cirrus_Amp_Discovery", mAmplifierCount ? "DETECTED" : "NONE");
    return mAmplifierCount;
}

bool CirrusAudioFixup::bulkRead(AmplifierState& amp, UInt32 reg, UInt8* data, size_t length, TraceSource source) {
    if (!data || !length) {
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    const bool ready = communicationReady() && amp.registerIO;
    const bool ok = ready && (source == TRACE_DUMP ? amp.registerIO->bulkReadForDiagnostic(reg, data, length)
                                                   : amp.registerIO->bulkRead(reg, data, length));
    const IOReturn result = ready ? amp.registerIO->lastError() : kIOReturnNotReady;
    mLastTransferReturn = result;
    recordTrace(source, amp.endpointIndex, false, true, reg, uint32_t(length), result);
    if (!ok && !mCapturingFailureSnapshot)
        recordDiagnosticFailure(amp, amp.endpoint.transport == cirrus::core::BusTransportType::SPI ? DIAG_SPI_TRANSFER : DIAG_I2C_TRANSFER,
                                reg, uint32_t(length), 0, result);
    mLastTransferReturn = result;
    return ok;
}

bool CirrusAudioFixup::bulkWrite(AmplifierState& amp, UInt32 reg, const UInt8* data, size_t length, TraceSource source) {
    if (length && !data) {
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    const bool ready = communicationReady() && amp.registerIO;
    const bool ok = ready && amp.registerIO->bulkWrite(reg, data, length);
    const IOReturn result = ready ? amp.registerIO->lastError() : kIOReturnNotReady;
    mLastTransferReturn = result;
    recordTrace(source, amp.endpointIndex, true, true, reg, uint32_t(length), result);
    if (!ok && !mCapturingFailureSnapshot)
        recordDiagnosticFailure(amp, amp.endpoint.transport == cirrus::core::BusTransportType::SPI ? DIAG_SPI_TRANSFER : DIAG_I2C_TRANSFER,
                                reg, uint32_t(length), 0, result);
    mLastTransferReturn = result;
    return ok;
}

bool CirrusAudioFixup::readRegister(AmplifierState& amp, UInt32 reg, UInt32* value, TraceSource source) {
    if (!value) {
        mLastTransferReturn = kIOReturnBadArgument;
        return false;
    }
    const bool ready = communicationReady() && amp.registerIO;
    const bool ok = ready && (source == TRACE_DUMP ? amp.registerIO->readForDiagnostic(reg, value) : amp.registerIO->read(reg, value));
    const IOReturn result = ready ? amp.registerIO->lastError() : kIOReturnNotReady;
    mLastTransferReturn = result;
    recordTrace(source, amp.endpointIndex, false, false, reg, ok ? *value : 0, result);
    if (!ok && !mCapturingFailureSnapshot)
        recordDiagnosticFailure(amp, amp.endpoint.transport == cirrus::core::BusTransportType::SPI ? DIAG_SPI_TRANSFER : DIAG_I2C_TRANSFER,
                                reg, 4, 0, result);
    mLastTransferReturn = result;
    return ok;
}

bool CirrusAudioFixup::writeRegister(AmplifierState& amp, UInt32 reg, UInt32 value, TraceSource source) {
    const bool ready = communicationReady() && amp.registerIO;
    const bool ok = ready && amp.registerIO->write(reg, value);
    const IOReturn result = ready ? amp.registerIO->lastError() : kIOReturnNotReady;
    mLastTransferReturn = result;
    recordTrace(source, amp.endpointIndex, true, false, reg, value, result);
    if (!ok && !mCapturingFailureSnapshot)
        recordDiagnosticFailure(amp, amp.endpoint.transport == cirrus::core::BusTransportType::SPI ? DIAG_SPI_TRANSFER : DIAG_I2C_TRANSFER,
                                reg, value, 0, result);
    mLastTransferReturn = result;
    return ok;
}

bool CirrusAudioFixup::updateRegisterBits(AmplifierState& amp, UInt32 reg, UInt32 mask, UInt32 value, TraceSource source) {
    UInt32 currentVal = 0;
    if (!readRegister(amp, reg, &currentVal, source)) {
        CIRRUS_LOG("updateRegisterBits failed: read error at 0x%05X", reg);
        return false;
    }

    UInt32 newVal = (currentVal & ~mask) | (value & mask);

    if (newVal == currentVal) {
        return true;
    }

    return writeRegister(amp, reg, newVal, source);
}

bool CirrusAudioFixup::pollRegisterBit(AmplifierState& amp, UInt32 reg, UInt32 mask, UInt32 targetVal, UInt32 timeoutMs,
                                       TraceSource source) {
    UInt32 val = 0;
    for (UInt32 i = 0; i < timeoutMs; i++) {
        if (!readRegister(amp, reg, &val, source)) {
            return false;
        }
        if ((val & mask) == targetVal) {
            CIRRUS_LOG("Amp %s: poll reg=0x%05X mask=0x%X expect=0x%X elapsed=%dms iterations=%d", amp.name, reg, mask, targetVal, i + 1,
                       i + 1);
            return true;
        }
        IOSleep(1);
    }

    CIRRUS_ERR("Amp %s: pollRegisterBit timeout! reg=0x%05X, mask=0x%X, val=0x%08X", amp.name, reg, mask, val);

    return false;
}

void CirrusAudioFixup::probeAmplifier(AmplifierState& amp) {
    const auto* definition = mDriverRegistry.find(mPlatformProfile->acpiHid, amp.deviceId, amp.endpoint.transport);
    if (!definition || !definition->inspect)
        return;
    AmplifierHostAdapter host;
    host.bind(this, &amp);
    ServiceRegisterIO io(this, amp, TRACE_DUMP);
    cirrus::platform::AmplifierBinding binding{
        amp.endpoint, amp.deviceId, uint32_t(amp.channel), 1, amp.name, mPlatformProfile->acpiHid, 2};
    cirrus::platform::BoardIdentity actual{uint16_t(mPlatformSubsystemId >> 16), uint16_t(mPlatformSubsystemId), mPlatformProfile->acpiHid};
    cirrus::platform::BoardConfiguration board{actual, &binding, 1, false, false, false};
    cirrus::firmware::FirmwareBundle firmware{};
    cirrus::platform::AmplifierDriverContext context{io,      amp.endpoint, board, actual, 0, firmware, mPlatformProfile->acpiHid,
                                                     nullptr, nullptr,      &host};
    definition->inspect(context);
}

bool CirrusAudioFixup::preflightI2CIdentity(const cirrus::platform::AmplifierDriverRegistration* definition) {
    if (!mPlatformAuthorized || bootArgEnabled("-cirrusro") || !definition || !mI2CController)
        return true;
    const auto* board = mPlatformProfile->boardConfiguration;
    if ((board && (!board->amplifiers || !board->count)) ||
        (!board && (!mPlatformProfile->endpoints || !mPlatformProfile->endpointCount))) {
        mAttachment.communication = "RESOURCE_REJECTED";
        recordAttachmentFailure("RESOURCE", "INVALID_BOARD_BINDINGS", kIOReturnBadArgument);
        return false;
    }
    const uint16_t address = board && board->amplifiers && board->count ? board->amplifiers[0].endpoint.i2cAddress
                             : mPlatformProfile->endpointCount          ? mPlatformProfile->endpoints[0].address
                                                                        : 0;
    if (!address)
        return true;
    if (board && board->amplifiers && board->count) {
        char path[256]{};
        int length = sizeof(path);
        const auto& endpoint = board->amplifiers[0].endpoint;
        if (endpoint.transport != cirrus::core::BusTransportType::I2C || endpoint.tenBitAddress || !mProvider ||
            !mProvider->getPath(path, &length, gIOServicePlane) || !endpoint.controllerPath || strcmp(path, endpoint.controllerPath)) {
            mAttachment.communication = "RESOURCE_REJECTED";
            recordAttachmentFailure("RESOURCE", "I2C_ENDPOINT_UNMATCHED", kIOReturnUnsupported);
            return false;
        }
    }
    cirrus::transport::I2CRegisterBus bus(mI2CController, {address, false});
    cirrus::core::RegisterMap map(bus, *definition->format(cirrus::core::BusTransportType::I2C), false);
    uint32_t id = 0;
    const bool readable = map.read(definition->identityRegister, &id);
    const auto result = map.lastError();
    recordTrace(TRACE_PROBE, 0, false, false, definition->identityRegister, readable ? id : 0, result);
    setProperty("Cirrus_I2C_Preflight_Return", uint64_t(uint32_t(result)), 32);
    setProperty("Cirrus_I2C_Preflight_DeviceID", uint64_t(uint32_t(id)), 32);
    if (!readable && result == kIOReturnUnsupported) {
        mAttachment.communication = "UNSUPPORTED";
        recordAttachmentFailure("COMMUNICATION", "I2C_ADDRESSED_FUNCTION_UNSUPPORTED", result);
        return false;
    }
    if (readable && (id & definition->identityMask) != definition->siliconId) {
        mAttachment.communication = "READY";
        mAttachment.amplifier = "IDENTITY_MISMATCH";
        mPlatformAuthorized = false;
        setProperty("Cirrus_Platform_Authorized", kOSBooleanFalse);
        recordAttachmentFailure("AMPLIFIER", "AMP_IDENTITY_MISMATCH", kIOReturnNotFound);
    }
    return true;
}
