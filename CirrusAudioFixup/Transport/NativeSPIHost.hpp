// Native SPI attachment. Owner serializes clients and joins them before teardown.
// Board authorization is separate: discovery never enables amplifier writes.
#pragma once
#include "Transport/ACPISPIResources.hpp"
#include "Transport/ACPIPath.hpp"
#include "Core/RegisterMap.hpp"
#include "Core/Endpoint.hpp"
#include "Platform/BackendRegistry.hpp"
#include "Transport/SPIRegisterBus.hpp"
#include "Transport/Intel/IntelLPSSSPIController.hpp"
#include "Transport/AMD/AMDACPIController.hpp"
#include <IOKit/IORegistryEntry.h>
namespace cirrus::transport {
class NativeSPIHost {
public:
    struct Failure {
        const char* reason{"NONE"};IOReturn result{kIOReturnSuccess};
        size_t index{0};uint32_t detail{0};char source[256]{};
    };
    const Failure& failure() const {return mFailure;}
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
        mFailure={};
        if(bound() || !amplifier || !owner || !core::RegisterMap::isValidConfig(format))return fail("INVALID_BIND_ARGUMENT",kIOReturnBadArgument);
        // _STA present/enabled. Do not turn on a firmware-disabled device.
        if((amplifier->getDeviceStatus()&3)!=3)return fail("ACPI_DEVICE_DISABLED",kIOReturnNotReady);
        OSObject* object=nullptr;
        IOReturn ret=amplifier->evaluateObject("_CRS",&object);
        auto* data=OSDynamicCast(OSData,object);
        if(ret!=kIOReturnSuccess || !data){OSSafeReleaseNULL(object);return fail("ACPI_CRS_UNAVAILABLE",ret==kIOReturnSuccess?kIOReturnNotFound:ret);}
        auto result=parseACPISPIResources(static_cast<const uint8_t*>(data->getBytesNoCopy()),data->getLength(),mResources);
        object->release();
        if(result!=ACPISPIResult::Success)return fail("ACPI_SPI_RESOURCE_REJECTED",kIOReturnUnsupported,0,nullptr,uint32_t(result));
        for(size_t i=0;i<mResources.count;++i) {
            auto& r=mResources.endpoints[i];
            if(r.sourceIndex!=0){fail("UNSUPPORTED_RESOURCE_SOURCE_INDEX",kIOReturnUnsupported,i,r.source,r.sourceIndex);shutdown();return mFailure.result;}
            // Multiple resources may share a controller, never the same CS.
            for(size_t j=0;j<i;++j)
                if(!strcmp(r.source,mResources.endpoints[j].source) && r.endpoint.chipSelect==mResources.endpoints[j].endpoint.chipSelect){fail("DUPLICATE_CHIP_SELECT",kIOReturnBadArgument,i,r.source,r.endpoint.chipSelect);shutdown();return mFailure.result;}
            IOACPIPlatformDevice* target=resolve(amplifier,r.source);
            if(!target){fail("CONTROLLER_ACPI_PATH_UNRESOLVED_OR_AMBIGUOUS",kIOReturnNotFound,i,r.source);shutdown();return mFailure.result;}
            size_t slot=0;
            while(slot<mControllers && mTargets[slot]!=target)++slot;
            for(size_t j=0;j<i;++j) {
                if(mSlots[j]==slot && r.endpoint.chipSelect==mResources.endpoints[j].endpoint.chipSelect) {
                    fail("DUPLICATE_CONTROLLER_CHIP_SELECT",kIOReturnBadArgument,i,r.source,r.endpoint.chipSelect);target->release();shutdown();return mFailure.result;
                }
            }
            char registryPath[256]{};int length=sizeof(registryPath);
            auto* plane=IORegistryEntry::getPlane("IOACPIPlane");
            if(!target->getPath(registryPath,&length,plane) || !canonicalACPIPath(registryPath,mPaths[i],sizeof(mPaths[i]))) {
                fail("INVALID_CONTROLLER_ACPI_PATH",kIOReturnBadArgument,i,r.source);target->release();shutdown();return mFailure.result;
            }
            mSlots[i]=slot;
            if(slot==mControllers) {
                mTargets[slot]=target; // Adopt resolver retain, including failure path.
                ++mControllers;
                auto* pci=findPCI(target);
                const bool intelController=pci!=nullptr;
                uint32_t controllerIdentity=0;
                if(pci) {
                    controllerIdentity=(uint32_t(pci->configRead16(0))<<16)|pci->configRead16(2);
                    mIntel[slot]=new intel::IntelLPSSSPIController;
                    ret=mIntel[slot]?mIntel[slot]->bind(pci,owner):kIOReturnNoMemory;
                    pci->release();
                    mBus[slot]=mIntel[slot];
                } else {
                    // This owner does not drive an independent AMD power parent.
                    // Require observable D0 instead of assuming MMIO is accessible.
                    uint32_t power=0;
                    if(target->evaluateInteger("_PSC",&power)!=kIOReturnSuccess || power!=0) {
                        fail("AMD_CONTROLLER_D0_UNVERIFIED",kIOReturnNotReady,i,r.source,power);shutdown();return mFailure.result;
                    }
                    mAMD[slot]=new amd::AMDACPIController;
                    ret=mAMD[slot]?mAMD[slot]->bind(target,owner):kIOReturnNoMemory;
                    mBus[slot]=mAMD[slot];
                }
                if(ret!=kIOReturnSuccess) {
                    const char* reason=intelController?(ret==kIOReturnUnsupported?"INTEL_CONTROLLER_ID_UNSUPPORTED":"INTEL_CONTROLLER_BIND_FAILED"):
                        (ret==kIOReturnUnsupported?"AMD_CONTROLLER_HID_UNSUPPORTED":"AMD_CONTROLLER_BIND_FAILED");
                    fail(reason,ret,i,r.source,controllerIdentity);shutdown();return ret;
                }
            } else target->release();
            // No board write permission is inferred from _CRS or HID.
            mPorts[i]=new SPIRegisterBus(mBus[slot],r.endpoint);
            if(!mPorts[i]){fail("REGISTER_BUS_ALLOCATION_FAILED",kIOReturnNoMemory,i,r.source);shutdown();return mFailure.result;}
            mRegisters[i]=new core::RegisterMap(*mPorts[i],format,false);
            if(!mRegisters[i]){fail("REGISTER_MAP_ALLOCATION_FAILED",kIOReturnNoMemory,i,r.source);shutdown();return mFailure.result;}
            ++mCount;
        }
        return kIOReturnSuccess;
    }
    // Call before lending register ports to clients. Replaces the read-only
    // discovery maps only after every endpoint has a verified board/backend.
    IOReturn configureAuthorized(const platform::BackendRegistry& registry,
                                 const platform::BoardConfiguration& board,
                                 const platform::BoardIdentity& actual,const char* deviceHid,bool allowWrites=true) {
        if(!bound() || mAuthorized || !deviceHid || !board.permits(actual) || board.count!=mCount)
            return fail("BOARD_AUTHORIZATION_REJECTED",kIOReturnNotPermitted);
        mFailure={};
        core::RegisterMap* prepared[ACPISPIResources::kMaxEndpoints]{};
        IOReturn result=kIOReturnSuccess;
        for(size_t i=0;i<mCount;++i) {
            const auto e=endpoint(i);
            const platform::AmplifierBinding* binding=nullptr;
            for(size_t j=0;j<board.count;++j)
                if(e.sameDevice(board.amplifiers[j].endpoint))binding=&board.amplifiers[j];
            const char* expected=binding?(binding->acpiHid?binding->acpiHid:actual.acpiHid):nullptr;
            if(!binding || !expected || strcmp(expected,deviceHid) || e.spiMode!=binding->endpoint.spiMode ||
               e.spiClockHz>binding->endpoint.spiClockHz) {result=fail("BOARD_ENDPOINT_OR_HID_MISMATCH",kIOReturnNotPermitted,i,e.controllerPath);break;}
            const auto* definition=registry.find(deviceHid,binding->siliconId,core::BusTransportType::SPI);
            if(!definition){result=fail("NO_REGISTERED_SPI_BACKEND",kIOReturnUnsupported,i,e.controllerPath,binding->siliconId);break;}
            prepared[i]=new core::RegisterMap(*mPorts[i],*definition->format(core::BusTransportType::SPI),allowWrites);
            if(!prepared[i]){result=fail("AUTHORIZED_MAP_ALLOCATION_FAILED",kIOReturnNoMemory,i,e.controllerPath);break;}
        }
        if(result!=kIOReturnSuccess) {
            for(auto* map:prepared)delete map;
            return result;
        }
        for(size_t i=0;i<mCount;++i){delete mRegisters[i];mRegisters[i]=prepared[i];}
        mAuthorized=true;return kIOReturnSuccess;
    }
    void suspend(){for(size_t i=0;i<mControllers;++i){if(mIntel[i])mIntel[i]->suspend();if(mAMD[i])mAMD[i]->suspend();}}
    IOReturn resume(){
        mFailure={};
        for(size_t i=0;i<mControllers;++i) {
            uint32_t power=0;
            if(mAMD[i] && (mTargets[i]->evaluateInteger("_PSC",&power)!=kIOReturnSuccess || power!=0)) {
                fail("AMD_RESUME_D0_UNVERIFIED",kIOReturnNotReady,i,controllerSource(i),power);suspend();return mFailure.result;
            }
            IOReturn ret=mIntel[i]?mIntel[i]->resume():mAMD[i]->resume();
            if(ret!=kIOReturnSuccess){fail("CONTROLLER_RESUME_FAILED",ret,i,controllerSource(i));suspend();return ret;}
        }
        return kIOReturnSuccess;
    }
    void shutdown(){
        for(size_t i=0;i<ACPISPIResources::kMaxEndpoints;++i){delete mRegisters[i];mRegisters[i]=nullptr;delete mPorts[i];mPorts[i]=nullptr;}
        for(size_t i=0;i<ACPISPIResources::kMaxEndpoints;++i){
            delete mIntel[i];mIntel[i]=nullptr;delete mAMD[i];mAMD[i]=nullptr;
            mBus[i]=nullptr;OSSafeReleaseNULL(mTargets[i]);
        }
        mCount=mControllers=0;mResources={};mAuthorized=false;
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
        char targetPath[256]{},targetName[256]{};int length=sizeof(targetPath);
        auto* plane=IORegistryEntry::getPlane("IOACPIPlane");
        const bool havePath=plane && target->getPath(targetPath,&length,plane) && canonicalACPIPath(targetPath,targetName,sizeof(targetName));
        while(auto* entry=it->getNextObject()) {
            auto* pci=OSDynamicCast(IOPCIDevice,entry);
            bool matches=pci && pci->getProperty("acpi-device")==target;
            if(pci && !matches && havePath) {
                auto* path=OSDynamicCast(OSString,pci->getProperty("acpi-path"));
                char canonical[256]{};
                matches=path && canonicalACPIPath(path->getCStringNoCopy(),canonical,sizeof(canonical)) && !strcmp(canonical,targetName);
            }
            if(matches) {
                if(found){duplicate=true;break;}found=pci;
            }
        }
        if(found && !duplicate)found->retain();
        it->release();return duplicate?nullptr:found;
    }
    const char* controllerSource(size_t slot) const {
        for(size_t i=0;i<mCount;++i)if(mSlots[i]==slot)return mPaths[i];
        return "";
    }
    IOReturn fail(const char* reason,IOReturn result,size_t index=0,const char* source=nullptr,uint32_t detail=0) {
        mFailure={};mFailure.reason=reason;mFailure.result=result;mFailure.index=index;mFailure.detail=detail;
        if(source){size_t n=strlen(source);if(n>=sizeof(mFailure.source))n=sizeof(mFailure.source)-1;memcpy(mFailure.source,source,n);}
        return result;
    }
    // Copy failure context before shutdown clears discovery paths/resources.
    // No hardware traffic or log allocation occurs during error reporting.
    Failure mFailure{};
    ACPISPIResources mResources{};
    size_t mCount{0},mControllers{0};
    bool mAuthorized{false};
    IOACPIPlatformDevice* mTargets[ACPISPIResources::kMaxEndpoints]{};
    intel::IntelLPSSSPIController* mIntel[ACPISPIResources::kMaxEndpoints]{};
    amd::AMDACPIController* mAMD[ACPISPIResources::kMaxEndpoints]{};
    SPIController* mBus[ACPISPIResources::kMaxEndpoints]{};
    core::RegisterMap* mRegisters[ACPISPIResources::kMaxEndpoints]{};
    SPIRegisterBus* mPorts[ACPISPIResources::kMaxEndpoints]{};
    size_t mSlots[ACPISPIResources::kMaxEndpoints]{};
    char mPaths[ACPISPIResources::kMaxEndpoints][256]{};
};
}
