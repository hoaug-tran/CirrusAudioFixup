// Native SPI attachment. Owner serializes clients and joins them before teardown.
// Board authorization is separate: discovery never enables amplifier writes.
#pragma once
#include "Transport/ACPISPIResources.hpp"
#include "Transport/ACPIPath.hpp"
#include "Core/RegisterMap.hpp"
#include "Core/Endpoint.hpp"
#include "Transport/SPIRegisterBus.hpp"
#include "Transport/Intel/IntelLPSSSPIController.hpp"
#include "Transport/AMD/AMDACPIController.hpp"
#include <IOKit/IORegistryEntry.h>
namespace cirrus::transport {
class NativeSPIHost {
public:
    ~NativeSPIHost(){shutdown();}
    bool bound() const {return mCount!=0;}
    size_t count() const {return mCount;}
    core::RegisterIO* registers(size_t i) const {return i<mCount?mRegisters[i]:nullptr;}
    const ACPISPIResource* resource(size_t i) const {return i<mCount?&mResources.endpoints[i]:nullptr;}
    core::BusEndpoint endpoint(size_t i) const {
        if(i>=mCount)return {};
        const auto& r=mResources.endpoints[i];
        return {core::BusTransportType::SPI,mPaths[i],0,false,r.endpoint.chipSelect,r.endpoint.clockHz,r.endpoint.mode};
    }
    IOReturn bind(IOACPIPlatformDevice* amplifier,IOService* owner,const core::RegisterMapConfig& format) {
        if(bound() || !amplifier || !owner || !core::RegisterMap::isValidConfig(format))return kIOReturnBadArgument;
        // _STA present/enabled. Do not turn on a firmware-disabled device.
        if((amplifier->getDeviceStatus()&3)!=3)return kIOReturnNotReady;
        OSObject* object=nullptr;
        IOReturn ret=amplifier->evaluateObject("_CRS",&object);
        auto* data=OSDynamicCast(OSData,object);
        if(ret!=kIOReturnSuccess || !data){OSSafeReleaseNULL(object);return kIOReturnNotFound;}
        auto result=parseACPISPIResources(static_cast<const uint8_t*>(data->getBytesNoCopy()),data->getLength(),mResources);
        object->release();
        if(result!=ACPISPIResult::Success)return kIOReturnUnsupported;
        for(size_t i=0;i<mResources.count;++i) {
            auto& r=mResources.endpoints[i];
            if(r.sourceIndex!=0){shutdown();return kIOReturnUnsupported;}
            // Multiple resources may share a controller, never the same CS.
            for(size_t j=0;j<i;++j)
                if(!strcmp(r.source,mResources.endpoints[j].source) && r.endpoint.chipSelect==mResources.endpoints[j].endpoint.chipSelect){shutdown();return kIOReturnBadArgument;}
            IOACPIPlatformDevice* target=resolve(amplifier,r.source);
            if(!target){shutdown();return kIOReturnNotFound;}
            size_t slot=0;
            while(slot<mControllers && mTargets[slot]!=target)++slot;
            for(size_t j=0;j<i;++j) {
                if(mSlots[j]==slot && r.endpoint.chipSelect==mResources.endpoints[j].endpoint.chipSelect) {
                    target->release();shutdown();return kIOReturnBadArgument;
                }
            }
            char registryPath[256]{};int length=sizeof(registryPath);
            auto* plane=IORegistryEntry::getPlane("IOACPIPlane");
            if(!target->getPath(registryPath,&length,plane) || !canonicalACPIPath(registryPath,mPaths[i],sizeof(mPaths[i]))) {
                target->release();shutdown();return kIOReturnBadArgument;
            }
            mSlots[i]=slot;
            if(slot==mControllers) {
                mTargets[slot]=target; // Adopt resolver retain, including failure path.
                ++mControllers;
                auto* pci=findPCI(target);
                if(pci) {
                    mIntel[slot]=new intel::IntelLPSSSPIController;
                    ret=mIntel[slot]?mIntel[slot]->bind(pci,owner):kIOReturnNoMemory;
                    pci->release();
                    mBus[slot]=mIntel[slot];
                } else {
                    // This owner does not drive an independent AMD power parent.
                    // Require observable D0 instead of assuming MMIO is accessible.
                    uint32_t power=0;
                    if(target->evaluateInteger("_PSC",&power)!=kIOReturnSuccess || power!=0) {
                        shutdown();return kIOReturnNotReady;
                    }
                    mAMD[slot]=new amd::AMDACPIController;
                    ret=mAMD[slot]?mAMD[slot]->bind(target,owner):kIOReturnNoMemory;
                    mBus[slot]=mAMD[slot];
                }
                if(ret!=kIOReturnSuccess){shutdown();return ret;}
            } else target->release();
            // No board write permission is inferred from _CRS or HID.
            mPorts[i]=new SPIRegisterBus(mBus[slot],r.endpoint);
            if(!mPorts[i]){shutdown();return kIOReturnNoMemory;}
            mRegisters[i]=new core::RegisterMap(*mPorts[i],format,false);
            if(!mRegisters[i]){shutdown();return kIOReturnNoMemory;}
            ++mCount;
        }
        return kIOReturnSuccess;
    }
    void suspend(){for(size_t i=0;i<mControllers;++i){if(mIntel[i])mIntel[i]->suspend();if(mAMD[i])mAMD[i]->suspend();}}
    IOReturn resume(){
        for(size_t i=0;i<mControllers;++i) {
            uint32_t power=0;
            if(mAMD[i] && (mTargets[i]->evaluateInteger("_PSC",&power)!=kIOReturnSuccess || power!=0)) {
                suspend();return kIOReturnNotReady;
            }
            IOReturn ret=mIntel[i]?mIntel[i]->resume():mAMD[i]->resume();
            if(ret!=kIOReturnSuccess){suspend();return ret;}
        }
        return kIOReturnSuccess;
    }
    void shutdown(){
        for(size_t i=0;i<ACPISPIResources::kMaxEndpoints;++i){delete mRegisters[i];mRegisters[i]=nullptr;delete mPorts[i];mPorts[i]=nullptr;}
        for(size_t i=0;i<ACPISPIResources::kMaxEndpoints;++i){
            delete mIntel[i];mIntel[i]=nullptr;delete mAMD[i];mAMD[i]=nullptr;
            mBus[i]=nullptr;OSSafeReleaseNULL(mTargets[i]);
        }
        mCount=mControllers=0;mResources={};
        memset(mPaths,0,sizeof(mPaths));memset(mSlots,0,sizeof(mSlots));
    }
private:
    NativeSPIHost(const NativeSPIHost&)=delete;
    NativeSPIHost& operator=(const NativeSPIHost&)=delete;
public:
    NativeSPIHost()=default;
private:
    static IOACPIPlatformDevice* resolve(IOACPIPlatformDevice* device,const char* source) {
        auto* plane=IORegistryEntry::getPlane("IOACPIPlane");
        if(!plane)return nullptr;
        IORegistryEntry* scope=device;
        do {
            char registryPath[256]{},canonical[256]{},wanted[256]{};int length=sizeof(registryPath);
            if(!scope->getPath(registryPath,&length,plane) || !canonicalACPIPath(registryPath,canonical,sizeof(canonical)) ||
               !resolveACPIPath(canonical,source,wanted,sizeof(wanted)))return nullptr;
            auto* it=IORegistryIterator::iterateOver(IORegistryEntry::getRegistryRoot(),plane,kIORegistryIterateRecursively);
            if(!it)return nullptr;
            IOACPIPlatformDevice* found=nullptr;bool duplicate=false;
            while(auto* entry=it->getNextObject()) {
                auto* candidate=OSDynamicCast(IOACPIPlatformDevice,entry);
                char path[256]{},normal[256]{};length=sizeof(path);
                if(candidate && candidate->getPath(path,&length,plane) && canonicalACPIPath(path,normal,sizeof(normal)) && !strcmp(normal,wanted)) {
                    if(found){duplicate=true;break;}found=candidate;
                }
            }
            if(found && !duplicate)found->retain();
            it->release();
            if(duplicate)return nullptr;
            if(found)return found;
            if(*source=='\\' || *source=='^')break;
            scope=scope->getParentEntry(plane);
        } while(scope);
        return nullptr;
    }
    static IOPCIDevice* findPCI(IOACPIPlatformDevice* target) {
        auto* it=IOService::getMatchingServices(IOService::serviceMatching("IOPCIDevice"));
        if(!it)return nullptr;
        IOPCIDevice* found=nullptr;bool duplicate=false;
        while(auto* entry=it->getNextObject()) {
            auto* pci=OSDynamicCast(IOPCIDevice,entry);
            if(pci && pci->getProperty("acpi-device")==target) {
                if(found){duplicate=true;break;}found=pci;
            }
        }
        if(found && !duplicate)found->retain();
        it->release();return duplicate?nullptr:found;
    }
    ACPISPIResources mResources{};
    size_t mCount{0},mControllers{0};
    IOACPIPlatformDevice* mTargets[4]{};
    intel::IntelLPSSSPIController* mIntel[4]{};
    amd::AMDACPIController* mAMD[4]{};
    SPIController* mBus[4]{};
    core::RegisterMap* mRegisters[4]{};
    SPIRegisterBus* mPorts[4]{};
    size_t mSlots[4]{};
    char mPaths[4][256]{};
};
}
