#include "CirrusAudioFixup.hpp"
#include <IOKit/IOMemoryDescriptor.h>
#include "Codecs/CS35L41/FirmwareDatabase.hpp"
#include "Codecs/CS35L41/FirmwareParser.hpp"
#include "Codecs/CS35L41/FirmwareUploader.hpp"
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/OSAtomic.h>

#define CIRRUS_BUILD_ID "commit-c8df89d-virt-mbox-test4-spkout"

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
    CIRRUS_LOG("CirrusAudioFixup initialized. Build ID: %s", CIRRUS_BUILD_ID);
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

    // toggle reset via amd gpio pin 6
    // resolve amdi0030 device dynamically to avoid hardcoding physical base 0xfed81500
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
                
                // toggle sequence: pull low, sleep, pull high to reset the codec
                // write output value low (reset active)
                val &= ~(1 << 22);
                val |= (1 << 23);
                gpioBase[6] = val;
                
                UInt32 verifyLow = gpioBase[6];
                setProperty("Cirrus_GPIO6_verifyLow", verifyLow, 32);
                CIRRUS_LOG("AMD GPIO 6 LOW verify = 0x%08X", verifyLow);
                
                IOSleep(5);
                
                // write output value high (reset inactive)
                val = gpioBase[6];
                val |= (1 << 22);
                val |= (1 << 23);
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

    if (bootArgEnabled("cirrus_readonly")) {
        CIRRUS_LOG("CirrusAudioFixup starting in READ-ONLY PROBE mode");
        uint32_t delayMs = 100;
        PE_parse_boot_argn("cirrus_probe_delay", &delayMs, sizeof(delayMs));
        scheduleReadOnlyProbe(delayMs);
    } else {
        CIRRUS_LOG("CirrusAudioFixup starting FULL DRIVER FLOW");
        fullDriverFlow();
    }

    registerService();
    return true;
}

void CirrusAudioFixup::fullDriverFlow() {
    CIRRUS_LOG("starting hardware initialization flow");
    
    for (unsigned i = 0; i < 2; ++i) {
        CS35L41Amp &amp = mAmps[i];
        CIRRUS_LOG("initializing amplifier: %s", amp.name);
        
        // initialize hardware and apply error corrections
        if (!initCodec(amp)) {
            CIRRUS_ERR("failed to initialize codec for %s", amp.name);
            continue;
        }
        if (!initializeHardwareErrata(amp)) {
            CIRRUS_ERR("failed to apply hardware errata for %s", amp.name);
            continue;
        }
        applyPLL(amp);
        applyASP(amp);
        applyGPIO(amp);
        
        // apply system-specific hardware configuration
        configureHardware(amp);
        
        discoverFirmware(amp);

        if (amp.firmwareValidated) {
            initializeFirmware(amp, "5D.0");
        }
        
        // log current serial port configuration
        logASPSnapshot(amp);
        
        // power up the amplifier stage
        powerUpAmplifier(amp);
        
        CIRRUS_LOG("amplifier %s initialized successfully", amp.name);
    }
    
    CIRRUS_LOG("hardware initialization flow complete");
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
    
    // check using voodooi2c's checkkernelarg style (string buffer)
    int strValue[16];
    if (PE_parse_boot_argn(name, &strValue, sizeof(strValue))) {
        // if it parses as a string "0", treat as false
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

void CirrusAudioFixup::initializeFirmware(CS35L41Amp &amp, const char* phaseArg) {
    CIRRUS_LOG("starting full initialization for amplifier: %s", amp.name);
    
    if (!amp.wmfwData || amp.wmfwSize == 0) {
        CIRRUS_LOG("no wmfw data found for %s, skipping firmware init", amp.name);
        return;
    }
    
    FirmwareImage *image = (FirmwareImage *)IOMalloc(sizeof(FirmwareImage));
    if (!image) {
        CIRRUS_ERR("failed to allocate memory for firmware image on %s", amp.name);
        return;
    }

    // parse the wmfw firmware format
    if (!CirrusFirmwareParser::parseWMFW(amp.wmfwData, amp.wmfwSize, image)) {
        CIRRUS_ERR("wmfw parsing failed for %s", amp.name);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    
    CIRRUS_LOG("wmfw parsing successful on %s. fw id: 0x%06X, expected algs: %u", amp.name, image->fw_id, image->n_algs);

    // map and schedule the wmfw image to hardware memory
    MappedImage *wmfwMapped = (MappedImage *)IOMalloc(sizeof(MappedImage));
    if (wmfwMapped) {
        if (CirrusFirmwareMapper::mapFirmwareImage(*image, *wmfwMapped)) {
            CIRRUS_LOG("mapping wmfw image successful on %s, starting upload", amp.name);
            UploadSession session;
            CirrusFirmwareScheduler::run(amp, this, *wmfwMapped, session);
        } else {
            CIRRUS_ERR("failed to map wmfw image on %s", amp.name);
        }
        IOFree(wmfwMapped, sizeof(MappedImage));
    }
    
    // dump xm memory and parse codec algorithms before starting the dsp
    CIRRUS_LOG("parsing dsp algorithms for %s", amp.name);
    parseDSPAlgorithms(amp, *image);

    // simple lambda to search for case-insensitive substrings
    auto containsStr = [](const char *str, const char *sub) -> bool {
        if (!str || !sub) return false;
        size_t str_len = strlen(str);
        size_t sub_len = strlen(sub);
        if (sub_len > str_len) return false;
        for (size_t i = 0; i <= str_len - sub_len; i++) {
            bool match = true;
            for (size_t j = 0; j < sub_len; j++) {
                char c1 = str[i + j];
                char c2 = sub[j];
                if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
                if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
                if (c1 != c2) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    };

    // match firmware controls for diagnostics
    amp.diagnosticControlCount = 0;
    for (uint32_t i = 0; i < image->wmfwControlCount; i++) {
        const WMFWControl &ctl = image->wmfwControls[i];
        if (containsStr(ctl.name, "PCM") || containsStr(ctl.name, "LEVEL") || 
            containsStr(ctl.name, "PEAK") || containsStr(ctl.name, "RMS") || 
            containsStr(ctl.name, "STREAM") || containsStr(ctl.name, "ACTIVE")) {
            
            if (amp.diagnosticControlCount < 10) {
                // find algorithm owner for this control
                uint32_t alg_id = 0;
                for (uint32_t a = 0; a < image->wmfwAlgorithmCount; a++) {
                    if (i >= image->wmfwAlgorithms[a].firstControl && 
                        i < image->wmfwAlgorithms[a].firstControl + image->wmfwAlgorithms[a].controlCount) {
                        alg_id = image->wmfwAlgorithms[a].id;
                        break;
                    }
                }
                
                // decode base address in algorithm info
                uint32_t alg_xm_base = 0;
                for (uint32_t a = 0; a < image->algorithmCount; a++) {
                    if (image->algorithms[a].id == alg_id) {
                        alg_xm_base = decodePointer(image->algorithms[a].baseWordOffset).wordOffset;
                        break;
                    }
                }
                
                if (alg_xm_base != 0) {
                    uint32_t wordOffset = alg_xm_base + ctl.offset;
                    uint32_t regAddress = 0;
                    CirrusFirmwareMapper::mapPackedAddress(RegionType::XM_PACKED, wordOffset, 0, regAddress);
                    
                    strlcpy(amp.diagnosticControls[amp.diagnosticControlCount].name, ctl.name, sizeof(amp.diagnosticControls[0].name));
                    amp.diagnosticControls[amp.diagnosticControlCount].address = regAddress;
                    CIRRUS_LOG("registered diagnostic control '%s' at 0x%08X on %s", ctl.name, regAddress, amp.name);
                    amp.diagnosticControlCount++;
                }
            }
        }
    }
    CIRRUS_LOG("found %u matching diagnostic controls on %s", amp.diagnosticControlCount, amp.name);
    
    if (image->algorithmCount > 0) {
        // parse and upload coefficient settings if bin file data is present
        if (amp.binData && amp.binSize > 0) {
            if (CirrusFirmwareParser::parseBIN(amp.binData, amp.binSize, image)) {
                CIRRUS_LOG("bin parsing successful on %s, found %u coefficients", amp.name, image->coefficientCount);
                
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
                    if (found || image->coefficients[j].id == image->fw_id) {
                        matchedBlocks++;
                    } else {
                        unknownBlocks++;
                    }
                }
                
                CIRRUS_LOG("parser results on %s: bin blocks=%u, matched=%u, unknown=%u", amp.name, image->coefficientCount, matchedBlocks, unknownBlocks);
                
                MappedImage *coeffMapped = (MappedImage *)IOMalloc(sizeof(MappedImage));
                if (coeffMapped) {
                    if (CirrusFirmwareMapper::mapCoefficients(*image, *coeffMapped)) {
                        CIRRUS_LOG("starting coefficient file upload on %s", amp.name);
                        UploadSession session;
                        CirrusFirmwareScheduler::run(amp, this, *coeffMapped, session, true);
                    } else {
                        CIRRUS_ERR("coefficient mapping failed on %s", amp.name);
                    }
                    IOFree(coeffMapped, sizeof(MappedImage));
                }
            } else {
                CIRRUS_ERR("bin file parsing failed on %s", amp.name);
            }
        } else {
            CIRRUS_LOG("no bin file data present on %s, skipping coefficient config", amp.name);
        }
    }
    
    // start dsp after firmware and coefficient data is written
    CIRRUS_LOG("bringing up dsp controller on %s", amp.name);
    bringupDSP(amp);
    
    if (verifyDSPAlive(amp)) {
        CIRRUS_LOG("dsp is successfully verified alive on %s", amp.name);
    } else {
        CIRRUS_ERR("dsp bringup failed or dsp is unresponsive on %s", amp.name);
    }

    IOFree(image, sizeof(FirmwareImage));
    CIRRUS_LOG("full firmware init complete for amplifier %s", amp.name);
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
            initCodec(amp);
        } else if (bootArgStrEquals("cirrus_phase", "4A2A") || 
                   bootArgStrEquals("cirrus_phase", "4A2B") ||
                   bootArgStrEquals("cirrus_phase", "4A2C")) {
            if (initCodec(amp)) {
                initializeHardwareErrata(amp);
            }
        } else if (bootArgStrEquals("cirrus_phase", "4B")) {
            if (initCodec(amp)) {
                if (initializeHardwareErrata(amp)) {
                    applyPLL(amp);
                    applyASP(amp);
                    applyGPIO(amp);
                    amp.final_crc = calculateRegistersCRC32(amp);
                    dumpAllRegisters(amp);
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
            if (initCodec(amp)) {
                if (initializeHardwareErrata(amp)) {
                    applyPLL(amp);
                    applyASP(amp);
                    applyGPIO(amp);
                    amp.final_crc = calculateRegistersCRC32(amp);
                    discoverFirmware(amp);
                    if (bootArgStrEquals("cirrus_phase", "5B")) {
                        bringupDSP(amp);
                    } else {
                        char phaseArg[16] = {0};
                        if (PE_parse_boot_argn("cirrus_phase", phaseArg, sizeof(phaseArg))) {
                            if (strncmp(phaseArg, "5D", 2) == 0) {
                                // initializeFirmware(amp, phaseArg);
                            } else if (strncmp(phaseArg, "5C", 2) == 0) {
                                uploadFirmware(amp, phaseArg);
                                
                                // boot the dsp after full wmfw upload completes
                                bool shouldBootDsp = (strncmp(phaseArg, "5C.4", 4) == 0);
                                if (shouldBootDsp) {
                                    CIRRUS_LOG("bringing up dsp after firmware upload on %s", amp.name);
                                    bringupDSP(amp);
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
    
    // update telemetry stats
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
    
    // add entry to circular trace buffer
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
        // overwrite the oldest entry on buffer overflow
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
    
    // only perform write if the value changed
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
    
    // read diagnostic register states when polling times out
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

bool CirrusAudioFixup::initCodec(CS35L41Amp &amp) {
    // log device info and verify checksum before doing reset
    UInt32 devid_before = 0, revid_before = 0;
    readRegister(amp, 0x00000, &devid_before);
    readRegister(amp, 0x00004, &revid_before);
    UInt32 crc_before = calculateRegistersCRC32(amp);
    
    CIRRUS_LOG("amplifier %s status before reset: devid=0x%08X revid=0x%08X crc=0x%08X", 
               amp.name, devid_before, revid_before, crc_before);
    CIRRUS_LOG("sending soft reset to %s", amp.name);
    
    // trigger soft reset on the chip
    if (!writeRegister(amp, CS35L41_SW_RESET, CS35L41_SW_RESET_VAL)) {
        CIRRUS_ERR("failed to send soft reset to %s", amp.name);
        return false;
    }
    
    // wait for the dsp core to boot
    IODelay(3000);
    
    // soft reset clears all registers including interrupt masks.
    // we must immediately mask all interrupts to avoid kernel panic / login window freeze.
    writeRegister(amp, CS35L41_IRQ1_MASK1, 0xFFFFFFFF, TRACE_PROBE);
    writeRegister(amp, CS35L41_IRQ1_MASK2, 0xFFFFFFFF, TRACE_PROBE);
    writeRegister(amp, CS35L41_IRQ1_MASK3, 0xFFFFFFFF, TRACE_PROBE);
    writeRegister(amp, CS35L41_IRQ1_MASK4, 0xFFFFFFFF, TRACE_PROBE);
    writeRegister(amp, CS35L41_IRQ2_MASK1, 0xFFFFFFFF, TRACE_PROBE);
    writeRegister(amp, CS35L41_IRQ2_MASK2, 0xFFFFFFFF, TRACE_PROBE);
    writeRegister(amp, CS35L41_IRQ2_MASK3, 0xFFFFFFFF, TRACE_PROBE);
    writeRegister(amp, CS35L41_IRQ2_MASK4, 0xFFFFFFFF, TRACE_PROBE);
    
    // poll for otp boot complete status
    if (!pollRegisterBit(amp, CS35L41_IRQ1_STATUS4, CS35L41_OTP_BOOT_DONE, CS35L41_OTP_BOOT_DONE, 100)) {
        CIRRUS_ERR("otp boot complete polling failed on %s", amp.name);
        return false;
    }
    
    // verify revision register values after boot
    UInt32 devid_after = 0, revid_after = 0;
    readRegister(amp, 0x00000, &devid_after);
    readRegister(amp, 0x00004, &revid_after);
    UInt32 crc_after = calculateRegistersCRC32(amp);
    
    char propName[64];
    snprintf(propName, sizeof(propName), "Cirrus_CRC_Before_%s", amp.name);
    setProperty(propName, crc_before, 32);
    snprintf(propName, sizeof(propName), "Cirrus_CRC_After_%s", amp.name);
    setProperty(propName, crc_after, 32);
    
    CIRRUS_LOG("amplifier %s status after reset: devid=0x%08X revid=0x%08X crc=0x%08X", 
               amp.name, devid_after, revid_after, crc_after);
               
    if (crc_before != crc_after) {
        CIRRUS_LOG("hardware state change detected successfully on %s: 0x%08X -> 0x%08X", 
                   amp.name, crc_before, crc_after);
    } else {
        CIRRUS_LOG("warning: register checksum did not change after reset on %s", amp.name);
    }
    
    UInt32 rev_only = revid_after & 0xFF;
    
    switch (rev_only) {
        case 0xB0:
        case 0xB1:
        case 0xB2:
            CIRRUS_LOG("revision 0x%02X verified for %s", rev_only, amp.name);
            break;
        default:
            CIRRUS_LOG("unknown chip revision 0x%02X found on %s", rev_only, amp.name);
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
    CIRRUS_LOG("starting full register dump for %s", amp.name);
    
    size_t numRegs = sizeof(cs35l41_reg_desc) / sizeof(cs35l41_reg_desc[0]);
    uint32_t successCount = 0;
    uint32_t crc = 0;
    
    // allocate 16kb memory buffer to format the register dump
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
    CIRRUS_LOG("dump complete: %u registers successfully read, crc32: 0x%08X", successCount, crc);
    
    snprintf(lineBuffer, sizeof(lineBuffer), "crc32: 0x%08X\n", crc);
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
    CIRRUS_LOG("starting finite state machine check for %s", amp.name);
    
    uint32_t crcT0 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("t0 checksum for %s: 0x%08X", amp.name, crcT0);
    
    IOSleep(1000); // check at t+1s
    uint32_t crcT1 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("t1 checksum for %s: 0x%08X", amp.name, crcT1);
    
    IOSleep(4000); // check at t+5s
    uint32_t crcT5 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("t5 checksum for %s: 0x%08X", amp.name, crcT5);
    
    IOSleep(25000); // check at t+30s
    uint32_t crcT30 = calculateRegistersCRC32(amp);
    CIRRUS_LOG("t30 checksum for %s: 0x%08X", amp.name, crcT30);
    
    if (crcT0 == crcT1 && crcT1 == crcT5 && crcT5 == crcT30) {
        CIRRUS_LOG("fsm state is stable on %s", amp.name);
    } else {
        CIRRUS_LOG("fsm state is changing on %s", amp.name);
    }
}

bool CirrusAudioFixup::unlockTestKey(CS35L41Amp &amp) {
    bool ret1 = writeRegister(amp, CS35L41_TEST_KEY_CTL, 0x00000055);
    bool ret2 = writeRegister(amp, CS35L41_TEST_KEY_CTL, 0x000000AA);
    
    if (!ret1 || !ret2) {
        CIRRUS_ERR("failed to unlock test keys on %s", amp.name);
        return false;
    }
    
    CIRRUS_LOG("test keys unlocked successfully on %s", amp.name);
    return true;
}

bool CirrusAudioFixup::lockTestKey(CS35L41Amp &amp) {
    bool ret1 = writeRegister(amp, CS35L41_TEST_KEY_CTL, 0x000000CC);
    bool ret2 = writeRegister(amp, CS35L41_TEST_KEY_CTL, 0x00000033);
    
    if (!ret1 || !ret2) {
        CIRRUS_ERR("failed to lock test keys on %s", amp.name);
        return false;
    }
    
    CIRRUS_LOG("test keys locked successfully on %s", amp.name);
    return true;
}

bool CirrusAudioFixup::initializeHardwareErrata(CS35L41Amp &amp) {
    CIRRUS_LOG("starting hardware errata initialization for %s", amp.name);
    
    UInt32 snapshot0[270], snapshot1[270], snapshot2[270], snapshot3[270];
    snapshotRegisters(amp, snapshot0);
    
    // 1. Unlock Test Key
    if (!unlockTestKey(amp)) {
        return false;
    }
    
    snapshotRegisters(amp, snapshot1);
    
    UInt32 crc_unlock = calculateRegistersCRC32(amp);
    char propName[64];
    snprintf(propName, sizeof(propName), "Cirrus_CRC_Unlock_%s", amp.name);
    setProperty(propName, crc_unlock, 32);
    CIRRUS_LOG("checksum after unlock on %s: 0x%08X", amp.name, crc_unlock);
    
    if (bootArgStrEquals("cirrus_phase", "4A2B") || bootArgStrEquals("cirrus_phase", "4A2C")) {
        // 2. Errata Patch
        if (!applyErrataPatch(amp)) {
            return false;
        }
        
        snapshotRegisters(amp, snapshot2);
        CIRRUS_LOG("register diffs after applying errata patch on %s:", amp.name);
        compareRegisterSnapshots(amp, snapshot1, snapshot2);
        
        UInt32 crc_errata = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_Errata_%s", amp.name);
        setProperty(propName, crc_errata, 32);
        CIRRUS_LOG("checksum after errata patch on %s: 0x%08X", amp.name, crc_errata);
    }
    
    if (bootArgStrEquals("cirrus_phase", "4A2C")) {
        // unpack otp values
        if (!unpackOTP(amp)) {
            CIRRUS_ERR("otp unpacking failed on %s, locking test key as rollback", amp.name);
            lockTestKey(amp); // rollback on error
            return false;
        }
        
        snapshotRegisters(amp, snapshot3);
        CIRRUS_LOG("register diffs after unpacking otp on %s:", amp.name);
        compareRegisterSnapshots(amp, snapshot2, snapshot3);
        
        UInt32 crc_otp = calculateRegistersCRC32(amp);
        snprintf(propName, sizeof(propName), "Cirrus_CRC_OTP_%s", amp.name);
        setProperty(propName, crc_otp, 32);
        CIRRUS_LOG("checksum after otp unpack on %s: 0x%08X", amp.name, crc_otp);
    }
    
    // 4. Lock Test Key
    if (!lockTestKey(amp)) {
        return false;
    }
    
    UInt32 crc_lock = calculateRegistersCRC32(amp);
    snprintf(propName, sizeof(propName), "Cirrus_CRC_Lock_%s", amp.name);
    setProperty(propName, crc_lock, 32);
    CIRRUS_LOG("checksum after locking test keys on %s: 0x%08X", amp.name, crc_lock);
    
    CIRRUS_LOG("hardware errata init completed for %s", amp.name);
    return true;
}

bool CirrusAudioFixup::applyErrataPatch(CS35L41Amp &amp) {
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
        CIRRUS_ERR("no errata patches found for revision 0x%02X on %s", rev_only, amp.name);
        return false;
    }
    
    CIRRUS_LOG("applying %lu errata patches for revision 0x%02X on %s", 
               table_to_apply->numPatches, rev_only, amp.name);
               
    for (size_t i = 0; i < table_to_apply->numPatches; i++) {
        if (!writeRegister(amp, table_to_apply->patches[i].reg, table_to_apply->patches[i].value)) {
            CIRRUS_ERR("failed to apply errata patch at register 0x%08X on %s", table_to_apply->patches[i].reg, amp.name);
            return false;
        }
    }
    
    // clear halo core enable bits to prepare for boot
    if (!updateRegisterBits(amp, 0x02BC1000, HALO_CORE_EN, 0x00000000)) {
        CIRRUS_ERR("failed to disable dsp core in core ctrl on %s", amp.name);
        return false;
    }
    
    return true;
}

bool CirrusAudioFixup::unpackOTP(CS35L41Amp &amp) {
    const cs35l41_otp_map_element_t *otp_map_match = nullptr;
    const cs35l41_otp_packed_element_t *otp_map;
    int bit_offset, word_offset, i;
    unsigned int bit_sum = 8;
    UInt32 otp_val;
    UInt32 otp_id_reg;
    UInt32 otp_mem[80];
    
    int elements_processed = 0;
    int elements_skipped = 0;
    int update_bits_calls = 0;
    
    if (!readRegister(amp, 0x00000010, &otp_id_reg)) {
        CIRRUS_ERR("failed to read otp id on %s", amp.name);
        return false;
    }
    
    CIRRUS_LOG("read otpid: 0x%02X on %s", otp_id_reg, amp.name);
    
    for (size_t i = 0; i < ARRAY_SIZE(cs35l41_otp_map_map); i++) {
        if (cs35l41_otp_map_map[i].id == otp_id_reg) {
            otp_map_match = &cs35l41_otp_map_map[i];
            break;
        }
    }
    
    if (!otp_map_match) {
        CIRRUS_ERR("no otp map found matching id %u on %s", otp_id_reg, amp.name);
        return false;
    }
    
    CIRRUS_LOG("selected otp map: %u for %s", otp_id_reg, amp.name);
    CIRRUS_LOG("number of packed otp map elements: %u on %s", otp_map_match->num_elements, amp.name);
    
    // read 80 words (320 bytes) from register 0x00000400 (otp_mem0)
    UInt8 otp_raw_buf[80 * 4];
    if (!bulkRead(amp, 0x00000400, otp_raw_buf, sizeof(otp_raw_buf))) {
        CIRRUS_ERR("failed to read otp memory on %s", amp.name);
        return false;
    }
    
    for (int i = 0; i < 80; i++) {
        // convert big-endian data from i2c buffer into host-endian uint32 words
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
                CIRRUS_ERR("failed to write unpacked otp value at register 0x%08X on %s", otp_map[i].reg, amp.name);
                return false;
            }
        } else {
            elements_skipped++;
        }
    }
    
    CIRRUS_LOG("otp unpacking complete on %s: processed=%d, skipped=%d, register writes=%d",
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
            snapshot[i] = 0xFFFFFFFF; // error marker
        }
    }
}

void CirrusAudioFixup::compareRegisterSnapshots(CS35L41Amp &amp, const UInt32 *oldSnapshot, const UInt32 *newSnapshot) {
    if (!amp.present) return;
    CIRRUS_LOG("comparing register diffs on %s:", amp.name);
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
    
    // export pll config telemetry
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
    { CS35L41_AMP_GAIN_CTRL, 0, 0x00000000, 0, false }, // unmute and set default 0db gain
    { CS35L41_DAC_PCM1_SRC, 0, 0x00000008, 0, false }, // route dac input directly to asp_rx1 (bypass dsp)
    { CS35L41_ASP_TX1_SRC, 0, 0x00000018, 0, false },
    { CS35L41_ASP_TX2_SRC, 0, 0x00000019, 0, false },
    { CS35L41_ASP_TX3_SRC, 0, 0x00000028, 0, false },
    { CS35L41_ASP_TX4_SRC, 0, 0x00000029, 0, false },
    { CS35L41_DSP1_RX1_SRC, 0, 0x00000008, 0, false },
    { CS35L41_DSP1_RX2_SRC, 0, 0x00000008, 0, false },
    { CS35L41_DSP1_RX3_SRC, 0, 0x00000018, 0, false },
    { CS35L41_DSP1_RX4_SRC, 0, 0x00000019, 0, false },
    { CS35L41_DSP1_RX5_SRC, 0, 0x00000029, 0, false },
    { CS35L41_SP_HIZ_CTRL, 0, 0x00000003, 0, false },
    { CS35L41_SP_ENABLES, 0, 0x00010001, 0, false }
};

static const RegisterSequence unmute_dsp_sequence[] = {
    { CS35L41_AMP_DIG_VOL_CTRL, 0, 0x00008000, 0, false }, // set hpf pcm enabled, volume 0.0 db
    { CS35L41_AMP_GAIN_CTRL,    0, 0x00000233, 0, false }  // set gain levels to default 17.5db
};

static const RegisterSequence mute_sequence[] = {
    { CS35L41_AMP_DIG_VOL_CTRL, 0, 0x0000A678, 0, false }, // mute digital volume
    { CS35L41_AMP_GAIN_CTRL,    0, 0x00000000, 0, false }  // set gain levels to 0 db
};


bool CirrusAudioFixup::applyASP(CS35L41Amp &amp) {
    uint32_t crc_before = calculateRegistersCRC32(amp);
    uint64_t startTime = mach_absolute_time();

    if (!applyRegisterSequence(amp, asp_sequence, sizeof(asp_sequence) / sizeof(RegisterSequence))) {
        return false;
    }

    // set asp_frame_rx_slot dynamically based on channel
    uint32_t rx_slot_val = 0;
    readRegister(amp, 0x00004820, &rx_slot_val);
    rx_slot_val &= ~0x3F;
    rx_slot_val |= ((strcmp(amp.name, "right") == 0) ? 1 : 0);
    writeRegister(amp, 0x00004820, rx_slot_val);

    // configure dac_pcm1_src dynamically (0x08 for left, 0x09 for right)
    uint32_t dac_src = (strcmp(amp.name, "right") == 0) ? 0x09 : 0x08;
    writeRegister(amp, CS35L41_DAC_PCM1_SRC, dac_src);

    // configure dsp rx sources dynamically (0x08 for left, 0x09 for right)
    uint32_t dsp_rx_src = (strcmp(amp.name, "right") == 0) ? 0x09 : 0x08;
    writeRegister(amp, CS35L41_DSP1_RX1_SRC, dsp_rx_src);
    writeRegister(amp, CS35L41_DSP1_RX2_SRC, dsp_rx_src);


    uint64_t endTime = mach_absolute_time();
    uint64_t elapsed_us = 0;
    absolutetime_to_nanoseconds(endTime - startTime, &elapsed_us);
    elapsed_us /= 1000;

    uint32_t crc_after = calculateRegistersCRC32(amp);
    
    // export asp config telemetry
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
    
    // export gpio config telemetry
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
        // skip disabled, powered-off, or unpopulated pci devices returning 0xffff/0xffffffff
        uint32_t pciVendor = 0;
        OSData *venData = OSDynamicCast(OSData, service->getProperty("vendor-id"));
        if (venData && venData->getLength() >= 4) {
            pciVendor = *((uint32_t*)venData->getBytesNoCopy());
        }
        if (pciVendor == 0xFFFF || pciVendor == 0xFFFFFFFF) {
            continue;
        }

        int score = 0;
        
        OSData *classCodeData = OSDynamicCast(OSData, service->getProperty("class-code"));
        if (classCodeData && classCodeData->getLength() >= 3) {
            const uint8_t *bytes = (const uint8_t*)classCodeData->getBytesNoCopy();
            // check if base/subclass is multimedia audio controller
            if (bytes[2] == 0x04 && bytes[1] == 0x03) {
                score += 10;
            }
        }
        
        const char *name = service->getName();
        if (name) {
            if (strcmp(name, "HDEF") == 0) score += 5;
            else if (strcmp(name, "HDAS") == 0) score += 3;
            else if (strcmp(name, "HDAU") == 0) score -= 10; // penalize hdmi audio devices
        }
        
        if (score > bestScore) {
            bestScore = score;
            bestController = service;
        }
    }
    
    if (bestController) {
        bestController->retain(); // caller is responsible for releasing
    }
    iter->release();
    return bestScore >= 0 ? bestController : nullptr;
}

#include "Codecs/CS35L41/FirmwareDatabase.hpp"

// compute simple crc32 hash for firmware verification
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

void CirrusAudioFixup::discoverFirmware(CS35L41Amp &amp) {
    CIRRUS_LOG("discovering firmware for amplifier: %s", amp.name);
    
    amp.firmwareValidated = false;
    
    char propStatus[64];
    snprintf(propStatus, sizeof(propStatus), "Cirrus_Phase5A_Status_%s", amp.name);
    
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
        
        // save pci properties for debug
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
    
    // fall back if pci info isn't found in registry or is invalid
    if (subVendor == 0 || subDevice == 0 || 
        subVendor == 0xFFFF || subDevice == 0xFFFF || 
        subVendor == 0xFFFFFFFF || subDevice == 0xFFFFFFFF) {
        CIRRUS_LOG("audio controller pci not found or invalid (subVendor=0x%08X, subDevice=0x%08X), using default 17aa:3847", subVendor, subDevice);
        subVendor = 0x17AA;
        subDevice = 0x3847;
    }
    
    uint32_t ssid = (subVendor << 16) | subDevice;
    int spkid = 0;
    
    char propSSID[64];
    snprintf(propSSID, sizeof(propSSID), "Cirrus_SSID_%s", amp.name);
    setProperty(propSSID, (uint64_t)ssid, 32);
    
    // match appropriate firmware resource from static table
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
        CIRRUS_ERR("firmware resource matching ssid %08X and spkid %d was not found", ssid, spkid);
        OSString *statusStr = OSString::withCString("UNSUPPORTED_SSID");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }
    
    if (foundRes->wmfw == nullptr || foundRes->bin == nullptr) {
        CIRRUS_ERR("missing firmware binary or tuning files for ssid %08X", ssid);
        OSString *statusStr = OSString::withCString("FILE_NOT_FOUND");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }
    
    if (foundRes->wmfwSize == 0 || foundRes->binSize == 0) {
        CIRRUS_ERR("invalid firmware sizes for ssid %08X", ssid);
        OSString *statusStr = OSString::withCString("INVALID_RESOURCE");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }
    
    if ((foundRes->wmfwSize % 4 != 0) || (foundRes->binSize % 4 != 0)) {
        CIRRUS_ERR("firmware files alignment must be 4-byte on ssid %08X", ssid);
        OSString *statusStr = OSString::withCString("INVALID_ALIGNMENT");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }
    
    if (foundRes->wmfwSize >= 4 && foundRes->wmfw[0] == 'W' && foundRes->wmfw[1] == 'M' && foundRes->wmfw[2] == 'F' && foundRes->wmfw[3] == 'W') {
        // magic header matches successfully
    } else {
        CIRRUS_ERR("wmfw magic header verification failed for ssid %08X", ssid);
        OSString *statusStr = OSString::withCString("INVALID_RESOURCE");
        if (statusStr) {
            setProperty(propStatus, statusStr);
            statusStr->release();
        }
        return;
    }
    
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
    
    uint32_t fwCrc = calculate_crc32(foundRes->wmfw, foundRes->wmfwSize);
    uint32_t binCrc = calculate_crc32(foundRes->bin, foundRes->binSize);
    snprintf(propFW, sizeof(propFW), "Cirrus_FW_CRC32_%s", amp.name); setProperty(propFW, (uint64_t)fwCrc, 32);
    snprintf(propFW, sizeof(propFW), "Cirrus_BIN_CRC32_%s", amp.name); setProperty(propFW, (uint64_t)binCrc, 32);
    
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
    
    amp.wmfwData = foundRes->wmfw;
    amp.wmfwSize = foundRes->wmfwSize;
    amp.binData = foundRes->bin;
    amp.binSize = foundRes->binSize;
    amp.firmwareValidated = true;
    
    CIRRUS_LOG("firmware matches found: %s (size: %lu, version: %08X, crc: %08X), tuning: %s (size: %lu, version: %08X, crc: %08X)",
               foundRes->fwName, amp.wmfwSize, fwVersion, fwCrc, foundRes->binName, amp.binSize, binVersion, binCrc);
    OSString *statusStr = OSString::withCString("READY");
    if (statusStr) {
        setProperty(propStatus, statusStr);
        statusStr->release();
    }
}

void CirrusAudioFixup::bringupDSP(CS35L41Amp &amp) {
    CIRRUS_LOG("releasing reset on dsp core for %s", amp.name);
    
    char propName[64];
    OSString *statusStr = nullptr;
    
    uint32_t pre_core_ctrl = 0, pre_clk_ctrl = 0, pre_mbox = 0;
    uint32_t pre_sys_id = 0, pre_sys_ver = 0, pre_sys_core = 0;
    readRegister(amp, CS35L41_DSP1_CCM_CORE_CTRL, &pre_core_ctrl);
    readRegister(amp, CS35L41_DSP_CLK_CTRL, &pre_clk_ctrl);
    readRegister(amp, CS35L41_DSP_MBOX_2, &pre_mbox);
    readRegister(amp, CS35L41_DSP1_SYS_ID, &pre_sys_id);
    readRegister(amp, CS35L41_DSP1_SYS_VERSION, &pre_sys_ver);
    readRegister(amp, CS35L41_DSP1_SYS_CORE_ID, &pre_sys_core);
    CIRRUS_LOG("dsp pre-reset registers on %s: core=0x%08X, clk=0x%08X, mbox=0x%08X, sysid=0x%08X, version=0x%08X, coreid=0x%08X", 
               amp.name, pre_core_ctrl, pre_clk_ctrl, pre_mbox, pre_sys_id, pre_sys_ver, pre_sys_core);
    
    uint64_t reset_start = mach_absolute_time();
    
    // release reset and enable dsp core
    updateRegisterBits(amp, CS35L41_DSP1_CCM_CORE_CTRL, HALO_CORE_RESET | HALO_CORE_EN, HALO_CORE_RESET | HALO_CORE_EN);
    updateRegisterBits(amp, CS35L41_DSP1_CCM_CORE_CTRL, HALO_CORE_RESET, 0);
    
    uint64_t reset_end = mach_absolute_time();
    uint64_t reset_time_us = (reset_end - reset_start) / 1000;
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_RESET_TIME_US_%s", amp.name);
    setProperty(propName, reset_time_us, 32);
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_RESET_%s", amp.name);
    statusStr = OSString::withCString("OK");
    if (statusStr) { setProperty(propName, statusStr); statusStr->release(); }
    
    uint32_t current_clk = 0;
    readRegister(amp, CS35L41_DSP_CLK_CTRL, &current_clk);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_CLOCK_RAW_%s", amp.name);
    setProperty(propName, (uint64_t)current_clk, 32);
    
    uint64_t mpu_start = mach_absolute_time();
    
    // unlock memory protection window configurations
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
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_MEM_WINDOW_%s", amp.name);
    statusStr = OSString::withCString("OK");
    if (statusStr) { setProperty(propName, statusStr); statusStr->release(); }
    
    uint32_t sys_id = 0, sys_ver = 0, sys_core = 0;
    readRegister(amp, CS35L41_DSP1_SYS_ID, &sys_id);
    readRegister(amp, CS35L41_DSP1_SYS_VERSION, &sys_ver);
    readRegister(amp, CS35L41_DSP1_SYS_CORE_ID, &sys_core);
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_ID_%s", amp.name); setProperty(propName, (uint64_t)sys_id, 32);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_VER_%s", amp.name); setProperty(propName, (uint64_t)sys_ver, 32);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_SYS_CORE_%s", amp.name); setProperty(propName, (uint64_t)sys_core, 32);
    
    uint32_t post_core_ctrl = 0, post_clk_ctrl = 0, post_mbox = 0;
    readRegister(amp, CS35L41_DSP1_CCM_CORE_CTRL, &post_core_ctrl);
    readRegister(amp, CS35L41_DSP_CLK_CTRL, &post_clk_ctrl);
    readRegister(amp, CS35L41_DSP_MBOX_2, &post_mbox);
    CIRRUS_LOG("dsp post-reset registers on %s: core=0x%08X, clk=0x%08X, mbox=0x%08X, sysid=0x%08X, version=0x%08X, coreid=0x%08X", 
               amp.name, post_core_ctrl, post_clk_ctrl, post_mbox, sys_id, sys_ver, sys_core);
    
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
    
    uint32_t mbox_init = 0;
    readRegister(amp, CS35L41_DSP_MBOX_2, &mbox_init);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_MAILBOX_RAW_INIT_%s", amp.name);
    setProperty(propName, (uint64_t)mbox_init, 32);
    
    uint32_t pre_halo_state = 0, pre_cal_status = 0, pre_max_temp = 0;
    readRegister(amp, 0x02800250, &pre_halo_state);
    readRegister(amp, 0x02800270, &pre_cal_status);
    readRegister(amp, 0x02800358, &pre_max_temp);
    CIRRUS_LOG("dsp control values before start on %s: state=0x%08X, cal=0x%08X, temp=0x%08X", amp.name, pre_halo_state, pre_cal_status, pre_max_temp);
    CIRRUS_LOG("dsp bringup complete for %s", amp.name);
}

bool CirrusAudioFixup::verifyDSPAlive(CS35L41Amp &amp) {
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

    CIRRUS_LOG("dsp status verify results for %s: core_reset=%s, core_en=%s, mailbox=%s, xm_read=%s",
               amp.name, reset_pass ? "ok" : "fail", core_pass ? "ok" : "fail", mbox_pass ? "ok" : "fail", xm_pass ? "ok" : "fail");

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
            uint32_t term = c * (size_log2 - c_log2);
            total_entropy_x1000 += term;
        }
    }
    
    return (uint32_t)(total_entropy_x1000 / size / 100);
}

void CirrusAudioFixup::parseDSPAlgorithms(CS35L41Amp &amp, FirmwareImage &outImage) {
    CIRRUS_LOG("starting xm ram dump and algorithm parsing for %s", amp.name);
    
    uint32_t dump_size = 16384;

    uint8_t *xm_dump_buffer = (uint8_t *)IOMallocData(dump_size);
    if (!xm_dump_buffer) {
        CIRRUS_ERR("failed to allocate memory buffer for xm dump");
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
        
        uint32_t read_addr = 0x02800000 + offset;
        if (!bulkRead(amp, read_addr, xm_dump_buffer + offset, read_len, TRACE_DUMP)) {
            CIRRUS_ERR("failed to read xm ram at offset 0x%08X", read_addr);
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
        
        uint32_t zero_pct = (zeroes * 100) / dump_size;
        uint32_t ff_pct = (ffs * 100) / dump_size;
        uint32_t entropy_x10 = compute_entropy_x10(xm_dump_buffer, dump_size);
        
        CIRRUS_LOG("xm dump results for %s: base=0x02800000 size=%u crc=0x%08X time=%u ms", amp.name, dump_size, crc, dump_time_ms);
        CIRRUS_LOG("xm data validation for %s: zeroes=%u%% ffs=%u%% entropy=%u.%u", amp.name, zero_pct, ff_pct, entropy_x10 / 10, entropy_x10 % 10);
        
        bool is_valid = (zero_pct < 100 && ff_pct < 100 && crc != 0);
        if (!is_valid) {
            CIRRUS_ERR("xm dump validation failed on %s", amp.name);
        } else {
            outImage.xm_dump_crc = crc;
            
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

void CirrusAudioFixup::uploadFirmware(CS35L41Amp &amp, const char* phaseArg) {
    CIRRUS_LOG("starting firmware upload on %s", amp.name);
    
    if (!amp.wmfwData || amp.wmfwSize == 0) {
        CIRRUS_LOG("no wmfw firmware data present on %s, skipping upload", amp.name);
        return;
    }
    
    FirmwareImage *image = (FirmwareImage *)IOMalloc(sizeof(FirmwareImage));
    if (!image) {
        CIRRUS_ERR("failed to allocate memory for firmware image on %s", amp.name);
        return;
    }

    if (!CirrusFirmwareParser::parseWMFW(amp.wmfwData, amp.wmfwSize, image)) {
        CIRRUS_ERR("wmfw parsing failed on %s", amp.name);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    MappedImage *mappedImg = (MappedImage *)IOMalloc(sizeof(MappedImage));
    if (!mappedImg) {
        CIRRUS_ERR("failed to allocate memory for mapped image on %s", amp.name);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    if (!CirrusFirmwareMapper::mapFirmwareImage(*image, *mappedImg)) {
        CIRRUS_ERR("firmware mapping failed on %s", amp.name);
        IOFree(mappedImg, sizeof(MappedImage));
        IOFree(image, sizeof(FirmwareImage));
        return;
    }
    
    UploadSession session;
    if (CirrusFirmwareScheduler::run(amp, this, *mappedImg, session)) {
        CIRRUS_LOG("firmware upload complete on %s", amp.name);
    } else {
        CIRRUS_ERR("firmware upload failed on %s", amp.name);
    }

    IOFree(mappedImg, sizeof(MappedImage));
    IOFree(image, sizeof(FirmwareImage));
}

void CirrusAudioFixup::configureHardware(CS35L41Amp &amp) {
    CIRRUS_LOG("configuring generic serial port settings on %s", amp.name);
    
    // set hardware interrupt masks
    writeRegister(amp, 0x00010110, 0x2000003F);
    writeRegister(amp, 0x00010114, 0x80020003);
    writeRegister(amp, 0x00010118, 0x41406000);
    writeRegister(amp, 0x0001011C, 0x2000000F);
    
    // enable serial port control overrides
    writeRegister(amp, 0x00002020, 0x00000006);
    
    CIRRUS_LOG("generic serial port config complete on %s", amp.name);
}

void CirrusAudioFixup::logASPSnapshot(CS35L41Amp &amp) {
    uint32_t frm_ctrl = 0, fmt = 0, clk = 0;
    uint32_t tx_wl = 0, rx_wl = 0, tx_slot = 0, rx_slot = 0;
    uint32_t rx1_src = 0, rx2_src = 0, rx3_src = 0, rx4_src = 0, rx5_src = 0;
    
    readRegister(amp, 0x00004818, &frm_ctrl);
    readRegister(amp, 0x0000480C, &fmt);
    readRegister(amp, 0x00004800, &clk);
    readRegister(amp, 0x00004810, &tx_slot);
    readRegister(amp, 0x00004820, &rx_slot);
    readRegister(amp, 0x00004830, &tx_wl);
    readRegister(amp, 0x00004840, &rx_wl);
    readRegister(amp, 0x00004C40, &rx1_src);
    readRegister(amp, 0x00004C44, &rx2_src);
    readRegister(amp, 0x00004C48, &rx3_src);
    readRegister(amp, 0x00004C4C, &rx4_src);
    readRegister(amp, 0x00004C50, &rx5_src);
    
    CIRRUS_LOG("asp snapshot for %s: frame_ctrl=0x%08X format=0x%08X clk=0x%08X", amp.name, frm_ctrl, fmt, clk);
    CIRRUS_LOG("asp snapshot slots for %s: rx_slot=0x%08X tx_slot=0x%08X rx_wl=0x%08X tx_wl=0x%08X", amp.name, rx_slot, tx_slot, rx_wl, tx_wl);
    CIRRUS_LOG("asp snapshot routing for %s: rx1_src=0x%08X rx2_src=0x%08X rx3_src=0x%08X rx4_src=0x%08X rx5_src=0x%08X", amp.name, rx1_src, rx2_src, rx3_src, rx4_src, rx5_src);
    
    bool pass = true;
    uint32_t expected_rx_slot = (strcmp(amp.name, "right") == 0) ? 1 : 0;
    if ((rx_slot & 0x3F) != expected_rx_slot) { CIRRUS_ERR("asp rx slot mismatch on %s", amp.name); pass = false; }
    
    uint32_t expected_rx_src = (strcmp(amp.name, "right") == 0) ? 0x09 : 0x08;
    if (rx1_src != expected_rx_src) { CIRRUS_ERR("asp rx1 source routing mismatch on %s", amp.name); pass = false; }
    if (rx2_src != expected_rx_src) { CIRRUS_ERR("asp rx2 source routing mismatch on %s", amp.name); pass = false; }
    
    if (pass) {
        CIRRUS_LOG("asp validation check: pass on %s", amp.name);
    } else {
        CIRRUS_ERR("asp validation check: fail on %s", amp.name);
    }
}

void CirrusAudioFixup::logDSPSnapshot(CS35L41Amp &amp) {
    uint32_t halo_state = 0, dsp_state = 0, mbox1 = 0, mbox2 = 0;
    readRegister(amp, 0x00013004, &mbox2);
    readRegister(amp, 0x00013020, &mbox1);
    readRegister(amp, 0x02BC1000, &dsp_state);
    
    CIRRUS_LOG("dsp status snapshot for %s: state=0x%08X core=0x%08X mbox1=%u mbox2=%u", amp.name, mbox2, dsp_state, mbox1, mbox2);
}

void CirrusAudioFixup::snapshotPlayback(CS35L41Amp &amp) {
    uint32_t irq1_sts1 = 0, irq1_sts2 = 0, irq1_sts3 = 0, irq1_sts4 = 0;
    uint32_t pwrmgt_sts = 0;
    
    readRegister(amp, 0x00010010, &irq1_sts1);
    readRegister(amp, 0x00010014, &irq1_sts2);
    readRegister(amp, 0x00010018, &irq1_sts3);
    readRegister(amp, 0x0001001C, &irq1_sts4);
    readRegister(amp, 0x00002908, &pwrmgt_sts);
    
    CIRRUS_LOG("playback interrupts snapshot for %s: irq1=0x%08X irq2=0x%08X irq3=0x%08X irq4=0x%08X power_status=0x%08X", 
               amp.name, irq1_sts1, irq1_sts2, irq1_sts3, irq1_sts4, pwrmgt_sts);
}

void CirrusAudioFixup::snapshotDiagnostics(CS35L41Amp &amp, const char* stage) {
    uint32_t irq1[4] = {0}, irq1_mask[4] = {0};
    uint32_t irq2[4] = {0}, irq2_mask[4] = {0};
    uint32_t sp_en = 0, sp_rate = 0, sp_fmt = 0, sp_hiz = 0;
    uint32_t pwr_sts = 0, strm_err = 0;

    CIRRUS_LOG("diagnostics dump (%s) for amplifier %s:", stage, amp.name);

    readRegister(amp, 0x00010010, &irq1[0]);
    readRegister(amp, 0x00010014, &irq1[1]);
    readRegister(amp, 0x00010018, &irq1[2]);
    readRegister(amp, 0x0001001C, &irq1[3]);
    
    bool pup_done = (irq1[0] & 0x01000000) != 0;
    bool amp_short = (irq1[0] & 0x80000000) != 0;
    bool dsp_error = (irq1[0] & 0x00000002) != 0; 
    bool pll_lock = (irq1[2] & 0x00000002) != 0;
    
    CIRRUS_LOG("decoded interrupts on %s: pup_done=%d amp_short=%d dsp_err=%d pll_lock=%d", amp.name, pup_done, amp_short, dsp_error, pll_lock);
    CIRRUS_LOG("irq1 status values on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X", amp.name, irq1[0], irq1[1], irq1[2], irq1[3]);

    readRegister(amp, 0x00010110, &irq1_mask[0]);
    readRegister(amp, 0x00010114, &irq1_mask[1]);
    readRegister(amp, 0x00010118, &irq1_mask[2]);
    readRegister(amp, 0x0001011C, &irq1_mask[3]);
    CIRRUS_LOG("irq1 mask registers on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X", amp.name, irq1_mask[0], irq1_mask[1], irq1_mask[2], irq1_mask[3]);

    readRegister(amp, 0x00010810, &irq2[0]);
    readRegister(amp, 0x00010814, &irq2[1]);
    readRegister(amp, 0x00010818, &irq2[2]);
    readRegister(amp, 0x0001081C, &irq2[3]);
    CIRRUS_LOG("irq2 status registers on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X", amp.name, irq2[0], irq2[1], irq2[2], irq2[3]);

    readRegister(amp, 0x00010910, &irq2_mask[0]);
    readRegister(amp, 0x00010914, &irq2_mask[1]);
    readRegister(amp, 0x00010918, &irq2_mask[2]);
    readRegister(amp, 0x0001091C, &irq2_mask[3]);
    CIRRUS_LOG("irq2 mask registers on %s: 1=0x%08X 2=0x%08X 3=0x%08X 4=0x%08X", amp.name, irq2_mask[0], irq2_mask[1], irq2_mask[2], irq2_mask[3]);

    readRegister(amp, 0x00004800, &sp_en);
    readRegister(amp, 0x00004804, &sp_rate);
    readRegister(amp, 0x00004808, &sp_fmt);
    readRegister(amp, 0x0000480C, &sp_hiz);
    CIRRUS_LOG("serial port configuration for %s: enables=0x%08X rate=0x%08X", amp.name, sp_en, sp_rate);
    CIRRUS_LOG("serial port format for %s: format=0x%08X hiz=0x%08X", amp.name, sp_fmt, sp_hiz);

    readRegister(amp, 0x00002908, &pwr_sts);
    readRegister(amp, 0x02BC5A08, &strm_err);
    CIRRUS_LOG("power status on %s: power_mgt=0x%08X arb_error=0x%08X", amp.name, pwr_sts, strm_err);
    
    uint32_t dsp_ts = 0, mdsync_rx = 0;
    readRegister(amp, 0x025C0800, &dsp_ts);
    readRegister(amp, 0x00003420, &mdsync_rx);
    CIRRUS_LOG("dsp execution registers on %s: timestamp=0x%08X mdsync=0x%08X", amp.name, dsp_ts, mdsync_rx);
    
    uint32_t dsp_mbox1 = 0, dsp_mbox2 = 0;
    readRegister(amp, 0x00013020, &dsp_mbox1);
    readRegister(amp, 0x00013004, &dsp_mbox2);
    CIRRUS_LOG("mailbox states on %s: mbox1=0x%08X mbox2=0x%08X", amp.name, dsp_mbox1, dsp_mbox2);

    for (uint32_t i = 0; i < amp.diagnosticControlCount; i++) {
        uint32_t val = 0;
        readRegister(amp, amp.diagnosticControls[i].address, &val);
        CIRRUS_LOG("control register on %s: %s (0x%08X) = 0x%08X", amp.name, amp.diagnosticControls[i].name, amp.diagnosticControls[i].address, val);
    }
}

void CirrusAudioFixup::logPowerSnapshot(CS35L41Amp &amp) {
    uint32_t pwr_ctrl1 = 0, pwr_ctrl2 = 0, pwr_ctrl3 = 0;
    readRegister(amp, 0x00002014, &pwr_ctrl1);
    readRegister(amp, 0x00002018, &pwr_ctrl2);
    readRegister(amp, 0x0000201C, &pwr_ctrl3);
    
    CIRRUS_LOG("power rails snapshot for %s: ctrl1=0x%08X ctrl2=0x%08X ctrl3=0x%08X", amp.name, pwr_ctrl1, pwr_ctrl2, pwr_ctrl3);
    
    bool pass = true;
    if ((pwr_ctrl1 & 1) == 0) { CIRRUS_ERR("global enable verify failed on %s", amp.name); pass = false; }
    if ((pwr_ctrl2 & 1) == 0) { CIRRUS_ERR("amplifier stage enable verify failed on %s", amp.name); pass = false; }
    if ((pwr_ctrl2 & 0x00003000) != 0x00003000) { CIRRUS_ERR("current and voltage monitors verify failed on %s", amp.name); pass = false; }
    
    if (pass) {
        CIRRUS_LOG("power verify results: pass on %s", amp.name);
    } else {
        CIRRUS_ERR("power verify results: fail on %s", amp.name);
    }
}

void CirrusAudioFixup::powerUpAmplifier(CS35L41Amp &amp) {
    CIRRUS_LOG("starting power up sequence for %s", amp.name);
    
    // unlock test register write permissions
    writeRegister(amp, 0x00000040, 0x00000055);
    writeRegister(amp, 0x00000040, 0x000000AA);
    
    // safe-to-active transition sequence startup
    writeRegister(amp, 0x0000742C, 0x0000000F);
    writeRegister(amp, 0x0000742C, 0x00000079);
    writeRegister(amp, 0x00007438, 0x00585941);
    
    // write global enable register bit
    uint32_t pwr_ctrl1 = 0;
    readRegister(amp, 0x00002014, &pwr_ctrl1);
    pwr_ctrl1 |= (1 << 0);
    writeRegister(amp, 0x00002014, pwr_ctrl1);
    
    logDSPSnapshot(amp);
    
    // wait for power up complete interrupt status bit
    uint32_t irq1_sts1 = 0;
    int timeout = 100;
    while (timeout > 0) {
        readRegister(amp, 0x00010010, &irq1_sts1);
        if (irq1_sts1 & 0x01000000) {
            break;
        }
        IODelay(1000);
        timeout--;
    }
    
    if (irq1_sts1 & 0x01000000) {
        CIRRUS_LOG("power up complete on %s, clearing interrupt", amp.name);
        writeRegister(amp, 0x00010010, 0x01000000);
        
        // safe-to-active speaker path enable config
        writeRegister(amp, 0x0000742C, 0x000000F9);
        writeRegister(amp, 0x00007438, 0x00580941);
    } else {
        CIRRUS_ERR("timeout waiting for power up complete status on %s", amp.name);
    }
    
    // lock write permissions to test registers
    writeRegister(amp, 0x00000040, 0x000000CC);
    writeRegister(amp, 0x00000040, 0x00000033);
    
    // enable hardware monitors and amplifier stage
    uint32_t pwr_ctrl2_old = 0;
    readRegister(amp, 0x00002018, &pwr_ctrl2_old);
    updateRegisterBits(amp, 0x00002018, 0x00003001, 0x00003001);
    
    uint32_t pwr_ctrl2_verify = 0;
    readRegister(amp, 0x00002018, &pwr_ctrl2_verify);
    CIRRUS_LOG("amplifier stage enabled on %s: 0x%08X -> 0x%08X", amp.name, pwr_ctrl2_old, pwr_ctrl2_verify);
    
    // configure default digital volume levels
    uint32_t dig_vol = 0, gain_ctrl = 0;
    readRegister(amp, CS35L41_AMP_DIG_VOL_CTRL, &dig_vol);
    readRegister(amp, CS35L41_AMP_GAIN_CTRL, &gain_ctrl);
    CIRRUS_LOG("default volume states on %s: digital_volume=0x%08X gain=0x%08X", amp.name, dig_vol, gain_ctrl);
    
    if (dig_vol != 0x00008000) {
        CIRRUS_LOG("unmuting digital volume control on %s", amp.name);
        writeRegister(amp, CS35L41_AMP_DIG_VOL_CTRL, 0x00008000);
    } else {
        CIRRUS_LOG("digital volume is already unmuted on %s", amp.name);
    }
    
    if (gain_ctrl != 0x00000000) {
        CIRRUS_LOG("setting default speaker gain register to 0dB on %s", amp.name);
        writeRegister(amp, CS35L41_AMP_GAIN_CTRL, 0x00000000);
    } else {
        CIRRUS_LOG("speaker gain is already at default value on %s", amp.name);
    }
    
    logPowerSnapshot(amp);
    snapshotDiagnostics(amp, "IDLE (POST-BOOT)");
}
