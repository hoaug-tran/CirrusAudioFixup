//
//  CirrusAudioFixup.cpp
//  CirrusAudioFixup
//

#include "CirrusAudioFixup.hpp"
#include <IOKit/IOMemoryDescriptor.h>

#define super IOService
OSDefineMetaClassAndStructors(CirrusAudioFixup, IOService)

static UInt32 readBE32(const UInt8 *data) {
    return (static_cast<UInt32>(data[0]) << 24) |
           (static_cast<UInt32>(data[1]) << 16) |
           (static_cast<UInt32>(data[2]) << 8)  |
            static_cast<UInt32>(data[3]);
}

static void writeBE32(UInt8 *data, UInt32 value) {
    data[0] = static_cast<UInt8>((value >> 24) & 0xFF);
    data[1] = static_cast<UInt8>((value >> 16) & 0xFF);
    data[2] = static_cast<UInt8>((value >> 8) & 0xFF);
    data[3] = static_cast<UInt8>(value & 0xFF);
}

bool CirrusAudioFixup::init(OSDictionary *properties) {
    if (!super::init(properties)) {
        CIRRUS_ERR("super::init failed");
        return false;
    }

    setProperty("CirrusReachedInit", kOSBooleanTrue);
    CIRRUS_LOG("init");
    return true;
}

IOService *CirrusAudioFixup::probe(IOService *provider, SInt32 *score) {
    IOService *result = super::probe(provider, score);

    setProperty("CirrusReachedProbe", kOSBooleanTrue);

    CIRRUS_LOG("probe provider=%s class=%s score=%d",
               provider ? provider->getName() : "null",
               provider ? provider->getMetaClass()->getClassName() : "null",
               score ? static_cast<int>(*score) : -1);

    if (score) {
        *score += 9000;
    }

    return result;
}

bool CirrusAudioFixup::start(IOService *provider) {
    setProperty("CirrusReachedStart", kOSBooleanTrue);
    CIRRUS_LOG("START CALLED OK");
    CIRRUS_LOG("start");

    if (!super::start(provider)) {
        CIRRUS_ERR("super::start failed");
        return false;
    }

    mProvider = provider;
    logProviderInfo(provider);
    dumpProviderProperties(provider);

    // AMD GPIO MMIO Toggle (Pin 6)
    // Find AMDI0030 device dynamically to avoid hardcoding 0xFED81500
    IOMemoryDescriptor *bmd = nullptr;
    OSDictionary *dict = IOService::nameMatching("AMDI0030");
    if (dict) {
        OSIterator *iter = IOService::getMatchingServices(dict);
        if (iter) {
            IOService *amdi0030 = OSDynamicCast(IOService, iter->getNextObject());
            if (amdi0030) {
                if (amdi0030->open(this)) {
                    IOMemoryDescriptor *bmd0 = amdi0030->getDeviceMemoryWithIndex(0);
                    if (bmd0 && bmd0->getLength() >= 0x400) {
                        CIRRUS_LOG("Found AMDI0030 base physical address: 0x%llX", (unsigned long long)bmd0->getPhysicalAddress());
                        bmd0->retain();
                        bmd = bmd0;
                    }
                    amdi0030->close(this);
                } else {
                    CIRRUS_ERR("Failed to open AMDI0030");
                }
            }
            iter->release();
        }
        dict->release();
    }
    if (bmd) {
        if (bmd->prepare() == kIOReturnSuccess) {
            IOMemoryMap *map = bmd->map();
            if (map) {
                volatile UInt32 *gpioBase = (volatile UInt32 *)map->getVirtualAddress();
                
                UInt32 val = gpioBase[6];
                setProperty("Cirrus_GPIO6_old", val, 32);
                CIRRUS_LOG("AMD GPIO 6 old value: 0x%08X", val);
                
                // Toggle sequence: Force LOW, sleep, force HIGH, sleep
                // Pull LOW (Reset active)
                val &= ~(1 << 22); // OUTPUT_VALUE_OFF = 0
                val |= (1 << 23);  // OUTPUT_ENABLE_OFF = 1
                gpioBase[6] = val;
                
                UInt32 verifyLow = gpioBase[6];
                setProperty("Cirrus_GPIO6_verifyLow", verifyLow, 32);
                CIRRUS_LOG("AMD GPIO 6 LOW verify = 0x%08X", verifyLow);
                
                IOSleep(5);
                
                // Pull HIGH (Reset inactive)
                val = gpioBase[6];
                val |= (1 << 22); // OUTPUT_VALUE_OFF = 1
                val |= (1 << 23); // OUTPUT_ENABLE_OFF = 1
                gpioBase[6] = val;
                
                UInt32 verifyHigh = gpioBase[6];
                setProperty("Cirrus_GPIO6_verifyHigh", verifyHigh, 32);
                CIRRUS_LOG("AMD GPIO 6 HIGH verify = 0x%08X", verifyHigh);
                
                map->release();
            }
            bmd->complete();
        }
        bmd->release();
    }
    
    IOSleep(15);

    if (!setupProbeTimer()) {
        CIRRUS_ERR("probe timer setup failed");
        return false;
    }

    bool probeEnabled = bootArgEnabled("cirrus_probe");
    setProperty("CirrusBootArgParsed", probeEnabled ? kOSBooleanTrue : kOSBooleanFalse);

    if (probeEnabled) {
        uint32_t delayMs = 100;
        PE_parse_boot_argn("cirrus_probe_delay", &delayMs, sizeof(delayMs));
        scheduleReadOnlyProbe(delayMs);
    } else {
        CIRRUS_LOG("passive mode; add boot-arg cirrus_probe=1 for read-only I2C probe");
    }

    registerService();
    return true;
}

void CirrusAudioFixup::stop(IOService *provider) {
    CIRRUS_LOG("stop");

    if (mProbeTimer && mWorkLoop) {
        mProbeTimer->cancelTimeout();
        mWorkLoop->removeEventSource(mProbeTimer);
    }

    OSSafeReleaseNULL(mProbeTimer);
    OSSafeReleaseNULL(mWorkLoop);
    mProvider = nullptr;

    super::stop(provider);
}

void CirrusAudioFixup::free() {
    CIRRUS_LOG("free");
    super::free();
}

bool CirrusAudioFixup::bootArgEnabled(const char *name) {
    UInt32 value = 0;
    if (PE_parse_boot_argn(name, &value, sizeof(value))) {
        return value != 0;
    }
    
    // Also check for the boolean flag prefix (e.g. -cirrus_probe)
    char flagBuf[64];
    snprintf(flagBuf, sizeof(flagBuf), "-%s", name);
    if (PE_parse_boot_argn(flagBuf, &value, sizeof(value))) {
        return true; // Presence of flag implies true
    }
    
    // Check using VoodooI2C's checkKernelArg style (string buffer)
    int strValue[16];
    if (PE_parse_boot_argn(name, &strValue, sizeof(strValue))) {
        // If it parses as a string "0", treat as false
        char* strPtr = reinterpret_cast<char*>(&strValue);
        if (strPtr[0] == '0' && strPtr[1] == '\0') {
            return false;
        }
        return true;
    }

    return false;
}

void CirrusAudioFixup::logProviderInfo(IOService *provider) {
    if (!provider) {
        CIRRUS_ERR("provider is null");
        return;
    }

    CIRRUS_LOG("provider name=%s class=%s",
               provider->getName(),
               provider->getMetaClass()->getClassName());

    OSObject *addrObj = provider->getProperty("i2cAddress");
    if (OSNumber *addr = OSDynamicCast(OSNumber, addrObj)) {
        CIRRUS_LOG("provider i2cAddress=0x%02X", addr->unsigned32BitValue());
    }

    OSObject *modeObj = provider->getProperty("Interrupt Mode");
    if (OSString *mode = OSDynamicCast(OSString, modeObj)) {
        CIRRUS_LOG("provider interrupt=%s", mode->getCStringNoCopy());
    }

    OSObject *pathObj = provider->getProperty("acpi-path");
    if (OSString *path = OSDynamicCast(OSString, pathObj)) {
        CIRRUS_LOG("provider acpi-path=%s", path->getCStringNoCopy());
    }
}

void CirrusAudioFixup::dumpProviderProperties(IOService *provider) {
    if (!provider) {
        return;
    }

    CIRRUS_LOG("provider properties follow");

    OSDictionary *properties = provider->getPropertyTable();
    OSCollectionIterator *iterator = OSCollectionIterator::withCollection(properties);
    if (!iterator) {
        CIRRUS_ERR("property iterator failed");
        return;
    }

    while (OSObject *key = iterator->getNextObject()) {
        OSString *keyString = OSDynamicCast(OSString, key);
        if (!keyString) {
            continue;
        }

        OSObject *value = properties->getObject(keyString);
        if (!value) {
            continue;
        }

        if (OSString *str = OSDynamicCast(OSString, value)) {
            CIRRUS_LOG("property %s=%s", keyString->getCStringNoCopy(), str->getCStringNoCopy());
        } else if (OSNumber *num = OSDynamicCast(OSNumber, value)) {
            CIRRUS_LOG("property %s=0x%llX", keyString->getCStringNoCopy(), num->unsigned64BitValue());
        } else if (OSBoolean *boo = OSDynamicCast(OSBoolean, value)) {
            CIRRUS_LOG("property %s=%s", keyString->getCStringNoCopy(), boo->getValue() ? "true" : "false");
        } else {
            CIRRUS_LOG("property %s class=%s", keyString->getCStringNoCopy(), value->getMetaClass()->getClassName());
        }
    }

    iterator->release();
}

bool CirrusAudioFixup::setupProbeTimer() {
    mWorkLoop = getWorkLoop();
    if (!mWorkLoop) {
        mWorkLoop = IOWorkLoop::workLoop();
    } else {
        mWorkLoop->retain();
    }

    if (!mWorkLoop) {
        return false;
    }

    mProbeTimer = IOTimerEventSource::timerEventSource(this, probeTimerFired);
    if (!mProbeTimer) {
        return false;
    }

    if (mWorkLoop->addEventSource(mProbeTimer) != kIOReturnSuccess) {
        OSSafeReleaseNULL(mProbeTimer);
        return false;
    }

    setProperty("CirrusTimerCreated", kOSBooleanTrue);
    return true;
}

void CirrusAudioFixup::scheduleReadOnlyProbe(UInt32 delayMs) {
    CIRRUS_LOG("read-only probe scheduled in %u ms", delayMs);
    mProbeTimer->setTimeoutMS(delayMs);
}

void CirrusAudioFixup::probeTimerFired(OSObject *owner, IOTimerEventSource *sender) {
    CirrusAudioFixup *self = OSDynamicCast(CirrusAudioFixup, owner);
    if (self) {
        self->setProperty("CirrusTimerFired", kOSBooleanTrue);
        self->runReadOnlyProbe();
    }
}

void CirrusAudioFixup::runReadOnlyProbe() {
    CIRRUS_LOG("read-only probe begin");

    // VoodooI2C owns the bus. This kext only asks for safe debug transfers.
    for (unsigned i = 0; i < 2; ++i) {
        probeAmp(mAmps[i]);
    }

    CIRRUS_LOG("read-only probe complete");
}

void CirrusAudioFixup::probeAmp(CS35L41Amp &amp) {
    UInt32 deviceId = 0;
    UInt32 revisionId = 0;

    CIRRUS_LOG("amp %s probe address=0x%02X", amp.name, amp.address);

    if (!readRegister(amp, CS35L41_DEVID_REG, &deviceId)) {
        CIRRUS_ERR("amp %s device-id read failed", amp.name);
        return;
    }

    if (!readRegister(amp, CS35L41_REVID_REG, &revisionId)) {
        CIRRUS_ERR("amp %s revision read failed", amp.name);
        return;
    }

    amp.deviceId = deviceId;
    amp.revisionId = revisionId;
    amp.present = (deviceId == CS35L41_DEVICE_ID);

    if (amp.present) {
        dumpRegisters(amp);
        testRegisterConsistency(amp);
    }

    CIRRUS_LOG("amp %s devid=0x%08X revision=0x%08X present=%s",
               amp.name, amp.deviceId, amp.revisionId, amp.present ? "yes" : "no");
}

bool CirrusAudioFixup::transferToAddress(UInt8 address,
                                         UInt8 *writeBuffer,
                                         UInt16 writeLength,
                                         UInt8 *readBuffer,
                                         UInt16 readLength) {
    if (!mProvider) {
        CIRRUS_ERR("transfer failed; provider is null");
        return false;
    }

    VoodooI2CAddressedTransfer request;
    request.address = address;
    request.writeBuffer = writeBuffer;
    request.writeLength = writeLength;
    request.readBuffer = readBuffer;
    request.readLength = readLength;

    setProperty("CirrusTransferCalled", kOSBooleanTrue);
    IOReturn ret = mProvider->callPlatformFunction(VOODOO_I2C_TRANSFER_TO_ADDRESS,
                                                   true,
                                                   &request,
                                                   nullptr,
                                                   nullptr,
                                                   nullptr);
    setProperty("CirrusTransferRet", (uint64_t)ret, 32);
    if (ret != kIOReturnSuccess) {
        CIRRUS_ERR("transfer address=0x%02X write=%u read=%u ret=0x%08X",
                   address, writeLength, readLength, ret);
        return false;
    }

    return true;
}

bool CirrusAudioFixup::bulkRead(CS35L41Amp &amp, UInt32 reg, UInt8 *data, size_t length) {
    UInt8 writeBuffer[4];
    writeBE32(writeBuffer, reg);
    return transferToAddress(amp.address, writeBuffer, sizeof(writeBuffer), data, length);
}

bool CirrusAudioFixup::bulkWrite(CS35L41Amp &amp, UInt32 reg, const UInt8 *data, size_t length) {
    UInt8 stackBuffer[8];
    UInt8 *writeBuffer = stackBuffer;
    bool useMalloc = (4 + length > sizeof(stackBuffer));
    
    if (useMalloc) {
        writeBuffer = (UInt8 *)IOMallocData(4 + length);
        if (!writeBuffer) return false;
    }
    
    writeBE32(writeBuffer, reg);
    if (length > 0 && data) {
        memcpy(writeBuffer + 4, data, length);
    }
    
    bool ret = transferToAddress(amp.address, writeBuffer, 4 + length, nullptr, 0);
    
    if (useMalloc) {
        IOFreeData(writeBuffer, 4 + length);
    }
    return ret;
}

bool CirrusAudioFixup::readRegister(CS35L41Amp &amp, UInt32 reg, UInt32 *value) {
    if (!value) return false;
    UInt8 readBuffer[4] { 0 };
    if (!bulkRead(amp, reg, readBuffer, sizeof(readBuffer))) {
        return false;
    }
    *value = readBE32(readBuffer);
    return true;
}

bool CirrusAudioFixup::writeRegister(CS35L41Amp &amp, UInt32 reg, UInt32 value) {
    UInt8 writeBuffer[4];
    writeBE32(writeBuffer, value);
    return bulkWrite(amp, reg, writeBuffer, sizeof(writeBuffer));
}

void CirrusAudioFixup::dumpRegisters(CS35L41Amp &amp) {
    struct RegDump {
        UInt32 addr;
        const char *name;
    } regs[] = {
        { CS35L41_DEVID_REG, "DEVID" },
        { CS35L41_REVID_REG, "REVID" },
        { CS35L41_FABID_REG, "FABID" },
        { CS35L41_OTPID_REG, "OTPID" },
        { CS35L41_PWR_CTRL1_REG, "PWR_CTRL1_GLOBAL_ENABLE" },
        { CS35L41_PWR_CTRL2_REG, "PWR_CTRL2" },
        { CS35L41_PWR_CTRL3_REG, "PWR_CTRL3" },
        { CS35L41_AMP_OUT_MUTE_REG, "AMP_OUT_MUTE" },
        { CS35L41_PWRMGT_STS_REG, "PWRMGT_STS" },
        { CS35L41_IRQ1_STATUS1_REG, "IRQ1_STATUS1" },
        { CS35L41_IRQ2_STATUS1_REG, "IRQ2_STATUS1" },
        { CS35L41_IRQ1_MASK1_REG, "IRQ1_MASK1" },
        { CS35L41_IRQ2_MASK1_REG, "IRQ2_MASK1" },
        { CS35L41_DSP_MBOX_1_REG, "DSP_MBOX_1" },
        { CS35L41_DSP_MBOX_2_REG, "DSP_MBOX_2" },
        { CS35L41_DSP_MBOX_3_REG, "DSP_MBOX_3" },
        { CS35L41_DSP_MBOX_4_REG, "DSP_MBOX_4" }
    };
    
    char propName[64];
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        UInt32 val = 0;
        if (readRegister(amp, regs[i].addr, &val)) {
            snprintf(propName, sizeof(propName), "Cirrus_Amp_%s_%s", amp.name, regs[i].name);
            setProperty(propName, val, 32);
        }
    }
}

void CirrusAudioFixup::testRegisterConsistency(CS35L41Amp &amp) {
    UInt32 val1 = 0, val2 = 0;
    
    CIRRUS_LOG("Amp %s: Starting Register Consistency Test", amp.name);
    
    // Phase A: GLOBAL_ENABLE
    if (readRegister(amp, CS35L41_PWR_CTRL1_REG, &val1)) {
        IOSleep(5000); // 5 seconds
        if (readRegister(amp, CS35L41_PWR_CTRL1_REG, &val2)) {
            setProperty("Cirrus_Test_GLOBAL_ENABLE_Match", (val1 == val2) ? kOSBooleanTrue : kOSBooleanFalse);
            CIRRUS_LOG("Amp %s Phase A: GLOBAL_ENABLE old=0x%08X new=0x%08X", amp.name, val1, val2);
        }
    }
    
    // Phase B: PWR_CTRL2
    if (readRegister(amp, CS35L41_PWR_CTRL2_REG, &val1)) {
        IOSleep(5000); // 5 seconds
        if (readRegister(amp, CS35L41_PWR_CTRL2_REG, &val2)) {
            setProperty("Cirrus_Test_PWR_CTRL2_Match", (val1 == val2) ? kOSBooleanTrue : kOSBooleanFalse);
            CIRRUS_LOG("Amp %s Phase B: PWR_CTRL2 old=0x%08X new=0x%08X", amp.name, val1, val2);
        }
    }
    
    // Phase C: DSP_MBOX_1
    if (readRegister(amp, CS35L41_DSP_MBOX_1_REG, &val1)) {
        IOSleep(5000); // 5 seconds
        if (readRegister(amp, CS35L41_DSP_MBOX_1_REG, &val2)) {
            setProperty("Cirrus_Test_MBOX1_Match", (val1 == val2) ? kOSBooleanTrue : kOSBooleanFalse);
            CIRRUS_LOG("Amp %s Phase C: MBOX1 old=0x%08X new=0x%08X", amp.name, val1, val2);
        }
    }
    
    // Phase D: Write Test on IRQ1_STATUS1
    UInt32 oldIrq = 0, newIrq = 0;
    if (readRegister(amp, CS35L41_IRQ1_STATUS1_REG, &oldIrq)) {
        CIRRUS_LOG("Amp %s Phase D: Write Test old_IRQ=0x%08X", amp.name, oldIrq);
        if (writeRegister(amp, CS35L41_IRQ1_STATUS1_REG, oldIrq)) {
            if (readRegister(amp, CS35L41_IRQ1_STATUS1_REG, &newIrq)) {
                setProperty("Cirrus_Test_Write_Success", kOSBooleanTrue);
                CIRRUS_LOG("Amp %s Phase D: Write Test new_IRQ=0x%08X", amp.name, newIrq);
            }
        }
    }
}
