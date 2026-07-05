//
//  CirrusAudioFixup.cpp
//  CirrusAudioFixup
//

#include "CirrusAudioFixup.hpp"
#include <IOKit/IOMemoryDescriptor.h>
#include "Codecs/CS35L41/CS35L41_FirmwareDatabase.hpp"
#include "Codecs/CS35L41/CS35L41_FirmwareParser.hpp"
#include "Codecs/CS35L41/CS35L41_FirmwareUploader.hpp"

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

    mTraceLock = IOLockAlloc();
    initTraceBuffer();

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
    if (mTraceLock) {
        IOLockFree(mTraceLock);
        mTraceLock = nullptr;
    }
    super::free();
}

bool CirrusAudioFixup::bootArgEnabled(const char *name) {
    UInt32 value = 0;
    if (PE_parse_boot_argn(name, &value, sizeof(value))) {
        return value != 0;
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

bool CirrusAudioFixup::bootArgStrEquals(const char *name, const char *expectedVal) {
    char val[64];
    if (PE_parse_boot_argn(name, val, sizeof(val))) {
        return strncmp(val, expectedVal, sizeof(val)) == 0;
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

void CirrusAudioFixup::phase5d_FirmwareInit(CS35L41Amp &amp, const char* phaseArg) {
    CIRRUS_LOG("Entering Phase 5D: Full Initialization for amp %s", amp.name);
    
    if (!amp.wmfwData || amp.wmfwSize == 0) {
        CIRRUS_LOG("Amp %s: No WMFW data found. Phase 5D skipped.", amp.name);
        return;
    }
    
    FirmwareImage *image = (FirmwareImage *)IOMalloc(sizeof(FirmwareImage));
    if (!image) {
        CIRRUS_ERR("Amp %s: Failed to allocate memory for FirmwareImage", amp.name);
        return;
    }

    // Step 1: WMFW Parse
    if (!CirrusFirmwareParser::parseWMFW(amp.wmfwData, amp.wmfwSize, image)) {
        CIRRUS_ERR("Amp %s: WMFW parse failed", amp.name);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    
    CIRRUS_LOG("Amp %s: Step 1 (WMFW Parse) OK. FW ID: 0x%06X, Expected Algs: %u", amp.name, image->fw_id, image->n_algs);

    // Step 2: WMFW Upload
    MappedImage *wmfwMapped = (MappedImage *)IOMalloc(sizeof(MappedImage));
    if (wmfwMapped) {
        if (CirrusFirmwareMapper::mapFirmwareImage(*image, *wmfwMapped)) {
            CIRRUS_LOG("Amp %s: Step 2 (WMFW Upload) Starting...", amp.name);
            UploadSession session;
            CirrusFirmwareScheduler::run(amp, this, *wmfwMapped, session);
        } else {
            CIRRUS_ERR("Amp %s: Step 2 WMFW Mapping Failed!", amp.name);
        }
        IOFree(wmfwMapped, sizeof(MappedImage));
    }
    
    // Step 3: DSP Bringup
    CIRRUS_LOG("Amp %s: Step 3 (DSP Bringup) Starting...", amp.name);
    phase5b_DSPBringup(amp);
    
    // Step 4: Verify DSP Alive
    if (phase5b_1_VerifyDSPAlive(amp)) {
        CIRRUS_LOG("Amp %s: Step 4 (Verify DSP Alive) OK.", amp.name);
        
        // Step 5: XM Dump & Algorithm Parse
        phase5c_1_DumpXMAndParseAlgorithms(amp, *image);
        
        if (image->algorithmCount > 0) {
            // Step 6: BIN Parse
            if (amp.binData && amp.binSize > 0) {
                if (CirrusFirmwareParser::parseBIN(amp.binData, amp.binSize, image)) {
                    CIRRUS_LOG("Amp %s: Step 6 (BIN Parse) OK. Found %u Coeffs.", amp.name, image->coefficientCount);
                    
                    uint32_t matchedBlocks = 0;
                    uint32_t unknownBlocks = 0;
                    for (uint32_t j = 0; j < image->coefficientCount; j++) {
                        bool found = false;
                        for (uint32_t i = 0; i < image->algorithmCount; i++) {
                            if (image->coefficients[j].id == image->algorithms[i].id) {
                                found = true;
                                break;
                            }
                        }
                        // Treat global coeff matching FW ID as matched
                        if (found || image->coefficients[j].id == image->fw_id) {
                            matchedBlocks++;
                        } else {
                            unknownBlocks++;
                        }
                    }
                    
                    CIRRUS_LOG("==== Parser Confidence ====");
                    CIRRUS_LOG("BIN blocks       : %u", image->coefficientCount);
                    CIRRUS_LOG("Matched          : %u", matchedBlocks);
                    CIRRUS_LOG("Unknown          : %u", unknownBlocks);
                    
                    // Step 7: Coefficient Mapper
                    MappedImage *coeffMapped = (MappedImage *)IOMalloc(sizeof(MappedImage));
                    if (coeffMapped) {
                        if (CirrusFirmwareMapper::mapCoefficients(*image, *coeffMapped)) {
                            // Step 8: Coefficient Upload
                            CIRRUS_LOG("Amp %s: Step 8 (Coefficient Upload) Starting...", amp.name);
                            UploadSession session;
                            CirrusFirmwareScheduler::run(amp, this, *coeffMapped, session);
                        } else {
                            CIRRUS_ERR("Amp %s: Step 7 Coefficient Mapping Failed!", amp.name);
                        }
                        IOFree(coeffMapped, sizeof(MappedImage));
                    }
                } else {
                    CIRRUS_ERR("Amp %s: Step 6 BIN parse failed", amp.name);
                }
            } else {
                CIRRUS_LOG("Amp %s: No BIN data, skipping coefficient upload", amp.name);
            }
            
            // Step 9: Mailbox Resume (DSP to RUN state)
            CIRRUS_LOG("Amp %s: Step 9 (Mailbox Resume) Starting...", amp.name);
            writeRegister(amp, CS35L41_DSP1_CCM_CORE_CTRL, HALO_CORE_EN, TRACE_DUMP);
            
            // Note: Since SP_ENABLES is 0 in phase4A2, no audio flows yet. CoreAudio will trigger SP_ENABLES later.
        } else {
            CIRRUS_ERR("Amp %s: Failed to extract Algorithm Table, aborting tuning upload.", amp.name);
        }
    } else {
        CIRRUS_ERR("Amp %s: Step 4 Verify DSP Alive FAILED! Aborting dump & tuning upload.", amp.name);
    }

    IOFree(image, sizeof(FirmwareImage));
    CIRRUS_LOG("Phase 5D Complete for amp %s", amp.name);
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

    for (unsigned i = 0; i < 2; ++i) {
        probeAmp(mAmps[i]);
    }
    
    publishStatistics();
    
    if (bootArgEnabled("cirrus_dump_trace")) {
        dumpTraceBuffer();
    }

    CIRRUS_LOG("read-only probe complete");
}

void CirrusAudioFixup::probeAmp(CS35L41Amp &amp) {
    UInt32 deviceId = 0;
    UInt32 revisionId = 0;

    CIRRUS_LOG("amp %s probe address=0x%02X", amp.name, amp.address);

    if (!readRegister(amp, CS35L41_DEVID_REG, &deviceId, TRACE_PROBE)) {
        CIRRUS_ERR("amp %s device-id read failed", amp.name);
        return;
    }

    if (!readRegister(amp, CS35L41_REVID_REG, &revisionId, TRACE_PROBE)) {
        CIRRUS_ERR("amp %s revision read failed", amp.name);
        return;
    }

    amp.deviceId = deviceId;
    amp.revisionId = revisionId;
    amp.present = (deviceId == CS35L41_DEVICE_ID);

    if (amp.present) {
        dumpAllRegisters(amp);
        
        if (bootArgStrEquals("cirrus_phase", "4A1")) {
            cs35l41_init_mac(amp);
        } else if (bootArgStrEquals("cirrus_phase", "4A2A") || 
                   bootArgStrEquals("cirrus_phase", "4A2B") ||
                   bootArgStrEquals("cirrus_phase", "4A2C")) {
            if (cs35l41_init_mac(amp)) {
                cs35l41_apply_phase4A2(amp);
            }
        } else if (bootArgStrEquals("cirrus_phase", "4B")) {
            if (cs35l41_init_mac(amp)) {
                if (cs35l41_apply_phase4A2(amp)) {
                    applyPLL(amp);
                    applyASP(amp);
                    applyGPIO(amp);
                    amp.final_crc = calculateRegistersCRC32(amp);
                    dumpAllRegisters(amp); // Phase 4B.4A Final Dump
                }
            }
        } else if (bootArgStrEquals("cirrus_phase", "5A") || 
                   bootArgStrEquals("cirrus_phase", "5B") ||
                   bootArgStrEquals("cirrus_phase", "5C.0") ||
                   bootArgStrEquals("cirrus_phase", "5C.1") ||
                   bootArgStrEquals("cirrus_phase", "5C.2") ||
                   bootArgStrEquals("cirrus_phase", "5C.3") ||
                   bootArgStrEquals("cirrus_phase", "5C.3.5") ||
                   bootArgStrEquals("cirrus_phase", "5C.4") ||
                   bootArgStrEquals("cirrus_phase", "5C") ||
                   bootArgStrEquals("cirrus_phase", "5D.0")) {
            if (cs35l41_init_mac(amp)) {
                if (cs35l41_apply_phase4A2(amp)) {
                    applyPLL(amp);
                    applyASP(amp);
                    applyGPIO(amp);
                    amp.final_crc = calculateRegistersCRC32(amp);
                    phase5a_FirmwareDiscovery(amp);
                    if (bootArgStrEquals("cirrus_phase", "5B")) {
                        phase5b_DSPBringup(amp);
                    } else {
                        char phaseArg[16] = {0};
                        if (PE_parse_boot_argn("cirrus_phase", phaseArg, sizeof(phaseArg))) {
                            if (strncmp(phaseArg, "5D", 2) == 0) {
                                phase5d_FirmwareInit(amp, phaseArg);
                            } else if (strncmp(phaseArg, "5C", 2) == 0) {
                                phase5c_FirmwareUpload(amp, phaseArg);
                                
                                // Only boot DSP after 5C.4 (full WMFW upload).
                                bool shouldBootDsp = (strncmp(phaseArg, "5C.4", 4) == 0);
                                if (shouldBootDsp) {
                                    CIRRUS_LOG("Amp %s: Bringing up DSP after 5C.4 Firmware Upload", amp.name);
                                    phase5b_DSPBringup(amp);
                                }
                            }
                        }
                    }
                }
            }
        }
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

void CirrusAudioFixup::initTraceBuffer() {
    if (mTraceLock) {
        IOLockLock(mTraceLock);
        mTraceHead = 0;
        mTraceTail = 0;
        memset(&mTraceStats, 0, sizeof(mTraceStats));
        IOLockUnlock(mTraceLock);
    }
}

void CirrusAudioFixup::recordTrace(TraceSource source, uint8_t ampIndex, bool isWrite, bool isBulk, uint32_t reg, uint32_t valOrLen, IOReturn ret) {
    if (!mTraceLock) return;
    
    uint64_t time = 0;
    clock_get_uptime(&time);
    uint64_t timeMs = 0;
    absolutetime_to_nanoseconds(time, &timeMs);
    timeMs /= 1000000; // ms

    IOLockLock(mTraceLock);
    
    // Update stats
    if (ret == kIOReturnOffline) {
        mTraceStats.noackCount++;
    } else if (ret == kIOReturnTimeout) {
        mTraceStats.retries++; // reuse retries for timeout
    }
    
    if (isBulk) {
        if (ret == kIOReturnSuccess) mTraceStats.bulkSuccess++;
        else mTraceStats.bulkFail++;
    } else {
        if (isWrite) {
            if (ret == kIOReturnSuccess) mTraceStats.writeSuccess++;
            else mTraceStats.writeFail++;
        } else {
            if (ret == kIOReturnSuccess) mTraceStats.readSuccess++;
            else mTraceStats.readFail++;
        }
    }
    
    // Add to ring buffer
    mTraceBuffer[mTraceTail].timestamp = timeMs;
    mTraceBuffer[mTraceTail].amp = ampIndex;
    mTraceBuffer[mTraceTail].isWrite = isWrite;
    mTraceBuffer[mTraceTail].isBulk = isBulk;
    mTraceBuffer[mTraceTail].reg = reg;
    mTraceBuffer[mTraceTail].value = valOrLen;
    mTraceBuffer[mTraceTail].ret = ret;
    mTraceBuffer[mTraceTail].source = source;
    
    mTraceTail = (mTraceTail + 1) % kTraceBufferSize;
    if (mTraceTail == mTraceHead) {
        // Overwrite oldest
        mTraceHead = (mTraceHead + 1) % kTraceBufferSize;
    }
    
    IOLockUnlock(mTraceLock);
}

void CirrusAudioFixup::publishStatistics() {
    if (!mTraceLock) return;
    IOLockLock(mTraceLock);
    setProperty("Cirrus_Read_Success", mTraceStats.readSuccess, 32);
    setProperty("Cirrus_Read_Fail", mTraceStats.readFail, 32);
    setProperty("Cirrus_Write_Success", mTraceStats.writeSuccess, 32);
    setProperty("Cirrus_Write_Fail", mTraceStats.writeFail, 32);
    setProperty("Cirrus_Bulk_Success", mTraceStats.bulkSuccess, 32);
    setProperty("Cirrus_Bulk_Fail", mTraceStats.bulkFail, 32);
    setProperty("Cirrus_NOACK_Count", mTraceStats.noackCount, 32);
    IOLockUnlock(mTraceLock);
}

void CirrusAudioFixup::dumpTraceBuffer() {
    if (!mTraceLock) return;
    IOLockLock(mTraceLock);
    
    CIRRUS_LOG("--- TRACE BUFFER DUMP START ---");
    
    size_t bufferSize = 128 * 1024;
    char *dumpBuffer = (char *)IOMallocData(bufferSize);
    if (!dumpBuffer) {
        IOLockUnlock(mTraceLock);
        return;
    }
    dumpBuffer[0] = '\0';
    size_t currentLen = 0;
    char lineBuffer[128];
    
    uint32_t curr = mTraceHead;
    while (curr != mTraceTail) {
        const TraceEntry &e = mTraceBuffer[curr];
        const char *srcStr = "OTHER";
        switch (e.source) {
            case TRACE_PROBE: srcStr = "Probe"; break;
            case TRACE_DUMP: srcStr = "Dump"; break;
            case TRACE_CONSISTENCY: srcStr = "Consist"; break;
            case TRACE_FIRMWARE: srcStr = "Firmware"; break;
            case TRACE_PLAYBACK: srcStr = "Playback"; break;
            default: break;
        }
        
        const char *ampStr = (e.amp == 0) ? "LEFT" : "RIGHT";
        
        if (e.isBulk) {
            snprintf(lineBuffer, sizeof(lineBuffer), "[%llu ms][%s][%s] BULK %s 0x%05X len=%u ret=0x%X\n",
                       e.timestamp, srcStr, ampStr, e.isWrite ? "WRITE" : "READ",
                       e.reg, e.value, e.ret);
        } else {
            snprintf(lineBuffer, sizeof(lineBuffer), "[%llu ms][%s][%s] %s 0x%05X %s 0x%08X ret=0x%X\n",
                       e.timestamp, srcStr, ampStr, e.isWrite ? "WRITE" : "READ",
                       e.reg, e.isWrite ? "<-" : "->", e.value, e.ret);
        }
        
        size_t lineLen = strlen(lineBuffer);
        if (currentLen + lineLen < bufferSize - 1) {
            strlcat(dumpBuffer, lineBuffer, bufferSize);
            currentLen += lineLen;
        }
        
        curr = (curr + 1) % kTraceBufferSize;
    }
    CIRRUS_LOG("--- TRACE BUFFER DUMP END ---");
    
    OSString *strObj = OSString::withCString(dumpBuffer);
    if (strObj) {
        setProperty("Cirrus_Trace_Dump", strObj);
        strObj->release();
    }
    IOFreeData(dumpBuffer, bufferSize);
    
    IOLockUnlock(mTraceLock);
}

bool CirrusAudioFixup::bulkRead(CS35L41Amp &amp, UInt32 reg, UInt8 *data, size_t length, TraceSource source) {
    UInt8 writeBuffer[4];
    writeBE32(writeBuffer, reg);
    bool success = transferToAddress(amp.address, writeBuffer, sizeof(writeBuffer), data, (UInt16)length);
    
    OSObject *transferRet = getProperty("CirrusTransferRet");
    IOReturn retCode = transferRet ? ((OSNumber*)transferRet)->unsigned32BitValue() : (success ? kIOReturnSuccess : kIOReturnError);
    uint8_t ampIdx = (amp.address == CS35L41_I2C_ADDR_RIGHT) ? 1 : 0;
    recordTrace(source, ampIdx, false, true, reg, (UInt32)length, retCode);
    
    return success;
}

bool CirrusAudioFixup::bulkWrite(CS35L41Amp &amp, UInt32 reg, const UInt8 *data, size_t length, TraceSource source) {
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
    
    OSObject *transferRet = getProperty("CirrusTransferRet");
    IOReturn retCode = transferRet ? ((OSNumber*)transferRet)->unsigned32BitValue() : (ret ? kIOReturnSuccess : kIOReturnError);
    uint8_t ampIdx = (amp.address == CS35L41_I2C_ADDR_RIGHT) ? 1 : 0;
    recordTrace(source, ampIdx, true, true, reg, (uint32_t)length, retCode);
    
    if (useMalloc) {
        IOFreeData(writeBuffer, 4 + length);
    }
    return ret;
}

bool CirrusAudioFixup::readRegister(CS35L41Amp &amp, UInt32 reg, UInt32 *value, TraceSource source) {
    if (!value) return false;
    UInt8 readBuffer[4] { 0 };
    
    UInt8 writeBuffer[4];
    writeBE32(writeBuffer, reg);
    bool success = transferToAddress(amp.address, writeBuffer, sizeof(writeBuffer), readBuffer, sizeof(readBuffer));
    
    OSObject *transferRet = getProperty("CirrusTransferRet");
    IOReturn retCode = transferRet ? ((OSNumber*)transferRet)->unsigned32BitValue() : (success ? kIOReturnSuccess : kIOReturnError);
    uint8_t ampIdx = (amp.address == CS35L41_I2C_ADDR_RIGHT) ? 1 : 0;
    
    if (success) {
        *value = readBE32(readBuffer);
        recordTrace(source, ampIdx, false, false, reg, *value, retCode);
        return true;
    } else {
        recordTrace(source, ampIdx, false, false, reg, 0, retCode);
        return false;
    }
}

bool CirrusAudioFixup::writeRegister(CS35L41Amp &amp, UInt32 reg, UInt32 value, TraceSource source) {
    UInt8 writeBuffer[4];
    writeBE32(writeBuffer, value);
    bool success = bulkWrite(amp, reg, writeBuffer, sizeof(writeBuffer), source);
    // bulkWrite already logs
    return success;
}

bool CirrusAudioFixup::updateRegisterBits(CS35L41Amp &amp, UInt32 reg, UInt32 mask, UInt32 value, TraceSource source) {
    UInt32 currentVal = 0;
    if (!readRegister(amp, reg, &currentVal, source)) {
        CIRRUS_LOG("updateRegisterBits failed: read error at 0x%05X", reg);
        return false;
    }
    
    UInt32 newVal = (currentVal & ~mask) | (value & mask);
    
    // Only write if there's a change
    if (newVal == currentVal) {
        return true;
    }
    
    return writeRegister(amp, reg, newVal, source);
}

bool CirrusAudioFixup::pollRegisterBit(CS35L41Amp &amp, UInt32 reg, UInt32 mask, UInt32 targetVal, UInt32 timeoutMs, TraceSource source) {
    UInt32 val = 0;
    for (UInt32 i = 0; i < timeoutMs; i++) {
        if (!readRegister(amp, reg, &val, source)) {
            return false;
        }
        if ((val & mask) == targetVal) {
            CIRRUS_LOG("Amp %s: poll reg=0x%05X mask=0x%X expect=0x%X elapsed=%dms iterations=%d",
                       amp.name, reg, mask, targetVal, i + 1, i + 1);
            return true;
        }
        IODelay(1000); // 1 ms delay
    }
    
    CIRRUS_ERR("Amp %s: pollRegisterBit timeout! reg=0x%05X, mask=0x%X, val=0x%08X", amp.name, reg, mask, val);
    
    // Dump diagnostic registers on timeout
    UInt32 st1=0, st2=0, st3=0, st4=0, pwr_ctrl2=0;
    readRegister(amp, 0x10010, &st1, source);
    readRegister(amp, 0x10014, &st2, source);
    readRegister(amp, 0x10018, &st3, source);
    readRegister(amp, 0x1001C, &st4, source);
    readRegister(amp, 0x02018, &pwr_ctrl2, source);
    CIRRUS_ERR("Amp %s: DIAGNOSTICS -> ST1=0x%08X ST2=0x%08X ST3=0x%08X ST4=0x%08X PWR_CTRL2=0x%08X",
               amp.name, st1, st2, st3, st4, pwr_ctrl2);
    
    return false;
}

bool CirrusAudioFixup::cs35l41_init_mac(CS35L41Amp &amp) {
    // Before Reset: Log REVID and Calculate CRC
    UInt32 devid_before = 0, revid_before = 0;
    readRegister(amp, 0x00000, &devid_before);
    readRegister(amp, 0x00004, &revid_before);
    UInt32 crc_before = calculateRegistersCRC32(amp);
    
    CIRRUS_LOG("Amp %s: Before Reset -> DEVID=0x%08X REVID=0x%08X CRC=0x%08X", 
               amp.name, devid_before, revid_before, crc_before);
    CIRRUS_LOG("Amp %s: Starting Soft Reset...", amp.name);
    
    // 1. Soft Reset
    if (!writeRegister(amp, CS35L41_SW_RESET, CS35L41_SW_RESET_VAL)) {
        CIRRUS_ERR("Amp %s: Failed to send SW_RESET", amp.name);
        return false;
    }
    
    // Wait for DSP to reboot. Linux uses usleep_range(2000, 2100).
    // IODelay provides microsecond precision in IOKit.
    IODelay(3000);
    
    // CRITICAL FIX: The Soft Reset above clears all registers, including IRQ masks!
    // We MUST reapply the IRQ masks immediately to prevent interrupt storms
    // which freeze macOS WindowServer at the login screen.
    writeRegister(amp, CS35L41_IRQ1_MASK1, 0xFFFFFFFF, TRACE_PROBE);
    writeRegister(amp, CS35L41_IRQ1_MASK2, 0xFFFFFFFF, TRACE_PROBE);
    writeRegister(amp, CS35L41_IRQ1_MASK3, 0xFFFFFFFF, TRACE_PROBE);
    writeRegister(amp, CS35L41_IRQ1_MASK4, 0xFFFFFFFF, TRACE_PROBE);
    writeRegister(amp, CS35L41_IRQ2_MASK1, 0xFFFFFFFF, TRACE_PROBE);
    writeRegister(amp, CS35L41_IRQ2_MASK2, 0xFFFFFFFF, TRACE_PROBE);
    writeRegister(amp, CS35L41_IRQ2_MASK3, 0xFFFFFFFF, TRACE_PROBE);
    writeRegister(amp, CS35L41_IRQ2_MASK4, 0xFFFFFFFF, TRACE_PROBE);
    
    // 2. Poll OTP_BOOT_DONE
    if (!pollRegisterBit(amp, CS35L41_IRQ1_STATUS4, CS35L41_OTP_BOOT_DONE, CS35L41_OTP_BOOT_DONE, 100)) {
        CIRRUS_ERR("Amp %s: OTP_BOOT_DONE polling failed! Aborting phase 4.", amp.name);
        return false;
    }
    
    // 3. Verify Revision After Reset
    UInt32 devid_after = 0, revid_after = 0;
    readRegister(amp, 0x00000, &devid_after);
    readRegister(amp, 0x00004, &revid_after);
    UInt32 crc_after = calculateRegistersCRC32(amp);
    
    char propName[64];
    snprintf(propName, sizeof(propName), "Cirrus_CRC_Before_%s", amp.name);
    setProperty(propName, crc_before, 32);
    snprintf(propName, sizeof(propName), "Cirrus_CRC_After_%s", amp.name);
    setProperty(propName, crc_after, 32);
    
    CIRRUS_LOG("Amp %s: After Reset -> DEVID=0x%08X REVID=0x%08X CRC=0x%08X", 
               amp.name, devid_after, revid_after, crc_after);
               
    if (crc_before != crc_after) {
        CIRRUS_LOG("Amp %s: Hardware state changed successfully! CRC diff: 0x%08X -> 0x%08X", 
                   amp.name, crc_before, crc_after);
    } else {
        CIRRUS_LOG("Amp %s: WARNING - CRC did not change after Soft Reset!", amp.name);
    }
    
    UInt32 rev_only = revid_after & 0xFF; // Only keep the lower byte
    
    switch (rev_only) {
        case 0xB0:
        case 0xB1:
        case 0xB2:
            CIRRUS_LOG("Amp %s: Revision 0x%02X confirmed.", amp.name, rev_only);
            break;
        default:
            CIRRUS_LOG("Amp %s: Unknown Revision 0x%02X.", amp.name, rev_only);
            break;
    }
    
    return true;
}

static uint32_t crc32_le(uint32_t crc, uint8_t const *buf, size_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *buf++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return ~crc;
}

uint32_t CirrusAudioFixup::calculateRegistersCRC32(CS35L41Amp &amp) {
    uint32_t crc = 0;
    size_t numRegs = sizeof(cs35l41_reg_desc) / sizeof(cs35l41_reg_desc[0]);
    for (size_t i = 0; i < numRegs; i++) {
        if (!cs35l41_reg_desc[i].readable) continue;
        uint32_t val = 0;
        if (readRegister(amp, cs35l41_reg_desc[i].addr, &val, TRACE_DUMP)) {
            crc = crc32_le(crc, (const uint8_t*)&cs35l41_reg_desc[i].addr, 4);
            crc = crc32_le(crc, (const uint8_t*)&val, 4);
        }
    }
    return crc;
}

void CirrusAudioFixup::dumpAllRegisters(CS35L41Amp &amp) {
    bool compact = bootArgEnabled("cirrus_dump_compact");
    CIRRUS_LOG("--- Full Register Dump Amp %s ---", amp.name);
    
    size_t numRegs = sizeof(cs35l41_reg_desc) / sizeof(cs35l41_reg_desc[0]);
    uint32_t successCount = 0;
    uint32_t crc = 0;
    
    // Allocate 16KB for dump string
    size_t bufferSize = 16 * 1024;
    char *dumpBuffer = (char *)IOMallocData(bufferSize);
    if (!dumpBuffer) return;
    dumpBuffer[0] = '\0';
    size_t currentLen = 0;
    
    char lineBuffer[128];
    
    for (size_t i = 0; i < numRegs; i++) {
        if (!cs35l41_reg_desc[i].readable) continue;
        
        uint32_t val = 0;
        if (readRegister(amp, cs35l41_reg_desc[i].addr, &val, TRACE_DUMP)) {
            successCount++;
            crc = crc32_le(crc, (const uint8_t*)&cs35l41_reg_desc[i].addr, 4);
            crc = crc32_le(crc, (const uint8_t*)&val, 4);
            
            if (compact) {
                snprintf(lineBuffer, sizeof(lineBuffer), "%07X: %08X\n", cs35l41_reg_desc[i].addr, val);
            } else {
                snprintf(lineBuffer, sizeof(lineBuffer), "%07X  %-35s = %08X\n", cs35l41_reg_desc[i].addr, cs35l41_reg_desc[i].name, val);
            }
            
            size_t lineLen = strlen(lineBuffer);
            if (currentLen + lineLen < bufferSize - 1) {
                strlcat(dumpBuffer, lineBuffer, bufferSize);
                currentLen += lineLen;
            }
        }
    }
    CIRRUS_LOG("--- Dump End: %u registers, CRC32: 0x%08X ---", successCount, crc);
    
    snprintf(lineBuffer, sizeof(lineBuffer), "--- CRC32: 0x%08X ---\n", crc);
    strlcat(dumpBuffer, lineBuffer, bufferSize);
    
    char propName[64];
    snprintf(propName, sizeof(propName), "Cirrus_Dump_%s", amp.name);
    
    OSString *strObj = OSString::withCString(dumpBuffer);
    if (strObj) {
        setProperty(propName, strObj);
        strObj->release();
    }
    IOFreeData(dumpBuffer, bufferSize);
}

void CirrusAudioFixup::runTimeBasedFSMCheck(CS35L41Amp &amp) {
    CIRRUS_LOG("Amp %s: Starting Time-based FSM Check", amp.name);
    
    uint32_t crcT0 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("Amp %s T0 CRC: 0x%08X", amp.name, crcT0);
    
    IOSleep(1000); // T+1
    uint32_t crcT1 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("Amp %s T+1s CRC: 0x%08X", amp.name, crcT1);
    
    IOSleep(4000); // T+5
    uint32_t crcT5 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("Amp %s T+5s CRC: 0x%08X", amp.name, crcT5);
    
    IOSleep(25000); // T+30
    uint32_t crcT30 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("Amp %s T+30s CRC: 0x%08X", amp.name, crcT30);
    
    if (crcT0 == crcT1 && crcT1 == crcT5 && crcT5 == crcT30) {
        CIRRUS_LOG("Amp %s: FSM is stable (CRCs match)", amp.name);
    } else {
        CIRRUS_LOG("Amp %s: FSM is running! (CRCs differ)", amp.name);
    }
}

bool CirrusAudioFixup::cs35l41_test_key_unlock(CS35L41Amp &amp) {
    bool ret1 = writeRegister(amp, CS35L41_TEST_KEY_CTL, 0x00000055);
    bool ret2 = writeRegister(amp, CS35L41_TEST_KEY_CTL, 0x000000AA);
    
    if (!ret1 || !ret2) {
        CIRRUS_ERR("Amp %s: Failed to unlock test key", amp.name);
        return false;
    }
    
    CIRRUS_LOG("Amp %s: Test Key Unlocked successfully.", amp.name);
    return true;
}

bool CirrusAudioFixup::cs35l41_test_key_lock(CS35L41Amp &amp) {
    bool ret1 = writeRegister(amp, CS35L41_TEST_KEY_CTL, 0x000000CC);
    bool ret2 = writeRegister(amp, CS35L41_TEST_KEY_CTL, 0x00000033);
    
    if (!ret1 || !ret2) {
        CIRRUS_ERR("Amp %s: Failed to lock test key", amp.name);
        return false;
    }
    
    CIRRUS_LOG("Amp %s: Test Key Locked successfully.", amp.name);
    return true;
}

bool CirrusAudioFixup::cs35l41_apply_phase4A2(CS35L41Amp &amp) {
    CIRRUS_LOG("Amp %s: --- Starting Phase 4A.2 ---", amp.name);
    
    UInt32 snapshot0[270], snapshot1[270], snapshot2[270], snapshot3[270];
    snapshotRegisters(amp, snapshot0);
    
    // 1. Unlock Test Key
    if (!cs35l41_test_key_unlock(amp)) {
        return false;
    }
    
    snapshotRegisters(amp, snapshot1);
    
    UInt32 crc_unlock = calculateRegistersCRC32(amp);
    char propName[64];
    snprintf(propName, sizeof(propName), "Cirrus_CRC_Unlock_%s", amp.name);
    setProperty(propName, crc_unlock, 32);
    CIRRUS_LOG("Amp %s: CRC after Unlock -> 0x%08X", amp.name, crc_unlock);
    
    if (bootArgStrEquals("cirrus_phase", "4A2B") || bootArgStrEquals("cirrus_phase", "4A2C")) {
        // 2. Errata Patch
        if (!cs35l41_register_errata_patch(amp)) {
            return false;
        }
        
        snapshotRegisters(amp, snapshot2);
        CIRRUS_LOG("Amp %s: --- Diff [Snapshot1 -> Snapshot2] (Errata) ---", amp.name);
        compareRegisterSnapshots(amp, snapshot1, snapshot2);
        
        UInt32 crc_errata = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_Errata_%s", amp.name);
        setProperty(propName, crc_errata, 32);
        CIRRUS_LOG("Amp %s: CRC after Errata -> 0x%08X", amp.name, crc_errata);
    }
    
    if (bootArgStrEquals("cirrus_phase", "4A2C")) {
        // 3. OTP Unpack
        if (!cs35l41_otp_unpack(amp)) {
            CIRRUS_ERR("Amp %s: OTP Unpack failed! Rolling back (Locking Test Key).", amp.name);
            cs35l41_test_key_lock(amp); // ROLLBACK
            return false;
        }
        
        snapshotRegisters(amp, snapshot3);
        CIRRUS_LOG("Amp %s: --- Diff [Snapshot2 -> Snapshot3] (OTP) ---", amp.name);
        compareRegisterSnapshots(amp, snapshot2, snapshot3);
        
        UInt32 crc_otp = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_OTP_%s", amp.name);
        setProperty(propName, crc_otp, 32);
        CIRRUS_LOG("Amp %s: CRC after OTP Unpack -> 0x%08X", amp.name, crc_otp);
    }
    
    // 4. Lock Test Key
    if (!cs35l41_test_key_lock(amp)) {
        return false;
    }
    
    UInt32 crc_lock = calculateRegistersCRC32(amp);
    snprintf(propName, sizeof(propName), "Cirrus_CRC_Lock_%s", amp.name);
    setProperty(propName, crc_lock, 32);
    CIRRUS_LOG("Amp %s: CRC after Lock -> 0x%08X", amp.name, crc_lock);
    
    CIRRUS_LOG("Amp %s: Phase 4A.2 completed.", amp.name);
    return true;
}

bool CirrusAudioFixup::cs35l41_register_errata_patch(CS35L41Amp &amp) {
    const ErrataTable errata_tables[] = {
        { 0xB2, cs35l41_revb2_errata_patch, sizeof(cs35l41_revb2_errata_patch) / sizeof(ErrataPatch) }
    };
    
    UInt32 rev_only = amp.revisionId & 0xFF;
    const ErrataTable *table_to_apply = nullptr;
    
    for (size_t i = 0; i < sizeof(errata_tables) / sizeof(ErrataTable); i++) {
        if (errata_tables[i].revid == rev_only) {
            table_to_apply = &errata_tables[i];
            break;
        }
    }
    
    if (!table_to_apply) {
        CIRRUS_ERR("Amp %s: No Errata patch found for Revision 0x%02X", amp.name, rev_only);
        return false;
    }
    
    CIRRUS_LOG("Amp %s: Applying %lu Errata Patches for Revision 0x%02X", 
               amp.name, table_to_apply->numPatches, rev_only);
               
    for (size_t i = 0; i < table_to_apply->numPatches; i++) {
        if (!writeRegister(amp, table_to_apply->patches[i].reg, table_to_apply->patches[i].value)) {
            CIRRUS_ERR("Amp %s: Failed to apply Errata Patch at 0x%08X", amp.name, table_to_apply->patches[i].reg);
            return false;
        }
    }
    
    // Write 0 to CCM_CORE_CTRL per Linux implementation
    if (!writeRegister(amp, 0x02BC1000, 0x00000000)) { // CS35L41_DSP1_CCM_CORE_CTRL
        CIRRUS_ERR("Amp %s: Failed to write CCM_CORE_CTRL", amp.name);
        return false;
    }
    
    return true;
}

bool CirrusAudioFixup::cs35l41_otp_unpack(CS35L41Amp &amp) {
    const cs35l41_otp_map_element_t *otp_map_match = nullptr;
    const cs35l41_otp_packed_element_t *otp_map;
    int bit_offset, word_offset, i;
    unsigned int bit_sum = 8;
    UInt32 otp_val;
    UInt32 otp_id_reg;
    UInt32 otp_mem[80]; // CS35L41_OTP_SIZE_WORDS is 80 (since size is 80 * 4 bytes?)
    // Actually, Linux reads 80 words. We'll use 80.
    
    int elements_processed = 0;
    int elements_skipped = 0;
    int update_bits_calls = 0;
    
    if (!readRegister(amp, 0x00000010, &otp_id_reg)) { // CS35L41_OTPID
        CIRRUS_ERR("Amp %s: Read OTP ID failed", amp.name);
        return false;
    }
    
    CIRRUS_LOG("Amp %s: Read OTPID = 0x%02X", amp.name, otp_id_reg);
    
    for (size_t i = 0; i < ARRAY_SIZE(cs35l41_otp_map_map); i++) {
        if (cs35l41_otp_map_map[i].id == otp_id_reg) {
            otp_map_match = &cs35l41_otp_map_map[i];
            break;
        }
    }
    
    if (!otp_map_match) {
        CIRRUS_ERR("Amp %s: OTP Map matching ID %u not found", amp.name, otp_id_reg);
        return false;
    }
    
    CIRRUS_LOG("Amp %s: Selected OTP Map = %u", amp.name, otp_id_reg);
    CIRRUS_LOG("Amp %s: Number of packed elements = %u", amp.name, otp_map_match->num_elements);
    
    // We must read 80 words (320 bytes) from CS35L41_OTP_MEM0 (0x00000400).
    UInt8 otp_raw_buf[80 * 4];
    if (!bulkRead(amp, 0x00000400, otp_raw_buf, sizeof(otp_raw_buf))) {
        CIRRUS_ERR("Amp %s: Read OTP Mem failed", amp.name);
        return false;
    }
    
    for (int i = 0; i < 80; i++) {
        // I2C reads big-endian, we need to convert to host endianness for uint32
        otp_mem[i] = (otp_raw_buf[i * 4] << 24) | 
                     (otp_raw_buf[i * 4 + 1] << 16) | 
                     (otp_raw_buf[i * 4 + 2] << 8) | 
                     (otp_raw_buf[i * 4 + 3]);
    }
    
    otp_map = otp_map_match->map;
    bit_offset = otp_map_match->bit_offset;
    word_offset = otp_map_match->word_offset;
    
    for (i = 0; i < otp_map_match->num_elements; i++) {
        elements_processed++;
        
        if (bit_offset + otp_map[i].size - 1 >= 32) {
            otp_val = (otp_mem[word_offset] &
                    GENMASK(31, bit_offset)) >> bit_offset;
            otp_val |= (otp_mem[++word_offset] &
                    GENMASK(bit_offset + otp_map[i].size - 33, 0)) <<
                    (32 - bit_offset);
            bit_offset += otp_map[i].size - 32;
        } else if (bit_offset + otp_map[i].size - 1 >= 0) {
            otp_val = (otp_mem[word_offset] &
                   GENMASK(bit_offset + otp_map[i].size - 1, bit_offset)
                  ) >> bit_offset;
            bit_offset += otp_map[i].size;
        } else { /* both bit_offset and otp_map[i].size are 0 */
            otp_val = 0;
        }

        bit_sum += otp_map[i].size;

        if (bit_offset == 32) {
            bit_offset = 0;
            word_offset++;
        }

        if (otp_map[i].reg != 0) {
            update_bits_calls++;
            if (!updateRegisterBits(amp, otp_map[i].reg,
                         GENMASK(otp_map[i].shift + otp_map[i].size - 1,
                             otp_map[i].shift),
                         otp_val << otp_map[i].shift)) {
                CIRRUS_ERR("Amp %s: Write OTP val failed at reg 0x%08X", amp.name, otp_map[i].reg);
                return false;
            }
        } else {
            elements_skipped++;
        }
    }
    
    CIRRUS_LOG("Amp %s: OTP Unpack Summary: Elements Processed=%d, Skipped=%d, UpdateBits Calls=%d",
               amp.name, elements_processed, elements_skipped, update_bits_calls);
    
    return true;
}

void CirrusAudioFixup::snapshotRegisters(CS35L41Amp &amp, UInt32 *snapshot) {
    if (!amp.present) return;
    for (int i = 0; i < sizeof(cs35l41_reg_desc)/sizeof(RegisterDesc); i++) {
        UInt32 val = 0;
        if (readRegister(amp, cs35l41_reg_desc[i].addr, &val)) {
            snapshot[i] = val;
        } else {
            snapshot[i] = 0xFFFFFFFF; // Error marker
        }
    }
}

void CirrusAudioFixup::compareRegisterSnapshots(CS35L41Amp &amp, const UInt32 *oldSnapshot, const UInt32 *newSnapshot) {
    if (!amp.present) return;
    CIRRUS_LOG("Amp %s: --- Register Diff Verification (Phase 4A.3) ---", amp.name);
    int diffCount = 0;
    
    for (int i = 0; i < sizeof(cs35l41_reg_desc)/sizeof(RegisterDesc); i++) {
        if (oldSnapshot[i] != newSnapshot[i] && oldSnapshot[i] != 0xFFFFFFFF && newSnapshot[i] != 0xFFFFFFFF) {
            CIRRUS_LOG("Amp %s: [DIFF] %s (0x%08X) changed from 0x%08X to 0x%08X",
                       amp.name,
                       cs35l41_reg_desc[i].name,
                       cs35l41_reg_desc[i].addr,
                       oldSnapshot[i],
                       newSnapshot[i]);
            diffCount++;
        }
    }
    
    CIRRUS_LOG("Amp %s: Total %d registers changed.", amp.name, diffCount);
}

bool CirrusAudioFixup::applyRegisterSequence(CS35L41Amp &amp, const RegisterSequence* sequence, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (sequence[i].updateBits) {
            if (!updateRegisterBits(amp, sequence[i].reg, sequence[i].mask, sequence[i].value, TRACE_PROBE)) {
                return false;
            }
        } else {
            if (!writeRegister(amp, sequence[i].reg, sequence[i].value, TRACE_PROBE)) {
                return false;
            }
        }
        if (sequence[i].delay_us > 0) {
            IODelay(sequence[i].delay_us);
        }
    }
    return true;
}

static const RegisterSequence pll_sequence[] = {
    { CS35L41_PLL_CLK_CTRL, 0, 0x00000430, 0, false },
    { CS35L41_DSP_CLK_CTRL, 0, 0x00000003, 0, false },
    { CS35L41_GLOBAL_CLK_CTRL, 0, 0x00000003, 0, false }
};

bool CirrusAudioFixup::applyPLL(CS35L41Amp &amp) {
    uint32_t crc_before = calculateRegistersCRC32(amp);
    uint64_t startTime = mach_absolute_time();

    if (!applyRegisterSequence(amp, pll_sequence, sizeof(pll_sequence) / sizeof(RegisterSequence))) {
        return false;
    }

    uint64_t endTime = mach_absolute_time();
    uint64_t elapsed_us = 0;
    absolutetime_to_nanoseconds(endTime - startTime, &elapsed_us);
    elapsed_us /= 1000;

    uint32_t crc_after = calculateRegistersCRC32(amp);
    
    // Telemetry
    if (strcmp(amp.name, "right") == 0) {
        setProperty("Cirrus_PLL_CRC_right", crc_after, 32);
        setProperty("Cirrus_PLL_TimeUS_right", elapsed_us, 32);
    } else {
        setProperty("Cirrus_PLL_CRC_left", crc_after, 32);
        setProperty("Cirrus_PLL_TimeUS_left", elapsed_us, 32);
    }
    
    return true;
}

static const RegisterSequence asp_sequence[] = {
    { CS35L41_SP_RATE_CTRL, 0, 0x00000021, 0, false },
    { CS35L41_SP_FORMAT, 0, 0x20200200, 0, false },
    { CS35L41_SP_TX_WL, 0, 0x00000018, 0, false },
    { CS35L41_SP_RX_WL, 0, 0x00000018, 0, false },
    { CS35L41_DAC_PCM1_SRC, 0, 0x00000032, 0, false },
    { CS35L41_ASP_TX1_SRC, 0, 0x00000018, 0, false },
    { CS35L41_ASP_TX2_SRC, 0, 0x00000019, 0, false },
    { CS35L41_ASP_TX3_SRC, 0, 0x00000028, 0, false },
    { CS35L41_ASP_TX4_SRC, 0, 0x00000029, 0, false },
    { CS35L41_DSP1_RX1_SRC, 0, 0x00000008, 0, false },
    { CS35L41_DSP1_RX2_SRC, 0, 0x00000009, 0, false },
    { CS35L41_DSP1_RX3_SRC, 0, 0x00000018, 0, false },
    { CS35L41_DSP1_RX4_SRC, 0, 0x00000019, 0, false },
    { CS35L41_DSP1_RX6_SRC, 0, 0x00000029, 0, false },
    { CS35L41_SP_HIZ_CTRL, 0, 0x00000003, 0, false },
    { CS35L41_SP_ENABLES, 0, 0x00010001, 0, false }
};

bool CirrusAudioFixup::applyASP(CS35L41Amp &amp) {
    uint32_t crc_before = calculateRegistersCRC32(amp);
    uint64_t startTime = mach_absolute_time();

    if (!applyRegisterSequence(amp, asp_sequence, sizeof(asp_sequence) / sizeof(RegisterSequence))) {
        return false;
    }

    uint64_t endTime = mach_absolute_time();
    uint64_t elapsed_us = 0;
    absolutetime_to_nanoseconds(endTime - startTime, &elapsed_us);
    elapsed_us /= 1000;

    uint32_t crc_after = calculateRegistersCRC32(amp);
    
    // Telemetry
    if (strcmp(amp.name, "right") == 0) {
        setProperty("Cirrus_ASP_CRC_right", crc_after, 32);
        setProperty("Cirrus_ASP_TimeUS_right", elapsed_us, 32);
    } else {
        setProperty("Cirrus_ASP_CRC_left", crc_after, 32);
        setProperty("Cirrus_ASP_TimeUS_left", elapsed_us, 32);
    }
    
    return true;
}

static const RegisterSequence gpio_sequence[] = {
    // gpio1.pol_inv = 0, gpio1.out_en = 1 (CS35L41_GPIO_DIR_SHIFT = 24, POL_SHIFT = 16) -> 0x01000000 mask, 0x00000000 val
    { CS35L41_GPIO1_CTRL1, 0x01010000, 0x00000000, 0, true },
    // gpio2.pol_inv = 0, gpio2.out_en = 0 -> 0x01000000 mask, 0x01000000 val (wait, default is 81000001, so out_en is false (1))
    { CS35L41_GPIO2_CTRL1, 0x01010000, 0x01000000, 0, true },
    // gpio1.func = CS35L41_GPIO1_MDSYNC (2)
    { CS35L41_GPIO_PAD_CONTROL, 0x07000000, 0x02000000, 0, true },
};

bool CirrusAudioFixup::applyGPIO(CS35L41Amp &amp) {
    uint32_t crc_before = calculateRegistersCRC32(amp);
    uint64_t startTime = mach_absolute_time();

    if (!applyRegisterSequence(amp, gpio_sequence, sizeof(gpio_sequence) / sizeof(RegisterSequence))) {
        return false;
    }

    uint64_t endTime = mach_absolute_time();
    uint64_t elapsed_us = 0;
    absolutetime_to_nanoseconds(endTime - startTime, &elapsed_us);
    elapsed_us /= 1000;

    uint32_t crc_after = calculateRegistersCRC32(amp);
    
    // Telemetry
    if (strcmp(amp.name, "right") == 0) {
        setProperty("Cirrus_GPIO_CRC_right", crc_after, 32);
        setProperty("Cirrus_GPIO_TimeUS_right", elapsed_us, 32);
    } else {
        setProperty("Cirrus_GPIO_CRC_left", crc_after, 32);
        setProperty("Cirrus_GPIO_TimeUS_left", elapsed_us, 32);
    }
    
    return true;
}

IOService* CirrusAudioFixup::getAudioController() {
    OSDictionary *matching = serviceMatching("IOPCIDevice");
    if (!matching) return nullptr;
    
    OSIterator *iter = getMatchingServices(matching);
    if (!iter) return nullptr;
    
    IOService *service;
    IOService *bestController = nullptr;
    int bestScore = -1;
    
    while ((service = OSDynamicCast(IOService, iter->getNextObject()))) {
        int score = 0;
        
        OSData *classCodeData = OSDynamicCast(OSData, service->getProperty("class-code"));
        if (classCodeData && classCodeData->getLength() >= 3) {
            const uint8_t *bytes = (const uint8_t*)classCodeData->getBytesNoCopy();
            // Subclass 03, Base Class 04 (Multimedia Audio Controller)
            if (bytes[2] == 0x04 && bytes[1] == 0x03) {
                score += 10;
            }
        }
        
        const char *name = service->getName();
        if (name) {
            if (strcmp(name, "HDEF") == 0) score += 5;
            else if (strcmp(name, "HDAS") == 0) score += 3;
            else if (strcmp(name, "HDAU") == 0) score -= 10; // Penalize HDMI audio
        }
        
        if (score > bestScore) {
            bestScore = score;
            bestController = service;
        }
    }
    
    if (bestController) {
        bestController->retain(); // Caller must release
    }
    iter->release();
    return bestScore >= 0 ? bestController : nullptr;
}

#include "Codecs/CS35L41/CS35L41_FirmwareDatabase.hpp"

// Simple CRC32 for firmware validation
static uint32_t calculate_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

void CirrusAudioFixup::phase5a_FirmwareDiscovery(CS35L41Amp &amp) {
    CIRRUS_LOG("Entering Phase 5A: Firmware Discovery for amp %s", amp.name);
    
    amp.firmwareValidated = false;
    
    char propStatus[64];
    snprintf(propStatus, sizeof(propStatus), "Cirrus_Phase5A_Status_%s", amp.name);
    
    // Analog Integrity Check
    uint32_t current_crc = calculateRegistersCRC32(amp);
    if (current_crc != amp.final_crc) {
        CIRRUS_ERR("Analog Integrity failed! Expected %08X, got %08X", amp.final_crc, current_crc);
        OSString *statusStr = OSString::withCString("ANALOG_STATE_CHANGED");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }
    
    uint32_t subVendor = 0;
    uint32_t subDevice = 0;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint32_t revisionId = 0;
    
    IOService *audioController = getAudioController();
    if (audioController) {
        OSData *subVenData = OSDynamicCast(OSData, audioController->getProperty("subsystem-vendor-id"));
        if (subVenData && subVenData->getLength() >= 4) subVendor = *((uint32_t*)subVenData->getBytesNoCopy());
        
        OSData *subDevData = OSDynamicCast(OSData, audioController->getProperty("subsystem-id"));
        if (subDevData && subDevData->getLength() >= 4) subDevice = *((uint32_t*)subDevData->getBytesNoCopy());
        
        OSData *venData = OSDynamicCast(OSData, audioController->getProperty("vendor-id"));
        if (venData && venData->getLength() >= 4) vendorId = *((uint32_t*)venData->getBytesNoCopy());
        
        OSData *devData = OSDynamicCast(OSData, audioController->getProperty("device-id"));
        if (devData && devData->getLength() >= 4) deviceId = *((uint32_t*)devData->getBytesNoCopy());
        
        OSData *revData = OSDynamicCast(OSData, audioController->getProperty("revision-id"));
        if (revData && revData->getLength() >= 4) revisionId = *((uint32_t*)revData->getBytesNoCopy());
        else if (revData && revData->getLength() >= 1) revisionId = *((uint8_t*)revData->getBytesNoCopy());
        
        // Log PCI Info
        char propPath[64];
        snprintf(propPath, sizeof(propPath), "Cirrus_PCI_Path_%s", amp.name);
        io_string_t pathStr;
        int pathLen = sizeof(pathStr);
        if (audioController->getPath(pathStr, &pathLen, gIOServicePlane)) {
            OSString *pStr = OSString::withCString(pathStr);
            if (pStr) {
                setProperty(propPath, pStr);
                pStr->release();
            }
        }
        
        OSString *pciDebug = OSDynamicCast(OSString, audioController->getProperty("pcidebug"));
        if (pciDebug) {
            char propBDF[64];
            snprintf(propBDF, sizeof(propBDF), "Cirrus_PCI_BDF_%s", amp.name);
            setProperty(propBDF, pciDebug);
        }
        
        char prop[64];
        snprintf(prop, sizeof(prop), "Cirrus_PCI_Vendor_%s", amp.name); setProperty(prop, (uint64_t)vendorId, 32);
        snprintf(prop, sizeof(prop), "Cirrus_PCI_Device_%s", amp.name); setProperty(prop, (uint64_t)deviceId, 32);
        snprintf(prop, sizeof(prop), "Cirrus_PCI_Revision_%s", amp.name); setProperty(prop, (uint64_t)revisionId, 32);
        snprintf(prop, sizeof(prop), "Cirrus_PCI_SubVendor_%s", amp.name); setProperty(prop, (uint64_t)subVendor, 32);
        snprintf(prop, sizeof(prop), "Cirrus_PCI_SubDevice_%s", amp.name); setProperty(prop, (uint64_t)subDevice, 32);
        
        audioController->release();
    }
    
    uint32_t ssid = (subVendor << 16) | subDevice;
    int spkid = 0; // Default to 0, or lookup from ACPI if needed
    
    // Telemetry setup
    char propSSID[64];
    snprintf(propSSID, sizeof(propSSID), "Cirrus_SSID_%s", amp.name);
    setProperty(propSSID, (uint64_t)ssid, 32);
    
    const FirmwareResource *foundRes = nullptr;
    for (size_t i = 0; i < firmwareTableSize; i++) {
        if (firmwareTable[i].subsystemVendor == subVendor &&
            firmwareTable[i].subsystemDevice == subDevice &&
            firmwareTable[i].spkid == spkid) {
            foundRes = &firmwareTable[i];
            break;
        }
    }
    
    if (!foundRes) {
        CIRRUS_ERR("Firmware not found for SSID %08X, spkid %d", ssid, spkid);
        OSString *statusStr = OSString::withCString("UNSUPPORTED_SSID");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }
    
    if (foundRes->wmfw == nullptr || foundRes->bin == nullptr) {
        CIRRUS_ERR("Firmware files missing for SSID %08X", ssid);
        OSString *statusStr = OSString::withCString("FILE_NOT_FOUND");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }
    
    // Validate Size
    if (foundRes->wmfwSize == 0 || foundRes->binSize == 0) {
        CIRRUS_ERR("Firmware size invalid for SSID %08X", ssid);
        OSString *statusStr = OSString::withCString("INVALID_RESOURCE");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }
    
    // Validate Alignment
    if ((foundRes->wmfwSize % 4 != 0) || (foundRes->binSize % 4 != 0)) {
        CIRRUS_ERR("Firmware not 4-byte aligned for SSID %08X", ssid);
        OSString *statusStr = OSString::withCString("INVALID_ALIGNMENT");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }
    
    // Basic verification of WMFW header magic
    if (foundRes->wmfwSize >= 4 && foundRes->wmfw[0] == 'W' && foundRes->wmfw[1] == 'M' && foundRes->wmfw[2] == 'F' && foundRes->wmfw[3] == 'W') {
        // Valid magic
    } else {
        CIRRUS_ERR("Invalid WMFW magic for SSID %08X", ssid);
        OSString *statusStr = OSString::withCString("INVALID_RESOURCE");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }
    
    // Export Firmware Meta Telemetry
    char propFW[64];
    snprintf(propFW, sizeof(propFW), "Cirrus_FW_Source_%s", amp.name);
    OSString *srcStr = OSString::withCString("Database");
    if (srcStr) {
        setProperty(propFW, srcStr);
        srcStr->release();
    }
    
    snprintf(propFW, sizeof(propFW), "Cirrus_FW_Type_%s", amp.name);
    OSString *typeStr = OSString::withCString(foundRes->isDummy ? "Dummy" : "Production");
    if (typeStr) {
        setProperty(propFW, typeStr);
        typeStr->release();
    }
    
    snprintf(propFW, sizeof(propFW), "Cirrus_FW_Size_%s", amp.name); setProperty(propFW, (uint64_t)foundRes->wmfwSize, 32);
    snprintf(propFW, sizeof(propFW), "Cirrus_BIN_Size_%s", amp.name); setProperty(propFW, (uint64_t)foundRes->binSize, 32);
    
    // CRC32 of FW
    uint32_t fwCrc = calculate_crc32(foundRes->wmfw, foundRes->wmfwSize);
    uint32_t binCrc = calculate_crc32(foundRes->bin, foundRes->binSize);
    snprintf(propFW, sizeof(propFW), "Cirrus_FW_CRC32_%s", amp.name); setProperty(propFW, (uint64_t)fwCrc, 32);
    snprintf(propFW, sizeof(propFW), "Cirrus_BIN_CRC32_%s", amp.name); setProperty(propFW, (uint64_t)binCrc, 32);
    
    // Parse Version (e.g. at offset 4 of WMFW)
    uint32_t fwVersion = 0;
    if (foundRes->wmfwSize >= 8) {
        fwVersion = foundRes->wmfw[4] | (foundRes->wmfw[5] << 8) | (foundRes->wmfw[6] << 16) | (foundRes->wmfw[7] << 24);
    }
    uint32_t binVersion = 0;
    if (foundRes->binSize >= 8) {
        binVersion = foundRes->bin[4] | (foundRes->bin[5] << 8) | (foundRes->bin[6] << 16) | (foundRes->bin[7] << 24);
    }
    
    char propFwVer[64];
    snprintf(propFwVer, sizeof(propFwVer), "Cirrus_FW_Version_%s", amp.name);
    setProperty(propFwVer, (uint64_t)fwVersion, 32);
    
    char propBinVer[64];
    snprintf(propBinVer, sizeof(propBinVer), "Cirrus_BIN_Version_%s", amp.name);
    setProperty(propBinVer, (uint64_t)binVersion, 32);
    
    // All checks passed
    amp.wmfwData = foundRes->wmfw;
    amp.wmfwSize = foundRes->wmfwSize;
    amp.binData = foundRes->bin;
    amp.binSize = foundRes->binSize;
    amp.firmwareValidated = true;
    
    CIRRUS_LOG("Firmware discovery OK: %s (Size: %lu, Ver: %08X, CRC: %08X), Tuning: %s (Size: %lu, Ver: %08X, CRC: %08X)",
               foundRes->fwName, amp.wmfwSize, fwVersion, fwCrc, foundRes->binName, amp.binSize, binVersion, binCrc);
    OSString *statusStr = OSString::withCString("READY");
    if (statusStr) {
        setProperty(propStatus, statusStr);
        statusStr->release();
    }
}

void CirrusAudioFixup::phase5b_DSPBringup(CS35L41Amp &amp) {
    CIRRUS_LOG("Entering Phase 5B: DSP Bring-up for amp %s", amp.name);
    
    char propName[64];
    OSString *statusStr = nullptr;
    
    // PRE Snapshot
    uint32_t pre_core_ctrl = 0, pre_clk_ctrl = 0, pre_mbox = 0;
    uint32_t pre_sys_id = 0, pre_sys_ver = 0, pre_sys_core = 0;
    readRegister(amp, CS35L41_DSP1_CCM_CORE_CTRL, &pre_core_ctrl);
    readRegister(amp, CS35L41_DSP_CLK_CTRL, &pre_clk_ctrl);
    readRegister(amp, CS35L41_DSP_MBOX_2, &pre_mbox);
    readRegister(amp, CS35L41_DSP1_SYS_ID, &pre_sys_id);
    readRegister(amp, CS35L41_DSP1_SYS_VERSION, &pre_sys_ver);
    readRegister(amp, CS35L41_DSP1_SYS_CORE_ID, &pre_sys_core);
    CIRRUS_LOG("Phase 5B PRE-Snapshot: CORE=0x%08X, CLK=0x%08X, MBOX=0x%08X, SYSID=0x%08X, VER=0x%08X, COREID=0x%08X", 
               pre_core_ctrl, pre_clk_ctrl, pre_mbox, pre_sys_id, pre_sys_ver, pre_sys_core);
    
    // 5B.1 DSP Reset Release
    uint64_t reset_start = mach_absolute_time();
    
    // update_bits for RESET | EN
    updateRegisterBits(amp, CS35L41_DSP1_CCM_CORE_CTRL, HALO_CORE_RESET | HALO_CORE_EN, HALO_CORE_RESET | HALO_CORE_EN);
    
    // update_bits to clear RESET
    updateRegisterBits(amp, CS35L41_DSP1_CCM_CORE_CTRL, HALO_CORE_RESET, 0);
    
    uint64_t reset_end = mach_absolute_time();
    uint64_t reset_time_us = (reset_end - reset_start) / 1000; // rough approximation for nanosecs to microsecs (assuming 1 tick = 1ns roughly on intel, but better use mach_timebase_info if precision matters, for diagnostics / 1000 is fine)
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_RESET_TIME_US_%s", amp.name);
    setProperty(propName, reset_time_us, 32);
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_RESET_%s", amp.name);
    statusStr = OSString::withCString("OK");
    if (statusStr) { setProperty(propName, statusStr); statusStr->release(); }
    
    // 5B.2 DSP Clock Raw
    uint32_t current_clk = 0;
    readRegister(amp, CS35L41_DSP_CLK_CTRL, &current_clk);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_CLOCK_RAW_%s", amp.name);
    setProperty(propName, (uint64_t)current_clk, 32);
    
    // 5B.3 MPU Unlock
    uint64_t mpu_start = mach_absolute_time();
    
    writeRegister(amp, CS35L41_DSP1_MPU_LOCK_CONFIG, 0x5555);
    writeRegister(amp, CS35L41_DSP1_MPU_LOCK_CONFIG, 0xAAAA);
    
    uint32_t unlock_val = 0xFFFFFFFF;
    writeRegister(amp, CS35L41_DSP1_MPU_XM_ACCESS0, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_YM_ACCESS0, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_WNDW_ACCESS0, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_XREG_ACCESS0, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_YREG_ACCESS0, unlock_val);
    
    writeRegister(amp, CS35L41_DSP1_MPU_XM_ACCESS1, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_YM_ACCESS1, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_WNDW_ACCESS1, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_XREG_ACCESS1, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_YREG_ACCESS1, unlock_val);
    
    writeRegister(amp, CS35L41_DSP1_MPU_XM_ACCESS2, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_YM_ACCESS2, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_WNDW_ACCESS2, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_XREG_ACCESS2, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_YREG_ACCESS2, unlock_val);
    
    writeRegister(amp, CS35L41_DSP1_MPU_XM_ACCESS3, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_YM_ACCESS3, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_WNDW_ACCESS3, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_XREG_ACCESS3, unlock_val);
    writeRegister(amp, CS35L41_DSP1_MPU_YREG_ACCESS3, unlock_val);
    
    uint64_t mpu_end = mach_absolute_time();
    uint64_t mpu_time_us = (mpu_end - mpu_start) / 1000;
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_MPU_TIME_US_%s", amp.name);
    setProperty(propName, mpu_time_us, 32);
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_MEM_WINDOW_%s", amp.name);
    statusStr = OSString::withCString("OK");
    if (statusStr) { setProperty(propName, statusStr); statusStr->release(); }
    
    // 5B.3.5 DSP Identity Check
    uint32_t sys_id = 0, sys_ver = 0, sys_core = 0;
    readRegister(amp, CS35L41_DSP1_SYS_ID, &sys_id);
    readRegister(amp, CS35L41_DSP1_SYS_VERSION, &sys_ver);
    readRegister(amp, CS35L41_DSP1_SYS_CORE_ID, &sys_core);
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_ID_%s", amp.name); setProperty(propName, (uint64_t)sys_id, 32);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_VER_%s", amp.name); setProperty(propName, (uint64_t)sys_ver, 32);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_CORE_%s", amp.name); setProperty(propName, (uint64_t)sys_core, 32);
    
    // POST Snapshot
    uint32_t post_core_ctrl = 0, post_clk_ctrl = 0, post_mbox = 0;
    readRegister(amp, CS35L41_DSP1_CCM_CORE_CTRL, &post_core_ctrl);
    readRegister(amp, CS35L41_DSP_CLK_CTRL, &post_clk_ctrl);
    readRegister(amp, CS35L41_DSP_MBOX_2, &post_mbox);
    CIRRUS_LOG("Phase 5B POST-Snapshot: CORE=0x%08X, CLK=0x%08X, MBOX=0x%08X, SYSID=0x%08X, VER=0x%08X, COREID=0x%08X", 
               post_core_ctrl, post_clk_ctrl, post_mbox, sys_id, sys_ver, sys_core);
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_STATE_%s", amp.name);
    if (sys_id == 0xFFFFFFFF && sys_ver == 0xFFFFFFFF && sys_core == 0xFFFFFFFF) {
        statusStr = OSString::withCString("FAIL_SYSINFO (DSP_UNREACHABLE)");
        if (statusStr) { setProperty(propName, statusStr); statusStr->release(); }
        return;
    } else if (sys_id == 0x00000000 && sys_ver == 0x00000000 && sys_core == 0x00000000) {
        statusStr = OSString::withCString("FAIL_SYSINFO (DSP_IN_RESET)");
        if (statusStr) { setProperty(propName, statusStr); statusStr->release(); }
        return;
    }
    
    // 5B.4 Mailbox Init Read
    uint32_t mbox_init = 0;
    readRegister(amp, CS35L41_DSP_MBOX_2, &mbox_init);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_MAILBOX_RAW_INIT_%s", amp.name);
    setProperty(propName, (uint64_t)mbox_init, 32);
    
    // 5B.5 Mailbox Timeline Polling
    uint32_t current_mbox = post_mbox;
    uint32_t previous_mbox = post_mbox;
    OSArray *timeline = OSArray::withCapacity(10);
    uint32_t transitions = 0;
    uint32_t first_non_zero_time = 0xFFFFFFFF;
    
    if (timeline) {
        char initialTimeline[64];
        snprintf(initialTimeline, sizeof(initialTimeline), "0ms: 0x%08X", previous_mbox);
        OSString *tStr = OSString::withCString(initialTimeline);
        if (tStr) { timeline->setObject(tStr); tStr->release(); }
    }
    
    uint64_t mbox_start = mach_absolute_time();
    int timeout_ms = 500;
    int ms = 0;
    
    while (ms <= timeout_ms) {
        readRegister(amp, CS35L41_DSP_MBOX_2, &current_mbox);
        
        if (current_mbox != previous_mbox) {
            transitions++;
            if (current_mbox != 0 && first_non_zero_time == 0xFFFFFFFF) {
                first_non_zero_time = ms;
            }
            if (timeline) {
                char transitionStr[64];
                snprintf(transitionStr, sizeof(transitionStr), "%dms: 0x%08X", ms, current_mbox);
                OSString *tStr = OSString::withCString(transitionStr);
                if (tStr) { timeline->setObject(tStr); tStr->release(); }
            }
            previous_mbox = current_mbox;
        }
        
        IODelay(1000); // 1ms delay
        ms++;
    }
    
    uint64_t mbox_end = mach_absolute_time();
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_MAILBOX_POLL_%s", amp.name);
    if (timeline) {
        setProperty(propName, timeline);
        timeline->release();
    }
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_MAILBOX_TRANSITIONS_%s", amp.name);
    setProperty(propName, (uint64_t)transitions, 32);
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_MAILBOX_READY_TIME_MS_%s", amp.name);
    setProperty(propName, (uint64_t)((mbox_end - mbox_start) / 1000000), 32);
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_STATE_%s", amp.name);
    if (transitions == 0 && post_mbox == 0x0) {
        statusStr = OSString::withCString("FAIL_MAILBOX_TIMEOUT");
    } else {
        statusStr = OSString::withCString("READY");
    }
    if (statusStr) { setProperty(propName, statusStr); statusStr->release(); }
    
    if (first_non_zero_time == 0xFFFFFFFF) {
        CIRRUS_LOG("Phase 5B Mailbox Stats: Polls=%d, Transitions=%d, Last=0x%08X, FirstNonZeroTime=N/A", ms, transitions, current_mbox);
    } else {
        CIRRUS_LOG("Phase 5B Mailbox Stats: Polls=%d, Transitions=%d, Last=0x%08X, FirstNonZeroTime=%dms", ms, transitions, current_mbox, first_non_zero_time);
    }
    
    CIRRUS_LOG("Phase 5B complete for amp %s. Transitions: %d", amp.name, transitions);
}

bool CirrusAudioFixup::phase5b_1_VerifyDSPAlive(CS35L41Amp &amp) {
    uint32_t core_ctrl = 0, sys_id = 0, mbox = 0;
    bool core_pass = false, reset_pass = false, mbox_pass = false, xm_pass = false;

    readRegister(amp, CS35L41_DSP1_CCM_CORE_CTRL, &core_ctrl, TRACE_DUMP);
    readRegister(amp, CS35L41_DSP1_SYS_ID, &sys_id, TRACE_DUMP);
    readRegister(amp, CS35L41_DSP_MBOX_2, &mbox, TRACE_DUMP);

    if (core_ctrl & HALO_CORE_EN) core_pass = true;
    if ((core_ctrl & HALO_CORE_RESET) == 0) reset_pass = true;
    if (sys_id != 0x00000000 && sys_id != 0xFFFFFFFF) mbox_pass = true;

    uint8_t dummy[4] = {0};
    if (bulkRead(amp, 0x02000000, dummy, 4, TRACE_DUMP)) {
        xm_pass = true;
    }

    CIRRUS_LOG("Amp %s: ==== Verify DSP Alive ====", amp.name);
    CIRRUS_LOG("Amp %s: CORE_CTRL        %s  value=0x%08X", amp.name, reset_pass ? "PASS" : "FAIL", core_ctrl);
    CIRRUS_LOG("Amp %s: CORE_ENABLE      %s", amp.name, core_pass ? "PASS" : "FAIL");
    CIRRUS_LOG("Amp %s: MAILBOX          %s  value=0x%08X", amp.name, mbox_pass ? "PASS" : "FAIL", mbox);
    CIRRUS_LOG("Amp %s: XM READ          %s  addr=0x02000000", amp.name, xm_pass ? "PASS" : "FAIL");

    return core_pass && reset_pass && mbox_pass && xm_pass;
}

static uint32_t compute_entropy_x10(const uint8_t *data, uint32_t size) {
    if (size == 0) return 0;
    uint32_t counts[256] = {0};
    for (uint32_t i = 0; i < size; i++) {
        counts[data[i]]++;
    }
    
    auto log2_x1000 = [](uint32_t x) -> uint32_t {
        if (x == 0) return 0;
        uint32_t l = 31 - __builtin_clz(x);
        uint32_t rem = x - (1U << l);
        uint32_t frac = (rem * 1000) >> l;
        return l * 1000 + frac;
    };
    
    uint32_t size_log2 = log2_x1000(size);
    uint64_t total_entropy_x1000 = 0;
    
    for (int i = 0; i < 256; i++) {
        uint32_t c = counts[i];
        if (c > 0) {
            uint32_t c_log2 = log2_x1000(c);
            // size_log2 >= c_log2 is guaranteed since size >= c
            uint32_t term = c * (size_log2 - c_log2);
            total_entropy_x1000 += term;
        }
    }
    
    return (uint32_t)(total_entropy_x1000 / size / 100);
}

void CirrusAudioFixup::phase5c_1_DumpXMAndParseAlgorithms(CS35L41Amp &amp, FirmwareImage &outImage) {
    CIRRUS_LOG("Entering Phase 5C.1: Dump XM and Parse Algorithms for %s", amp.name);
    
    // Dump full 16KB of XM memory (CS35L41 XM RAM size is at least 8KB, usually 12KB, we dump up to 16KB to be safe)
    uint32_t dump_size = 16384;

    uint8_t *xm_dump_buffer = (uint8_t *)IOMallocData(dump_size);
    if (!xm_dump_buffer) {
        CIRRUS_ERR("Failed to allocate %u bytes for XM dump", dump_size);
        return;
    }
    memset(xm_dump_buffer, 0, dump_size);

    uint64_t dump_start = mach_absolute_time();
    uint32_t offset = 0;
    uint32_t chunk_size = 252;
    bool dump_success = true;
    
    while (offset < dump_size) {
        uint32_t read_len = dump_size - offset;
        if (read_len > chunk_size) read_len = chunk_size;
        
        uint32_t read_addr = 0x02000000 + offset;
        if (!bulkRead(amp, read_addr, xm_dump_buffer + offset, read_len, TRACE_DUMP)) {
            CIRRUS_ERR("Failed to read XM RAM at offset 0x%08X", read_addr);
            dump_success = false;
            break;
        }
        offset += read_len;
    }
    
    uint64_t dump_end = mach_absolute_time();
    uint32_t dump_time_ms = (uint32_t)((dump_end - dump_start) / 1000000);
    
    if (dump_success) {
        uint32_t crc = CirrusFirmwareParser::calculate_crc32(xm_dump_buffer, dump_size);
        
        uint32_t zeroes = 0, ffs = 0;
        for (uint32_t i = 0; i < dump_size; i++) {
            if (xm_dump_buffer[i] == 0x00) zeroes++;
            if (xm_dump_buffer[i] == 0xFF) ffs++;
        }
        
        CIRRUS_LOG("==== XM Dump ====");
        CIRRUS_LOG("Base             0x02000000");
        CIRRUS_LOG("Size             %u", dump_size);
        CIRRUS_LOG("CRC              0x%08X", crc);
        CIRRUS_LOG("Time             %u ms", dump_time_ms);
        
        uint32_t zero_pct = (zeroes * 100) / dump_size;
        uint32_t ff_pct = (ffs * 100) / dump_size;
        uint32_t entropy_x10 = compute_entropy_x10(xm_dump_buffer, dump_size);
        
        CIRRUS_LOG("==== Dump Validation ====");
        CIRRUS_LOG("Zero bytes       %u%% (%u bytes)", zero_pct, zeroes);
        CIRRUS_LOG("FF bytes         %u%% (%u bytes)", ff_pct, ffs);
        CIRRUS_LOG("Entropy          %u.%u bits/byte", entropy_x10 / 10, entropy_x10 % 10);
        
        bool is_valid = (zero_pct < 100 && ff_pct < 100 && crc != 0);
        CIRRUS_LOG("CRC              %s", is_valid ? "PASS" : "FAIL");
        
        CIRRUS_LOG("==== First 64 bytes ====");
        for (uint32_t i = 0; i < 64 && i < dump_size; i += 16) {
            CIRRUS_LOG("%02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                       xm_dump_buffer[i], xm_dump_buffer[i+1], xm_dump_buffer[i+2], xm_dump_buffer[i+3],
                       xm_dump_buffer[i+4], xm_dump_buffer[i+5], xm_dump_buffer[i+6], xm_dump_buffer[i+7],
                       xm_dump_buffer[i+8], xm_dump_buffer[i+9], xm_dump_buffer[i+10], xm_dump_buffer[i+11],
                       xm_dump_buffer[i+12], xm_dump_buffer[i+13], xm_dump_buffer[i+14], xm_dump_buffer[i+15]);
        }
        
        CIRRUS_LOG("==== Last 64 bytes ====");
        for (uint32_t i = dump_size - 64; i < dump_size; i += 16) {
            CIRRUS_LOG("%02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                       xm_dump_buffer[i], xm_dump_buffer[i+1], xm_dump_buffer[i+2], xm_dump_buffer[i+3],
                       xm_dump_buffer[i+4], xm_dump_buffer[i+5], xm_dump_buffer[i+6], xm_dump_buffer[i+7],
                       xm_dump_buffer[i+8], xm_dump_buffer[i+9], xm_dump_buffer[i+10], xm_dump_buffer[i+11],
                       xm_dump_buffer[i+12], xm_dump_buffer[i+13], xm_dump_buffer[i+14], xm_dump_buffer[i+15]);
        }
        
        if (!is_valid) {
            CIRRUS_ERR("Dump Validation FAILED!");
        } else {
            outImage.xm_dump_crc = crc;
            
            // Export dump to IORegistry for offline parsing
            char propName[64];
            snprintf(propName, sizeof(propName), "Cirrus_XM_Dump_%s", amp.name);
            OSData *dumpData = OSData::withBytes(xm_dump_buffer, dump_size);
            if (dumpData) {
                setProperty(propName, dumpData);
                dumpData->release();
            }
            
            CirrusFirmwareParser::parseAlgorithmTable(xm_dump_buffer, dump_size, outImage);
        }
    }
    
    IOFreeData(xm_dump_buffer, dump_size);
}

void CirrusAudioFixup::phase5c_FirmwareUpload(CS35L41Amp &amp, const char* phaseArg) {
    CIRRUS_LOG("Entering Phase 5C: Firmware Upload for amp %s", amp.name);
    
    if (!amp.wmfwData || amp.wmfwSize == 0) {
        CIRRUS_LOG("Amp %s: No WMFW data found. Phase 5C skipped.", amp.name);
        return;
    }
    
    FirmwareImage *image = (FirmwareImage *)IOMalloc(sizeof(FirmwareImage));
    if (!image) {
        CIRRUS_ERR("Amp %s: Failed to allocate memory for FirmwareImage", amp.name);
        return;
    }

    if (!CirrusFirmwareParser::parseWMFW(amp.wmfwData, amp.wmfwSize, image)) {
        CIRRUS_ERR("Amp %s: WMFW parse failed", amp.name);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    MappedImage *mappedImg = (MappedImage *)IOMalloc(sizeof(MappedImage));
    if (!mappedImg) {
        CIRRUS_ERR("Amp %s: Failed to allocate memory for MappedImage", amp.name);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    if (!CirrusFirmwareMapper::mapFirmwareImage(*image, *mappedImg)) {
        CIRRUS_ERR("Amp %s: Firmware Address Mapping Failed!", amp.name);
        IOFree(mappedImg, sizeof(MappedImage));
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    
    UploadSession session;
    if (CirrusFirmwareScheduler::run(amp, this, *mappedImg, session)) {
        CIRRUS_LOG("Phase 5C Complete for amp %s", amp.name);
    } else {
        CIRRUS_ERR("Amp %s: Phase 5C FAILED", amp.name);
    }

    IOFree(mappedImg, sizeof(MappedImage));
    IOFree(image, sizeof(FirmwareImage));
}
