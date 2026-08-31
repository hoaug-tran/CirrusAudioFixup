#include "CirrusAudioFixup.hpp"
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
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
        if (mProbeTimer) {
            mProbeTimer->setTimeoutMS(2000);
        }
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

        if (amp.wmfwData && amp.wmfwSize > 0 && amp.binData && amp.binSize > 0) {
            initializeFirmware(amp, "5D.0");
        }
        
        // log current serial port configuration
        logASPSnapshot(amp);
        
        // power up the amplifier stage
        powerUpAmplifier(amp);
        // Mark hardware init done. dspAlive is only set true once HALO reaches RUN.
        // If DSP stalled (clock not available at boot time), initialized stays true to
        // skip OTP/errata re-init, but dspAlive=false lets the monitor trigger a DSP
        // restart once I2S BCLK arrives and pll_lock fires.
        amp.initialized = true;
        amp.dspAlive = verifyDSPAlive(amp);
        
        CIRRUS_LOG("amplifier %s initialized successfully (dspAlive=%d)", amp.name, amp.dspAlive);
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
    amp.firmwareValidated = false;
    amp.dspAlive = false;
    amp.monitorCount = 0;

    if (!amp.wmfwData || amp.wmfwSize == 0 || !amp.binData || amp.binSize == 0) {
        CIRRUS_ERR("firmware or tuning data missing for %s; keeping DSP stopped", amp.name);
        return;
    }

    bool wmfwUploaded = false;
    bool coefficientsUploaded = false;

    // STOP the DSP BEFORE uploading anything. If the core is still running from a
    // previous OS/BIOS, uploading firmware would overwrite its memory mid-execution,
    // crashing it and latching an MPU violation until a soft-reset. Safe here because
    // the DSP clock has already been enabled by applyPLL earlier in the boot flow.
    stopDSP(amp);
    
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
            wmfwUploaded = CirrusFirmwareScheduler::run(amp, this, *wmfwMapped, session);
        } else {
            CIRRUS_ERR("failed to map wmfw image on %s", amp.name);
        }
        IOFree(wmfwMapped, sizeof(MappedImage));
    }
    if (!wmfwUploaded) {
        CIRRUS_ERR("wmfw upload failed on %s; keeping DSP stopped", amp.name);
        IOFree(image, sizeof(FirmwareImage));
        return;
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
    
    // Parse and upload tuning. Production DSP mode requires a complete BIN.
    if (CirrusFirmwareParser::parseBIN(amp.binData, amp.binSize, image)) {
        CIRRUS_LOG("bin parsing successful on %s, found %u coefficient blocks", amp.name, image->coefficientCount);
        MappedImage *coeffMapped = (MappedImage *)IOMalloc(sizeof(MappedImage));
        if (coeffMapped) {
            if (CirrusFirmwareMapper::mapCoefficients(*image, *coeffMapped) && coeffMapped->regionCount > 0) {
                CIRRUS_LOG("starting coefficient file upload on %s", amp.name);
                UploadSession session;
                coefficientsUploaded = CirrusFirmwareScheduler::run(amp, this, *coeffMapped, session);
            } else {
                CIRRUS_ERR("coefficient mapping produced no uploadable regions on %s", amp.name);
            }
            IOFree(coeffMapped, sizeof(MappedImage));
        }
    } else {
        CIRRUS_ERR("bin file parsing failed on %s", amp.name);
    }

    if (!coefficientsUploaded) {
        CIRRUS_ERR("coefficient upload failed on %s; keeping DSP stopped and using bypass routing", amp.name);
        stopDSP(amp);
        IOFree(image, sizeof(FirmwareImage));
        return;
    }

    amp.firmwareValidated = true;

    // start dsp only after firmware and every coefficient region verified
    CIRRUS_LOG("bringing up dsp controller on %s", amp.name);
    bringupDSP(amp);
    
    if (verifyDSPAlive(amp)) {
        CIRRUS_LOG("dsp is successfully verified alive on %s", amp.name);
        amp.dspAlive = true;
        amp.monitorCount = 1;
    } else {
        CIRRUS_ERR("dsp bringup failed or dsp is unresponsive on %s; disabling DSP mode", amp.name);
        amp.firmwareValidated = false;
        amp.dspAlive = false;
        amp.monitorCount = 0;
        stopDSP(amp);
    }

    // Always emit the consolidated boot report (pass or fail) so the log always
    // carries a single-block root-cause verdict for this amp.
    logDSPBootReport(amp);

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
        if (self->bootArgEnabled("cirrus_readonly")) {
            self->runReadOnlyProbe();
        } else {
            self->runBackgroundMonitor();
        }
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

static uint32_t executeHdaVerbInternal(IOMemoryMap *map, uint8_t codecAddr, uint8_t nid, uint16_t verb, uint16_t param) {
    if (!map) return 0;
    volatile uint8_t *base = (volatile uint8_t *)map->getVirtualAddress();
    if (!base) return 0;

    volatile uint32_t *ic  = (volatile uint32_t*)(base + 0x60);
    volatile uint32_t *ir  = (volatile uint32_t*)(base + 0x64);
    volatile uint16_t *irs = (volatile uint16_t*)(base + 0x68);

    int timeout = 1000;
    while ((*irs & 0x0001) && timeout > 0) {
        IODelay(1);
        timeout--;
    }
    if (*irs & 0x0001) {
        CIRRUS_ERR("HDA Verb Timeout (ICB busy): NID 0x%02X verb 0x%03X param 0x%04X", nid, verb, param);
        return 0;
    }

    uint32_t payload = 0;
    if ((verb & 0xFF) == 0x00) { // 4-bit verb (e.g. 0x200, 0x400, 0x500) with 16-bit param
        payload = (((uint32_t)(verb >> 8) & 0xF) << 16) | (param & 0xFFFF);
    } else { // 12-bit verb (e.g. 0x706, 0x70C, 0xF06) with 8-bit param
        payload = (((uint32_t)verb & 0xFFF) << 8) | (param & 0xFF);
    }

    uint32_t cmd = ((uint32_t)(codecAddr & 0xF) << 28) |
                   ((uint32_t)(nid & 0x7F) << 20) |
                   (payload & 0xFFFFF);
    *ic = cmd;
    *irs = 0x0003;

    timeout = 1000;
    while (!(*irs & 0x0002) && timeout > 0) {
        IODelay(1);
        timeout--;
    }
    if (!(*irs & 0x0002)) {
        CIRRUS_ERR("HDA Verb Timeout (IRV not set): NID 0x%02X payload 0x%05X irs 0x%04X", nid, payload, *irs);
        return 0;
    }

    return *ir;
}

void CirrusAudioFixup::syncAlc287HdaCodec() {
    IOService *audioCtrl = getAudioController();
    if (!audioCtrl) return;
    
    IOPCIDevice *pciDev = OSDynamicCast(IOPCIDevice, audioCtrl);
    if (!pciDev) {
        audioCtrl->release();
        return;
    }

    IOMemoryMap *map = pciDev->mapDeviceMemoryWithRegister(0x10);
    if (!map) {
        audioCtrl->release();
        return;
    }

    // Always keep EAPD powered ON for external amps (Node 0x14, 0x1B, 0x21)
    executeHdaVerbInternal(map, 0, 0x14, 0x70C, 0x02);
    executeHdaVerbInternal(map, 0, 0x1B, 0x70C, 0x02);
    executeHdaVerbInternal(map, 0, 0x21, 0x70C, 0x02);

    uint32_t res02 = executeHdaVerbInternal(map, 0, 0x02, 0xF06, 0x00);
    uint8_t stream02 = (res02 >> 4) & 0x0F;
    uint8_t chan02   = res02 & 0x0F;
    uint32_t fmt02   = executeHdaVerbInternal(map, 0, 0x02, 0xA00, 0x00) & 0xFFFF;

    uint32_t res03 = executeHdaVerbInternal(map, 0, 0x03, 0xF06, 0x00);
    uint8_t stream03 = (res03 >> 4) & 0x0F;
    uint8_t chan03   = res03 & 0x0F;
    uint32_t fmt03   = executeHdaVerbInternal(map, 0, 0x03, 0xA00, 0x00) & 0xFFFF;

    uint32_t res06 = executeHdaVerbInternal(map, 0, 0x06, 0xF06, 0x00);
    uint8_t stream06 = (res06 >> 4) & 0x0F;

    uint8_t activeStream = (stream06 != 0) ? stream06 : ((stream03 != 0) ? stream03 : stream02);

    if (activeStream != 0) {
        uint32_t fmt = (fmt03 != 0) ? fmt03 : ((fmt02 != 0) ? fmt02 : 0x0031); // 24-bit 48kHz I2S
        uint8_t chan = (stream03 != 0) ? chan03 : chan02;

        // Clone active stream tag and format to Node 0x06 (I2S DAC)
        executeHdaVerbInternal(map, 0, 0x06, 0x200, fmt);
        executeHdaVerbInternal(map, 0, 0x06, 0x706, (activeStream << 4) | chan);

        executeHdaVerbInternal(map, 0, 0x17, 0x701, 0x01); // Select Connection 1 (Node 0x03)
        executeHdaVerbInternal(map, 0, 0x17, 0x707, 0x40); // Pin Ctrl = OUT
        executeHdaVerbInternal(map, 0, 0x1E, 0x705, 0x00);
        executeHdaVerbInternal(map, 0, 0x1E, 0x707, 0x40);
        executeHdaVerbInternal(map, 0, 0x06, 0x705, 0x00);

        executeHdaVerbInternal(map, 0, 0x20, 0x500, 0x10); executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x0906);
        executeHdaVerbInternal(map, 0, 0x20, 0x500, 0x26); executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x0102);

        executeHdaVerbInternal(map, 0, 0x20, 0x500, 0x24); executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x41);
        executeHdaVerbInternal(map, 0, 0x20, 0x500, 0x26); executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x0C);
        executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x00);
        executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x1A);
        executeHdaVerbInternal(map, 0, 0x20, 0x400, 0xB020);
        executeHdaVerbInternal(map, 0, 0x20, 0x500, 0x26); executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x02);
        executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x00);
        executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x00);
        executeHdaVerbInternal(map, 0, 0x20, 0x400, 0xB020);

        executeHdaVerbInternal(map, 0, 0x20, 0x500, 0x24); executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x42);
        executeHdaVerbInternal(map, 0, 0x20, 0x500, 0x26); executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x0C);
        executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x00);
        executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x2A);
        executeHdaVerbInternal(map, 0, 0x20, 0x400, 0xB020);
        executeHdaVerbInternal(map, 0, 0x20, 0x500, 0x26); executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x02);
        executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x00);
        executeHdaVerbInternal(map, 0, 0x20, 0x400, 0x00);
        executeHdaVerbInternal(map, 0, 0x20, 0x400, 0xB020);
    }

    map->release();
    audioCtrl->release();
}

void CirrusAudioFixup::runBackgroundMonitor() {
    syncAlc287HdaCodec();
    IODelay(5000); // 5ms delay for CS35L41 PLL lock onto I2S BCLK
    for (unsigned i = 0; i < 2; ++i) {
        CS35L41Amp &amp = mAmps[i];
        if (!amp.present) continue;
        
        uint32_t pwr_ctrl1 = 0;
        uint32_t pll_lock_sts = 0;
        uint32_t mbox2 = 0;
        uint32_t timestamp = 0;
        
        if (!readRegister(amp, 0x00002014, &pwr_ctrl1, TRACE_DUMP)) continue;
        
        readRegister(amp, 0x00010018, &pll_lock_sts, TRACE_DUMP);
        bool global_en = (pwr_ctrl1 & 0x01) != 0;
        bool pll_lock  = (pll_lock_sts & 0x00000002) != 0;
        
        readRegister(amp, 0x00013004, &mbox2, TRACE_DUMP); // CSPL mailbox status
        
        // ── Initialization check ──
        // Only attempt full re-init if boot-time probe failed entirely (!initialized).
        // If hardware errata/OTP is done (initialized=true) but DSP never reached RUN
        // (!dspAlive), attempt a firmware-only restart once PLL locks, which means
        // I2S BCLK has arrived from ALC287 and the DSP can now execute.
        if (!amp.initialized) {
            if (pll_lock) {
                CIRRUS_LOG("Background Monitor: Amp %s needs full initialization (pll_lock=%d)", amp.name, pll_lock);
                if (initCodec(amp)) {
                    if (initializeHardwareErrata(amp)) {
                        applyPLL(amp);
                        applyASP(amp);
                        applyGPIO(amp);
                        configureHardware(amp);
                        if (amp.firmwareValidated) {
                            initializeFirmware(amp, "5D.0");
                        }
                        powerUpAmplifier(amp);
                        amp.initialized = true;
                        CIRRUS_LOG("Background Monitor: Amp %s initialized successfully (dspAlive=%d)", amp.name, amp.dspAlive);
                    }
                }
            }
            continue; // skip playback detection until initialized
        }
        
        // Hardware errata/OTP done but DSP stalled at boot (no I2S clock).
        // Now that pll_lock=1 means BCLK is present, retry firmware startup only.
        if (!amp.dspAlive && pll_lock) {
            CIRRUS_LOG("Background Monitor: Amp %s DSP stalled at boot; retrying firmware start (pll_lock=1 → BCLK present)", amp.name);
            if (amp.firmwareValidated) {
                initializeFirmware(amp, "5D.0-retry");
            }
            powerUpAmplifier(amp);
            amp.dspAlive = verifyDSPAlive(amp);
            if (amp.dspAlive) {
                CIRRUS_LOG("Background Monitor: Amp %s DSP reached RUN on retry", amp.name);
            } else {
                CIRRUS_LOG("Background Monitor: Amp %s DSP still not in RUN after retry; will retry on next pll_lock pulse", amp.name);
            }
            continue;
        }
        
        // NOTE: do NOT bail on monitorCount here. In bypass mode monitorCount is
        // always 0, and bailing meant we never logged playback-time state — which
        // is exactly the data we need to see whether BCLK/I2S data actually reaches
        // the codec when audio plays.

        // ── Playback detection ──
        // When audio starts, the SoC drives BCLK over I2S which locks the CS35L41 PLL
        // (bit 1 of 0x10018). While GLOBAL_EN is 0 (idle), the DSP timestamp (0x025C0800)
        // is static at 0 because the DSP clock is gated. Therefore, playback START must be
        // detected via pll_lock (or dspTsMoving once active).
        readRegister(amp, 0x025C0800, &timestamp, TRACE_DUMP);
        bool dspTsMoving = (timestamp != 0 && amp.lastTimestamp != 0 && timestamp != amp.lastTimestamp);
        // Detect playback based on PLL lock status (BCLK active from ALC287)
        bool hasAudio = pll_lock;

        // Also sample the ASP status + a few clock/routing regs so the log tells us
        // exactly what the SoC side is doing during playback.
        // SP_ENABLES=0x4800, SP_RATE_CTRL=0x4804, SP_FORMAT=0x4808 (per Linux cs35l41.h).
        // Sample all of them so the log distinguishes the enable bits from the frame
        // format. Previously 0x4808 (SP_FORMAT) was mislabeled as SP_ENABLES.
        uint32_t asp_enables = 0, sp_rate = 0, sp_fmt = 0, dac_src = 0, amp_vol = 0;
        readRegister(amp, 0x00004800, &asp_enables, TRACE_DUMP); // SP_ENABLES
        readRegister(amp, 0x00004804, &sp_rate, TRACE_DUMP);     // SP_RATE_CTRL
        readRegister(amp, 0x00004808, &sp_fmt, TRACE_DUMP);      // SP_FORMAT
        readRegister(amp, 0x00004C00, &dac_src, TRACE_DUMP);     // DAC_PCM1_SRC
        readRegister(amp, 0x00006000, &amp_vol, TRACE_DUMP);     // AMP_DIG_VOL_CTRL

        CIRRUS_LOG("background monitor %s: global_en=%d pll_lock=%d mbox2=0x%08X ts=0x%08X->0x%08X audio=%d active=%d "
                   "sp_en=0x%08X sp_rate=0x%08X sp_fmt=0x%08X dac_src=0x%08X vol=0x%08X mode=%s",
                   amp.name, global_en, pll_lock, mbox2, amp.lastTimestamp, timestamp, hasAudio, amp.playbackActive,
                   asp_enables, sp_rate, sp_fmt, dac_src, amp_vol, (amp.monitorCount >= 1) ? "DSP" : "BYPASS");

        amp.lastTimestamp = timestamp;
        
        if (hasAudio && !amp.playbackActive) {
            // Transition: idle → playing
            amp.playbackStableCount++;
            if (amp.playbackStableCount >= 1) {
                bool dspMode = (amp.monitorCount >= 1);
                CIRRUS_LOG("Background Monitor: Playback STARTED on %s (mode=%s) — enabling output path",
                           amp.name, dspMode ? "DSP" : "BYPASS");
                amp.playbackActive = true;
                amp.playbackStableCount = 0;

                // Re-enable ASP RX/TX. NOTE: SP_ENABLES is 0x4800, NOT 0x4808.
                // 0x4808 is SP_FORMAT; writing the enable bits there previously
                // clobbered the 32-bit I2S frame format (0x20200200 -> 0x00010001)
                // and killed the audio data path, producing silence on playback.
                writeRegister(amp, 0x00004800, 0x00010001); // SP_ENABLES: RX1_EN=1, TX1_EN=1

                if (dspMode) {
                    // Re-apply FULL cs35l41_hda_config_dsp (Linux cs35l41_hda.c lines 68-88)
                    writeRegister(amp, CS35L41_PLL_CLK_CTRL,   0x00000430); // 3.072MHz BCLK in, PLL_REFCLK_EN=1
                    writeRegister(amp, CS35L41_DSP_CLK_CTRL,   0x00000003); // DSP CLK EN
                    writeRegister(amp, CS35L41_GLOBAL_CLK_CTRL,0x00000003); // GLOBAL_FS = 48 kHz
                    writeRegister(amp, CS35L41_SP_ENABLES,     0x00010001); // ASP_RX1_EN=1, ASP_TX1_EN=1
                    writeRegister(amp, CS35L41_SP_RATE_CTRL,   0x00000021); // ASP_BCLK_FREQ = 3.072 MHz
                    writeRegister(amp, CS35L41_SP_FORMAT,      0x20200200); // 32-bit RX/TX slots, I2S, clk consumer
                    writeRegister(amp, CS35L41_SP_HIZ_CTRL,    0x00000003); // Hi-Z unused/disabled
                    writeRegister(amp, CS35L41_SP_TX_WL,       0x00000018); // 24 cycles/slot
                    writeRegister(amp, CS35L41_SP_RX_WL,       0x00000018); // 24 cycles/slot
                    writeRegister(amp, CS35L41_DAC_PCM1_SRC,   0x00000032); // DACPCM1_SRC = ERR_VOL (DSP Output!)
                    writeRegister(amp, CS35L41_ASP_TX1_SRC,    0x00000018); // ASPTX1 SRC = VMON
                    writeRegister(amp, CS35L41_ASP_TX2_SRC,    0x00000019); // ASPTX2 SRC = IMON
                    writeRegister(amp, CS35L41_ASP_TX3_SRC,    0x00000028); // ASPTX3 SRC = VPMON
                    writeRegister(amp, CS35L41_ASP_TX4_SRC,    0x00000029); // ASPTX4 SRC = VBSTMON
                    writeRegister(amp, CS35L41_DSP1_RX1_SRC,   0x00000008); // DSP1RX1 SRC = ASPRX1
                    writeRegister(amp, CS35L41_DSP1_RX2_SRC,   0x00000008); // DSP1RX2 SRC = ASPRX1
                    writeRegister(amp, CS35L41_DSP1_RX3_SRC,   0x00000018); // DSP1RX3 SRC = VMON
                    writeRegister(amp, CS35L41_DSP1_RX4_SRC,   0x00000019); // DSP1RX4 SRC = IMON
                    writeRegister(amp, CS35L41_DSP1_RX5_SRC,   0x00000029); // DSP1RX5 SRC = VBSTMON

                    // Enable VMON + IMON for speaker protection feedback
                    uint32_t pwr_ctrl2 = 0;
                    readRegister(amp, 0x00002018, &pwr_ctrl2);
                    pwr_ctrl2 |= (1 << 12) | (1 << 13); // VMON_EN | IMON_EN
                    writeRegister(amp, 0x00002018, pwr_ctrl2);

                    // Send RESUME command (mbox cmd = 2)
                    writeRegister(amp, 0x00013020, 2);
                    IODelay(10000); // 10ms wait for DSP to process

                    uint32_t mbox2_after = 0;
                    readRegister(amp, 0x00013004, &mbox2_after);
                    CIRRUS_LOG("Background Monitor: After RESUME on %s: mbox2=0x%08X", amp.name, mbox2_after);
                } else {
                    // BYPASS: no DSP handshake. Re-apply the FULL known-good serial-port + routing config
                    writeRegister(amp, CS35L41_PLL_CLK_CTRL,   0x00000430); // 3.072MHz BCLK in, PLL_REFCLK_EN=1
                    writeRegister(amp, CS35L41_DSP_CLK_CTRL,   0x00000003); // DSP CLK EN
                    writeRegister(amp, CS35L41_GLOBAL_CLK_CTRL,0x00000003); // GLOBAL_FS = 48 kHz
                    writeRegister(amp, CS35L41_SP_ENABLES,     0x00010000); // ASP_RX1_EN=1
                    writeRegister(amp, CS35L41_SP_RATE_CTRL,   0x00000021); // ASP_BCLK_FREQ = 3.072 MHz
                    writeRegister(amp, CS35L41_SP_FORMAT,      0x20200200); // 32-bit RX/TX, I2S, clk consumer
                    writeRegister(amp, CS35L41_SP_HIZ_CTRL,    0x00000002); // Hi-Z unused
                    writeRegister(amp, CS35L41_SP_TX_WL,       0x00000018); // 24 cycles/slot
                    writeRegister(amp, CS35L41_SP_RX_WL,       0x00000018); // 24 cycles/slot
                    writeRegister(amp, CS35L41_DAC_PCM1_SRC,   0x00000008); // DACPCM1_SRC = ASPRX1
                    writeRegister(amp, CS35L41_ASP_TX1_SRC,    0x00000018); // ASPTX1 SRC = VMON
                    writeRegister(amp, CS35L41_ASP_TX2_SRC,    0x00000019); // ASPTX2 SRC = IMON
                    writeRegister(amp, CS35L41_ASP_TX3_SRC,    0x00000032); // ASPTX3 SRC = ERRVOL
                    writeRegister(amp, CS35L41_ASP_TX4_SRC,    0x00000033); // ASPTX4 SRC = CLASSH_TGT
                    writeRegister(amp, CS35L41_DSP1_RX1_SRC,   0x00000008); // DSP1RX1 SRC = ASPRX1
                    writeRegister(amp, CS35L41_DSP1_RX2_SRC,   0x00000009); // DSP1RX2 SRC = ASPRX2
                    writeRegister(amp, CS35L41_DSP1_RX3_SRC,   0x00000018); // DSP1RX3 SRC = VMON
                    writeRegister(amp, CS35L41_DSP1_RX4_SRC,   0x00000019); // DSP1RX4 SRC = IMON
                    writeRegister(amp, CS35L41_DSP1_RX5_SRC,   0x00000020); // DSP1RX5 SRC = ERRVOL
                }

                // 1. Enable AMP output (AMP_EN bit in PWR_CTRL2) BEFORE global enable (matching Linux cs35l41_hda_play_start)
                uint32_t pwr_ctrl2b = 0;
                readRegister(amp, 0x00002018, &pwr_ctrl2b);
                pwr_ctrl2b |= (1 << 0); // AMP_EN = 1
                writeRegister(amp, 0x00002018, pwr_ctrl2b);

                // Enable External Boost FET switch on GPIO1 (Linux cs35l41_hda_play_start line 565)
                writeRegister(amp, CS35L41_GPIO1_CTRL1, 0x00008001);

                // 2. Wait for PLL lock
                uint32_t pll_sts = 0;
                int pll_timeout = 50;
                while (pll_timeout > 0) {
                    readRegister(amp, 0x00010018, &pll_sts);
                    if (pll_sts & 0x02) break;
                    IODelay(1000);
                    pll_timeout--;
                }
                CIRRUS_LOG("Background Monitor: PLL lock status on %s: sts=0x%08X (locked=%d)",
                           amp.name, pll_sts, (pll_sts & 0x02) ? 1 : 0);

                // 3. cs35l41_global_enable: safe-to-active start
                writeRegister(amp, 0x00000040, 0x00000055);
                writeRegister(amp, 0x00000040, 0x000000AA);
                writeRegister(amp, 0x0000742C, 0x0000000F);
                writeRegister(amp, 0x0000742C, 0x00000079);
                writeRegister(amp, 0x00007438, 0x00585941);

                uint32_t pwr1 = 0;
                readRegister(amp, 0x00002014, &pwr1);
                writeRegister(amp, 0x00002014, pwr1 | 0x01); // GLOBAL_EN = 1
                
                // 4. Poll PUP_DONE (bit 24 of IRQ1_STATUS1)
                uint32_t irq1_sts = 0;
                int pup_timeout = 100;
                int elapsed_pup = 0;
                while (pup_timeout > 0) {
                    readRegister(amp, 0x00010010, &irq1_sts);
                    if (irq1_sts & 0x01000000) break;
                    IODelay(1000);
                    elapsed_pup++;
                    pup_timeout--;
                }
                if (irq1_sts & 0x01000000) {
                    CIRRUS_LOG("Background Monitor: PUP_DONE observed on %s after %d ms", amp.name, elapsed_pup);
                    writeRegister(amp, 0x00010010, 0x01000000); // clear PUP_DONE
                } else {
                    CIRRUS_ERR("Background Monitor: PUP_DONE NOT observed on %s within 100ms (irq1=0x%08X)", amp.name, irq1_sts);
                }

                // 5. Execute mode completion
                if (dspMode) {
                    writeRegister(amp, 0x00013020, 7); // SPK_OUT_ENABLE
                    IODelay(10000); // Wait up to 10ms for DSP
                    uint32_t mb2 = 0;
                    readRegister(amp, 0x00013004, &mb2);
                    CIRRUS_LOG("Background Monitor: After SPK_OUT_ENABLE on %s: mbox2=0x%08X", amp.name, mb2);

                    // Unmute DSP (cs35l41_hda_unmute_dsp: Linux cs35l41_hda.c lines 95-98)
                    writeRegister(amp, CS35L41_AMP_DIG_VOL_CTRL, 0x00008000); // HPF_PCM_EN=1, 0.0 dB, unmuted
                    writeRegister(amp, CS35L41_AMP_GAIN_CTRL,    0x00000233); // AMP_GAIN_PCM = 17.5 dB for DSP
                } else {
                    writeRegister(amp, 0x0000742C, 0x000000F9);
                    writeRegister(amp, 0x00007438, 0x00580941);

                    // Unmute Bypass (cs35l41_hda_unmute: Linux cs35l41_hda.c lines 90-93)
                    writeRegister(amp, CS35L41_AMP_DIG_VOL_CTRL, 0x00008000); // HPF_PCM_EN=1, 0.0 dB
                    writeRegister(amp, CS35L41_AMP_GAIN_CTRL,    0x00000084); // AMP_GAIN_PCM = 4.5 dB
                }

                writeRegister(amp, 0x00000040, 0x000000CC);
                writeRegister(amp, 0x00000040, 0x00000033);

                // 6. Comprehensive diagnostic summary after play start
                uint32_t post_pwr1 = 0, post_pwr2 = 0, post_vol = 0, post_gain = 0;
                readRegister(amp, 0x00002014, &post_pwr1);
                readRegister(amp, 0x00002018, &post_pwr2);
                readRegister(amp, CS35L41_AMP_DIG_VOL_CTRL, &post_vol);
                readRegister(amp, CS35L41_AMP_GAIN_CTRL, &post_gain);
                CIRRUS_LOG("Background Monitor: Playback ENABLED on %s: pwr1=0x%08X pwr2=0x%08X vol=0x%08X gain=0x%08X",
                           amp.name, post_pwr1, post_pwr2, post_vol, post_gain);
            }
        } else if (!hasAudio && amp.playbackActive) {
            // Transition: playing → idle
            amp.playbackStableCount++;
            if (amp.playbackStableCount >= 2) { // need 2 consecutive idle readings (~4s) before pausing
                bool dspMode = (amp.monitorCount >= 1);
                CIRRUS_LOG("Background Monitor: Playback STOPPED on %s (mode=%s) — disabling output path",
                           amp.name, dspMode ? "DSP" : "BYPASS");
                amp.playbackActive = false;
                amp.playbackStableCount = 0;
                
                // Mute amplifier volume and gain (cs35l41_hda_mute: Linux cs35l41_hda.c lines 100-103)
                writeRegister(amp, CS35L41_AMP_GAIN_CTRL,    0x00000000); // 0.5 dB (MUTE)
                writeRegister(amp, CS35L41_AMP_DIG_VOL_CTRL, 0x0000A678); // HPF_PCM_EN=1, MUTE

                // 1. cs35l41_global_enable: active-to-safe start
                writeRegister(amp, 0x00000040, 0x00000055);
                writeRegister(amp, 0x00000040, 0x000000AA);
                writeRegister(amp, 0x00007438, 0x00585941);
                
                uint32_t pwr1 = 0;
                readRegister(amp, 0x00002014, &pwr1);
                writeRegister(amp, 0x00002014, pwr1 & ~1); // GLOBAL_EN = 0
                
                writeRegister(amp, 0x0000742C, 0x00000009);
                
                // 2. Wait for PDN_DONE (bit 23 of IRQ1_STATUS1)
                uint32_t irq1_sts = 0;
                int pdn_timeout = 100;
                int elapsed_pdn = 0;
                while (pdn_timeout > 0) {
                    readRegister(amp, 0x00010010, &irq1_sts);
                    if (irq1_sts & 0x00800000) break;
                    IODelay(1000);
                    elapsed_pdn++;
                    pdn_timeout--;
                }
                if (irq1_sts & 0x00800000) {
                    CIRRUS_LOG("Background Monitor: PDN_DONE observed on %s after %d ms", amp.name, elapsed_pdn);
                    writeRegister(amp, 0x00010010, 0x00800000); // clear PDN_DONE
                } else {
                    CIRRUS_ERR("Background Monitor: PDN_DONE NOT observed on %s within 100ms (irq1=0x%08X)", amp.name, irq1_sts);
                }
                
                writeRegister(amp, 0x00007438, 0x00580941);
                writeRegister(amp, 0x00000040, 0x000000CC);
                writeRegister(amp, 0x00000040, 0x00000033);

                // 3. Disable AMP_EN AFTER active_to_safe (matching Linux cs35l41_hda_pause_done)
                uint32_t pwr_ctrl2b = 0;
                readRegister(amp, 0x00002018, &pwr_ctrl2b);
                pwr_ctrl2b &= ~1; // AMP_EN = 0
                writeRegister(amp, 0x00002018, pwr_ctrl2b);
                
                if (dspMode) {
                    // Send PAUSE command (mbox cmd = 1)
                    writeRegister(amp, 0x00013020, 1);
                    IODelay(5000);
                    uint32_t mb2 = 0;
                    readRegister(amp, 0x00013004, &mb2);
                    CIRRUS_LOG("Background Monitor: After PAUSE on %s: mbox2=0x%08X", amp.name, mb2);
                }

                // Disable External Boost FET switch on GPIO1 (Linux cs35l41_hda_pause_done line 627)
                writeRegister(amp, CS35L41_GPIO1_CTRL1, 0x00008000);
            }
        } else {
            // Stable state — reset stability counter
            amp.playbackStableCount = 0;
        }
    }
    
    if (mProbeTimer) {
        mProbeTimer->setTimeoutMS(2000);
    }
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
    
    amp.deviceId = devid_before;
    amp.revisionId = revid_before;
    amp.present = (devid_before == CS35L41_DEVICE_ID);
    
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
    
    amp.deviceId = devid_after;
    amp.revisionId = revid_after;
    amp.present = (devid_after == CS35L41_DEVICE_ID);
    
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
    
    bool result = false;
    size_t numRegs = sizeof(cs35l41_reg_desc)/sizeof(RegisterDesc);
    size_t allocSize = numRegs * sizeof(UInt32);
    
    char propName[64] = {0};
    UInt32 crc_unlock = 0;
    UInt32 crc_errata = 0;
    UInt32 crc_otp = 0;
    UInt32 crc_lock = 0;
    
    UInt32 *snapshot0 = (UInt32*)IOMallocData(allocSize);
    UInt32 *snapshot1 = (UInt32*)IOMallocData(allocSize);
    UInt32 *snapshot2 = (UInt32*)IOMallocData(allocSize);
    UInt32 *snapshot3 = (UInt32*)IOMallocData(allocSize);
    
    if (!snapshot0 || !snapshot1 || !snapshot2 || !snapshot3) {
        CIRRUS_ERR("failed to allocate memory for register snapshots on %s", amp.name);
        goto cleanup;
    }
    
    snapshotRegisters(amp, snapshot0);
    
    // 1. Unlock Test Key
    if (!unlockTestKey(amp)) {
        goto cleanup;
    }
    
    snapshotRegisters(amp, snapshot1);
    
    crc_unlock = calculateRegistersCRC32(amp);
    snprintf(propName, sizeof(propName), "Cirrus_CRC_Unlock_%s", amp.name);
    setProperty(propName, crc_unlock, 32);
    CIRRUS_LOG("checksum after unlock on %s: 0x%08X", amp.name, crc_unlock);
    
    // 2. Errata Patch
    if (!applyErrataPatch(amp)) {
        goto cleanup;
    }
    
    snapshotRegisters(amp, snapshot2);
    CIRRUS_LOG("register diffs after applying errata patch on %s:", amp.name);
    compareRegisterSnapshots(amp, snapshot1, snapshot2);
    
    crc_errata = calculateRegistersCRC32(amp);
    snprintf(propName, sizeof(propName), "Cirrus_CRC_Errata_%s", amp.name);
    setProperty(propName, crc_errata, 32);
    CIRRUS_LOG("checksum after errata patch on %s: 0x%08X", amp.name, crc_errata);
    
    // 3. unpack otp values
    if (!unpackOTP(amp)) {
        CIRRUS_ERR("otp unpacking failed on %s, locking test key as rollback", amp.name);
        lockTestKey(amp); // rollback on error
        goto cleanup;
    }
    
    snapshotRegisters(amp, snapshot3);
    CIRRUS_LOG("register diffs after unpacking otp on %s:", amp.name);
    compareRegisterSnapshots(amp, snapshot2, snapshot3);
    
    crc_otp = calculateRegistersCRC32(amp);
    snprintf(propName, sizeof(propName), "Cirrus_CRC_OTP_%s", amp.name);
    setProperty(propName, crc_otp, 32);
    CIRRUS_LOG("checksum after otp unpack on %s: 0x%08X", amp.name, crc_otp);
    
    // 4. Lock Test Key
    if (!lockTestKey(amp)) {
        goto cleanup;
    }
    
    crc_lock = calculateRegistersCRC32(amp);
    snprintf(propName, sizeof(propName), "Cirrus_CRC_Lock_%s", amp.name);
    setProperty(propName, crc_lock, 32);
    CIRRUS_LOG("checksum after locking test keys on %s: 0x%08X", amp.name, crc_lock);
    
    CIRRUS_LOG("hardware errata init completed for %s", amp.name);
    result = true;

cleanup:
    if (snapshot0) IOFreeData(snapshot0, allocSize);
    if (snapshot1) IOFreeData(snapshot1, allocSize);
    if (snapshot2) IOFreeData(snapshot2, allocSize);
    if (snapshot3) IOFreeData(snapshot3, allocSize);
    
    return result;
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

    // poll for pll lock status
    uint32_t pll_lock_sts = 0;
    int pll_timeout = 50;
    while (pll_timeout > 0) {
        readRegister(amp, 0x00010018, &pll_lock_sts);
        if (pll_lock_sts & 0x00000002) {
            break;
        }
        IODelay(1000);
        pll_timeout--;
    }
    
    if (pll_lock_sts & 0x00000002) {
        CIRRUS_LOG("PLL lock verified successfully on %s", amp.name);
    } else {
        CIRRUS_LOG("warning: PLL did NOT lock on %s (current status: 0x%08X). I2S clocks might be inactive.", amp.name, pll_lock_sts);
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

    // configure dac_pcm1_src
    // If DSP firmware is used, DAC source must be ERR_VOL (0x32) because DSP processes the audio
    // If no DSP, DAC source is directly ASPRX1 (0x08)
    uint32_t dac_pcm1_src = amp.firmwareValidated ? 0x32 : 0x08;
    writeRegister(amp, CS35L41_DAC_PCM1_SRC, dac_pcm1_src);

    // configure dsp rx sources to ASPRX1 (0x08) for both left and right
    writeRegister(amp, CS35L41_DSP1_RX1_SRC, 0x08);
    writeRegister(amp, CS35L41_DSP1_RX2_SRC, 0x08);


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

void CirrusAudioFixup::stopDSP(CS35L41Amp &amp) {
    CIRRUS_LOG("stopping and soft-resetting dsp core on %s", amp.name);
    // Halt the core (HALO_CORE_EN = 0)
    updateRegisterBits(amp, CS35L41_DSP1_CCM_CORE_CTRL, HALO_CORE_EN, 0);
    // Assert CORE_SOFT_RESET to clear latched state
    updateRegisterBits(amp, CS35L41_DSP1_CORE_SOFT_RESET, 1, 1);
    IODelay(1000); // give it a moment to reset
}

void CirrusAudioFixup::discoverFirmware(CS35L41Amp &amp) {
    CIRRUS_LOG("discovering firmware for amplifier: %s", amp.name);
    
    amp.firmwareValidated = false;
    
    char propStatus[64];
    snprintf(propStatus, sizeof(propStatus), "Cirrus_Phase5A_Status_%s", amp.name);

    if (bootArgEnabled("cirrus_nodsp")) {
        CIRRUS_LOG("Non-DSP Bypass mode forced via boot-arg for %s", amp.name);
        OSString *statusStr = OSString::withCString("BYPASS");
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
    // Linux resolves speaker-ID to 1 on this platform (SPKID: 1 in dmesg). Match it.
    int spkid = 1;
    
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
    
    // select per-channel tuning: left uses bin (l0), right uses binRight (r0) when present
    bool isRight = (amp.address == CS35L41_I2C_ADDR_RIGHT);
    const uint8_t *chanBin = (isRight && foundRes->binRight) ? foundRes->binRight : foundRes->bin;
    size_t chanBinSize = (isRight && foundRes->binRight) ? foundRes->binRightSize : foundRes->binSize;

    uint32_t fwCrc = calculate_crc32(foundRes->wmfw, foundRes->wmfwSize);
    uint32_t binCrc = calculate_crc32(chanBin, chanBinSize);
    snprintf(propFW, sizeof(propFW), "Cirrus_FW_CRC32_%s", amp.name); setProperty(propFW, (uint64_t)fwCrc, 32);
    snprintf(propFW, sizeof(propFW), "Cirrus_BIN_CRC32_%s", amp.name); setProperty(propFW, (uint64_t)binCrc, 32);
    
    uint32_t fwVersion = 0;
    if (foundRes->wmfwSize >= 8) {
        fwVersion = foundRes->wmfw[4] | (foundRes->wmfw[5] << 8) | (foundRes->wmfw[6] << 16) | (foundRes->wmfw[7] << 24);
    }
    uint32_t binVersion = 0;
    if (chanBinSize >= 8) {
        binVersion = chanBin[4] | (chanBin[5] << 8) | (chanBin[6] << 16) | (chanBin[7] << 24);
    }
    
    char propFwVer[64];
    snprintf(propFwVer, sizeof(propFwVer), "Cirrus_FW_Version_%s", amp.name);
    setProperty(propFwVer, (uint64_t)fwVersion, 32);
    
    char propBinVer[64];
    snprintf(propBinVer, sizeof(propBinVer), "Cirrus_BIN_Version_%s", amp.name);
    setProperty(propBinVer, (uint64_t)binVersion, 32);
    
    amp.wmfwData = foundRes->wmfw;
    amp.wmfwSize = foundRes->wmfwSize;
    amp.binData = chanBin;
    amp.binSize = chanBinSize;
    amp.firmwareValidated = false; // Discovery found blobs; upload verification owns validation.
    
    CIRRUS_LOG("firmware matches found: %s (size: %lu, version: %08X, crc: %08X), tuning: %s (size: %lu, version: %08X, crc: %08X)",
               foundRes->fwName, amp.wmfwSize, fwVersion, fwCrc, foundRes->binName, amp.binSize, binVersion, binCrc);
    // Explicit channel/tuning selection so a bad log immediately shows which bin loaded.
    CIRRUS_LOG("tuning selection on %s: channel=%s spkid=%d bin=%s crc=0x%08X size=%lu (l0_crc=0x%08X r0_crc=0x%08X r0_present=%d)",
               amp.name, isRight ? "RIGHT(r0)" : "LEFT(l0)", spkid,
               (isRight && foundRes->binRight) ? "r0" : "l0", binCrc, chanBinSize,
               calculate_crc32(foundRes->bin, foundRes->binSize),
               foundRes->binRight ? calculate_crc32(foundRes->binRight, foundRes->binRightSize) : 0,
               foundRes->binRight ? 1 : 0);
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
    
    // CRITICAL ORDERING: configure the MPU BEFORE releasing the core from reset.
    // This mirrors Linux cs_dsp_run(), which calls lock_memory (cs_dsp_halo_configure_mpu)
    // and only afterwards start_core (cs_dsp_halo_start_core). On HALO, clearing
    // HALO_CORE_RESET starts instruction fetch immediately; if the MPU is still in its
    // power-on locked state the core's first XM/YM/PM accesses are blocked, it faults,
    // and HALO_STATE never advances to RUN(2) (heartbeat/timestamp stays 0). The previous
    // code released the core first and configured the MPU afterwards, which is the boot bug.
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
    
    // Final step of Linux cs_dsp_halo_configure_mpu: re-lock the MPU config register.
    // The previous kext code omitted this write entirely.
    writeRegister(amp, CS35L41_DSP1_MPU_LOCK_CONFIG, 0);
    
    uint64_t mpu_end = mach_absolute_time();
    (void)mpu_start; (void)mpu_end;
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_MEM_WINDOW_%s", amp.name);
    statusStr = OSString::withCString("OK");
    if (statusStr) { setProperty(propName, statusStr); statusStr->release(); }
    
    uint32_t current_clk = 0;
    readRegister(amp, CS35L41_DSP_CLK_CTRL, &current_clk);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_CLOCK_RAW_%s", amp.name);
    setProperty(propName, (uint64_t)current_clk, 32);
    
    uint64_t reset_start = mach_absolute_time();
    
    // release reset and enable dsp core (Linux cs_dsp_halo_start_core) — MPU is now configured
    updateRegisterBits(amp, CS35L41_DSP1_CCM_CORE_CTRL, HALO_CORE_RESET | HALO_CORE_EN, HALO_CORE_RESET | HALO_CORE_EN);
    updateRegisterBits(amp, CS35L41_DSP1_CCM_CORE_CTRL, HALO_CORE_RESET, 0);
    
    uint64_t reset_end = mach_absolute_time();
    uint64_t reset_time_us = (reset_end - reset_start) / 1000;
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_RESET_TIME_US_%s", amp.name);
    setProperty(propName, reset_time_us, 32);
    
    snprintf(propName, sizeof(propName), "Cirrus_DSP_RESET_%s", amp.name);
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

    // Mirror Linux cs35l41_runtime_resume: after releasing the core, poll HALO_STATE
    // until it reaches HALO_STATE_CODE_RUN (2). This confirms the firmware actually
    // booted. Linux uses 1ms interval / 15ms timeout. Purely diagnostic here — we log
    // the outcome and let powerUp continue regardless so we always collect state.
    const uint32_t HALO_STATE_REG = 0x02800250;
    const uint32_t HALO_STATE_CODE_RUN = 2;
    uint32_t halo_state = pre_halo_state;
    int halo_timeout = 15; // 15 x 1ms
    while (halo_timeout > 0) {
        readRegister(amp, HALO_STATE_REG, &halo_state);
        if (halo_state == HALO_STATE_CODE_RUN) break;
        IODelay(1000);
        halo_timeout--;
    }
    if (halo_state == HALO_STATE_CODE_RUN) {
        CIRRUS_LOG("HALO firmware reached RUN state on %s (halo_state=0x%08X)", amp.name, halo_state);
    } else {
        CIRRUS_ERR("HALO firmware did NOT reach RUN on %s (halo_state=0x%08X, expected 0x2). "
                   "DSP boot likely failed — check wmfw/coeff upload PASS count above.", amp.name, halo_state);
    }
    snprintf(propName, sizeof(propName), "Cirrus_DSP_HALO_STATE_%s", amp.name);
    setProperty(propName, (uint64_t)halo_state, 32);

    // ── Heartbeat sampling ──────────────────────────────────────────────
    // The DSP firmware increments a free-running timestamp counter every tick
    // while it executes code. Sampling it three times a few ms apart is the
    // single most reliable "is the core actually running code?" test:
    //   - INCREMENTING  -> firmware is alive and executing
    //   - STUCK (all equal, and non-zero) -> core booted then halted/faulted
    //   - STUCK at 0    -> core never executed a single instruction
    // This is emitted on EVERY boot (pass or fail) so we always have the data.
    const uint32_t HALO_HEARTBEAT_REG = 0x025C0800;
    uint32_t hb0 = 0, hb1 = 0, hb2 = 0;
    readRegister(amp, HALO_HEARTBEAT_REG, &hb0);
    IODelay(2000);
    readRegister(amp, HALO_HEARTBEAT_REG, &hb1);
    IODelay(2000);
    readRegister(amp, HALO_HEARTBEAT_REG, &hb2);
    const bool heartbeatMoving = (hb0 != hb1) && (hb1 != hb2);
    const char *hbVerdict;
    if (heartbeatMoving) {
        hbVerdict = "INCREMENTING (firmware executing)";
    } else if (hb0 == 0 && hb1 == 0 && hb2 == 0) {
        hbVerdict = "STUCK_AT_ZERO (core never executed any instruction)";
    } else {
        hbVerdict = "STUCK (core booted then halted/faulted)";
    }
    CIRRUS_LOG("dsp heartbeat on %s: t0=0x%08X t1=0x%08X t2=0x%08X -> %s",
               amp.name, hb0, hb1, hb2, hbVerdict);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_HEARTBEAT_%s", amp.name);
    setProperty(propName, (uint64_t)hb2, 32);
    snprintf(propName, sizeof(propName), "Cirrus_DSP_HEARTBEAT_VERDICT_%s", amp.name);
    statusStr = OSString::withCString(hbVerdict);
    if (statusStr) { setProperty(propName, statusStr); statusStr->release(); }

    // ── MPU fault report ────────────────────────────────────────────────
    // Only meaningful when the core did NOT reach RUN. The HALO MPU latches the
    // FIRST offending access: *_VIO_STATUS names the violation type and
    // *_VIO_ADDR gives the exact DSP address that faulted. SCRATCH1..4 are where
    // the firmware ROM writes a panic/abort code before halting. Together these
    // tell us precisely WHY the core stalled (which region, which address, which
    // panic code) instead of guessing.
    if (halo_state != HALO_STATE_CODE_RUN) {
        uint32_t xm_vio_sts = 0, xm_vio_addr = 0;
        uint32_t ym_vio_sts = 0, ym_vio_addr = 0;
        uint32_t pm_vio_sts = 0, pm_vio_addr = 0;
        readRegister(amp, 0x02BC3104, &xm_vio_sts);   // MPU_XM_VIO_STATUS
        readRegister(amp, 0x02BC3100, &xm_vio_addr);  // MPU_XM_VIO_ADDR
        readRegister(amp, 0x02BC310C, &ym_vio_sts);   // MPU_YM_VIO_STATUS
        readRegister(amp, 0x02BC3108, &ym_vio_addr);  // MPU_YM_VIO_ADDR
        readRegister(amp, 0x02BC3114, &pm_vio_sts);   // MPU_PM_VIO_STATUS
        readRegister(amp, 0x02BC3110, &pm_vio_addr);  // MPU_PM_VIO_ADDR
        
        bool actual_mpu_fault = ((xm_vio_sts & 0x007E0000) != 0) || ((ym_vio_sts & 0x007E0000) != 0) || ((pm_vio_sts & 0x007E0000) != 0);

        CIRRUS_ERR("MPU FAULT REPORT on %s: XM vio_sts=0x%08X vio_addr=0x%08X | "
                   "YM vio_sts=0x%08X vio_addr=0x%08X | PM vio_sts=0x%08X vio_addr=0x%08X",
                   amp.name, xm_vio_sts, xm_vio_addr, ym_vio_sts, ym_vio_addr, pm_vio_sts, pm_vio_addr);
        if (actual_mpu_fault) {
            CIRRUS_ERR("  -> MPU BLOCKED an access. A non-zero vio_sts (bits 17-22) means the core was "
                       "denied memory access (MPU still locked / wrong region). This is the fault cause.");
        } else {
            CIRRUS_ERR("  -> No MPU violation latched. Core stall is NOT an MPU block; "
                       "suspect firmware image integrity (addressing/byte-order) or missing errata.");
        }

        uint32_t scratch[4] = {0};
        readRegister(amp, 0x02B805C0, &scratch[0]);   // SCRATCH1
        readRegister(amp, 0x02B805C8, &scratch[1]);   // SCRATCH2
        readRegister(amp, 0x02B805D0, &scratch[2]);   // SCRATCH3
        readRegister(amp, 0x02B805D8, &scratch[3]);   // SCRATCH4
        CIRRUS_ERR("DSP SCRATCH on %s: s1=0x%08X s2=0x%08X s3=0x%08X s4=0x%08X "
                   "(non-zero => firmware ROM wrote a panic/abort code)",
                   amp.name, scratch[0], scratch[1], scratch[2], scratch[3]);

        snprintf(propName, sizeof(propName), "Cirrus_DSP_MPU_XM_VIO_%s", amp.name);
        setProperty(propName, (uint64_t)xm_vio_sts, 32);
        snprintf(propName, sizeof(propName), "Cirrus_DSP_MPU_YM_VIO_%s", amp.name);
        setProperty(propName, (uint64_t)ym_vio_sts, 32);
        snprintf(propName, sizeof(propName), "Cirrus_DSP_MPU_PM_VIO_%s", amp.name);
        setProperty(propName, (uint64_t)pm_vio_sts, 32);
        snprintf(propName, sizeof(propName), "Cirrus_DSP_SCRATCH1_%s", amp.name);
        setProperty(propName, (uint64_t)scratch[0], 32);
    }

    CIRRUS_LOG("dsp bringup complete for %s", amp.name);
}

bool CirrusAudioFixup::verifyDSPAlive(CS35L41Amp &amp) {
    uint32_t core_ctrl = 0, sys_id = 0, mbox = 0, halo_state = 0;
    bool core_pass = false, reset_pass = false, sysid_pass = false, mbox_pass = false, xm_pass = false;

    readRegister(amp, CS35L41_DSP1_CCM_CORE_CTRL, &core_ctrl, TRACE_DUMP);
    readRegister(amp, CS35L41_DSP1_SYS_ID, &sys_id, TRACE_DUMP);
    readRegister(amp, CS35L41_DSP_MBOX_2, &mbox, TRACE_DUMP);
    readRegister(amp, 0x02800250, &halo_state, TRACE_DUMP); // HALO_STATE

    if (core_ctrl & HALO_CORE_EN) core_pass = true;
    if ((core_ctrl & HALO_CORE_RESET) == 0) reset_pass = true;
    if (sys_id != 0x00000000 && sys_id != 0xFFFFFFFF) sysid_pass = true;
    // mbox_2 holds the CSPL firmware status: RUNNING(0) or PAUSED(1) are healthy.
    // Previously this flag was (wrongly) derived from sys_id, hiding the real mbox value.
    if (mbox == CSPL_MBOX_STS_RUNNING || mbox == CSPL_MBOX_STS_PAUSED) mbox_pass = true;

    uint8_t dummy[4] = {0};
    if (bulkRead(amp, 0x02000000, dummy, 4, TRACE_DUMP)) {
        xm_pass = true;
    }

    // Fold the MPU violation + firmware panic scratch into the verdict. When the
    // DSP fails to come alive, these name the exact fault (which region, which
    // address, which panic code) instead of leaving us to guess.
    uint32_t xm_vio = 0, xm_vio_addr = 0;
    uint32_t ym_vio = 0, ym_vio_addr = 0;
    uint32_t pm_vio = 0, pm_vio_addr = 0;
    uint32_t sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0;
    readRegister(amp, 0x2BC3104, &xm_vio, TRACE_DUMP);      // MPU_XM_VIO_STATUS
    readRegister(amp, 0x2BC3100, &xm_vio_addr, TRACE_DUMP); // MPU_XM_VIO_ADDR
    readRegister(amp, 0x2BC310C, &ym_vio, TRACE_DUMP);      // MPU_YM_VIO_STATUS
    readRegister(amp, 0x2BC3108, &ym_vio_addr, TRACE_DUMP); // MPU_YM_VIO_ADDR
    readRegister(amp, 0x2BC3114, &pm_vio, TRACE_DUMP);      // MPU_PM_VIO_STATUS
    readRegister(amp, 0x2BC3110, &pm_vio_addr, TRACE_DUMP); // MPU_PM_VIO_ADDR
    readRegister(amp, 0x2B805C0, &sc1, TRACE_DUMP);         // SCRATCH1
    readRegister(amp, 0x2B805C8, &sc2, TRACE_DUMP);         // SCRATCH2
    readRegister(amp, 0x2B805D0, &sc3, TRACE_DUMP);         // SCRATCH3
    readRegister(amp, 0x2B805D8, &sc4, TRACE_DUMP);         // SCRATCH4
    // Only bits 17-22 are real MPU access violations. Bit 16 (0x00010000) is a
    // benign status/config flag, so masking prevents a false "FAULT" verdict.
    const uint32_t MPU_VIO_MASK = 0x007E0000;
    bool mpu_clean = ((xm_vio & MPU_VIO_MASK) == 0 &&
                      (ym_vio & MPU_VIO_MASK) == 0 &&
                      (pm_vio & MPU_VIO_MASK) == 0);

    CIRRUS_LOG("dsp status verify results for %s: core_reset=%s, core_en=%s, sysid=%s(0x%08X), "
               "mailbox=%s(fw_status=%u), halo_state=0x%08X, xm_read=%s",
               amp.name, reset_pass ? "ok" : "fail", core_pass ? "ok" : "fail",
               sysid_pass ? "ok" : "fail", sys_id,
               mbox_pass ? "ok" : "fail", mbox, halo_state, xm_pass ? "ok" : "fail");
    CIRRUS_LOG("dsp verify MPU/scratch for %s: mpu=%s xm_vio=0x%08X@0x%08X ym_vio=0x%08X@0x%08X "
               "pm_vio=0x%08X@0x%08X scratch=[0x%08X 0x%08X 0x%08X 0x%08X]",
               amp.name, mpu_clean ? "clean" : "FAULT",
               xm_vio, xm_vio_addr, ym_vio, ym_vio_addr, pm_vio, pm_vio_addr,
               sc1, sc2, sc3, sc4);

    uint32_t hb0 = 0, hb1 = 0;
    readRegister(amp, 0x025C0800, &hb0, TRACE_DUMP);
    IODelay(2000);
    readRegister(amp, 0x025C0800, &hb1, TRACE_DUMP);
    bool heartbeat_pass = (hb0 != hb1);

    const uint32_t HALO_STATE_RUN = 2;
    bool run_pass = (halo_state == HALO_STATE_RUN);
    if (!run_pass || !heartbeat_pass || !mpu_clean) {
        CIRRUS_ERR("dsp verdict for %s: halo=0x%08X heartbeat=0x%08X->0x%08X mpu=%s; DSP mode disabled",
                   amp.name, halo_state, hb0, hb1, mpu_clean ? "clean" : "FAULT");
    }

    return core_pass && reset_pass && sysid_pass && mbox_pass && xm_pass &&
           run_pass && heartbeat_pass && mpu_clean;
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
    
    uint32_t expected_rx_src = 0x08; // both left and right receive channel data on ASPRX1
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

// ── Consolidated DSP boot report ────────────────────────────────────────
// One self-contained block that gathers every signal needed to diagnose a
// silent-audio boot and prints a single human-readable verdict. Designed so a
// log reader can jump straight to "===== DSP BOOT REPORT" and know exactly
// what failed and why, without correlating scattered lines. Read-only.
void CirrusAudioFixup::logDSPBootReport(CS35L41Amp &amp) {
    uint32_t core_ctrl = 0, sys_id = 0, halo_state = 0;
    uint32_t mbox1 = 0, mbox2 = 0;
    uint32_t xm_vio = 0, ym_vio = 0, pm_vio = 0;
    uint32_t xm_vio_addr = 0, ym_vio_addr = 0, pm_vio_addr = 0;
    uint32_t sc1 = 0, sc2 = 0, sc3 = 0, sc4 = 0;
    uint32_t ts0 = 0, ts1 = 0, ts2 = 0;

    readRegister(amp, CS35L41_DSP1_CCM_CORE_CTRL, &core_ctrl, TRACE_DUMP);
    readRegister(amp, CS35L41_DSP1_SYS_ID, &sys_id, TRACE_DUMP);
    readRegister(amp, 0x02800250, &halo_state, TRACE_DUMP);
    readRegister(amp, 0x00013020, &mbox1, TRACE_DUMP);
    readRegister(amp, 0x00013004, &mbox2, TRACE_DUMP);
    readRegister(amp, 0x2BC3104, &xm_vio, TRACE_DUMP);
    readRegister(amp, 0x2BC3100, &xm_vio_addr, TRACE_DUMP);
    readRegister(amp, 0x2BC310C, &ym_vio, TRACE_DUMP);
    readRegister(amp, 0x2BC3108, &ym_vio_addr, TRACE_DUMP);
    readRegister(amp, 0x2BC3114, &pm_vio, TRACE_DUMP);
    readRegister(amp, 0x2BC3110, &pm_vio_addr, TRACE_DUMP);
    readRegister(amp, 0x2B805C0, &sc1, TRACE_DUMP);
    readRegister(amp, 0x2B805C8, &sc2, TRACE_DUMP);
    readRegister(amp, 0x2B805D0, &sc3, TRACE_DUMP);
    readRegister(amp, 0x2B805D8, &sc4, TRACE_DUMP);

    // Heartbeat: three samples ~2ms apart.
    readRegister(amp, 0x025C0800, &ts0, TRACE_DUMP);
    IODelay(2000);
    readRegister(amp, 0x025C0800, &ts1, TRACE_DUMP);
    IODelay(2000);
    readRegister(amp, 0x025C0800, &ts2, TRACE_DUMP);

    bool core_en   = (core_ctrl & HALO_CORE_EN) != 0;
    bool core_out_of_reset = (core_ctrl & HALO_CORE_RESET) == 0;
    bool sysid_ok  = (sys_id != 0x00000000 && sys_id != 0xFFFFFFFF);
    bool run_ok    = (halo_state == 2);
    bool mbox_ok   = (mbox2 == CSPL_MBOX_STS_RUNNING || mbox2 == CSPL_MBOX_STS_PAUSED);
    bool mpu_clean = (xm_vio == 0 && ym_vio == 0 && pm_vio == 0);
    bool hb_alive  = (ts0 != ts1 && ts1 != ts2);

    // Overall verdict + the single most-likely root cause.
    const char *verdict;
    const char *rootCause;
    if (run_ok && hb_alive && mpu_clean && mbox_ok) {
        verdict = "HEALTHY";
        rootCause = "none - DSP is executing firmware";
    } else if (!core_en || !core_out_of_reset) {
        verdict = "FAIL";
        rootCause = "core never enabled/released from reset (bringup sequence issue)";
    } else if (!mpu_clean) {
        verdict = "FAIL";
        rootCause = "MPU memory protection violation - see xm/ym/pm_vio addr (bad addressing or MPU config)";
    } else if (!hb_alive && (ts0 == 0 && ts1 == 0 && ts2 == 0)) {
        verdict = "FAIL";
        rootCause = "heartbeat stuck at 0 - core faulted on first instruction (check scratch panic code)";
    } else if (!hb_alive) {
        verdict = "FAIL";
        rootCause = "heartbeat frozen - firmware halted after boot (check scratch panic code)";
    } else if (!run_ok) {
        verdict = "DEGRADED";
        rootCause = "HALO_STATE not RUN(2) but heartbeat alive - firmware booting/stuck in init";
    } else if (!mbox_ok) {
        verdict = "DEGRADED";
        rootCause = "mailbox not RUNNING/PAUSED - CSPL command handshake incomplete";
    } else {
        verdict = "UNKNOWN";
        rootCause = "unexpected combination - inspect raw values below";
    }

    CIRRUS_LOG("===== DSP BOOT REPORT for %s =====", amp.name);
    CIRRUS_LOG("  VERDICT   : %s", verdict);
    CIRRUS_LOG("  ROOT CAUSE: %s", rootCause);
    CIRRUS_LOG("  core      : ctrl=0x%08X en=%s reset_released=%s", core_ctrl,
               core_en ? "yes" : "NO", core_out_of_reset ? "yes" : "NO");
    CIRRUS_LOG("  sys_id    : 0x%08X (%s)", sys_id, sysid_ok ? "ok" : "BAD");
    CIRRUS_LOG("  halo_state: 0x%08X (%s, want RUN=2)", halo_state, run_ok ? "RUN" : "not-run");
    CIRRUS_LOG("  mailbox   : mbox1=0x%08X mbox2=0x%08X (%s)", mbox1, mbox2,
               mbox_ok ? "ok" : "not-running");
    CIRRUS_LOG("  mpu       : %s xm_vio=0x%08X@0x%08X ym_vio=0x%08X@0x%08X pm_vio=0x%08X@0x%08X",
               mpu_clean ? "clean" : "FAULT",
               xm_vio, xm_vio_addr, ym_vio, ym_vio_addr, pm_vio, pm_vio_addr);
    CIRRUS_LOG("  scratch   : [0x%08X 0x%08X 0x%08X 0x%08X] (firmware panic code if non-zero)",
               sc1, sc2, sc3, sc4);
    CIRRUS_LOG("  heartbeat : t0=0x%08X t1=0x%08X t2=0x%08X (%s)", ts0, ts1, ts2,
               hb_alive ? "ALIVE" : "DEAD");
    CIRRUS_LOG("===== END DSP BOOT REPORT for %s =====", amp.name);

    // Mirror the verdict into ioreg for userspace tooling (ioreg -l | grep Cirrus).
    char propName[80];
    snprintf(propName, sizeof(propName), "Cirrus_DSP_BOOT_VERDICT_%s", amp.name);
    OSString *vStr = OSString::withCString(verdict);
    if (vStr) { setProperty(propName, vStr); vStr->release(); }
    snprintf(propName, sizeof(propName), "Cirrus_DSP_BOOT_ROOTCAUSE_%s", amp.name);
    OSString *rStr = OSString::withCString(rootCause);
    if (rStr) { setProperty(propName, rStr); rStr->release(); }
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
    CIRRUS_LOG("starting power up sequence for %s (DSP Mode=%d)", amp.name, amp.monitorCount);
    
    // 1. Configure routing and enables first
    if (amp.monitorCount == 1) {
        writeRegister(amp, CS35L41_SP_ENABLES, 0x00010001);   // ASP_RX1_EN = 1, ASP_TX1_EN = 1
        writeRegister(amp, CS35L41_SP_HIZ_CTRL, 0x00000003);  // Hi-Z unused/disabled
        writeRegister(amp, CS35L41_DAC_PCM1_SRC, 0x00000032); // DACPCM1_SRC = ERR_VOL
        writeRegister(amp, CS35L41_ASP_TX3_SRC, 0x00000028);  // ASPTX3 SRC = VPMON
        writeRegister(amp, CS35L41_ASP_TX4_SRC, 0x00000029);  // ASPTX4 SRC = VBSTMON
        writeRegister(amp, CS35L41_DSP1_RX5_SRC, 0x00000029); // DSP1RX5 SRC = VBSTMON
    } else {
        writeRegister(amp, CS35L41_SP_ENABLES, 0x00010000);   // ASP_RX1_EN = 1
        writeRegister(amp, CS35L41_SP_HIZ_CTRL, 0x00000002);
        writeRegister(amp, CS35L41_DAC_PCM1_SRC, 0x08);
        writeRegister(amp, CS35L41_ASP_TX3_SRC, 0x00000032);
        writeRegister(amp, CS35L41_ASP_TX4_SRC, 0x00000033);
        writeRegister(amp, CS35L41_DSP1_RX5_SRC, 0x00000020);
    }

    // 2. Enable VMON_EN, IMON_EN, AMP_EN in CS35L41_PWR_CTRL2 (0x00002018)
    updateRegisterBits(amp, 0x00002018, 0x00003001, 0x00003001);

    // 3. Send RESUME (2) to virtual mailbox if DSP is running
    if (amp.monitorCount == 1) {
        // Snapshot mailbox + core state on BOTH sides of the RESUME handshake so a
        // failed boot records whether mbox2 transitioned RUNNING(0)/PAUSED(1) and
        // whether the core state / heartbeat moved at all.
        uint32_t mb1_pre = 0, mb2_pre = 0, hs_pre = 0, ts_pre = 0;
        readRegister(amp, 0x00013020, &mb1_pre);
        readRegister(amp, 0x00013004, &mb2_pre);
        readRegister(amp, 0x02800250, &hs_pre);
        readRegister(amp, 0x025C0800, &ts_pre);
        CIRRUS_LOG("pre-RESUME snapshot on %s: mbox1=0x%08X mbox2=0x%08X halo_state=0x%08X timestamp=0x%08X",
                   amp.name, mb1_pre, mb2_pre, hs_pre, ts_pre);
        CIRRUS_LOG("sending RESUME command to DSP on %s", amp.name);
        writeRegister(amp, 0x00013020, 2); // CSPL_MBOX_CMD_RESUME
        IODelay(1000);
        uint32_t mb1_post = 0, mb2_post = 0;
        readRegister(amp, 0x00013020, &mb1_post);
        readRegister(amp, 0x00013004, &mb2_post);
        CIRRUS_LOG("post-RESUME snapshot on %s: mbox1=0x%08X mbox2=0x%08X (mbox2: 0=RUNNING 1=PAUSED)",
                   amp.name, mb1_post, mb2_post);
    }

    // 4. Unlock test register write permissions
    writeRegister(amp, 0x00000040, 0x00000055);
    writeRegister(amp, 0x00000040, 0x000000AA);
    
    // 4b. Protection Release: clear any latched Safe-Mode errors (e.g. amp_short)
    //     before enabling. Mirrors Linux cs35l41_error_release; GLOBAL_EN must be
    //     cleared first. Harmless no-op if nothing is latched.
    {
        uint32_t pc1 = 0;
        readRegister(amp, 0x00002014, &pc1);
        writeRegister(amp, 0x00002014, pc1 & ~0x1u); // clear GLOBAL_EN
        const uint32_t rel_mask = 0x02 | 0x04 | 0x08 | 0x10 | 0x40; // AMP_SHORT|BST_SHORT|BST_OVP|BST_UVP|TEMP
        writeRegister(amp, 0x00002034, 0x00000000);
        updateRegisterBits(amp, 0x00002034, rel_mask, rel_mask);
        updateRegisterBits(amp, 0x00002034, rel_mask, 0x00000000);
        writeRegister(amp, 0x00010010, 0x90008002); // clear latched IRQ1 error status bits
    }
    
    // 5. EXT_BOOST boot sequence: cs35l41_reset_to_safe
    // writeRegister(amp, 0x00011008, 0x00000001); // handled by GPIO init, defaults to 81000001 or 00000001. We'll leave it as is.
    writeRegister(amp, 0x00007438, 0x00585941);
    writeRegister(amp, 0x00007414, 0x08C82222);
    writeRegister(amp, 0x0000742C, 0x00000009);
    
    // 6. Disable BST FET
    uint32_t pwr_ctrl2 = 0;
    readRegister(amp, 0x00002018, &pwr_ctrl2);
    pwr_ctrl2 &= ~0x00003000;
    pwr_ctrl2 |= (2 << 12); // CS35L41_BST_DIS_FET_OFF
    writeRegister(amp, 0x00002018, pwr_ctrl2);
    
    // 7. Lock write permissions to test registers

    writeRegister(amp, 0x00000040, 0x000000CC);
    writeRegister(amp, 0x00000040, 0x00000033);
    
    // 11. Configure default digital volume levels and gain
    // Linux: bypass (no-DSP) uses 0x84 (4.5dB), DSP mode uses 0x233 (17.5dB)
    uint32_t expected_gain = bootArgEnabled("cirrus_nodsp") ? 0x00000084 : 0x00000233;
    
    uint32_t dig_vol = 0, gain_ctrl = 0;
    readRegister(amp, CS35L41_AMP_DIG_VOL_CTRL, &dig_vol);
    readRegister(amp, CS35L41_AMP_GAIN_CTRL, &gain_ctrl);
    CIRRUS_LOG("default volume states on %s: digital_volume=0x%08X gain=0x%08X", amp.name, dig_vol, gain_ctrl);
    
    // Linux cs35l41_hda_unmute: DIG_VOL_CTRL = 0x00008000 (HPF_PCM_EN=1, 0.0 dB). This IS unmute.
    if (dig_vol != 0x00008000) {
        CIRRUS_LOG("unmuting digital volume control on %s", amp.name);
        writeRegister(amp, CS35L41_AMP_DIG_VOL_CTRL, 0x00008000);
    } else {
        CIRRUS_LOG("digital volume is already unmuted on %s", amp.name);
    }
    
    if (gain_ctrl != expected_gain) {
        CIRRUS_LOG("setting speaker gain register to 0x%08X on %s", expected_gain, amp.name);
        writeRegister(amp, CS35L41_AMP_GAIN_CTRL, expected_gain);
    } else {
        CIRRUS_LOG("speaker gain is already at expected value on %s", amp.name);
    }
    
    logPowerSnapshot(amp);
    snapshotDiagnostics(amp, "IDLE (POST-BOOT)");
}
