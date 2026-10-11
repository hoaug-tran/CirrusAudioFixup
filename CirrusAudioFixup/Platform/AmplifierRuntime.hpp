// Own silicon backend instances selected from immutable board/firmware bindings.
// Bus/controller providers are borrowed. Owner serializes every method and joins
// all callers before destruction; controller D0 precedes resume. No CPU/model
// switch, board name, register width, reset sequence or transport framing here.
#pragma once
#include "Platform/BackendRegistry.hpp"
#include "Core/AmplifierGroup.hpp"
namespace cirrus::platform {
class AmplifierRuntime {
public:
    static constexpr size_t kCapacity=core::AmplifierGroup::kCapacity;
    explicit AmplifierRuntime(bool resumePlaybackAutomatically=true):mResumeAutomatically(resumePlaybackAutomatically){}
    ~AmplifierRuntime(){unbind();}
    AmplifierRuntime(const AmplifierRuntime&)=delete;
    AmplifierRuntime& operator=(const AmplifierRuntime&)=delete;
    bool bound() const {return mGroup!=nullptr;}
    size_t count() const {return mCount;}
    core::AmplifierStage stage() const {return mGroup?mGroup->stage():core::AmplifierStage::Detached;}
    IOReturn lastReleaseResult() const {return mReleaseResult;}
    const core::AmplifierLifecycleStatus* status(size_t i) const {
        if(i>=mCount)return nullptr;
        return &mSlots[i].lifecycle->status();
    }
    const BackendDefinition* backend(size_t i) const {return i<mCount?mSlots[i].definition:nullptr;}
    IOReturn bind(const BackendRegistry& registry,const BackendContext* contexts,size_t count) {
        if(!contexts)return kIOReturnBadArgument;
        return bindContexts(registry,count,[&](size_t i)->const BackendContext&{return contexts[i];});
    }
    IOReturn bind(const BackendRegistry& registry,const BackendContext* const* contexts,size_t count) {
        if(!contexts || count>kCapacity)return kIOReturnBadArgument;
        for(size_t i=0;i<count;++i)if(!contexts[i])return kIOReturnBadArgument;
        return bindContexts(registry,count,[&](size_t i)->const BackendContext&{return *contexts[i];});
    }
private:
    template<class ContextAt>
    IOReturn bindContexts(const BackendRegistry& registry,size_t count,ContextAt contextAt) {
        if(bound() || !count || count>kCapacity)return kIOReturnBadArgument;
        const auto& board=contextAt(0).board;
        if(board.count!=count || !board.permits(contextAt(0).actualBoard))return kIOReturnNotPermitted;
        // Validate the complete topology before calling any factory. Contexts
        // may arrive in any order but cover each manifest binding exactly once.
        for(size_t i=0;i<count;++i) {
            const auto& c=contextAt(i);
            if(&c.board!=&board || !board.permits(c.actualBoard) || c.bindingIndex>=count ||
               !c.endpoint.sameDevice(board.amplifiers[c.bindingIndex].endpoint))return kIOReturnNotPermitted;
            for(size_t j=0;j<i;++j)
                if(c.bindingIndex==contextAt(j).bindingIndex || &c.io==&contextAt(j).io || c.endpoint.sameDevice(contextAt(j).endpoint))return kIOReturnBadArgument;
        }
        mGroup=new core::AmplifierGroup;
        if(!mGroup)return kIOReturnNoMemory;
        core::AmplifierLifecycle* members[kCapacity]{};
        for(size_t i=0;i<count;++i) {
            auto& slot=mSlots[i];
            ++mCount; // Own partial slot state from this point onward.
            slot.context=new BackendContext(contextAt(i));
            if(!slot.context){releaseUninitialized();return kIOReturnNoMemory;}
            slot.hooks=registry.create(*slot.context,slot.definition);
            if(!slot.hooks){releaseUninitialized();return kIOReturnNotPermitted;}
            slot.lifecycle=new core::AmplifierLifecycle(*slot.hooks);
            if(!slot.lifecycle){releaseUninitialized();return kIOReturnNoMemory;}
            members[i]=slot.lifecycle;
        }
        if(!mGroup->configure(members,count,mResumeAutomatically)){releaseUninitialized();return kIOReturnBadArgument;}
        return kIOReturnSuccess;
    }
public:
    IOReturn runtimeTick(size_t i,const core::PlaybackContext& context) {
        if(i>=mCount)return kIOReturnBadArgument;
        if(stage()!=core::AmplifierStage::Ready && stage()!=core::AmplifierStage::Playing)return kIOReturnNotReady;
        auto& slot=mSlots[i];IOReturn result=slot.hooks->runtimeTick(context);
        if(result==kIOReturnSuccess && !slot.hooks->managesPlaybackTransitions() && !context.hasAudio &&
           slot.lifecycle->status().stage==core::AmplifierStage::Playing)result=slot.lifecycle->stopPlayback();
        return result==kIOReturnSuccess?result:mGroup->failRuntime(slot.lifecycle->reportRuntimeFailure(result));
    }
    bool playbackNeedsPreparation(size_t i) const {return i<mCount && mSlots[i].hooks->playbackNeedsPreparation();}
    bool playbackPrepared(size_t i) const {return i<mCount && mSlots[i].hooks->playbackPrepared();}
    bool playbackPending(size_t i) const {return i<mCount && mSlots[i].hooks->playbackPending();}
    bool playbackFaulted(size_t i) const {return i<mCount && mSlots[i].hooks->playbackFaulted();}
    bool validatePlayback(size_t i,const core::PlaybackContext& context){return i<mCount && mSlots[i].hooks->validatePlayback(context);}
    IOReturn cancelPlayback(size_t i) {
        if(i>=mCount)return kIOReturnBadArgument;
        auto& slot=mSlots[i];auto current=slot.lifecycle->status().stage;
        if(current!=core::AmplifierStage::Ready && current!=core::AmplifierStage::Playing)return kIOReturnNotReady;
        IOReturn result=slot.hooks->managesPlaybackTransitions()?slot.hooks->cancelPlayback():slot.lifecycle->stopPlayback();
        return result==kIOReturnSuccess?result:mGroup->failRuntime(slot.lifecycle->reportRuntimeFailure(result));
    }
    void captureDiagnostics(size_t i){if(i<mCount)mSlots[i].hooks->captureDiagnostics();}
    IOReturn configureInterrupt(size_t i,bool enabled){return i<mCount?mSlots[i].hooks->configureInterrupt(enabled):kIOReturnBadArgument;}
    IOReturn handleInterrupt(size_t i){return i<mCount?mSlots[i].hooks->handleInterrupt():kIOReturnBadArgument;}
    void refreshPlaybackState(){if(mGroup)mGroup->refreshPlaybackState();}
    IOReturn bringUp(){return mGroup?mGroup->bringUp():kIOReturnNotReady;}
    IOReturn startPlayback(){return mGroup?mGroup->startPlayback():kIOReturnNotReady;}
    IOReturn stopPlayback(){return mGroup?mGroup->stopPlayback():kIOReturnNotReady;}
    IOReturn suspend(){return mGroup?mGroup->suspend():kIOReturnNotReady;}
    IOReturn resume(){return mGroup?mGroup->resume():kIOReturnNotReady;}
    // Only after confirmed reset/power removal; never use this to suppress a
    // failed shutdown while the hardware may still be powered.
    void powerLost(){if(mGroup)mGroup->powerLost();}
    IOReturn unbind() {
        if(!mGroup)return mReleaseResult;
        mReleaseResult=mGroup->detach();
        // Cleanup results are retained before releasing backend-owned objects.
        for(size_t i=0;i<mCount;++i)mReleased[i]=mSlots[i].lifecycle->status();
        mReleasedCount=mCount;
        releaseUninitialized();return mReleaseResult;
    }
    const core::AmplifierLifecycleStatus* releasedStatus(size_t i) const {return i<mReleasedCount?&mReleased[i]:nullptr;}
private:
    // Constructors/factories must not perform power or firmware writes. This
    // path is also used when binding fails before lifecycle identification.
    void releaseUninitialized() {
        delete mGroup;mGroup=nullptr;
        for(size_t i=mCount;i>0;--i) {
            auto& slot=mSlots[i-1];
            delete slot.lifecycle;
            if(slot.hooks)slot.definition->destroy(slot.hooks);
            delete slot.context;
            slot={};
        }
        mCount=0;
    }
    struct Slot {
        BackendContext* context{nullptr};
        const BackendDefinition* definition{nullptr};
        core::AmplifierLifecycleHooks* hooks{nullptr};
        core::AmplifierLifecycle* lifecycle{nullptr};
    };
    core::AmplifierGroup* mGroup{nullptr};
    Slot mSlots[kCapacity]{};
    size_t mCount{0},mReleasedCount{0};
    core::AmplifierLifecycleStatus mReleased[kCapacity]{};
    bool mResumeAutomatically{true};
    IOReturn mReleaseResult{kIOReturnSuccess};
};
}
