// Native SPI controller ownership + registered silicon runtime. No board/model
// special cases. Host IOService serializes calls and joins clients before unbind.
#pragma once
#include "Platform/AmplifierRuntime.hpp"
#include "Transport/NativeSPIHost.hpp"
namespace cirrus::platform {
class NativeSPIAmplifierRuntime {
    static_assert(transport::ACPISPIResources::kMaxEndpoints==AmplifierRuntime::kCapacity,"SPI and amplifier capacities must agree");
public:
    ~NativeSPIAmplifierRuntime(){unbind();}
    NativeSPIAmplifierRuntime()=default;
    NativeSPIAmplifierRuntime(const NativeSPIAmplifierRuntime&)=delete;
    NativeSPIAmplifierRuntime& operator=(const NativeSPIAmplifierRuntime&)=delete;
    const AmplifierRuntime& runtime() const {return mRuntime;}
    bool bound() const {return mRuntime.bound();}
    IOReturn bind(IOACPIPlatformDevice* device,IOService* owner,const BackendRegistry& registry,
                  const BoardConfiguration& board,const BoardIdentity& actual,
                  const firmware::FirmwareBundle* firmware,size_t count,
                  void* const* platformContexts=nullptr) {
        if(mHost.bound() || !device || !owner || !firmware || count!=board.count ||
           count>transport::ACPISPIResources::kMaxEndpoints || !board.permits(actual))return kIOReturnBadArgument;
        OSObject* object=nullptr;
        IOReturn result=device->evaluateObject("_HID",&object);
        mHid=OSDynamicCast(OSString,object);
        if(result!=kIOReturnSuccess || !mHid){OSSafeReleaseNULL(object);mHid=nullptr;return kIOReturnNotFound;}
        const char* hid=mHid->getCStringNoCopy();
        const auto* first=registry.find(hid,board.amplifiers[0].siliconId,core::BusTransportType::SPI);
        if(!first){releaseHost();return kIOReturnUnsupported;}
        result=mHost.bind(device,owner,*first->format(core::BusTransportType::SPI));
        if(result==kIOReturnSuccess)result=mHost.configureAuthorized(registry,board,actual,hid);
        if(result!=kIOReturnSuccess){releaseHost();return result;}
        const BackendContext* contexts[transport::ACPISPIResources::kMaxEndpoints]{};
        for(size_t i=0;i<count;++i) {
            auto endpoint=mHost.endpoint(i);
            size_t binding=0;
            while(binding<count && !endpoint.sameDevice(board.amplifiers[binding].endpoint))++binding;
            if(binding==count){result=kIOReturnNotPermitted;break;}
            contexts[i]=new BackendContext{*mHost.registers(i),endpoint,board,actual,binding,firmware[binding],hid,platformContexts?platformContexts[binding]:nullptr};
            if(!contexts[i]){result=kIOReturnNoMemory;break;}
        }
        if(result==kIOReturnSuccess)result=mRuntime.bind(registry,contexts,count);
        for(auto* context:contexts)delete context;
        if(result!=kIOReturnSuccess)releaseHost();
        return result;
    }
    IOReturn bringUp(){return mSuspended?kIOReturnNotReady:mRuntime.bringUp();}
    IOReturn startPlayback(){return mSuspended?kIOReturnNotReady:mRuntime.startPlayback();}
    IOReturn stopPlayback(){return mSuspended?kIOReturnNotReady:mRuntime.stopPlayback();}
    IOReturn suspend() {
        if(mSuspended)return kIOReturnNotReady;
        IOReturn result=mRuntime.suspend();
        // Failed cleanup keeps controllers accessible for owner diagnostics.
        if(result!=kIOReturnSuccess)return result;
        mHost.suspend();mSuspended=true;return kIOReturnSuccess;
    }
    IOReturn resume() {
        if(!mSuspended)return kIOReturnNotReady;
        // Controller D0 must precede identity/firmware/playback transactions.
        IOReturn result=mHost.resume();
        if(result!=kIOReturnSuccess)return result;
        mSuspended=false;
        if(mNeedsBringUp){mNeedsBringUp=false;return mRuntime.bringUp();}
        return mRuntime.resume();
    }
    // Owner has confirmed amplifier reset or actual power removal. Clearing
    // local fault state does not reset hardware, and never restarts playback.
    IOReturn notifyPowerLost() {
        if(!bound())return kIOReturnNotReady;
        mRuntime.powerLost();mHost.suspend();mSuspended=true;mNeedsBringUp=true;
        return kIOReturnSuccess;
    }
    IOReturn unbind() {
        IOReturn result=mRuntime.unbind();
        // Backends are destroyed before any borrowed register port/controller.
        releaseHost();return result;
    }
private:
    void releaseHost(){mHost.shutdown();OSSafeReleaseNULL(mHid);mSuspended=false;mNeedsBringUp=false;}
    transport::NativeSPIHost mHost;
    AmplifierRuntime mRuntime;
    OSString* mHid{nullptr};
    bool mSuspended{false},mNeedsBringUp{false};
};
}
