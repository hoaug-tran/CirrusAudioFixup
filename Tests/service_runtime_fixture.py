"""Production orchestration extraction with mocked silicon primitives.

This fixture intentionally borrows mock chip state to inspect isolated stage
faults. It is not the production ownership contract. check_cs35l41_driver links
and exercises the complete independent production driver.
"""
from pathlib import Path
from production_source_fixture import method
ROOT=Path(__file__).resolve().parents[1]
HEADER=(ROOT/'CirrusAudioFixup/CirrusAudioFixup.hpp').read_text()
a=HEADER.index('    struct AmplifierHostAdapter final')
b=HEADER.index('    char mRuntimeControllerPath',a)
PLATFORM_ADAPTER=HEADER[a:b]
PRODUCTION_DRIVER=""
FIXTURE_SHIM=r'''

    struct FixtureHost final : cirrus::platform::AmplifierHost {
        CirrusAudioFixup* owner=nullptr; AmplifierState* amp=nullptr;
        void bind(CirrusAudioFixup* o,AmplifierState* a){owner=o;amp=a;}
        void clear(){owner=nullptr;amp=nullptr;}
        AmplifierState* state() const{return owner?amp:nullptr;}
        bool authorized() const override{return owner->mPlatformAuthorized;}
        const char* name() const override{return amp->name;}
        void setDiagnosticStage(cirrus::diagnostics::DriverStage s) override{owner->setDiagnosticStage(*amp,s);}
        void markDiagnosticSuccess(cirrus::diagnostics::DriverStage s) override{owner->markDiagnosticSuccess(*amp,s);}
        void recordDiagnosticFailure(cirrus::diagnostics::DiagnosticFailure f,uint32_t r,uint32_t e,uint32_t a,IOReturn ret,bool=true) override{owner->recordDiagnosticFailure(*amp,f,r,e,a,ret);}
    };
    struct FixtureHooks final : cirrus::core::AmplifierLifecycleHooks {
        FixtureHost& host;CirrusAudioFixup& owner;AmplifierState& mAmplifier;
        const cirrus::platform::AmplifierDriverContext& mContext;
        FixtureHooks(const cirrus::platform::AmplifierDriverContext& c,FixtureHost& h):host(h),owner(*h.owner),mAmplifier(*h.amp),mContext(c){}
        cirrus::core::BringUpDisposition disposition() const override{return mAmplifier.bringUpDisposition;}
        IOReturn identify() override{uint32_t id=0;if(!mContext.io.read(0,&id))return mContext.io.lastError();return id==0x35a40?0:kIOReturnNotFound;}
        IOReturn authorizeBoard() override{return host.authorized()?0:kIOReturnNotPermitted;}
        IOReturn initialize() override{auto r=initializeHardware();if(r!=0)mAmplifier.playbackFaulted=true;return r;}
        IOReturn loadFirmware() override{auto r=loadFirmwareFlow();if(r!=0)mAmplifier.playbackFaulted=true;return r;}
        IOReturn verifyReady() override{auto r=verifyReadyFlow();if(r!=0)mAmplifier.playbackFaulted=true;return r;}
        IOReturn safeShutdown() override{auto r=shutdownFlow();if(r!=0)mAmplifier.playbackFaulted=true;return r;}
        IOReturn startPlayback() override{return owner.commitCS35L41Playback(mAmplifier);}
        IOReturn stopPlayback() override{return owner.stopPlayback(mAmplifier)?0:kIOReturnNotReady;}
        void captureDiagnostics() override{owner.captureCS35L41FailureSnapshot(mAmplifier,mAmplifier.diagnostic.latestFailure);}
        IOReturn configureInterrupt(bool e) override{return owner.configureCS35L41Interrupt(mAmplifier,e)?0:kIOReturnNotReady;}
        IOReturn handleInterrupt() override{if(owner.checkProtectionStatus(mAmplifier))return 0;mAmplifier.playbackFaulted=true;return kIOReturnNotReady;}
        bool managesPlaybackTransitions() const override{return true;}
        IOReturn runtimeTick(const cirrus::core::PlaybackContext& c) override{owner.monitorCS35L41(mAmplifier,c);return 0;}
        bool validatePlayback(const cirrus::core::PlaybackContext& c) override{return owner.validateCS35L41Playback(mAmplifier,c);}
        bool playbackNeedsPreparation() const override{return mAmplifier.initialized && !mAmplifier.playbackActive && (!mAmplifier.playbackFaulted || (mAmplifier.pllRecoveryPending && mAmplifier.pllRecoveryAttempts<3));}
        bool playbackPrepared() const override{return mAmplifier.initialized && !mAmplifier.playbackFaulted && mAmplifier.playbackTransition.phase==cirrus::devices::amplifiers::cs35l41::PlaybackPhase::Prepared;}
        bool playbackFaulted() const override{return mAmplifier.playbackFaulted && (!mAmplifier.pllRecoveryPending || mAmplifier.pllRecoveryAttempts>=3);}
        bool playbackStartCancelled() const override{return mAmplifier.playbackStartCancelled;}
        bool playbackPending() const override{return mAmplifier.playbackTransition.pending() || mAmplifier.shutdownTransition.pending();}
        IOReturn cancelPlayback() override{if(!mAmplifier.shutdownTransition.pending() && mAmplifier.cleanupAttempts<3 && (mAmplifier.playbackActive || mAmplifier.playbackTransition.pending()))owner.stopRuntimePlayback(mAmplifier);return 0;}
        cirrus::core::ObservedPlayback observedPlayback() const override{return mAmplifier.playbackActive?cirrus::core::ObservedPlayback::Active:cirrus::core::ObservedPlayback::Idle;}
        cirrus::core::AmplifierReport report() const override{return {true,mAmplifier.initialized,playbackFaulted(),mAmplifier.firmwareIdentityPending,mAmplifier.present,mAmplifier.dspAlive && mAmplifier.firmwareValidated?"READY_DSP":owner.bootArgEnabled("-cirrusnodsp")?"READY_BYPASS_EXPLICIT":"OUTPUT_DISABLED_DSP_UNVERIFIED","WAITING_FOR_CODEC_IDENTITY"};}
        IOReturn prepareBringUpRetry(uint64_t now) override{owner.discoverFirmware(mAmplifier);if(mAmplifier.firmwareIdentityPending){if(now<mAmplifier.firmwareIdentityDeadlineMs)return kIOReturnNotReady;mAmplifier.firmwareIdentityPending=false;mAmplifier.playbackFaulted=true;owner.recordDiagnosticFailure(mAmplifier,cirrus::diagnostics::DIAG_FIRMWARE_MISSING);return kIOReturnTimeout;}return 0;}
        bool stopAt(const char* phase){if(!owner.stopAfterDebugStage(phase))return false;mAmplifier.bringUpDisposition=cirrus::core::BringUpDisposition::DebugHalt;return true;}
        bool bootArgEnabled(const char* key){return owner.bootArgEnabled(key);}
        bool bootArgStrEquals(const char* key,const char* value){return owner.bootArgStrEquals(key,value);}
        void setDiagnosticStage(AmplifierState& a,cirrus::diagnostics::DriverStage s){owner.setDiagnosticStage(a,s);}
        void markDiagnosticSuccess(AmplifierState& a,cirrus::diagnostics::DriverStage s){owner.markDiagnosticSuccess(a,s);}
        template<class... T> void recordDiagnosticFailure(AmplifierState& a,cirrus::diagnostics::DiagnosticFailure f,T... t){owner.recordDiagnosticFailure(a,f,t...);}
        bool initializeCS35L41Device(AmplifierState& a){return owner.initializeCS35L41Device(a);}
        bool initializeHardwareErrata(AmplifierState& a){return owner.initializeHardwareErrata(a);}
        bool applyPLL(AmplifierState& a){return owner.applyPLL(a);}
        bool applyASP(AmplifierState& a){return owner.applyASP(a);}
        bool applyGPIO(AmplifierState& a){return owner.applyGPIO(a);}
        bool configureHardware(AmplifierState& a){return owner.configureHardware(a);}
        bool stopDSP(AmplifierState& a){return owner.stopDSP(a);}
        bool powerUpAmplifier(AmplifierState& a){return owner.powerUpAmplifier(a);}
        bool verifyIdleConfiguration(AmplifierState& a){return owner.verifyIdleConfiguration(a);}
        bool verifyDSPAlive(AmplifierState& a){return owner.verifyDSPAlive(a);}
        bool stopPlayback(AmplifierState& a){return owner.stopPlayback(a);}
        void discoverFirmware(AmplifierState& a){owner.discoverFirmware(a);}
        void logASPSnapshot(AmplifierState& a){owner.logASPSnapshot(a);}
        void initializeFirmware(AmplifierState& a,const char* p){owner.initializeFirmware(a,p);}
'''
DRIVER=(ROOT/'CirrusAudioFixup/Devices/Amplifiers/CS35L41/CS35L41Driver.cpp').read_text()
for flow in ('initializeHardware','loadFirmwareFlow','verifyReadyFlow','shutdownFlow'):
    body=method(DRIVER,'CS35L41Driver',flow).replace('CS35L41Driver::','').replace('mHost.debugHalted()','owner.mDebugPhaseHalted')
    FIXTURE_SHIM+=body+'\n'
FIXTURE_SHIM+=r'''
    };
    static cirrus::core::AmplifierLifecycleHooks* createFixtureDriver(const cirrus::platform::AmplifierDriverContext& c){auto* h=static_cast<FixtureHost*>(c.platformContext);if(h->amp->model!=cirrus::core::AmplifierModel::CS35L41)return nullptr;return new FixtureHooks(c,*h);}
    static void destroyFixtureDriver(cirrus::core::AmplifierLifecycleHooks* p){delete p;}
'''
FIELDS=r'''
    cirrus::diagnostics::AttachmentState mAttachment{};
    void publishAttachmentStatus(){}
    cirrus::platform::AmplifierManager mAmplifierManager{false};
    cirrus::platform::AmplifierDriverRegistry fixtureRegistry;
    cirrus::platform::BoardConfiguration fixtureBoard;
    cirrus::platform::AmplifierBinding fixtureBindings[2]{};
    cirrus::firmware::FirmwareBundle fixtureFirmware[2]{};
    FixtureHost fixtureOwners[2]{};
    cirrus::core::RegisterIO* fixturePorts[2]{};
    bool fixtureRegistered=false;
    bool bindAmplifierDrivers();
    void releaseAmplifierDrivers();
    size_t expectedAmplifierCount() const {return mPlatformProfile->endpointCount;}
    ~CirrusAudioFixup(){releaseAmplifierDrivers();}
'''
FIELDS=FIXTURE_SHIM+FIELDS
BIND=r'''
bool CirrusAudioFixup::bindAmplifierDrivers() {
    if(mAmplifierManager.bound())return true;
    cirrus::core::RegisterMapConfig f;f.readable=[](uint32_t){return true;};f.writable=f.readable;
    static const cirrus::platform::AmplifierDriverRegistration d{"fixture-service","CLSA0100",0x35a40,1,f,{},createFixtureDriver,destroyFixtureDriver,true};
    if(!fixtureRegistered){assert(fixtureRegistry.add(d));fixtureRegistered=true;}
    const cirrus::platform::AmplifierDriverContext* contexts[2]{};
    size_t count=0;for(const auto& amp:mAmplifiers)if(amp.present)++count;
    if(!count)return false;
    fixtureBoard={{0x17aa,0x382b,"CLSA0100"},fixtureBindings,count,true,true,true};
    for(size_t i=0;i<2;++i) {
        auto& amp=mAmplifiers[i];if(!amp.present)continue;
        amp.endpoint={cirrus::core::BusTransportType::I2C,"fixture-controller",amp.address};
        fixtureBindings[i]={amp.endpoint,0x35a40,uint32_t(amp.channel),1,amp.name,"CLSA0100",2};
        fixtureFirmware[i]={{0x35a40,0x17aa,0x382b,1,uint32_t(amp.channel),amp.name},cirrus::firmware::FirmwareFormat::Deferred};
        fixtureOwners[i].bind(this,&amp);fixturePorts[i]=new ServiceRegisterIO(this,amp);
        contexts[i]=new cirrus::platform::AmplifierDriverContext{*fixturePorts[i],amp.endpoint,fixtureBoard,fixtureBoard.identity,i,fixtureFirmware[i],"CLSA0100",&fixtureOwners[i],"isolated-primitives",static_cast<cirrus::platform::AmplifierHost*>(&fixtureOwners[i])};
    }
    auto result=mAmplifierManager.bind(fixtureRegistry,contexts,count);
    for(auto* context:contexts)delete context;
    if(result!=kIOReturnSuccess){for(auto& amp:mAmplifiers)amp.playbackFaulted=true;releaseAmplifierDrivers();return false;}
    return true;
}
void CirrusAudioFixup::releaseAmplifierDrivers(){mAmplifierManager.unbind();for(auto& platform:fixtureOwners)platform.clear();for(auto*& port:fixturePorts){delete port;port=nullptr;}}
'''
IOLIB='#pragma once\n#include "Diagnostics/DiagnosticTypes.hpp"\nconstexpr int kIOReturnNotReady=-2,kIOReturnBadArgument=-3,kIOReturnNotPermitted=-4,kIOReturnNotFound=-5,kIOReturnNoMemory=-6,kIOReturnUnsupported=-7,kIOReturnTimeout=-8;\ninline void IOSleep(unsigned){}\n'
