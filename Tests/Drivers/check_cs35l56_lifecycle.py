"""Execute the complete CS35L56 driver with real firmware and bounded OS mocks."""
import subprocess
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
#include <vector>
#include <memory>
#include <string>
#include "IOKit/IOService.h"
#include "Core/AmplifierGroup.hpp"
#include "Devices/Amplifiers/CS35L56/CS35L56Driver.hpp"
#include "Devices/Amplifiers/CS35L56/Board/CS35L56BoardDiscovery.hpp"
#include "Devices/Amplifiers/CS35L56/DSPControl.hpp"
#include "Devices/Amplifiers/CS35L56/CS35L56DriverRegistration.hpp"
#include "Devices/Amplifiers/CS35L56/Resources/FirmwareData.hpp"
#include "Firmware/Calibration/EFICalibration.hpp"
#include "Firmware/WMFW/WMFWParser.hpp"
using namespace cirrus;
using namespace cirrus::devices::amplifiers::cs35l56;
using namespace cirrus::devices::amplifiers::cs35l56::registers;
struct IO : core::RegisterIO {
 std::map<uint32_t,uint32_t> regs;
 std::map<uint32_t,uint8_t> bytes;
 unsigned scalar=0,failAt=0,writes=0,sequences=0,uploads=0,pauses=0,failBulkWrites=0;
 uint32_t failedReadRegister=0xffffffff;
 int error=0;bool failed=false,stuckMailbox=false,stuckBoot=false,stuckPower=false,failUpload=false;
 std::vector<uint8_t> tuning;
 IO() {
  regs[kRegDeviceId]=kValDeviceId;regs[kRegRevisionId]=0xb0;regs[kRegOtpId]=3;
  regs[kRegDspHaloState]=2;regs[kRegDspVirtualMailbox2]=1;regs[kRegDspPowerState]=1;
  regs[kRegProtectionStatus]=1;regs[kRegFirmwareVersion]=0x030404;
  regs[kRegUserMute]=1;regs[kRegAspEnables]=0;regs[kRegTransducerState]=0;
  auto image=std::make_unique<FirmwareImage>();auto mapped=std::make_unique<MappedImage>();
  assert(WMFWParser::parseWMFW(cs35l56Firmware[0].wmfw,cs35l56Firmware[0].wmfwSize,image.get()));
  assert(WMFWMemoryMapper::mapFirmwareImage(*image,*mapped));
  const auto& r=mapped->regions[1];assert(r.regionType==RegionType::XM_PACKED&&r.firmwareAddress==0);
  std::vector<uint8_t> packed;
  for(size_t i=0;i<r.size;i+=4)for(int b=3;b>=0;--b)packed.push_back(r.data.begin[i+b]);
  uint32_t address=kRegDspXMemory;
  for(size_t i=0;i+3<=packed.size();i+=3){bytes[address++]=0;bytes[address++]=packed[i+2];bytes[address++]=packed[i+1];bytes[address++]=packed[i];}
 }
 bool operation(){error=0;if(++scalar==failAt){failed=true;error=-77;return false;}return true;}
 int32_t lastError()const override{return error;}
 bool read(uint32_t reg,uint32_t* out)override{
  if(!operation()){failedReadRegister=reg;return false;}
  *out=regs[reg];
  if(reg==kRegDspHaloState&&stuckBoot)*out=0;
  if(reg==kRegDspPowerState&&stuckPower)*out=2;
  if(reg==kRegDspVirtualMailbox1&&stuckMailbox)*out=1;
  return true;
 }
 bool write(uint32_t reg,uint32_t value)override{
  if(!operation())return false;++writes;regs[reg]=value;
  if(reg==kRegDspVirtualMailbox1){
   if(value==kCommandShutdown)regs[kRegDspPowerState]=1;
   if(value==kCommandAudioPause){++pauses;regs[kRegTransducerState]=3;}
   if(value==kCommandAudioPlay)regs[kRegTransducerState]=0;
   regs[reg]=0;
  }
  return true;
 }
 bool updateBits(uint32_t reg,uint32_t mask,uint32_t value)override{
  uint32_t old=0;return read(reg,&old)&&write(reg,(old&~mask)|(value&mask));
 }
 bool writeSequence(const uint32_t* addresses,const uint32_t* values,size_t count,uint32_t delay)override{
  assert(count==2&&addresses[0]==kRegDspVirtualMailbox1&&addresses[1]==kRegDspRawMailbox1&&delay==4700);
  assert(values[0]==kCommandSystemReset&&values[1]==kCommandPing);++sequences;
  if(!operation())return false;regs[kRegDspHaloState]=2;regs[kRegDspVirtualMailbox2]=1;return true;
 }
 bool bulkRead(uint32_t reg,uint8_t* out,size_t n)override{
  error=0;assert(n&&n<=252&&!(reg&3)&&!(n&3));
  for(size_t i=0;i<n;++i)out[i]=bytes[reg+uint32_t(i)];return true;
 }
 bool bulkWrite(uint32_t reg,const uint8_t* data,size_t n)override{
  assert(n&&n<=252&&dspMemoryRegister(reg)&&dspMemoryRegister(reg+uint32_t(n)-4));
  if(failUpload||failBulkWrites){if(failBulkWrites)--failBulkWrites;error=-77;return false;}error=0;++uploads;
  for(size_t i=0;i<n;++i)bytes[reg+uint32_t(i)]=data[i];
  regs[kRegFirmwareVersion]=kFirmwareVersion;
  tuning.insert(tuning.end(),data,data+n);
  return true;
 }
 bool pollBit(uint32_t,uint32_t,uint32_t,uint32_t)override{assert(false);return false;}
};
struct Host : platform::AmplifierHost {
 bool allow=true,ro=false,route=true;const char* phase=nullptr;
 std::map<std::string,std::string> text;
 bool authorized()const override{return allow;}bool supportsResources()const override{return true;}
 const char* name()const override{return "cs35l56-test";}
 bool bootArgEnabled(const char* key)const override{return ro&&!strcmp(key,"-cirrusro");}
 bool stopAfterDebugStage(const char* key)override{return phase&&!strcmp(phase,key);}
 void setDiagnosticStage(diagnostics::DriverStage)override{}
 void markDiagnosticSuccess(diagnostics::DriverStage)override{}
 void recordDiagnosticFailure(diagnostics::DiagnosticFailure,uint32_t,uint32_t,uint32_t,IOReturn,bool)override{}
 void publish(const char* key,const char* value)override{text[key]=value;}
 bool hasRouteEvents()const override{return route;}
 bool routeUnchanged(uint32_t generation)const override{return route&&generation==7;}
 platform::hda::HDAStreamState streamObservation()const override{
  platform::hda::HDAStreamState s{};s.observed=s.streamActive=s.converterPrepared=route;s.lastStreamTag=1;s.lastFormat=0x31;return s;
 }
};
struct Fixture {
 IO io;Host host;
 core::BusEndpoint endpoint{core::BusTransportType::SPI,"\\_SB.PC00.SPI0",0,false,0,4000000,0};
 platform::BoardIdentity identity{0x1043,0x1c63,"CSC3556"};
 platform::AmplifierBinding binding{endpoint,kValDeviceId,0,0,"speaker-protection","CSC3556"};
 platform::BoardConfiguration board{identity,&binding,1,true,true,true,true};
 firmware::FirmwareBundle assets{{kValDeviceId,0x1043,0x1c63,0,0,"speaker-protection"},firmware::FirmwareFormat::Deferred};
 platform::AmplifierDriverContext context{io,endpoint,board,identity,0,assets,"CSC3556",nullptr,nullptr,&host};
 CS35L56Driver driver{context};core::AmplifierLifecycle lifecycle{driver};
 IORegistryEntry options;OSData data;
 Fixture(bool calibration=true){
  if(calibration){
   data.bytes.resize(48);data.bytes[0]=48;data.bytes[4]=2;
   for(size_t i:{size_t(8),size_t(28)}){data.bytes[i+8]=1;data.bytes[i+16]=23;data.bytes[i+17]=1;data.bytes[i+18]=0x34;data.bytes[i+19]=0x12;}
   options.properties["CirrusSmartAmpCalibrationData"]=&data;fixtureOptions=&options;
  }else fixtureOptions=nullptr;
 }
 ~Fixture(){fixtureOptions=nullptr;assert(fixtureLiveAllocations==0);}
 core::PlaybackContext playback(){return {true,true,true,true,7,1,0x31,0};}
 void idle(){assert(io.regs[kRegAspEnables]==0&&io.regs[kRegUserMute]==1);}
};
struct Properties : platform::PlatformProperties {
 bool valid=true;uint32_t indices[2]{0,1},boost[2]{0,0};
 bool contains(const char* key)const override{return !strcmp(key,"spk-id-gpios");}
 bool array(const char* key,uint32_t* out,size_t count)const override{
  if(!valid||count!=2)return false;
  const uint32_t* source=!strcmp(key,"cirrus,dev-index")?indices:!strcmp(key,"cirrus,boost-type")?boost:nullptr;
  if(!source)return false;out[0]=source[0];out[1]=source[1];return true;
 }
};
struct GPIO : platform::BoardDiscoveryContext::GPIOReader {
 uint32_t speaker[2]{0,0};bool valid=true;
 bool read(const char* key,size_t index,uint32_t& out)override{assert(!strcmp(key,"spk-id-gpios")&&index<2);out=speaker[index];return valid;}
};
int main(){
 {Properties props;GPIO gpio;const char* reason=nullptr;
  core::BusEndpoint endpoints[2]{{core::BusTransportType::SPI,"\\\\_SB.SPI9",0,false,1,4000000,0},
                               {core::BusTransportType::SPI,"\\\\_SB.SPI9",0,false,0,4000000,0}};
  platform::BoardDiscoveryContext c{{0x1043,0x1c63,"CSC3556"},endpoints,2,props,&gpio};
  auto* board=resolveBoardConfiguration(c,reason);assert(board&&board->configuration.permits(c.identity));
  assert(board->configuration.amplifiers[0].channelIndex==1&&board->configuration.amplifiers[1].channelIndex==0);
  assert(board->configuration.amplifiers[0].endpoint.controllerPath!=endpoints[0].controllerPath);delete board;
  gpio.speaker[1]=1;assert(!resolveBoardConfiguration(c,reason));gpio.speaker[0]=1;assert(!resolveBoardConfiguration(c,reason));
  gpio.speaker[0]=gpio.speaker[1]=0;props.boost[0]=1;assert(!resolveBoardConfiguration(c,reason));props.boost[0]=0;
  props.indices[1]=0;assert(!resolveBoardConfiguration(c,reason));props.indices[1]=1;
  endpoints[1].spiChipSelect=1;assert(!resolveBoardConfiguration(c,reason));endpoints[1].spiChipSelect=0;
  c.identity.subsystemDevice=0xffff;assert(!resolveBoardConfiguration(c,reason));}
 unsigned scalarCount=0;std::vector<uint8_t> left,right;
 for(unsigned channel:{0U,1U}){
  Fixture f;f.binding.channelIndex=channel;f.assets.target.channelIndex=channel;f.context.endpoint.spiChipSelect=f.endpoint.spiChipSelect=f.binding.endpoint.spiChipSelect=channel;
  assert(f.lifecycle.bringUp()==0);assert(f.driver.report().initialized&&!f.driver.report().faulted);
  assert(std::string(f.driver.report().readyVerdict)=="READY_DSP");f.idle();assert(f.io.sequences==2&&f.io.uploads>0);
  (channel?right:left)=f.io.tuning;
  if(!channel)scalarCount=f.io.scalar;
  auto context=f.playback();assert(f.driver.runtimeTick(context)==0&&f.driver.playbackPrepared());
  assert(f.lifecycle.startPlayback()==0&&f.io.regs[kRegAspEnables]==(kAspReceiveMask|(1U<<f.endpoint.spiChipSelect))&&f.io.regs[kRegUserMute]==0);
  f.host.route=false;assert(f.driver.runtimeTick(context)==0);f.idle();f.lifecycle.refreshPlaybackState();
  f.host.route=true;assert(f.lifecycle.suspend()==0);f.idle();assert(f.lifecycle.resume()==0);f.idle();
  assert(f.io.sequences==3);assert(f.driver.safeShutdown()==0);f.idle();
 }
 assert(left!=right);
 for(unsigned failure=1;failure<=scalarCount;++failure){
  Fixture f;f.io.failAt=failure;const auto result=f.lifecycle.bringUp();assert(f.io.failed);
  if(result==0){assert(f.driver.report().initialized&&!f.driver.report().faulted);
   assert(f.io.failedReadRegister==kRegIrqStatus||f.io.failedReadRegister==kRegDspHaloState||
          f.io.failedReadRegister==kRegDspVirtualMailbox2||std::string(f.driver.report().readyVerdict)=="READY_DSP_OUTPUT_DISABLED_CALIBRATION");}
  else{assert(result==-77);assert(f.lifecycle.status().firstFailure==-77);assert(f.lifecycle.status().stage==core::AmplifierStage::Faulted);}
  f.idle();assert(f.driver.safeShutdown()==0);
 }
 for(unsigned allocation=1;allocation<=3;++allocation){
  Fixture f;fixtureAllocationFailAt=fixtureAllocations+allocation;
  assert(f.lifecycle.bringUp()==kIOReturnNoMemory);fixtureAllocationFailAt=0;f.idle();assert(f.driver.safeShutdown()==0);
 }
 {Fixture f(false);assert(f.lifecycle.bringUp()==0);f.idle();assert(!f.driver.validatePlayback(f.playback()));assert(f.lifecycle.startPlayback()!=0);f.idle();}
 for(unsigned invalid:{0U,1U,2U,3U}){
  Fixture f;
  if(invalid==0)f.host.ro=true;
  if(invalid==1)f.board.wiringVerified=false;
  if(invalid==2)f.io.regs[kRegRevisionId]=0xc0;
  if(invalid==3)f.io.regs[kRegDeviceId]=0x35a41;
  assert(f.lifecycle.bringUp()!=0&&f.io.writes==0&&f.io.uploads==0&&f.io.sequences==0);
 }
 for(bool secured:{false,true}){Fixture f;f.io.regs[secured?kRegRestricted:kRegFirmwareVersion]=secured?7:0x030902;f.io.regs[kRegProtectionStatus]=0;
  assert(f.lifecycle.bringUp()==kIOReturnUnsupported&&f.io.writes==0&&f.io.uploads==0);}
 {Fixture f;f.io.regs[kRegProtectionStatus]=0;f.io.regs[kRegFirmwareVersion]=kFirmwareVersion;
  assert(f.lifecycle.bringUp()==0&&f.io.sequences==1);f.idle();assert(f.driver.safeShutdown()==0);}
 for(const char* phase:{"probe","firmware","dsp"}){Fixture f;f.host.phase=phase;
  assert(f.lifecycle.bringUp()==kIOReturnNotReady&&f.lifecycle.status().stage==core::AmplifierStage::DebugHalted);f.idle();assert(f.driver.safeShutdown()==0);}
 for(unsigned timeout:{0U,1U,2U}){Fixture f;
  if(timeout==0)f.io.stuckBoot=true;if(timeout==1)f.io.stuckMailbox=true;if(timeout==2)f.io.stuckPower=true;
  assert(f.lifecycle.bringUp()!=0);f.io.stuckMailbox=false;f.io.stuckBoot=false;f.io.stuckPower=false;assert(f.driver.safeShutdown()==0);f.idle();}
 {Fixture f;f.io.failUpload=true;assert(f.lifecycle.bringUp()!=0);assert(f.io.sequences==1);f.idle();f.io.failUpload=false;assert(f.driver.safeShutdown()==0);}
 {Fixture f;f.io.failBulkWrites=2;assert(f.lifecycle.bringUp()==-77);
  assert(f.io.error==0&&f.lifecycle.status().firstFailure==-77);f.idle();assert(f.driver.safeShutdown()==0);}
 // Sweep playback and cleanup transfer failures independently of firmware retries.
 unsigned startTransfers=0,stopTransfers=0;
 {Fixture f;assert(f.lifecycle.bringUp()==0);auto c=f.playback();assert(f.driver.runtimeTick(c)==0);
  auto before=f.io.scalar;assert(f.driver.startPlayback()==0);startTransfers=f.io.scalar-before;
  before=f.io.scalar;assert(f.driver.stopPlayback()==0);stopTransfers=f.io.scalar-before;assert(f.driver.safeShutdown()==0);}
 for(unsigned failure=1;failure<=startTransfers;++failure){Fixture f;assert(f.lifecycle.bringUp()==0);
  assert(f.driver.runtimeTick(f.playback())==0);f.io.failAt=f.io.scalar+failure;
  const auto result=f.driver.startPlayback();
  if(failure==1)assert(result==0);
  else assert(result==-77);
  if(failure!=1)f.idle();assert(f.driver.safeShutdown()==0);}
 for(unsigned failure=1;failure<=stopTransfers;++failure){Fixture f;assert(f.lifecycle.bringUp()==0);
  assert(f.driver.runtimeTick(f.playback())==0);assert(f.driver.startPlayback()==0);f.io.failAt=f.io.scalar+failure;
  assert(f.driver.stopPlayback()==-77);assert(f.driver.safeShutdown()==0);f.idle();}
 for(uint32_t reg:{kRegIrqFault1,kRegIrqFault8}){Fixture f;assert(f.lifecycle.bringUp()==0);
  f.io.regs[reg]=0x80000000;assert(f.driver.runtimeTick(f.playback())==0);
  assert(f.driver.startPlayback()!=0);f.idle();assert(f.driver.safeShutdown()==0);}
 {Fixture f;assert(f.lifecycle.bringUp()==0);assert(f.driver.runtimeTick(f.playback())==0);
  assert(f.driver.startPlayback()==0);f.io.regs[kRegFirmwareVersion]=0;
  assert(f.driver.runtimeTick(f.playback())!=0);f.idle();assert(f.driver.safeShutdown()==0);}
 // A failure on the second member must mute the already-started first member.
 {Fixture a,b;b.binding.channelIndex=b.assets.target.channelIndex=1;b.context.endpoint.spiChipSelect=b.endpoint.spiChipSelect=b.binding.endpoint.spiChipSelect=1;
  core::AmplifierLifecycle* members[]{&a.lifecycle,&b.lifecycle};core::AmplifierGroup group;
  assert(group.configure(members,2,false)&&group.bringUp()==0);
  assert(a.driver.runtimeTick(a.playback())==0&&b.driver.runtimeTick(b.playback())==0);
  b.io.failAt=b.io.scalar+2;assert(group.startPlayback()==-77);a.idle();b.idle();
  assert(a.driver.safeShutdown()==0&&b.driver.safeShutdown()==0);}
 // A cold reset while idle invalidates readiness before AUDIO_PLAY.
 {Fixture f;assert(f.lifecycle.bringUp()==0);assert(f.driver.runtimeTick(f.playback())==0);
  f.io.regs[kRegProtectionStatus]=1;assert(f.driver.startPlayback()!=0);f.idle();assert(f.driver.safeShutdown()==0);}
 {Fixture f;assert(f.lifecycle.bringUp()==0);const auto writes=f.io.writes;
  for(unsigned i=0;i<10;++i)assert(f.driver.cancelPlayback()==0);
  assert(f.io.writes==writes);assert(f.driver.safeShutdown()==0);}
 // OTP/PTE is the CS35L56 UID source, unlike CS35L41 DIE_STS.
 {Fixture f;const uint8_t pte[]{0x11,0x22,0,0,0x33,0x44,0x55,0x66,0,0,0,0x77};
  for(size_t i=0;i<sizeof(pte);++i)f.io.bytes[kRegOtpMemory53+uint32_t(i)]=pte[i];
  uint64_t uid=0;assert(DSPControl::readSiliconUid(f.io,uid)&&uid==0x0033445577226611ULL);
  const uint8_t target[]{0x11,0x66,0x22,0x77,0x55,0x44,0x33,0};
  for(size_t i=0;i<sizeof(target);++i)f.data.bytes[8+i]=target[i];
  assert(f.lifecycle.bringUp()==0&&std::string(f.driver.report().readyVerdict)=="READY_DSP");f.idle();assert(f.driver.safeShutdown()==0);}
 // Strict EFI bounds, occupied records, target UID and measured status.
 {Fixture f;firmware::EFICalibration cal{};auto bytes=f.data.bytes;
  assert(firmware::EFICalibration::select(bytes.data(),bytes.size(),0,0,cal)&&cal.resistance==0x1234&&cal.ambient==23);
  for(size_t n=0;n<48;++n)assert(!firmware::EFICalibration::select(bytes.data(),n,0,0,cal));
  bytes[8]=1;assert(!firmware::EFICalibration::select(bytes.data(),bytes.size(),2,0,cal));
  assert(firmware::EFICalibration::select(bytes.data(),bytes.size(),1,0,cal));
  bytes[25]=0;assert(!firmware::EFICalibration::select(bytes.data(),bytes.size(),1,0,cal));}
 assert(fixtureLiveAllocations==0);
}
'''
with tempfile.TemporaryDirectory(prefix="cirrus-cs35l56-lifecycle-") as directory:
    tmp=Path(directory)
    install(tmp)
    (tmp/"test.cpp").write_text(CPP)
    for flags in sanitizer_flags():
        exe=tmp/"test"
        subprocess.run(["g++","-std=c++17","-O1","-Wall","-Wextra","-Werror","-Wno-misleading-indentation",*flags,
                        "-I"+str(tmp),"-I"+str(ROOT/"CirrusAudioFixup"),str(tmp/"test.cpp"),
                        *map(str,cs35l56_sources()),"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
print("PASS complete CS35L56: real firmware/tuning, calibration, idle, route/playback, PM, allocation/transfer faults and debug stops")
