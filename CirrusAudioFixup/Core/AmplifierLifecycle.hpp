// Common lifecycle ordering for future silicon backends. Owner serializes all
// calls on a sleepable workloop and guarantees controller/platform lifetime.
// Firmware formats, calibration, reset, boost and HDA routing stay in backend
// and board/resource providers; no generic register cache is replayed on wake.
#pragma once
#include <IOKit/IOLib.h>
#include <stdint.h>
namespace cirrus::core {
enum class AmplifierStage { Detached, Identify, Authorize, Initialize, Firmware, Verify,
                            Ready, Start, Playing, Stop, Shutdown, Suspended, Faulted, Waiting, DebugHalted };
enum class BringUpDisposition { Continue, Wait, DebugHalt };
class AmplifierLifecycleHooks {
public:
    virtual ~AmplifierLifecycleHooks()=default;
    virtual BringUpDisposition disposition() const {return BringUpDisposition::Continue;}
    virtual IOReturn identify()=0; // Read-only and reject unknown silicon.
    virtual IOReturn authorizeBoard()=0; // Exact wiring/resource/tuning validation.
    virtual IOReturn initialize()=0;
    virtual IOReturn loadFirmware()=0; // No-DSP backends explicitly return success.
    virtual IOReturn verifyReady()=0;
    virtual IOReturn startPlayback()=0; // Includes board route and ready checks.
    virtual IOReturn stopPlayback()=0;
    virtual IOReturn safeShutdown()=0; // Bounded, usable after partial initialization.
};
struct AmplifierLifecycleStatus {
    AmplifierStage stage{AmplifierStage::Detached};
    AmplifierStage firstFailureStage{AmplifierStage::Detached};
    IOReturn firstFailure{kIOReturnSuccess};
    IOReturn latestFailure{kIOReturnSuccess};
    IOReturn cleanupResult{kIOReturnSuccess};
    uint32_t failures{0};
};
class AmplifierLifecycle {
public:
    explicit AmplifierLifecycle(AmplifierLifecycleHooks& hooks):mHooks(hooks) {}
    const AmplifierLifecycleStatus& status() const {return mStatus;}
    IOReturn bringUp() {
        if(mStatus.stage!=AmplifierStage::Detached)return kIOReturnNotReady;
        IOReturn ret=step(AmplifierStage::Identify,&AmplifierLifecycleHooks::identify);
        if(ret!=kIOReturnSuccess)return ret;
        ret=step(AmplifierStage::Authorize,&AmplifierLifecycleHooks::authorizeBoard);
        if(ret!=kIOReturnSuccess)return ret;
        mHardwareTouched=true;
        ret=step(AmplifierStage::Initialize,&AmplifierLifecycleHooks::initialize);
        if(ret!=kIOReturnSuccess)return ret;
        ret=step(AmplifierStage::Firmware,&AmplifierLifecycleHooks::loadFirmware);
        if(ret!=kIOReturnSuccess)return ret;
        ret=step(AmplifierStage::Verify,&AmplifierLifecycleHooks::verifyReady);
        if(ret==kIOReturnSuccess)mStatus.stage=AmplifierStage::Ready;
        return ret;
    }
    IOReturn startPlayback() {
        if(mStatus.stage!=AmplifierStage::Ready)return kIOReturnNotReady;
        IOReturn ret=step(AmplifierStage::Start,&AmplifierLifecycleHooks::startPlayback);
        if(ret==kIOReturnSuccess)mStatus.stage=AmplifierStage::Playing;
        return ret;
    }
    IOReturn stopPlayback() {
        if(mStatus.stage==AmplifierStage::Ready)return kIOReturnSuccess;
        if(mStatus.stage!=AmplifierStage::Playing)return kIOReturnNotReady;
        IOReturn ret=step(AmplifierStage::Stop,&AmplifierLifecycleHooks::stopPlayback);
        if(ret==kIOReturnSuccess)mStatus.stage=AmplifierStage::Ready;
        return ret;
    }
    IOReturn suspend() {
        if(mStatus.stage!=AmplifierStage::Ready && mStatus.stage!=AmplifierStage::Playing)return kIOReturnNotReady;
        mResumePlayback=mStatus.stage==AmplifierStage::Playing;
        IOReturn ret=stopPlayback(); if(ret!=kIOReturnSuccess)return ret;
        ret=step(AmplifierStage::Shutdown,&AmplifierLifecycleHooks::safeShutdown);
        if(ret==kIOReturnSuccess) {mHardwareTouched=false;mStatus.stage=AmplifierStage::Suspended;}
        return ret;
    }
    // Owner restores bus/platform power before calling resume. Wake revalidates
    // identity, board and firmware; stale cached coefficients are not replayed.
    IOReturn resume() {
        if(mStatus.stage!=AmplifierStage::Suspended)return kIOReturnNotReady;
        mStatus.stage=AmplifierStage::Detached;
        IOReturn ret=bringUp();
        return ret==kIOReturnSuccess && mResumePlayback ? startPlayback() : ret;
    }
    IOReturn detach() {
        if(mStatus.stage==AmplifierStage::Faulted)return kIOReturnNotReady;
        if(mStatus.stage==AmplifierStage::Playing) {
            IOReturn ret=stopPlayback();if(ret!=kIOReturnSuccess)return ret;
        }
        if(mHardwareTouched) {
            IOReturn ret=step(AmplifierStage::Shutdown,&AmplifierLifecycleHooks::safeShutdown);
            if(ret!=kIOReturnSuccess)return ret;
        }
        mHardwareTouched=false;mResumePlayback=false;mStatus.stage=AmplifierStage::Detached;
        return kIOReturnSuccess;
    }
    // Explicit owner notification after confirmed power removal / reset.
    // Avoid touching inaccessible hardware; preserve first-failure evidence.
    void powerLost() {mHardwareTouched=false;mResumePlayback=false;mStatus.stage=AmplifierStage::Detached;}
private:
    using Operation=IOReturn(AmplifierLifecycleHooks::*)();
    IOReturn step(AmplifierStage stage,Operation operation) {
        mStatus.stage=stage; IOReturn ret=(mHooks.*operation)();
        if(ret==kIOReturnSuccess && (stage==AmplifierStage::Initialize || stage==AmplifierStage::Firmware || stage==AmplifierStage::Verify)) {
            const auto disposition=mHooks.disposition();
            if(disposition!=BringUpDisposition::Continue) {
                // A deliberate debug stop or pending identity is neither ready
                // nor a hardware failure. Owner may detach/reset before retry.
                mStatus.stage=disposition==BringUpDisposition::Wait ? AmplifierStage::Waiting : AmplifierStage::DebugHalted;
                return kIOReturnNotReady;
            }
        }
        if(ret!=kIOReturnSuccess) {
            if(!mStatus.failures) {mStatus.firstFailure=ret;mStatus.firstFailureStage=stage;}
            mStatus.latestFailure=ret;++mStatus.failures;
            // Do not retry failed shutdown or blindly rerun failed power writes.
            if(mHardwareTouched && stage!=AmplifierStage::Shutdown) {
                mStatus.cleanupResult=mHooks.safeShutdown();
                if(mStatus.cleanupResult==kIOReturnSuccess)mHardwareTouched=false;
            } else if(stage==AmplifierStage::Shutdown)mStatus.cleanupResult=ret;
            mStatus.stage=AmplifierStage::Faulted;
        }
        return ret;
    }
    AmplifierLifecycleHooks& mHooks;
    AmplifierLifecycleStatus mStatus{};
    bool mHardwareTouched{false},mResumePlayback{false};
};
// Synchronous owner adapter: callback references never escape this call.
// Playback remains unavailable until a persistent backend owns those hooks.
template<class Initialize,class Firmware,class Verify,class Cleanup,class Disposition>
AmplifierLifecycleStatus runAmplifierBringUp(Initialize& initialize,Firmware& firmware,
                                            Verify& verify,Cleanup& cleanup,Disposition& disposition) {
    class Hooks final:public AmplifierLifecycleHooks {
    public:
        Hooks(Initialize& i,Firmware& f,Verify& v,Cleanup& c,Disposition& d):init(i),fw(f),ready(v),shutdown(c),state(d) {}
        IOReturn identify() override {return kIOReturnSuccess;} // Owner already probed silicon.
        IOReturn authorizeBoard() override {return kIOReturnSuccess;} // Owner gates before invocation.
        IOReturn initialize() override {return init();}
        IOReturn loadFirmware() override {return fw();}
        IOReturn verifyReady() override {return ready();}
        IOReturn safeShutdown() override {return shutdown();}
        IOReturn startPlayback() override {return kIOReturnNotReady;}
        IOReturn stopPlayback() override {return kIOReturnNotReady;}
        BringUpDisposition disposition() const override {return state();}
    private:
        Initialize& init;Firmware& fw;Verify& ready;Cleanup& shutdown;Disposition& state;
    } hooks(initialize,firmware,verify,cleanup,disposition);
    AmplifierLifecycle lifecycle(hooks);
    lifecycle.bringUp();
    return lifecycle.status();
}
}
