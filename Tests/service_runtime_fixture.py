"""Shared harness for the actual service backend and owned runtime (no hardware)."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SOURCE=(ROOT/'CirrusAudioFixup/Devices/CS35L41/CS35L41Backend.cpp').read_text()
a=SOURCE.index('class CirrusAudioFixup::CS35L41Backend')
b=len(SOURCE)
PRODUCTION_BACKEND=SOURCE[a:b].replace('cirrus::devices::cs35l41::createBackend(', 'CirrusAudioFixup::createCS35L41Backend(').replace('cirrus::devices::cs35l41::destroyBackend(', 'CirrusAudioFixup::destroyBackend(')
FIELDS=r'''
    class CS35L41Backend;
    struct AmplifierBackendOwner {CirrusAudioFixup* owner;AmplifierState* amp;};
    cirrus::platform::AmplifierRuntime mAmplifierRuntime{false};
    cirrus::platform::AmplifierBackendRegistry fixtureRegistry;
    cirrus::platform::BoardConfiguration fixtureBoard;
    cirrus::platform::AmplifierBinding fixtureBindings[2]{};
    cirrus::firmware::FirmwareBundle fixtureFirmware[2]{};
    AmplifierBackendOwner fixtureOwners[2]{};
    cirrus::core::RegisterIO* fixturePorts[2]{};
    bool fixtureRegistered=false;
    static cirrus::core::AmplifierLifecycleHooks* createCS35L41Backend(const cirrus::platform::AmplifierBackendContext&);
    static void destroyBackend(cirrus::core::AmplifierLifecycleHooks*);
    bool bindAmplifierRuntime();
    void teardownAmplifierRuntime();
    size_t expectedAmplifierCount() const {return mPlatformProfile->endpointCount;}
    ~CirrusAudioFixup(){teardownAmplifierRuntime();}
'''
BIND=r'''
bool CirrusAudioFixup::bindAmplifierRuntime() {
    if(mAmplifierRuntime.bound())return true;
    cirrus::core::RegisterMapConfig f;f.readable=[](uint32_t){return true;};f.writable=f.readable;
    static const cirrus::platform::AmplifierBackendDefinition d{"fixture-service","CLSA0100",0x35a40,1,f,{},createCS35L41Backend,destroyBackend,true};
    if(!fixtureRegistered){assert(fixtureRegistry.add(d));fixtureRegistered=true;}
    const cirrus::platform::AmplifierBackendContext* contexts[2]{};
    size_t count=0;for(const auto& amp:mAmplifiers)if(amp.present)++count;
    if(!count)return false;
    fixtureBoard={{0x17aa,0x382b,"CLSA0100"},fixtureBindings,count,true,true,true};
    for(size_t i=0;i<2;++i) {
        auto& amp=mAmplifiers[i];if(!amp.present)continue;
        amp.endpoint={cirrus::core::BusTransportType::I2C,"fixture-controller",amp.address};
        fixtureBindings[i]={amp.endpoint,0x35a40,uint32_t(amp.channel),1,amp.name,"CLSA0100",2};
        fixtureFirmware[i]={{0x35a40,0x17aa,0x382b,1,uint32_t(amp.channel),amp.name},cirrus::firmware::FirmwareFormat::Deferred};
        fixtureOwners[i]={this,&amp};fixturePorts[i]=new ServiceRegisterIO(this,amp);
        contexts[i]=new cirrus::platform::AmplifierBackendContext{*fixturePorts[i],amp.endpoint,fixtureBoard,fixtureBoard.identity,i,fixtureFirmware[i],"CLSA0100",&fixtureOwners[i]};
    }
    auto result=mAmplifierRuntime.bind(fixtureRegistry,contexts,count);
    for(auto* context:contexts)delete context;
    if(result!=kIOReturnSuccess){for(auto& amp:mAmplifiers)amp.playbackFaulted=true;teardownAmplifierRuntime();return false;}
    return true;
}
void CirrusAudioFixup::teardownAmplifierRuntime(){mAmplifierRuntime.unbind();for(auto*& port:fixturePorts){delete port;port=nullptr;}}
'''
IOLIB='#pragma once\n#include "Diagnostics/DiagnosticTypes.hpp"\nconstexpr int kIOReturnNotReady=-2,kIOReturnBadArgument=-3,kIOReturnNotPermitted=-4,kIOReturnNotFound=-5,kIOReturnNoMemory=-6,kIOReturnUnsupported=-7,kIOReturnTimeout=-8;\ninline void IOSleep(unsigned){}\n'
