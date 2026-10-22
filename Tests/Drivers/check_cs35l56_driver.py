"""Exercise CS35L56 wire frames, bounded probe, error evidence and write rejection."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tests/Fixtures"))
from driver_kernel_fixture import install
from host_build import sanitizer_flags

CPP = r'''
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include "Devices/Amplifiers/CS35L56/CS35L56DriverRegistration.hpp"
#include "Devices/Amplifiers/CS35L56/CS35L56Device.hpp"
#include "Runtime/AmplifierHost.hpp"
using namespace cirrus;
using namespace cirrus::devices::amplifiers::cs35l56;
struct Bus : core::RegisterBus {
 unsigned calls=0,failAt=0; uint32_t id=0xab035a56,rev=0xb0;
 std::vector<std::vector<uint8_t>> frames;
 size_t maxPayloadBytes(size_t,bool) const override {return 256;}
 IOReturn command(const uint8_t* tx,size_t nt,uint8_t* rx,size_t nr) override {
  assert(nt==6&&nr==4&&rx&&tx[0]==0x80&&tx[1]==0&&tx[2]==0&&tx[4]==0&&tx[5]==0);
  frames.emplace_back(tx,tx+nt); if(++calls==failAt)return -77;
  uint32_t value=tx[3]==0?id:tx[3]==4?rev:tx[3]==12?0x11223344:3;
  for(unsigned i=0;i<4;++i)rx[i]=uint8_t(value>>(24-8*i));
  return 0;
 }
};
struct Host : platform::AmplifierHost {
 uint8_t index=0;std::map<std::string,std::string> strings;std::map<std::string,uint64_t> numbers;
 bool authorized() const override {return false;}const char* name() const override {return "probe";}
 uint8_t endpointIndex() const override {return index;}
 void setDiagnosticStage(diagnostics::DriverStage) override {}
 void markDiagnosticSuccess(diagnostics::DriverStage) override {}
 void recordDiagnosticFailure(diagnostics::DiagnosticFailure,uint32_t,uint32_t,uint32_t,IOReturn,bool) override {assert(false);}
 void publish(const char* k,const char* v) override {strings[k]=v;}
 void publish(const char* k,uint64_t v,unsigned) override {numbers[k]=v;}
};
struct Properties : platform::PlatformProperties {
 bool has=true; bool contains(const char* key)const override {return has&&!strcmp(key,"spk-id-gpios");}
 bool array(const char*,uint32_t*,size_t)const override {return false;}
};
struct GPIO : platform::BoardDiscoveryContext::GPIOReader {
 unsigned calls=0; bool ok=true;uint32_t values[2]{0,0};
 bool read(const char* property,size_t index,uint32_t& value)override {
  assert(!strcmp(property,"spk-id-gpios")&&index<2);++calls;value=values[index];return ok;
 }
};
int main(){
 auto probe=[](core::RegisterIO& io){CS35L56Device device("test",{});device.probe(io);return device.state();};
 auto format=cs35l56RegisterFormat();assert(core::RegisterMap::isValidConfig(format));
 Bus bus;core::RegisterMap io(bus,format,true);auto r=probe(io);
 assert(r.result==0&&r.identified&&r.revisionSupported&&r.readMask==15&&r.deviceId==0xab035a56&&r.otpId==3);
 assert(bus.frames==std::vector<std::vector<uint8_t>>({{0x80,0,0,0,0,0},{0x80,0,0,4,0,0},{0x80,0,0,12,0,0},{0x80,0,0,16,0,0}}));
 unsigned before=bus.calls;uint8_t bytes[8]{};uint32_t value=0;
 assert(!io.write(0,0)&&!io.bulkWrite(0,bytes,4)&&!io.noIncrementWrite(0,bytes,4));
 assert(!io.updateBits(4,0xff,0));assert(bus.calls==before+1);before=bus.calls;
 assert(!io.read(8,&value)&&!io.read(0x20,&value)&&!io.read(0xe010,&value));
 assert(!io.bulkRead(4,bytes,8)&&bus.calls==before);
 for(unsigned failure=1;failure<=4;++failure){
  Bus b;b.failAt=failure;core::RegisterMap map(b,format);auto failed=probe(map);
  const unsigned registers[]{0,4,12,16};assert(failed.result==-77&&failed.failedRegister==registers[failure-1]);
  assert(b.calls==failure&&failed.readMask==((1U<<(failure-1))-1));
 }
 for(auto id:{0U,0xffffffffU,0x35a54U,0x35a57U,0x35a41U}){
  Bus b;b.id=id;core::RegisterMap map(b,format);auto mismatch=probe(map);
  assert(mismatch.result==kIOReturnNotFound&&!mismatch.identified&&b.calls==1);
 }
 Bus unknown;unknown.rev=0xc0;core::RegisterMap unknownIO(unknown,format);
 auto unverified=probe(unknownIO);assert(unverified.identified&&!unverified.revisionSupported);
 platform::AmplifierDriverRegistry registry;assert(registerDrivers(registry));
 assert(registry.find("CSC3556",0x35a56,core::BusTransportType::SPI)==&kDriverRegistrations[0]);
 assert(!registry.find("CSC3556",0x35a56,core::BusTransportType::I2C));
 assert(kDriverRegistrations[0].diagnosticOnly&&!kDriverRegistrations[0].firmwareReset&&!kDriverRegistrations[0].supportsDeferredFirmware);
 assert(registry.acceptsDebugPhase("CSC3556","probe")&&!registry.acceptsDebugPhase("CSC3556","dsp"));
 core::BusEndpoint endpoint{core::BusTransportType::SPI,"\\_SB.PC00.SPI0",0,false,0,4000000,0};
 platform::BoardIdentity identity{0x1043,0x1c63,"CSC3556"};
 platform::AmplifierBinding binding{endpoint,0x35a56,0,0,"test","CSC3556",2};
 platform::BoardConfiguration board{identity,&binding,1,true,true,true};
 firmware::FirmwareBundle fw{{0x35a56,0x1043,0x1c63,0,0,"test"}};
 Host host;platform::AmplifierDriverContext context{io,endpoint,board,identity,0,fw,"CSC3556",nullptr,nullptr,&host};
 const platform::AmplifierDriverRegistration* selected=nullptr;
 assert(board.permits(identity)&&fw.matches(fw.target));assert(!registry.create(context,selected)&&!selected);
 inspectDevice(context);host.index=1;bus.failAt=bus.calls+2;inspectDevice(context);
 assert(host.strings["Cirrus_CS35L56_0_Probe_Status"]=="IDENTIFIED_B0_READ_ONLY");
 assert(host.strings["Cirrus_CS35L56_1_Probe_Status"]=="FAILED_NO_WAKE_ATTEMPT");
 assert(host.numbers["Cirrus_CS35L56_0_Probe_Read_Mask"]==15&&host.numbers["Cirrus_CS35L56_1_Probe_Read_Mask"]==1);
 Properties properties;GPIO gpio;platform::BoardDiscoveryContext discovery{identity,nullptr,2,properties,&gpio};const char* reason=nullptr;
 assert(!discoverBoard(discovery,reason)&&std::string(reason)=="CS35L56_READ_ONLY_SPKID0"&&gpio.calls==2);
 gpio.values[1]=1;assert(!discoverBoard(discovery,reason)&&std::string(reason)=="CS35L56_SPEAKER_ID_UNAVAILABLE_OR_INCONSISTENT");
 gpio.values[0]=1;assert(!discoverBoard(discovery,reason)&&std::string(reason)=="CS35L56_READ_ONLY_SPKID1");
 gpio.ok=false;assert(!discoverBoard(discovery,reason)&&std::string(reason)=="CS35L56_SPEAKER_ID_UNAVAILABLE_OR_INCONSISTENT");
}
'''
with tempfile.TemporaryDirectory(prefix="cirrus-cs35l56-") as directory:
    tmp = Path(directory)
    install(tmp)
    (tmp / "test.cpp").write_text(CPP)
    for flags in sanitizer_flags():
        exe = tmp / "test"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I" + str(tmp), "-I" + str(ROOT / "CirrusAudioFixup"),
                        str(tmp / "test.cpp"), str(ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L56/CS35L56DriverFactory.cpp"),
                        str(ROOT / "CirrusAudioFixup/Devices/Amplifiers/CS35L56/CS35L56Diagnostics.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print("PASS CS35L56 SPI frames, bounded identity reads, native errors, per-endpoint evidence and lifecycle/write rejection")
