"""Link the complete independent production driver and factory; audit ownership.

Only kernel resources and bus responses are fake. Real silicon, firmware lookup,
errata, OTP, idle checks, shutdown and lifecycle methods execute unchanged.
"""
from pathlib import Path
import subprocess,tempfile
from driver_kernel_fixture import install
ROOT=Path(__file__).resolve().parents[1]
CPP=r'''
#include "Devices/Amplifiers/CS35L41/CS35L41DriverFactory.hpp"
#include "Devices/Amplifiers/CS35L41/CS35L41Driver.hpp"
#include "Devices/Amplifiers/CS35L41/DeviceConfiguration.hpp"
#include "Devices/AmplifierDrivers.hpp"
#include "Platform/AmplifierManager.hpp"
#include <cassert>
#include <type_traits>
using namespace cirrus;
using namespace devices::amplifiers::cs35l41;
bool gCirrusDebug=false;
struct Port:core::RegisterIO {
    std::map<uint32_t,uint32_t> values;
    unsigned calls=0,failAt=0,reads=0,writes=0,diagnosticReads=0;
    int error=0;
    bool alive=true;
    std::vector<diagnostics::TraceSource> sources;
    Port(){values[0]=registers::kValDeviceId;values[4]=0xb2;values[0x10]=3;values[0x1001c]=2;values[0x10098]=2;}
    bool step(){assert(alive);error=++calls==failAt?-99:0;return error==0;}
    int32_t lastError()const override{return error;}
    bool read(uint32_t r,uint32_t* v)override{++reads;if(!step())return false;*v=values[r];return true;}
    bool write(uint32_t r,uint32_t v)override{++writes;if(!step())return false;if(r==0x10010 || r==0x10018 || r==0x1001c)values[r]&=~v;else values[r]=v;if(r==0x20)values[0x1001c]=2;return true;}
    bool updateBits(uint32_t r,uint32_t m,uint32_t v)override{uint32_t old=0;return read(r,&old) && write(r,(old&~m)|(v&m));}
    bool bulkRead(uint32_t,uint8_t* p,size_t n)override{if(!step())return false;memset(p,0,n);return true;}
    bool bulkWrite(uint32_t,const uint8_t*,size_t)override{return step();}
    bool pollBit(uint32_t r,uint32_t m,uint32_t v,uint32_t)override{uint32_t old=0;return read(r,&old) && (old&m)==v;}
    bool readWithTrace(uint32_t r,uint32_t* p,diagnostics::TraceSource s)override{sources.push_back(s);if(s==diagnostics::TRACE_DUMP)++diagnosticReads;return read(r,p);}
    bool writeWithTrace(uint32_t r,uint32_t v,diagnostics::TraceSource s)override{sources.push_back(s);return write(r,v);}
    bool bulkReadWithTrace(uint32_t r,uint8_t* p,size_t n,diagnostics::TraceSource s)override{sources.push_back(s);return bulkRead(r,p,n);}
};
struct Host:platform::AmplifierHost {
    diagnostics::DiagnosticState diag{};
    bool alive=true,allow=true,bypass=true,capturing=false;
    std::string phase;std::map<std::string,std::string> properties;
    unsigned captures=0,publications=0;
    bool authorized()const override{assert(alive);return allow;}
    const char* name()const override{return "owned-driver";}
    bool supportsResources()const override{return alive;}
    const diagnostics::DiagnosticState& diagnostics()const override{return diag;}
    bool bootArgEnabled(const char* s)const override{return bypass && !strcmp(s,"-cirrusnodsp");}
    bool bootArgStrEquals(const char* s,const char* v)const override{return !strcmp(s,"-cirrusphase") && phase==v;}
    bool stopAfterDebugStage(const char* s)override{return phase==s;}
    bool debugHalted()const override{return !phase.empty();}
    void setDiagnosticStage(cirrus::diagnostics::DriverStage s)override{assert(alive);diag.stage=s;}
    void markDiagnosticSuccess(cirrus::diagnostics::DriverStage s)override{diag.stage=diag.lastGoodStage=s;}
    void recordDiagnosticFailure(cirrus::diagnostics::DiagnosticFailure f,uint32_t r,uint32_t e,uint32_t a,IOReturn ret,bool)override{
        assert(alive);if(diag.firstFailure==cirrus::diagnostics::DIAG_OK)diag.firstFailure=f;
        diag.latestFailure=f;diag.failureCount++;diag.reg=r;diag.expected=e;diag.actual=a;diag.ioReturn=ret;
    }
    void publish(const char* key,const char* value)override{assert(alive);properties[key]=value;++publications;}
    void publish(const char* key,OSObject* value)override{assert(alive);if(auto* s=dynamic_cast<OSString*>(value))properties[key]=s->value;++publications;}
    void publishVerdict()override{assert(alive);}
    void beginDiagnosticCapture()override{assert(!capturing);capturing=true;++captures;}
    void endDiagnosticCapture()override{assert(capturing);capturing=false;}
};
struct Fixture {
    Port port;Host host;
    core::BusEndpoint endpoint{core::BusTransportType::I2C,"owned-controller",0x40};
    platform::AmplifierBinding binding{endpoint,0x35a40,0,1,"owned-driver","CLSA0100",2};
    platform::BoardConfiguration board{{0x17aa,0x382b,"CLSA0100"},&binding,1,true,true,true};
    firmware::FirmwareBundle assets{{0x35a40,0x17aa,0x382b,1,0,"owned-driver"},firmware::FirmwareFormat::Deferred};
    platform::AmplifierDriverContext context{port,endpoint,board,board.identity,0,assets,"CLSA0100",nullptr,nullptr,&host};
    platform::AmplifierDriverRegistry registry;
    platform::AmplifierManager runtime{false};
    Fixture(){assert(devices::registerDrivers(registry));}
    ~Fixture(){runtime.unbind();host.alive=port.alive=false;}
    void bind(){assert(runtime.bind(registry,&context,1)==0);}
};
int main(){
    static_assert(!std::is_base_of<IOService,CS35L41Driver>::value);
    for(auto bus:{core::BusTransportType::I2C,core::BusTransportType::SPI}){
        Fixture f;f.endpoint.transport=bus;if(bus==core::BusTransportType::SPI)f.endpoint.spiClockHz=1000000;
        f.binding.endpoint=f.context.endpoint=f.endpoint;f.bind();assert(f.runtime.bringUp()==0 && f.runtime.report(0).initialized);
        assert(f.runtime.suspend()==0 && f.runtime.resume()==0);f.runtime.unbind();
    }
    {
        Fixture internal;
        const PowerConfiguration config{BoostTopology::Internal,1000,4500,24};
        internal.binding.driverConfiguration=&config;
        internal.binding.driverConfigurationSize=sizeof(config);
        internal.binding.driverConfigurationKind=kPowerConfigurationKind;
        internal.bind();assert(internal.runtime.bringUp()==0);
        assert((internal.port.values[0x3810]&0xffff)==0x4932);
        assert((internal.port.values[0x3814]&0xff03)==0x7500);
        assert((internal.port.values[0x3808]&0x7f)==0x4a);
        assert(!internal.port.values.count(0x7414));
        assert(internal.runtime.suspend()==0&&internal.runtime.resume()==0);
    }
    {
        Fixture configured;
        DeviceConfiguration config{{BoostTopology::Internal,1200,4100,47},0,0};
        configured.binding.driverConfiguration=&config;configured.binding.driverConfigurationSize=sizeof(config);
        configured.binding.driverConfigurationKind=kDeviceConfigurationKind;configured.binding.interruptGpio=0;
        configured.port.values[0x242c]=0x00030000;
        configured.bind();
        // The concrete driver owns a copy; later mutation of discovery storage
        // must not change electrical configuration across resume.
        config.power.peakMilliamp=5000;
        assert(configured.runtime.bringUp()==0);
        assert((configured.port.values[0x3814]&0xff03)==0x6b01);
        assert((configured.port.values[0x3808]&0x7f)==0x42);
        assert((configured.port.values[0x242c]&0x07000000)==0);
        assert((configured.port.values[0x242c]&0x00030000)==0);
        assert(configured.runtime.suspend()==0&&configured.runtime.resume()==0);
    }
    Fixture good;good.bind();assert(good.port.calls==0);assert(good.runtime.bringUp()==0);
    assert(good.runtime.report(0).initialized && !good.runtime.report(0).faulted);
    assert(!strcmp(good.runtime.report(0).readyVerdict,"READY_BYPASS_EXPLICIT"));
    const unsigned total=good.port.calls;assert(total>100);
    good.runtime.captureDiagnostics(0);assert(good.host.captures==1 && !good.host.capturing && good.port.diagnosticReads>0);
    assert(good.runtime.suspend()==0 && !good.runtime.report(0).initialized);
    assert(good.runtime.resume()==0 && good.runtime.report(0).initialized);
    assert(good.runtime.unbind()==0);const auto after=good.port.calls;assert(good.runtime.unbind()==0 && good.port.calls==after);
    good.bind();assert(good.runtime.report(0).managed && !good.runtime.report(0).initialized);assert(good.runtime.bringUp()==0);
    for(unsigned at=1;at<=total;++at){
        Fixture f;f.port.failAt=at;f.bind();auto result=f.runtime.bringUp();
        if(result){assert(f.runtime.stage()==core::AmplifierStage::Faulted);assert(!f.runtime.report(0).initialized);assert(f.runtime.status(0)->firstFailure!=0);}
        f.runtime.unbind();const auto ended=f.port.calls;f.runtime.unbind();assert(f.port.calls==ended);
    }
    Fixture identity;identity.port.failAt=1;identity.bind();assert(identity.runtime.bringUp()==-99);assert(identity.host.diag.ioReturn==-99);
    Fixture unknown;unknown.port.values[0]=0x35a41;unknown.bind();assert(unknown.runtime.bringUp()==kIOReturnNotFound && unknown.port.writes==0);
    Fixture deny;deny.host.allow=false;deny.bind();assert(deny.runtime.bringUp()==kIOReturnNotPermitted && deny.port.writes==0);
    for(const char* phase:{"probe","otp","errata","clock","asp","gpio","platform","firmware","dsp"}){
        Fixture f;f.host.phase=phase;f.bind();assert(f.runtime.bringUp()==kIOReturnNotReady);assert(f.runtime.stage()==core::AmplifierStage::DebugHalted);assert(!f.runtime.report(0).initialized);
    }
    Fixture wait;wait.host.bypass=false;wait.bind();assert(wait.runtime.bringUp()==kIOReturnNotReady);assert(wait.runtime.stage()==core::AmplifierStage::Waiting);
    const auto before=wait.port.writes;const auto deadline=fixtureTimeMs+30001;
    assert(wait.runtime.retryBringUp(fixtureTimeMs)==kIOReturnNotReady && wait.port.writes==before);
    assert(wait.runtime.retryBringUp(deadline)==kIOReturnTimeout && wait.runtime.stage()==core::AmplifierStage::Faulted);
    assert(wait.host.properties["Cirrus_Phase5A_Status_owned-driver"]=="CODEC_IDENTITY_TIMEOUT");
    Fixture bad;bad.context.host=nullptr;assert(!createDriver(bad.context) && bad.port.calls==0);
    bad.context.host=&bad.host;
    const PowerConfiguration invalidPower{BoostTopology::Internal,1000,5000,24};
    bad.binding.driverConfiguration=&invalidPower;bad.binding.driverConfigurationSize=sizeof(invalidPower);
    bad.binding.driverConfigurationKind=kPowerConfigurationKind;
    assert(!createDriver(bad.context)&&bad.port.calls==0);
    bad.binding.driverConfiguration=nullptr;bad.binding.driverConfigurationSize=0;bad.binding.driverConfigurationKind=nullptr;
    bad.binding.channelIndex=2;assert(!createDriver(bad.context) && bad.port.calls==0);
    bad.binding.channelIndex=0;bad.binding.siliconId=0x35a41;assert(!createDriver(bad.context) && bad.port.calls==0);
    bad.binding.siliconId=0x35a40;bad.binding.endpoint.i2cAddress=0x41;assert(!createDriver(bad.context) && bad.port.calls==0);
    bad.binding.endpoint=bad.endpoint;bad.context.bindingIndex=1;assert(!createDriver(bad.context) && bad.port.calls==0);
    bad.context.bindingIndex=0;bad.board.wiringVerified=false;assert(!createDriver(bad.context) && bad.port.calls==0);
    bad.board.wiringVerified=true;bad.board.amplifiers=nullptr;assert(!createDriver(bad.context) && bad.port.calls==0);

    printf("PASS complete owned CS35L41 driver: %u transfer fault positions, reset/OTP/errata/idle, native identity errors, wait timeout, debug phases, PM and teardown/rebind\n",total);
}
'''
with tempfile.TemporaryDirectory(prefix='cirrus-independent-driver-') as directory:
    p=Path(directory);install(p);(p/'driver.cpp').write_text(CPP)
    for flags in ([],['-fsanitize=address,undefined','-fno-omit-frame-pointer']):
        subprocess.run(['clang++','-std=c++17','-O0','-g',*flags,'-I'+str(p),'-I'+str(ROOT/'CirrusAudioFixup'),str(p/'driver.cpp'),str(ROOT/'CirrusAudioFixup/Devices/Amplifiers/CS35L41/CS35L41DriverFactory.cpp'),str(ROOT/'CirrusAudioFixup/Devices/Amplifiers/CS35L41/CS35L41Driver.cpp'),'-o',str(p/'driver')],check=True)
        subprocess.run([str(p/'driver')],check=True)
