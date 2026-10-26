"""Exercise CS35L56 wire frames, bounded probe, error evidence and write rejection."""
import subprocess
import os
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tests/Fixtures"))
from driver_kernel_fixture import install
from host_build import sanitizer_flags
from production_source_fixture import cs35l56_sources

CPP = r'''
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include "Devices/Amplifiers/CS35L56/CS35L56DriverRegistration.hpp"
#include "Devices/Amplifiers/CS35L56/CS35L56Device.hpp"
#include "Devices/Amplifiers/CS35L56/DSPControl.hpp"
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
struct DSPIO : core::RegisterIO {
 unsigned reads=0,writes=0,sequences=0,failRead=0,readDelayMs=0; bool sequenceOK=true,writeOK=true,failAllReads=false;
 uint32_t mailbox2=1,halo=2,power=1,mailbox1=0,revision=0xb0;
 std::vector<uint32_t> addresses;
 int32_t lastError()const override{return -77;}
 bool read(uint32_t reg,uint32_t* value)override{
  fixtureTimeMs+=readDelayMs;
  addresses.push_back(reg);if(++reads==failRead||failAllReads){*value=2;return false;}
  if(reg==registers::kRegRevisionId)*value=revision;
  else if(reg==registers::kRegDspVirtualMailbox2)*value=mailbox2;
  else if(reg==registers::kRegDspHaloState)*value=halo;
  else if(reg==registers::kRegDspPowerState)*value=power;
  else if(reg==registers::kRegTransducerState)*value=3;
  else {assert(reg==registers::kRegDspVirtualMailbox1);*value=mailbox1;}
  return true;
 }
 bool write(uint32_t reg,uint32_t value)override{
  assert(reg==registers::kRegDspVirtualMailbox1&&value==0x02000005);++writes;return writeOK;
 }
 bool writeSequence(const uint32_t* regs,const uint32_t* values,size_t count,uint32_t delay)override{
  assert(count==2&&delay==4700&&regs[0]==0x11020&&regs[1]==0x11000);
  assert(values[0]==0x02000007&&values[1]==0x0a000000);++sequences;return sequenceOK;
 }
 bool updateBits(uint32_t,uint32_t,uint32_t)override{assert(false);return false;}
 bool bulkRead(uint32_t,uint8_t*,size_t)override{assert(false);return false;}
 bool bulkWrite(uint32_t,const uint8_t*,size_t)override{assert(false);return false;}
 bool pollBit(uint32_t,uint32_t,uint32_t,uint32_t)override{assert(false);return false;}
};
static uint32_t slept=0;
static void delay(uint32_t us){assert(us==1000||us==250);slept+=us;}
static void checkDSP(){
 DSPIO identity;assert(CS35L56Device::prepareIdentity(identity)==kIOReturnSuccess);
 assert(identity.addresses==std::vector<uint32_t>({4,0x28021e0})&&!identity.writes&&!identity.sequences);
 for(uint32_t revision:{0U,0xa0U,0xb2U,0xc0U,0xffffffffU}){DSPIO unsupported;unsupported.revision=revision;
  assert(CS35L56Device::prepareIdentity(unsupported)==kIOReturnUnsupported&&unsupported.reads==1);}
 DSPIO failedRevision;failedRevision.failRead=1;
 assert(CS35L56Device::prepareIdentity(failedRevision)==-77&&failedRevision.reads==1);
 DSPIO io;assert(DSPControl::resetSPI(io,delay));
 assert(io.sequences==1&&io.addresses==std::vector<uint32_t>({0x11024,0x28021e0}));
 assert(DSPControl::shutdown(io,delay)&&io.writes==1);
 for(unsigned failure=1;failure<=2;++failure){DSPIO transient;transient.failRead=failure;slept=0;
  assert(DSPControl::resetSPI(transient,delay)&&transient.reads==3&&slept==1000&&transient.sequences==1);}
 DSPIO transientBoot;transientBoot.failRead=1;slept=0;
 assert(DSPControl::waitForBoot(transientBoot,delay)&&transientBoot.reads==2&&slept==1000);
 DSPIO absentBoot;absentBoot.failAllReads=true;slept=0;
 assert(!DSPControl::waitForBoot(absentBoot,delay)&&absentBoot.reads==251&&slept==250000&&absentBoot.lastError()==-77);
 for(bool failed:{false,true}){DSPIO slow;slow.halo=0;slow.failAllReads=failed;slow.readDelayMs=100;slept=0;
  assert(!DSPControl::waitForBoot(slow,delay)&&slow.reads==3&&slept==2000);}
 DSPIO absentReset;absentReset.failAllReads=true;slept=0;
 assert(!DSPControl::resetSPI(absentReset,delay)&&absentReset.reads==251&&slept==250000&&absentReset.sequences==1);
 for(auto reg:absentReset.addresses)assert(reg==registers::kRegDspVirtualMailbox2);
 DSPIO failedMailbox;failedMailbox.failRead=1;slept=0;
 assert(!DSPControl::shutdown(failedMailbox,delay)&&failedMailbox.reads==1&&slept==0);
 DSPIO failedPower;failedPower.failRead=2;slept=0;
 assert(!DSPControl::shutdown(failedPower,delay)&&failedPower.reads==2&&slept==0);
 DSPIO failedIdle;failedIdle.failRead=1;slept=0;
 assert(!DSPControl::waitForIdle(failedIdle,delay)&&failedIdle.reads==1&&slept==0);
 DSPIO unsupported;unsupported.sequenceOK=false;
 assert(!DSPControl::resetSPI(unsupported,delay)&&unsupported.reads==0&&unsupported.writes==0);
 for(uint32_t invalid:{0U,0xffffffffU}){DSPIO bad;bad.mailbox2=invalid;slept=0;
  assert(!DSPControl::resetSPI(bad,delay)&&bad.reads==251&&slept==250000);
  for(auto reg:bad.addresses)assert(reg==registers::kRegDspVirtualMailbox2);}
 for(uint32_t invalid:{0U,1U,0xffffU,0xffffffffU}){DSPIO bad;bad.halo=invalid;slept=0;
  assert(!DSPControl::waitForBoot(bad,delay)&&bad.reads==251&&slept==250000);}
 for(uint32_t valid:{2U,3U,0xfffeU}){DSPIO ready;ready.halo=valid;assert(DSPControl::waitForBoot(ready,delay));}
 DSPIO noWrite;noWrite.writeOK=false;assert(!DSPControl::shutdown(noWrite,delay)&&!noWrite.reads);
 DSPIO busy;busy.mailbox1=1;slept=0;assert(!DSPControl::shutdown(busy,delay)&&busy.reads==21&&slept==5000);
 DSPIO notDown;notDown.power=2;slept=0;assert(!DSPControl::shutdown(notDown,delay)&&notDown.reads==252&&slept==250000);
 DSPIO absent;assert(!DSPControl::resetSPI(absent,nullptr)&&!DSPControl::shutdown(absent,nullptr));
 assert(!DSPControl::waitForBoot(absent,nullptr)&&!absent.reads&&!absent.sequences&&!absent.writes);
}
int main(){
 checkDSP();
 auto probe=[](core::RegisterIO& io){CS35L56Device device("test",{});device.probe(io);return device.state();};
 auto format=cs35l56RegisterFormat();assert(core::RegisterMap::isValidConfig(format));
 Bus bus;core::RegisterMap io(bus,format,false);auto r=probe(io);
 assert(r.result==0&&r.identified&&r.revisionSupported&&r.readMask==15&&r.deviceId==0xab035a56&&r.otpId==3);
 assert(bus.frames==std::vector<std::vector<uint8_t>>({{0x80,0,0,0,0,0},{0x80,0,0,4,0,0},{0x80,0,0,12,0,0},{0x80,0,0,16,0,0}}));
 unsigned before=bus.calls;uint8_t bytes[8]{};uint32_t value=0;
 assert(!io.write(0,0)&&!io.bulkWrite(0,bytes,4)&&!io.noIncrementWrite(0,bytes,4));
 uint32_t resetRegisters[]={0x11020,0x11000},resetValues[]={0x02000007,0x0a000000};
 assert(!io.writeSequence(resetRegisters,resetValues,2,4700));
 assert(bus.calls==before&&io.lastReturn()==kIOReturnNotPermitted);
 assert(!io.updateBits(4,0xff,0));assert(bus.calls==before+1);before=bus.calls;
 assert(!io.read(8,&value)&&!io.read(0x20,&value)&&!io.read(0xe018,&value));
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
 assert(!kDriverRegistrations[0].diagnosticOnly&&kDriverRegistrations[0].firmwareReset&&kDriverRegistrations[0].supportsDeferredFirmware);
 assert(registry.acceptsDebugPhase("CSC3556","probe")&&registry.acceptsDebugPhase("CSC3556","dsp"));
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
 assert(!discoverBoard(discovery,reason)&&std::string(reason)=="CS35L56_PROPERTIES_INCOMPLETE"&&gpio.calls==0);

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
                        str(tmp / "test.cpp"), *map(str, cs35l56_sources()), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
# Compile real kernel headers at O0 as well: aggregate zero-initialization can
# consume kernel stack even when the destination itself was heap-allocated.
if shutil.which("clang++") and (ROOT / "MacKernelSDK/Headers/IOKit/IOLib.h").is_file():
    environment = dict(os.environ)
    environment.pop("CCC_OVERRIDE_OPTIONS", None)
    with tempfile.TemporaryDirectory(prefix="cirrus-cs35l56-kernel-") as directory:
        for optimization in ("-O0", "-O2"):
            for source in cs35l56_sources():
                subprocess.run(["clang++", "-std=c++17", optimization, "-Wall", "-Wextra", "-Werror",
                    "-Wframe-larger-than=2048", "-target", "x86_64-apple-darwin", "-mmacosx-version-min=11.0",
                    "-fapple-kext", "-fno-exceptions", "-fno-rtti", "-DKERNEL", "-DKERNEL_PRIVATE", "-DDRIVER_PRIVATE",
                    "-DAPPLE", "-D__APPLE__", "-nostdinc", "-isystem", str(ROOT / "MacKernelSDK/Headers"),
                    "-isystem", str(ROOT / "MacKernelSDK/Headers/i386"), "-I"+str(ROOT / "CirrusAudioFixup"),
                    "-Wno-ignored-attributes", "-Wno-unused-parameter", "-c", str(source),
                    "-o", str(Path(directory) / (source.stem+".o"))], env=environment, check=True)
    print("PASS Darwin kernel CS35L56 Debug/Release object compilation and 2048-byte frame limit")
else:
    print("SKIP Darwin kernel frame check: pinned SDK or clang++ unavailable")
print("PASS CS35L56 SPI frames, bounded identity reads, native errors, per-endpoint evidence and lifecycle/write rejection")
