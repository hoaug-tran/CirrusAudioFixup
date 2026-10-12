from production_source_fixture import method
"""Verify production service port tracing/PM and provider/map framing boundaries."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=(ROOT/'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text()
def function(name):
    return method(SOURCE,'CirrusAudioFixup',name)
names=['bulkRead','bulkWrite','readRegister','writeRegister','updateRegisterBits']
preamble=r'''

#include "Core/RegisterIO.hpp"
#include "Core/BusEndpoint.hpp"
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <cstring>
#include <vector>
using UInt8=uint8_t;using UInt32=uint32_t;using IOReturn=int;
using namespace cirrus::diagnostics;
constexpr int kIOReturnSuccess=0,kIOReturnNotReady=-1,kIOReturnBadArgument=-2;
constexpr unsigned DIAG_I2C_TRANSFER=2,DIAG_SPI_TRANSFER=31;
#define CIRRUS_LOG(...) ((void)0)
struct AmplifierState {cirrus::core::RegisterIO* registerIO=nullptr;cirrus::core::BusEndpoint endpoint{};uint8_t endpointIndex=0;};
class CirrusAudioFixup {
public:
    bool powered=true,stopping=false,mCapturingFailureSnapshot=false,nested=false;
    IOReturn mLastTransferReturn=0,diagnosticReturn=0,traceReturn=0;
    TraceSource source=TRACE_OTHER;uint8_t tracedEndpoint=0;
    bool nativeBusReady()const{return powered && !stopping;}
    void recordTrace(TraceSource s,uint8_t i,bool,bool,uint32_t,uint32_t,IOReturn result){source=s;tracedEndpoint=i;traceReturn=result;}
    void recordDiagnosticFailure(AmplifierState& a,unsigned,uint32_t,uint32_t,uint32_t,IOReturn result){diagnosticReturn=result;if(nested){nested=false;mCapturingFailureSnapshot=true;uint32_t v=0;readRegister(a,0,&v,TRACE_DUMP);mCapturingFailureSnapshot=false;}}
'''
checks=r'''

struct Port:cirrus::core::RegisterIO {
    unsigned calls=0,diagnosticCalls=0;int error=0;
    int32_t lastError()const override{return error;}
    bool read(uint32_t,uint32_t* p)override{++calls;if(error)return false;*p=0x11223344;return true;}
    bool write(uint32_t,uint32_t)override{++calls;return !error;}
    bool updateBits(uint32_t,uint32_t,uint32_t)override{return false;}
    bool bulkRead(uint32_t,uint8_t* p,size_t n)override{++calls;if(error)return false;memset(p,0x55,n);return true;}
    bool bulkWrite(uint32_t,const uint8_t*,size_t)override{++calls;return !error;}
    bool pollBit(uint32_t,uint32_t,uint32_t,uint32_t)override{return false;}
    bool readForDiagnostic(uint32_t,uint32_t*)override{++diagnosticCalls;error=-66;return false;}
    bool bulkReadForDiagnostic(uint32_t,uint8_t*,size_t)override{++diagnosticCalls;error=-66;return false;}
};
int main(){
    for(auto bus:{cirrus::core::BusTransportType::I2C,cirrus::core::BusTransportType::SPI}){
        CirrusAudioFixup s;Port p;AmplifierState amp; amp.registerIO=&p;amp.endpoint.transport=bus;amp.endpointIndex=7;
        uint32_t v=0;uint8_t data[252]{};
        assert(s.readRegister(amp,0,&v,TRACE_PLAYBACK) && v==0x11223344 && s.source==TRACE_PLAYBACK && s.tracedEndpoint==7);
        assert(s.writeRegister(amp,0,1,TRACE_PROBE));
        assert(s.bulkRead(amp,0,data,sizeof(data),TRACE_FIRMWARE) && data[0]==0x55);
        assert(s.bulkWrite(amp,0,data,sizeof(data),TRACE_FIRMWARE));
        auto count=p.calls;assert(!s.readRegister(amp,0,nullptr,TRACE_OTHER) && p.calls==count && s.mLastTransferReturn==kIOReturnBadArgument);
        assert(!s.bulkRead(amp,0,nullptr,4,TRACE_OTHER) && p.calls==count);
        assert(!s.readRegister(amp,0,&v,TRACE_DUMP) && p.diagnosticCalls==1 && p.calls==count);
        p.error=0;assert(!s.bulkRead(amp,0,data,4,TRACE_DUMP) && p.diagnosticCalls==2 && p.calls==count);
        for(unsigned mode=0;mode<4;++mode){p.error=-77;s.nested=true;v=0x1234;
            bool ok=mode==0?s.readRegister(amp,0,&v,TRACE_OTHER):mode==1?s.bulkRead(amp,0,data,4,TRACE_OTHER):mode==2?s.bulkWrite(amp,0,data,4,TRACE_OTHER):s.writeRegister(amp,0,1,TRACE_OTHER);
            assert(!ok && s.mLastTransferReturn==-77 && s.traceReturn==-66 && s.diagnosticReturn==-77);if(!mode)assert(v==0x1234);
        }
        p.error=0;count=p.calls;s.powered=false;assert(!s.bulkWrite(amp,0,data,4,TRACE_OTHER) && p.calls==count && s.mLastTransferReturn==kIOReturnNotReady);
        s.powered=true;s.stopping=true;assert(!s.readRegister(amp,0,&v,TRACE_OTHER) && p.calls==count);
        amp.registerIO=nullptr;s.stopping=false;assert(!s.readRegister(amp,0,&v,TRACE_OTHER) && p.calls==count);
    }
}
'''
declarations='\n'.join(function(n).split('{',1)[0].replace('CirrusAudioFixup::','')+';' for n in names)
with tempfile.TemporaryDirectory(prefix='cirrus-transport-check-') as directory:
    tmp=Path(directory);source=tmp/'transport.cpp'
    source.write_text(preamble+declarations+'\n};\n'+'\n'.join(function(n) for n in names)+checks)
    subprocess.run(['clang++','-std=c++17','-I'+str(ROOT/'CirrusAudioFixup'),str(source),'-o',str(tmp/'transport')],check=True)
    subprocess.run([str(tmp/'transport')],check=True)
    iokit = tmp / 'IOKit'
    iokit.mkdir()
    (iokit / 'IOLib.h').write_text(r'''
#pragma once
#include <cstddef>
#include <cstdint>
using UInt8=uint8_t; using UInt16=uint16_t; using UInt32=uint32_t; using IOReturn=int;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnNotReady=-1,kIOReturnBadArgument=-2,kIOReturnNotPermitted=-3,kIOReturnTimeout=-4;
inline void IODelay(unsigned) {}
inline void IOSleep(unsigned) {}
''', encoding='utf-8')
    (iokit / 'IOService.h').write_text(r'''
#pragma once
#include "IOLib.h"
class IOService {
public:
    virtual ~IOService()=default;
    virtual IOReturn callPlatformFunction(const char*,bool,void*,void*,void*,void*)=0;
};
''', encoding='utf-8')
    direct = tmp / 'direct_transport.cpp'
    direct.write_text(r'''
#include <cassert>
#include <cstring>
#include <vector>
#include "Transport/VoodooI2CProvider.hpp"
#include "Transport/VoodooI2CController.hpp"
#include "Transport/I2CRegisterBus.hpp"
#include "Core/RegisterMap.hpp"
bool gCirrusDebug=false;
struct Provider final : IOService {
    IOReturn result=0; unsigned calls=0; uint8_t address=0; std::vector<uint8_t> bytes;
    IOReturn callPlatformFunction(const char* name,bool wait,void* ptr,void*,void*,void*) override {
        assert(std::strcmp(name,VOODOO_I2C_TRANSFER_TO_ADDRESS)==0 && wait);
        auto& request=*static_cast<VoodooI2CAddressedTransfer*>(ptr);
        ++calls; address=request.address;
        if(request.writeBuffer) bytes.assign(request.writeBuffer,request.writeBuffer+request.writeLength);
        if(result==0 && request.readBuffer)
            for(unsigned i=0;i<request.readLength;++i) request.readBuffer[i]=uint8_t(0x11*(i+1));
        return result;
    }
};
int main() {
    Provider provider;
    cirrus::transport::VoodooI2CController registerController(&provider);
    cirrus::transport::I2CRegisterBus registerEndpoint(&registerController,{0x41,false});
    cirrus::core::RegisterMap bus(registerEndpoint,{},true);
    uint32_t value=0;
    assert(!bus.read(0,nullptr)&&bus.lastError()==kIOReturnBadArgument);
    assert(!bus.pollBit(0,1,1,0)&&bus.lastError()==kIOReturnBadArgument);
    assert(bus.read(0x02800398,&value) && value==0x11223344);
    assert(provider.address==0x41 && provider.bytes==std::vector<uint8_t>({2,0x80,3,0x98}));
    assert(bus.write(0x2018,0x12345678));
    assert(provider.bytes==std::vector<uint8_t>({0,0,0x20,0x18,0x12,0x34,0x56,0x78}));
    cirrus::transport::VoodooI2CController controller(&provider);
    cirrus::transport::I2CRegisterBus endpoint(&controller,{0x41,false});
    cirrus::core::RegisterMap generic(endpoint,{},true);
    assert(generic.read(0x02800398,&value)&&value==0x11223344);
    assert(generic.write(0x2018,0x12345678));
    assert(provider.bytes==std::vector<uint8_t>({0,0,0x20,0x18,0x12,0x34,0x56,0x78}));
    uint8_t prefix[4]{};
    assert(controller.transfer({0x41,true},prefix,4,nullptr,0)==kIOReturnBadArgument);
    provider.result=-77;
    assert(!bus.write(0x2018,0) && bus.lastReturn()==-77);
    unsigned beforePoll=provider.calls;
    assert(!bus.pollBit(0x10010,1,1,100) && bus.lastReturn()==-77);
    assert(provider.calls==beforePoll+1);
    cirrus::transport::VoodooI2CProvider missing(nullptr,0x40);
    assert(!missing.transfer(prefix,4,nullptr,0) && missing.lastReturn()==kIOReturnNotReady);
    cirrus::transport::VoodooI2CProvider invalid(&provider,0x80);
    assert(!invalid.transfer(prefix,4,nullptr,0) && invalid.lastReturn()==kIOReturnBadArgument);
}
''', encoding='utf-8')
    subprocess.run([
        'g++','-std=c++17','-O0','-I',str(tmp),'-I',str(ROOT/'CirrusAudioFixup'),
        str(direct),'-o',str(tmp/'direct_transport.exe')
    ],check=True)
    subprocess.run([str(tmp/'direct_transport.exe')],check=True)

print("PASS production port PM/trace/diagnostic error preservation and VoodooI2C ABI/framing checks")

print("PASS production port PM/trace/diagnostic error preservation and VoodooI2C ABI/framing checks")
