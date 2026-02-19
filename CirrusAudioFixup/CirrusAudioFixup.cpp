//
//  CirrusAudioFixup.cpp
//  CirrusAudioFixup
//
//  Skeleton kext — only logs to verify matching.
//  NO I2C transactions, NO GPIO access, NO blocking calls.
//
//  Ventura 13.0+ / macOS Hackintosh (Lenovo Legion 7 2021)
//  CS35L41 internal speaker amplifier via VoodooI2C
//

#include "CirrusAudioFixup.hpp"

// ──────────────────────────────────────────────────────────────
//  Required IOKit macros
// ──────────────────────────────────────────────────────────────
OSDefineMetaClassAndStructors(CirrusAudioFixup, IOService)

// In C++ IOKit kexts, 'super' is NOT a built-in keyword (unlike ObjC).
// This #define is the standard IOKit convention used in every Apple kext.
#define super IOService

// ──────────────────────────────────────────────────────────────
//  init — called once per matched device, before start
// ──────────────────────────────────────────────────────────────
bool CirrusAudioFixup::init(OSDictionary *properties) {
    if (!super::init(properties)) {
        CIRRUS_ERR("super::init failed");
        return false;
    }

    CIRRUS_LOG("init() — kext loaded and initialised");
    return true;
}

// ──────────────────────────────────────────────────────────────
//  probe — opportunity to inspect provider before committing
// ──────────────────────────────────────────────────────────────
IOService *CirrusAudioFixup::probe(IOService *provider, SInt32 *score) {
    IOService *result = super::probe(provider, score);

    CIRRUS_LOG("probe() — provider: %s | score: %d",
               provider ? provider->getName() : "null",
               score ? (int)*score : -1);

    // Boost score so we win over any generic fallback driver
    if (score) *score += 9000;

    return result;
}

// ──────────────────────────────────────────────────────────────
//  detectChannel  — read _UID property to identify L/R channel
// ──────────────────────────────────────────────────────────────
void CirrusAudioFixup::detectChannel(IOService *provider) {
    // VoodooI2C exposes ACPI _UID as the "UID", "uid", or
    // acpi-uid property on the nub. Try each variant.
    OSObject *uidObj = provider->getProperty("UID");
    if (!uidObj) uidObj = provider->getProperty("uid");
    if (!uidObj) uidObj = provider->getProperty("acpi-uid");

    OSNumber *uidNum = OSDynamicCast(OSNumber, uidObj);

    if (uidNum) {
        // SPL has _UID = 0  → left  → 0x40
        // SPR has _UID = 1  → right → 0x41
        mIsLeft     = (uidNum->unsigned32BitValue() == 0);
        mI2CAddress = mIsLeft ? CS35L41_I2C_ADDR_LEFT
                               : CS35L41_I2C_ADDR_RIGHT;

        CIRRUS_LOG("detectChannel() — UID=%u → %s channel (I2C 0x%02X)",
                   uidNum->unsigned32BitValue(),
                   mIsLeft ? "LEFT" : "RIGHT",
                   mI2CAddress);
    } else {
        // No UID property found — default to LEFT (first enumerated)
        mIsLeft     = true;
        mI2CAddress = CS35L41_I2C_ADDR_LEFT;
        CIRRUS_LOG("detectChannel() — UID not found, defaulting to LEFT (0x%02X)",
                   mI2CAddress);
    }
}

// ──────────────────────────────────────────────────────────────
//  start — main entry point; driver is active after this returns true
// ──────────────────────────────────────────────────────────────
bool CirrusAudioFixup::start(IOService *provider) {
    CIRRUS_LOG("start() — BEGIN");

    if (!super::start(provider)) {
        CIRRUS_ERR("super::start failed — aborting");
        return false;
    }

    // ── 1. Identify which channel we are ────────────────────
    detectChannel(provider);

    // ── 2. Dump provider properties for debugging ────────────
    CIRRUS_LOG("start() — provider name   : %s", provider->getName());
    CIRRUS_LOG("start() — provider class  : %s",
               provider->getMetaClass()->getClassName());

    // Log the ACPI path if available
    OSObject *acpiPath = provider->getProperty("acpi-path");
    if (auto *str = OSDynamicCast(OSString, acpiPath))
        CIRRUS_LOG("start() — ACPI path       : %s", str->getCStringNoCopy());

    // ── 3. Safety guard: check for boot-arg "cirrus=1" ──────
    //    Without this flag the kext loads but does NOTHING.
    //    Add "cirrus=1" to boot-args when you want real I2C work.
    uint32_t cirrusEnabled = 0;
    PE_parse_boot_argn("cirrus", &cirrusEnabled, sizeof(cirrusEnabled));

    if (!cirrusEnabled) {
        CIRRUS_LOG("start() — boot-arg 'cirrus=1' NOT set → passive mode");
        CIRRUS_LOG("start() — kext is LOADED and MATCHED but will do nothing");
        CIRRUS_LOG("start() — add boot-arg cirrus=1 to enable hardware init");
    } else {
        CIRRUS_LOG("start() — boot-arg 'cirrus=1' DETECTED → active mode");
        CIRRUS_LOG("start() — %s channel ready for hardware init (I2C 0x%02X)",
                   mIsLeft ? "LEFT" : "RIGHT", mI2CAddress);
        // TODO: call initCS35L41() here once I2C code is ready
    }

    // ── 4. Publish ourselves so other drivers can find us ───
    registerService();

    CIRRUS_LOG("start() — SUCCESS ✓ (%s channel matched)",
               mIsLeft ? "LEFT" : "RIGHT");
    return true;
}

// ──────────────────────────────────────────────────────────────
//  stop — called when the driver is unloaded or system sleeps
// ──────────────────────────────────────────────────────────────
void CirrusAudioFixup::stop(IOService *provider) {
    CIRRUS_LOG("stop() — %s channel (I2C 0x%02X)",
               mIsLeft ? "LEFT" : "RIGHT", mI2CAddress);
    super::stop(provider);
}

// ──────────────────────────────────────────────────────────────
//  free — final cleanup
// ──────────────────────────────────────────────────────────────
void CirrusAudioFixup::free() {
    CIRRUS_LOG("free()");
    super::free();
}
