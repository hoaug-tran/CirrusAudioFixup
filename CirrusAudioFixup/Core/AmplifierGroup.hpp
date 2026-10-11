// Coordinate borrowed amplifier lifecycles on the owner's serialized workloop.
// Roll back healthy peers when one member fails; never start later members
// after failure or automatically retry a faulted power sequence.
#pragma once
#include "AmplifierLifecycle.hpp"
#include <stddef.h>
namespace cirrus::core {
class AmplifierGroup {
public:
    static constexpr size_t kCapacity=16;
    bool configure(AmplifierLifecycle* const* members,size_t count) {
        if(mCount || !members || !count || count>kCapacity)return false;
        for(size_t i=0;i<count;++i) {
            if(!members[i] || members[i]->status().stage!=AmplifierStage::Detached)return false;
            for(size_t j=0;j<i;++j)if(members[i]==members[j])return false;
        }
        for(size_t i=0;i<count;++i)mMembers[i]=members[i];mCount=count;return true;
    }
    AmplifierStage stage() const {return mStage;}
    IOReturn firstFailure() const {return mFirstFailure;}
    IOReturn bringUp() {
        if(!mCount || mStage!=AmplifierStage::Detached)return kIOReturnNotReady;
        for(size_t i=0;i<mCount;++i) {
            IOReturn ret=mMembers[i]->bringUp();
            if(ret!=kIOReturnSuccess)return fail(ret,i);
        }
        mStage=AmplifierStage::Ready;return kIOReturnSuccess;
    }
    IOReturn startPlayback() {
        if(mStage!=AmplifierStage::Ready)return kIOReturnNotReady;
        for(size_t i=0;i<mCount;++i) {
            IOReturn ret=mMembers[i]->startPlayback();
            if(ret!=kIOReturnSuccess)return fail(ret,mCount);
        }
        mStage=AmplifierStage::Playing;return kIOReturnSuccess;
    }
    IOReturn stopPlayback() {
        if(mStage!=AmplifierStage::Playing && mStage!=AmplifierStage::Ready)return kIOReturnNotReady;
        IOReturn result=kIOReturnSuccess;
        for(size_t i=mCount;i>0;--i) {
            IOReturn ret=mMembers[i-1]->stopPlayback();if(result==kIOReturnSuccess)result=ret;
        }
        if(result!=kIOReturnSuccess)return fail(result,mCount);
        mStage=AmplifierStage::Ready;return kIOReturnSuccess;
    }
    IOReturn suspend() {
        if(mStage!=AmplifierStage::Ready && mStage!=AmplifierStage::Playing)return kIOReturnNotReady;
        mResumePlayback=mStage==AmplifierStage::Playing;
        IOReturn result=stopPlayback();if(result!=kIOReturnSuccess)return result;
        for(size_t i=mCount;i>0;--i) {
            IOReturn ret=mMembers[i-1]->suspend();if(result==kIOReturnSuccess)result=ret;
        }
        if(result!=kIOReturnSuccess)return fail(result,mCount);
        mStage=AmplifierStage::Suspended;return kIOReturnSuccess;
    }
    IOReturn resume() {
        if(mStage!=AmplifierStage::Suspended)return kIOReturnNotReady;
        for(size_t i=0;i<mCount;++i) {
            IOReturn ret=mMembers[i]->resume();if(ret!=kIOReturnSuccess)return fail(ret,i);
        }
        mStage=AmplifierStage::Ready;
        return mResumePlayback ? startPlayback() : kIOReturnSuccess;
    }
    IOReturn detach() {
        IOReturn result=kIOReturnSuccess;
        for(size_t i=mCount;i>0;--i) {
            IOReturn ret=mMembers[i-1]->detach();if(result==kIOReturnSuccess)result=ret;
        }
        if(result!=kIOReturnSuccess)return fail(result,0);
        mResumePlayback=false;mStage=AmplifierStage::Detached;return kIOReturnSuccess;
    }
    void powerLost() {
        for(size_t i=0;i<mCount;++i)mMembers[i]->powerLost();
        mResumePlayback=false;mStage=AmplifierStage::Detached;
    }
private:
    IOReturn fail(IOReturn result,size_t rollbackCount) {
        if(mFirstFailure==kIOReturnSuccess)mFirstFailure=result;
        for(size_t i=rollbackCount;i>0;--i) {
            auto* member=mMembers[i-1];
            if(member->status().stage!=AmplifierStage::Faulted)member->detach();
        }
        mStage=AmplifierStage::Faulted;return result;
    }
    AmplifierLifecycle* mMembers[kCapacity]{};
    size_t mCount{0};
    AmplifierStage mStage{AmplifierStage::Detached};
    IOReturn mFirstFailure{kIOReturnSuccess};
    bool mResumePlayback{false};
};
}
