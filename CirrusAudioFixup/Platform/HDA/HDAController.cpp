#include "Platform/HDA/HDAController.hpp"

#include "Support/Logging.hpp"

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

namespace cirrus::platform::hda {

static bool gTopologyLogged = false;
static uint32_t gMissCount = 0;

IOService* HDAController::getAudioController() {
    OSDictionary* matching = IOService::serviceMatching("IOPCIDevice");
    if (!matching)
        return nullptr;

    OSIterator* iter = IOService::getMatchingServices(matching);
    matching->release();
    if (!iter)
        return nullptr;

    IOService* service;
    IOService* bestController = nullptr;
    int bestScore = -1;

    while ((service = OSDynamicCast(IOService, iter->getNextObject()))) {
        int score = 0;
        uint32_t pciVendor = 0;
        IOPCIDevice* pci = OSDynamicCast(IOPCIDevice, service);
        if (pci) {
            pciVendor = pci->configRead16(kIOPCIConfigVendorID);
        } else {
            OSData* venData = OSDynamicCast(OSData, service->getProperty("vendor-id"));
            if (venData && venData->getLength() >= 2)
                pciVendor = *((uint16_t*)venData->getBytesNoCopy());
        }
        if (pciVendor == 0xFFFF || pciVendor == 0)
            continue;

        OSData* classCodeData = OSDynamicCast(OSData, service->getProperty("class-code"));
        if (classCodeData && classCodeData->getLength() >= 3) {
            const uint8_t* bytes = (const uint8_t*)classCodeData->getBytesNoCopy();
            if (bytes[2] == 0x04 && bytes[1] == 0x03)
                score += 50;
        }

        const char* name = service->getName();
        if (name) {
            if (strcmp(name, "HDEF") == 0 || strcmp(name, "HDAS") == 0 || strcmp(name, "CAVS") == 0 || strcmp(name, "AZAL") == 0 ||
                strcmp(name, "ALZA") == 0 || strcmp(name, "AUDIO") == 0)
                score += 30;
            else if (strcmp(name, "HDAU") == 0 || strcmp(name, "B0D3") == 0)
                score -= 40;
        }

        if (pciVendor == 0x10DE || pciVendor == 0x1002)
            score -= 50;
        else if (pciVendor == 0x1022 || pciVendor == 0x8086)
            score += 30;

        if (score >= 40 && score > bestScore) {
            bestScore = score;
            bestController = service;
        }
    }

    iter->release();
    if (bestScore >= 40 && bestController) {
        bestController->retain();
        return bestController;
    }
    return nullptr;
}

bool HDAController::supportedFormat(uint16_t format) {
    const uint32_t baseRate = (format & 0x4000) ? 44100 : 48000;
    const uint32_t multiplier = ((format >> 11) & 7) + 1;
    const uint32_t divisor = ((format >> 8) & 7) + 1;
    const uint32_t sampleSize = (format >> 4) & 7;

    return !(format & 0x8080) && multiplier <= 4 && baseRate * multiplier == 48000 * divisor && (format & 15) == 1 && sampleSize >= 1 &&
           sampleSize <= 3;
}

bool HDAController::syncCodec(HDAStreamState& state) {
    IOService* audioCtrl = getAudioController();
    if (!audioCtrl) {
        if (gMissCount < 20)
            gMissCount++;
        state.observed = false;
        return false;
    }

    IOPCIDevice* pciDev = OSDynamicCast(IOPCIDevice, audioCtrl);
    if (!pciDev) {
        audioCtrl->release();
        return false;
    }

    IOMemoryMap* map = pciDev->mapDeviceMemoryWithRegister(0x10);
    if (!map || map->getLength() < 0x80) {
        if (map)
            map->release();
        audioCtrl->release();
        return false;
    }
    volatile uint8_t* base = (volatile uint8_t*)map->getVirtualAddress();
    state.observed = base != nullptr;

    bool outputRunning = false;
    uint8_t activeStream = 0;
    uint16_t activeFormat = 0;
    uint8_t activeDescriptor = 0xFF;

    if (base) {
        uint16_t gcap = *(volatile uint16_t*)(base + 0x00);
        uint32_t gctl = *(volatile uint32_t*)(base + 0x08);
        if (gcap != 0xFFFF && gctl != 0xFFFFFFFF && (gctl & 1)) {
            uint8_t inputStreams = (gcap >> 8) & 0x0F;
            uint8_t outputStreams = (gcap >> 12) & 0x0F;
            uint8_t bidirectionalStreams = (gcap >> 3) & 0x1F;
            uint8_t firstOutput = inputStreams;
            uint8_t descriptorCount = inputStreams + outputStreams + bidirectionalStreams;

            if (map->getLength() >= 0x80U + uint32_t(descriptorCount) * 0x20U) {
                if (!gTopologyLogged) {
                    CIRRUS_LOG("HDA controller %s %04X:%04X GCAP=0x%04X ISS=%u OSS=%u BSS=%u",
                               pciDev->getName() ? pciDev->getName() : "unnamed", pciDev->configRead16(kIOPCIConfigVendorID),
                               pciDev->configRead16(kIOPCIConfigDeviceID), gcap, inputStreams, outputStreams, bidirectionalStreams);
                    gTopologyLogged = true;
                }

                for (uint8_t index = firstOutput; index < descriptorCount; ++index) {
                    bool fixedOutput = index < (uint8_t)(firstOutput + outputStreams);
                    volatile uint8_t* sd = base + 0x80 + (index * 0x20);
                    uint32_t sdCtl = *(volatile uint32_t*)(sd + 0x00);
                    bool bidirectionalOutput = !fixedOutput && ((sdCtl & (1U << 19)) != 0);
                    if ((fixedOutput || bidirectionalOutput) && (sdCtl & 0x00000003) == 0x00000002) {
                        uint8_t candidateStream = (sdCtl >> 20) & 0x0F;
                        if (candidateStream != 0) {
                            activeStream = candidateStream;
                            activeFormat = *(volatile uint16_t*)(sd + 0x12);
                            activeDescriptor = index;
                            outputRunning = true;
                        }
                    }
                }
            }
        }
    }

    state.streamActive = outputRunning;
    if (outputRunning) {
        state.lastStreamTag = activeStream;
        state.lastFormat = activeFormat;
        state.lastDescriptor = activeDescriptor;
    }

    map->release();
    audioCtrl->release();
    return outputRunning;
}

}
