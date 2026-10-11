#include "AMDSPIController.hpp"
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <libkern/c++/OSString.h>
namespace cirrus::transport::amd {
uint8_t AMDSPIController::MMIOWindow::read8(uint32_t o) { uint8_t v=base[o]; OSSynchronizeIO(); return v; }
void AMDSPIController::MMIOWindow::write8(uint32_t o,uint8_t v) { base[o]=v; OSSynchronizeIO(); }
uint32_t AMDSPIController::MMIOWindow::read32(uint32_t o) {
    uint32_t v=*reinterpret_cast<volatile uint32_t*>(base+o); OSSynchronizeIO(); return v;
}
void AMDSPIController::MMIOWindow::write32(uint32_t o,uint32_t v) {
    *reinterpret_cast<volatile uint32_t*>(base+o)=v; OSSynchronizeIO();
}
uint64_t AMDSPIController::MMIOWindow::nowUs() {
    uint64_t ns=0; absolutetime_to_nanoseconds(mach_absolute_time(),&ns); return ns/1000;
}
bool AMDSPIController::isD0() const {
    if (!mProvider || !mPowerAvailable) return false;
    // Missing _PSC is legal. The owning service must still order all power
    // resources before bind/resume and call suspend before removing power.
    if (mProvider->validateObject("_PSC") != kIOReturnSuccess) return true;
    UInt32 state=0;
    return mProvider->evaluateInteger("_PSC",&state)==kIOReturnSuccess && state==0;
}
IOReturn AMDSPIController::bind(IOACPIPlatformDevice* provider,IOService* owner) {
    if (mProvider || mLock) return kIOReturnBusy;
    if (!provider || !owner) return kIOReturnBadArgument;
    OSObject* object=nullptr;
    IOReturn result=provider->evaluateObject("_HID",&object);
    auto* hid=OSDynamicCast(OSString,object);
    AMDSPIRevision revision=AMDSPIRevision::V1;
    bool supported=false;
    if (result==kIOReturnSuccess && hid) {
        if (hid->isEqualTo("AMDI0061")) supported=true;
        else if (hid->isEqualTo("AMDI0062")) { supported=true; revision=AMDSPIRevision::V2; }
    }
    if (object) object->release();
    if (!supported) return kIOReturnUnsupported;
    mLock=IOLockAlloc(); if (!mLock) return kIOReturnNoMemory;
    mProvider=provider; mProvider->retain(); mOwner=owner;
    if (provider->isOpen() || !provider->open(owner)) {
        provider->release(); mProvider=nullptr; mOwner=nullptr;
        IOLockFree(mLock); mLock=nullptr; return kIOReturnExclusiveAccess;
    }
    mPowerAvailable=true;
    if (!isD0()) { shutdown(); return kIOReturnNotReady; }
    mMap=provider->mapDeviceMemoryWithIndex(0);
    if (!mMap || mMap->getLength()<AMDSPIEngine::kMinimumWindowBytes || !mMap->getVirtualAddress()) {
        shutdown(); return kIOReturnNoResources;
    }
    mWindow.base=reinterpret_cast<volatile uint8_t*>(mMap->getVirtualAddress());
    result=mEngine.initialize(revision);
    if (result!=kIOReturnSuccess) shutdown();
    return result;
}
IOReturn AMDSPIController::command(const SPIEndpoint& ep,const uint8_t* tx,size_t nt,uint8_t* rx,size_t nr) {
    if (!mLock) return kIOReturnNotReady;
    IOLockLock(mLock);
    IOReturn result=mWindow.base && isD0() ? mEngine.command(ep,tx,nt,rx,nr) : kIOReturnNotReady;
    IOLockUnlock(mLock); return result;
}
void AMDSPIController::suspend() {
    if (!mLock) return;
    IOLockLock(mLock); mEngine.suspend(); mPowerAvailable=false; IOLockUnlock(mLock);
}
IOReturn AMDSPIController::resume() {
    if (!mLock) return kIOReturnNotReady;
    IOLockLock(mLock); mPowerAvailable=true;
    IOReturn result=mWindow.base && isD0() ? mEngine.resume() : kIOReturnNotReady;
    if (result!=kIOReturnSuccess) mPowerAvailable=false;
    IOLockUnlock(mLock); return result;
}
void AMDSPIController::shutdown() {
    // The owner has stopped/joined clients; no caller can wait on this lock.
    if (mLock) IOLockLock(mLock);
    if (mWindow.base && isD0()) mEngine.shutdown(); else mEngine.abandon();
    mWindow.base=nullptr; mPowerAvailable=false;
    if (mMap) { mMap->release(); mMap=nullptr; }
    if (mProvider) { mProvider->close(mOwner); mProvider->release(); mProvider=nullptr; }
    mOwner=nullptr;
    if (mLock) { IOLockUnlock(mLock); IOLockFree(mLock); mLock=nullptr; }
}
}
