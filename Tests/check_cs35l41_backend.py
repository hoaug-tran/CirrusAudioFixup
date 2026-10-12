"""Compile the production CS35L41 backend without IOService; audit its bridge lifetime."""
from pathlib import Path
import subprocess
import tempfile
from service_runtime_fixture import IOLIB

ROOT = Path(__file__).resolve().parents[1]
CPP = r'''
#include <cassert>
#include <vector>
#include "Devices/CS35L41/CS35L41Backend.hpp"
#include "Devices/CS35L41/Hardware/Registers.hpp"
#include "Devices/CS35L41/DSPControl.hpp"
#include "Platform/AmplifierRuntime.hpp"
using namespace cirrus;
using namespace devices::cs35l41;

struct Port final : core::RegisterIO {
    unsigned reads=0,writes=0;
    bool readOK=true;
    uint32_t id=registers::kValDeviceId;
    int32_t error=-99;
    int32_t lastError() const override { return error; }
    bool read(uint32_t reg,uint32_t* value) override {
        assert(reg==registers::kRegDeviceId);++reads;*value=id;return readOK;
    }
    bool write(uint32_t,uint32_t) override {++writes;return true;}
    bool updateBits(uint32_t,uint32_t,uint32_t) override {++writes;return true;}
    bool bulkRead(uint32_t,uint8_t*,size_t) override {++reads;return true;}
    bool bulkWrite(uint32_t,const uint8_t*,size_t) override {++writes;return true;}
    bool pollBit(uint32_t,uint32_t,uint32_t,uint32_t) override {++reads;return true;}
};

struct Host final : Platform {
    State chip{};
    Port port;
    core::BusEndpoint device{core::BusTransportType::I2C,"test-controller",0x40};
    core::AudioChannel routing=core::AudioChannel::Left;
    core::AmplifierModel silicon=core::AmplifierModel::CS35L41;
    bool bound=true,allow=true;
    unsigned boots=0,loads=0,verifies=0,stops=0,shutdowns=0,snapshots=0,cancels=0,ticks=0,irqCalls=0;
    IOReturn initError=0,shutdownError=0,identityError=0;
    uint32_t identityActual=0;
    std::vector<int> calls;
    State* state() const override {return bound?const_cast<State*>(&chip):nullptr;}
    core::BusEndpoint endpoint() const override {return device;}
    core::AudioChannel channel() const override {return routing;}
    core::AmplifierModel model() const override {return silicon;}
    bool authorized() const override {return allow;}
    IOReturn initialize() override {assert(bound);calls.push_back(1);++boots;chip.playbackFaulted=false;return initError;}
    IOReturn loadFirmware() override {assert(bound);calls.push_back(2);++loads;chip.firmwareValidated=true;return 0;}
    IOReturn verifyReady() override {assert(bound);calls.push_back(3);++verifies;chip.initialized=true;return 0;}
    IOReturn commitPlayback() override {assert(bound);calls.push_back(4);chip.playbackActive=true;return 0;}
    bool stopPlayback() override {assert(bound);calls.push_back(5);++stops;chip.playbackActive=false;return true;}
    IOReturn shutdown() override {
        assert(bound);calls.push_back(6);++shutdowns;
        chip.initialized=chip.firmwareValidated=chip.playbackActive=false;
        return shutdownError;
    }
    void captureDiagnostics() override {assert(bound);++snapshots;}
    bool configureInterrupt(bool) override {assert(bound);++irqCalls;return true;}
    bool checkProtection() override {assert(bound);++irqCalls;return true;}
    void monitor(const core::PlaybackContext&) override {assert(bound);++ticks;}
    bool validatePlayback(const core::PlaybackContext&) override {assert(bound);return true;}
    void cancelPlayback() override {assert(bound);++cancels;}
    void recordIdentityFailure(uint32_t actual,IOReturn result) override {
        assert(bound);identityActual=actual;identityError=result;
        chip.diagnostic.firstFailure=diagnostics::DIAG_DEVICE_ID;
        chip.diagnostic.ioReturn=result;
        // A diagnostic can change lastError; returned failure must remain original.
        port.error=-101;
    }
};

struct Board {
    platform::AmplifierBinding binding;
    platform::BoardConfiguration manifest;
    firmware::FirmwareBundle firmware;
    platform::AmplifierBackendContext context;
    explicit Board(Host& h)
        : binding{h.device,registers::kValDeviceId,0,1,"test-tuning","CLSA0100",2},
          manifest{{0x17aa,0x382b,"CLSA0100"},&binding,1,true,true,true},
          firmware{{registers::kValDeviceId,0x17aa,0x382b,1,0,"test-tuning"},firmware::FirmwareFormat::Deferred},
          context{h.port,h.device,manifest,manifest.identity,0,firmware,"CLSA0100",static_cast<Platform*>(&h)} {}
};

bool access(uint32_t) {return true;}
const platform::AmplifierBackendDefinition& definition() {
    static const auto d=[] {
        core::RegisterMapConfig f;f.readable=access;f.writable=access;
        return platform::AmplifierBackendDefinition{"cs35l41-test","CLSA0100",registers::kValDeviceId,1,f,{},createBackend,destroyBackend,true};
    }();
    return d;
}
void bind(platform::AmplifierRuntime& runtime,platform::AmplifierBackendRegistry& registry,Board& board) {
    const platform::AmplifierBackendContext* contexts[]={&board.context};
    assert(runtime.bind(registry,contexts,1)==0);
}


struct HaltPort final : core::RegisterIO {
    struct Operation {uint32_t reg,mask,value;};
    std::vector<Operation> operations;
    unsigned failAt=0;
    bool active=false;
    bool read(uint32_t reg,uint32_t* value) override {
        operations.push_back({reg,0,0});*value=active?registers::kValHaloCoreEnable:0;
        return operations.size()!=failAt;
    }
    bool updateBits(uint32_t reg,uint32_t mask,uint32_t value) override {
        operations.push_back({reg,mask,value});return operations.size()!=failAt;
    }
    bool write(uint32_t,uint32_t) override {assert(false);return false;}
    bool bulkRead(uint32_t,uint8_t*,size_t) override {assert(false);return false;}
    bool bulkWrite(uint32_t,const uint8_t*,size_t) override {assert(false);return false;}
    bool pollBit(uint32_t,uint32_t,uint32_t,uint32_t) override {assert(false);return false;}
};
unsigned delayCalls=0;
void delay(uint32_t duration) {assert(duration==1000);++delayCalls;}

int main() {
    for(unsigned failure=0;failure<4;++failure) {
        HaltPort io;State state;state.dspAlive=true;state.monitorCount=17;io.failAt=failure;delayCalls=0;
        assert(DSPControl::halt(io,state,delay)==(failure==0));
        assert(!state.dspAlive && !state.monitorCount && delayCalls==1);
        assert(io.operations.size()==(failure==1 || failure==2 ? 2U:3U));
        assert(io.operations[0].reg==registers::kRegDsp1CcmCoreControl &&
               io.operations[0].mask==registers::kValHaloCoreEnable && io.operations[0].value==0);
        assert(io.operations[1].reg==registers::kRegDsp1CoreSoftReset && io.operations[1].mask==1 && io.operations[1].value==1);
        if(io.operations.size()==3)assert(io.operations[2].reg==registers::kRegDsp1CcmCoreControl);
    }
    {
        HaltPort io;State state;io.active=true;assert(!DSPControl::halt(io,state,delay));
        const auto count=io.operations.size();assert(!DSPControl::halt(io,state,nullptr));assert(io.operations.size()==count);
    }

    // A rejected factory must neither probe nor mutate chip state.
    for(unsigned mode=0;mode<9;++mode) {
        Host h;Board b(h);h.chip.finalCrc=0xabcdef;
        switch(mode) {
        case 0:b.context.platformContext=nullptr;break;
        case 1:h.bound=false;break;
        case 2:h.device.i2cAddress=0x41;break;
        case 3:h.routing=core::AudioChannel::Right;break;
        case 4:h.routing=core::AudioChannel::LeftTweeter;break;
        case 5:h.silicon=core::AmplifierModel::CS35L56;break;
        case 6:b.context.bindingIndex=1;break;
        case 7:b.manifest.amplifiers=nullptr;break;
        case 8:b.binding.siliconId=0x56;break;
        }
        assert(createBackend(b.context)==nullptr);
        assert(!h.port.reads && !h.port.writes && !h.boots && h.chip.finalCrc==0xabcdef);
    }
    // Transport failure differs from successful read of an unknown device.
    for(int error:{-99,-1,0}) {
        Host h;Board b(h);h.port.readOK=false;h.port.error=error;
        auto* backend=createBackend(b.context);assert(backend);
        const auto expected=error==-99?-99:kIOReturnNotReady;
        assert(backend->identify()==expected && h.identityError==expected && h.chip.diagnostic.ioReturn==expected);
        assert(h.port.error==-101 && h.chip.playbackFaulted && !h.boots && !h.port.writes);
        destroyBackend(backend);
    }
    {
        Host h;Board b(h);h.port.id=0x56;
        auto* backend=createBackend(b.context);assert(backend);
        assert(backend->identify()==kIOReturnNotFound && h.identityActual==0x56);
        assert(!h.port.writes && !h.boots);destroyBackend(backend);
    }
    platform::AmplifierBackendRegistry registry;assert(registry.add(definition()));
    {
        Host h;Board b(h);platform::AmplifierRuntime runtime(false);bind(runtime,registry,b);
        h.allow=false;assert(runtime.bringUp()==kIOReturnNotPermitted);
        assert(!h.boots && !h.shutdowns && !h.port.writes);
        runtime.unbind();h.bound=false;
    }
    {
        Host h;Board b(h);platform::AmplifierRuntime runtime(false);bind(runtime,registry,b);
        // Runtime copied the context; the temporary owner's object can change.
        b.context.platformContext=nullptr;
        assert(runtime.bringUp()==0 && h.calls==std::vector<int>({1,2,3}));
        assert(runtime.configureInterrupt(0,true)==0 && runtime.handleInterrupt(0)==0 && h.irqCalls==2);
        runtime.captureDiagnostics(0);assert(h.snapshots==1);
        assert(runtime.startPlayback()==0 && h.chip.playbackActive);
        assert(runtime.suspend()==0 && h.shutdowns==1 && !h.chip.initialized);
        assert(runtime.resume()==0 && h.boots==2 && !h.chip.playbackActive);
        assert(runtime.unbind()==0 && h.shutdowns==2 && !h.chip.initialized);
        assert(runtime.releasedStatus(0)->cleanupResult==0);
        h.bound=false;const auto shutdowns=h.shutdowns;
        assert(runtime.unbind()==0 && h.shutdowns==shutdowns);
        h.bound=true;b.context.platformContext=static_cast<Platform*>(&h);
        bind(runtime,registry,b);assert(runtime.bringUp()==0 && h.boots==3);
        assert(runtime.unbind()==0 && h.shutdowns==3);h.bound=false;
    }
    {
        Host h;Board b(h);h.initError=-77;h.shutdownError=-88;
        platform::AmplifierRuntime runtime(false);bind(runtime,registry,b);
        assert(runtime.bringUp()==-77 && h.shutdowns==1 && h.chip.playbackFaulted);
        runtime.captureDiagnostics(0);assert(h.snapshots==1);
        assert(runtime.unbind()!=0 && h.shutdowns==1);
        const auto* status=runtime.releasedStatus(0);
        assert(status && status->firstFailure==-77 && status->cleanupResult==-88);
        h.bound=false;runtime.unbind();assert(h.shutdowns==1);
    }
    {
        Host h;Board b(h);
        {platform::AmplifierRuntime runtime(false);bind(runtime,registry,b);assert(runtime.bringUp()==0);}
        assert(h.shutdowns==1 && !h.chip.initialized);h.bound=false;
    }
}
'''
with tempfile.TemporaryDirectory(prefix='cirrus-cs35l41-backend-') as directory:
    tmp = Path(directory)
    (tmp / 'IOKit').mkdir()
    (tmp / 'IOKit/IOLib.h').write_text(IOLIB)
    (tmp / 'backend.cpp').write_text(CPP)
    for sanitizer in (False, True):
        binary = tmp / ('sanitized' if sanitizer else 'ordinary')
        command = ['g++', '-std=c++17', '-O1', '-g', '-I' + str(tmp), '-I' + str(ROOT / 'CirrusAudioFixup'),
                   str(tmp / 'backend.cpp'), str(ROOT / 'CirrusAudioFixup/Devices/CS35L41/CS35L41Backend.cpp'), '-o', str(binary)]
        if sanitizer:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
print('PASS independent CS35L41 backend: factory rejection, identity errors, DSP halt/reset fault sweep, PM, cleanup evidence and teardown/rebind lifetime (ASan/UBSan)')
