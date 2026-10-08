from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.cpp').read_text(encoding='utf-8')
HEADER = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.hpp').read_text(encoding='utf-8')
TRANSPORT_HEADER = (ROOT / 'CirrusAudioFixup/Transport/VoodooI2CTransport.hpp').read_text(encoding='utf-8')

def function(name):
    start = re.search(r'bool CirrusAudioFixup::' + name + r'\(', SOURCE).start()
    return SOURCE[start:SOURCE.index('\n}', start) + 2]

names = ['transferToAddress', 'bulkRead', 'bulkWrite', 'readRegister', 'writeRegister', 'updateRegisterBits']
abi_start = TRANSPORT_HEADER.index('struct VoodooI2CAddressedTransfer {')
abi_end = TRANSPORT_HEADER.index('\n};', abi_start) + 4
abi = TRANSPORT_HEADER[abi_start:abi_end]
preamble = r'''
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
using UInt8=uint8_t; using UInt16=uint16_t; using UInt32=uint32_t; using IOReturn=int;
enum TraceSource { TRACE_OTHER };
constexpr int kIOReturnSuccess=0,kIOReturnNotReady=-1,kIOReturnBadArgument=-2,kIOReturnNoMemory=-3;
constexpr bool kOSBooleanTrue=true;
constexpr unsigned CS35L41_I2C_ADDR_RIGHT=0x41,DIAG_I2C_TRANSFER=2;
#define VOODOO_I2C_TRANSFER_TO_ADDRESS "VoodooI2CTransferToAddress"
#define CIRRUS_ERR(...) ((void)0)
#define CIRRUS_LOG(...) ((void)0)
bool failAllocation=false; unsigned allocations=0,frees=0;
void* IOMallocData(size_t n) { if(failAllocation) return nullptr; ++allocations; return malloc(n); }
void IOFreeData(void* p,size_t) { ++frees; free(p); }
void writeBE32(UInt8* p,UInt32 v) { for(unsigned i=0;i<4;++i) p[i]=v>>(24-8*i); }
UInt32 readBE32(const UInt8* p) { return UInt32(p[0])<<24 | UInt32(p[1])<<16 | UInt32(p[2])<<8 | p[3]; }
struct CS35L41Amp { UInt8 address=0x40; };
using AmplifierState = CS35L41Amp;
namespace cirrus { namespace devices { namespace cs35l41 { namespace registers { constexpr uint8_t kI2cAddressRight = 0x41; } } } }
'''
mock = r'''
struct Provider {
    unsigned calls=0; UInt8 address=0; UInt16 readLength=0; int result=0;
    bool waited=false; std::vector<UInt8> bytes;
    IOReturn callPlatformFunction(const char* name,bool wait,void* p,void*,void*,void*) {
        if(strcmp(name,VOODOO_I2C_TRANSFER_TO_ADDRESS)) abort();
        auto& request=*static_cast<VoodooI2CAddressedTransfer*>(p);
        ++calls; waited=wait; address=request.address; readLength=request.readLength;
        bytes.clear();
        if(request.writeBuffer) bytes.assign(request.writeBuffer,request.writeBuffer+request.writeLength);
        if(!result && request.readBuffer) for(unsigned i=0;i<readLength;++i) request.readBuffer[i]=0x11*(i+1);
        return result;
    }
};
namespace cirrus { namespace transport {
class VoodooI2CTransport {
public:
    VoodooI2CTransport(Provider* provider,UInt8 address):mProvider(provider),mAddress(address) {}
    bool transfer(UInt8* writeBuffer,UInt16 writeLength,UInt8* readBuffer,UInt16 readLength) {
        VoodooI2CAddressedTransfer request{mAddress,writeBuffer,writeLength,readBuffer,readLength};
        mLastReturn=mProvider->callPlatformFunction(VOODOO_I2C_TRANSFER_TO_ADDRESS,true,&request,nullptr,nullptr,nullptr);
        return mLastReturn==kIOReturnSuccess;
    }
    IOReturn lastReturn() const { return mLastReturn; }
private:
    Provider* mProvider; UInt8 mAddress; IOReturn mLastReturn{kIOReturnSuccess};
};
}}
class CirrusAudioFixup {
public:
    Provider provider; Provider* mProvider=&provider;
    bool mPowerAvailable=true,mStopping=false,mCapturingFailureSnapshot=false,mProbingAmplifiers=false;
    bool nestedSnapshot=false;
    IOReturn mLastTransferReturn=0,diagnosticReturn=0,traceReturn=0;
    void setProperty(const char*,bool) {}
    void setProperty(const char*,uint64_t,unsigned) {}
    void recordTrace(TraceSource,uint8_t,bool,bool,uint32_t,uint32_t,IOReturn ret) { traceReturn=ret; }
    void recordDiagnosticFailure(CS35L41Amp&,unsigned,uint32_t,uint32_t,uint32_t,IOReturn ret,bool=true) {
        diagnosticReturn=ret;
        if(nestedSnapshot) {
            nestedSnapshot=false; provider.result=0;
            UInt8 data[4]{};
            transferToAddress(0x40,data,4,nullptr,0);
        }
    }
'''
checks = r'''
unsigned failures=0;
void check(bool ok,const char* label) { if(!ok) { ++failures; fprintf(stderr,"FAIL %s\n",label); } }
int main() {
    CirrusAudioFixup d; CS35L41Amp left,right; right.address=0x41;
    UInt32 value=0;
    check(d.readRegister(left,0x02800398,&value,TRACE_OTHER) && value==0x11223344,"big endian register read");
    check(d.provider.calls==1 && d.provider.waited && d.provider.readLength==4 &&
          d.provider.bytes==std::vector<UInt8>({2,0x80,3,0x98}),"combined address-write/read request");
    check(d.writeRegister(right,0x2018,0x12345678,TRACE_OTHER),"register write");
    check(d.provider.address==0x41 && !d.provider.readLength &&
          d.provider.bytes==std::vector<UInt8>({0,0,0x20,0x18,0x12,0x34,0x56,0x78}),"right address and big endian write");
    std::vector<UInt8> payload(252,0xAB);
    check(d.bulkWrite(left,0x02C0015C,payload.data(),payload.size(),TRACE_OTHER) &&
          d.provider.bytes.size()==256 && allocations==frees,"252-byte upload plus 4-byte address");
    check(d.updateRegisterBits(left,0x2018,0,0,TRACE_OTHER),"unchanged update is read-only");
    check(d.provider.readLength==4,"unchanged update does not write");
    for(unsigned mode=0;mode<3;++mode) {
        CirrusAudioFixup f; f.provider.result=-77; f.nestedSnapshot=true;
        bool ok=mode==0?f.readRegister(left,0,&value,TRACE_OTHER):
                mode==1?f.bulkRead(left,0,payload.data(),4,TRACE_OTHER):
                        f.bulkWrite(left,0,payload.data(),4,TRACE_OTHER);
        check(!ok && f.diagnosticReturn==-77 && f.traceReturn==-77,"original I2C error recorded");
        check(f.mLastTransferReturn==-77,"diagnostic snapshot cannot replace caller transport error");
    }
    CirrusAudioFixup invalid;
    check(!invalid.bulkRead(left,0,nullptr,4,TRACE_OTHER) && invalid.mLastTransferReturn==kIOReturnBadArgument,"bulk read argument error propagated");
    check(!invalid.bulkWrite(left,0,payload.data(),65532,TRACE_OTHER) && invalid.mLastTransferReturn==kIOReturnBadArgument,"bulk write length cannot truncate");
    check(!invalid.readRegister(left,0,nullptr,TRACE_OTHER) && invalid.mLastTransferReturn==kIOReturnBadArgument,"null register output reports error");
    failAllocation=true;
    check(!invalid.bulkWrite(left,0,payload.data(),252,TRACE_OTHER) && invalid.mLastTransferReturn==kIOReturnNoMemory,"allocation error propagated");
    failAllocation=false;
    check(invalid.provider.calls==0,"invalid requests never reach provider");
    for(unsigned mode=0;mode<4;++mode) {
        CirrusAudioFixup bad; UInt8 data[4]{};
        bool ok=mode==0?bad.transferToAddress(0x80,data,4,nullptr,0):
                mode==1?bad.transferToAddress(0x40,nullptr,4,data,4):
                mode==2?bad.transferToAddress(0x40,data,4,nullptr,4):
                        bad.transferToAddress(0x40,nullptr,0,nullptr,0);
        check(!ok && bad.provider.calls==0 && bad.mLastTransferReturn==kIOReturnBadArgument,"invalid request rejected at transport boundary");
    }
    d.mPowerAvailable=false;
    auto count=d.provider.calls;
    check(!d.readRegister(left,0,&value,TRACE_OTHER) && d.provider.calls==count && d.mLastTransferReturn==kIOReturnNotReady,"suspend blocks provider");
    if(failures) return 1;
    puts("PASS production transport endian/address/length/ownership, argument/allocation failures and nested diagnostic error preservation");
}
'''
declarations='\n'.join(function(n).split('{',1)[0].replace('CirrusAudioFixup::','')+';' for n in names)
with tempfile.TemporaryDirectory(prefix='cirrus-transport-check-') as directory:
    tmp=Path(directory)
    source=tmp/'transport.cpp'
    source.write_text(preamble+abi+mock+declarations+'\n};\n'+'\n'.join(function(n) for n in names)+checks,encoding='utf-8')
    subprocess.run(['g++','-std=c++17','-O0',str(source),'-o',str(tmp/'transport.exe')],check=True)
    subprocess.run([str(tmp/'transport.exe')],check=True)

    iokit = tmp / 'IOKit'
    iokit.mkdir()
    (iokit / 'IOLib.h').write_text(r'''
#pragma once
#include <cstddef>
#include <cstdint>
using UInt8=uint8_t; using UInt16=uint16_t; using UInt32=uint32_t; using IOReturn=int;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnNotReady=-1,kIOReturnBadArgument=-2;
inline void IODelay(unsigned) {}
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
#include "Transport/VoodooI2CTransport.hpp"
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
    cirrus::transport::VoodooI2CTransport bus(&provider,0x41);
    uint32_t value=0;
    assert(bus.read(0x02800398,&value) && value==0x11223344);
    assert(provider.address==0x41 && provider.bytes==std::vector<uint8_t>({2,0x80,3,0x98}));
    assert(bus.write(0x2018,0x12345678));
    assert(provider.bytes==std::vector<uint8_t>({0,0,0x20,0x18,0x12,0x34,0x56,0x78}));
    provider.result=-77;
    assert(!bus.write(0x2018,0) && bus.lastReturn()==-77);
    cirrus::transport::VoodooI2CTransport missing(nullptr,0x40);
    assert(!missing.read(0,&value) && missing.lastReturn()==kIOReturnNotReady);
    cirrus::transport::VoodooI2CTransport invalid(&provider,0x80);
    assert(!invalid.read(0,&value) && invalid.lastReturn()==kIOReturnBadArgument);
}
''', encoding='utf-8')
    subprocess.run([
        'g++','-std=c++17','-O0','-I',str(tmp),'-I',str(ROOT/'CirrusAudioFixup'),
        str(direct),'-o',str(tmp/'direct_transport.exe')
    ],check=True)
    subprocess.run([str(tmp/'direct_transport.exe')],check=True)
