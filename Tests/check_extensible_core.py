"""Production generic register maps, board identity, assets and lifecycle failures."""
from pathlib import Path
import tempfile,subprocess
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cirrus-core-') as directory:
    p=Path(directory);(p/'IOKit').mkdir()
    (p/'IOKit/IOLib.h').write_text('''#pragma once
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,
kIOReturnNotPermitted=-3,kIOReturnTimeout=-4,kIOReturnNoMemory=-5,kIOReturnUnsupported=-6;
inline unsigned sleeps=0;inline void IOSleep(unsigned){++sleeps;}
''')
    (p/'test.cpp').write_text(r'''
#include <cassert>
#include <cstring>
#include <vector>
#include "Core/RegisterMap.hpp"
#include "Transport/I2CController.hpp"
#include "Transport/I2CRegisterBus.hpp"
#include "Platform/BoardConfiguration.hpp"
#include "Firmware/FirmwareBundle.hpp"
#include "Core/AmplifierLifecycle.hpp"
#include "Core/AmplifierGroup.hpp"
#include "Platform/AmplifierDriverRegistry.hpp"
#include "Platform/AmplifierManager.hpp"
using namespace cirrus::core;
struct Lock: RegisterOperationLock {
 bool held=false;unsigned calls=0;void lock() override {assert(!held);held=true;++calls;}
 void unlock() override {assert(held);held=false;}
};
struct Bus: RegisterBus {
 size_t limit=1024;unsigned calls=0,failAt=0;Lock* lock=nullptr;
 std::vector<std::vector<uint8_t>> frames;std::vector<uint8_t> response{0x11,0x22,0x33,0x44};
 size_t maxPayloadBytes(size_t,bool) const override {return limit;}
 IOReturn command(const uint8_t* tx,size_t nt,uint8_t* rx,size_t nr) override {
  assert(nt<=262 && nr<=256);if(lock)assert(lock->held);
  frames.emplace_back(tx,tx+nt);if(++calls==failAt)return -99;
  for(size_t i=0;i<nr;++i)rx[i]=response[i%response.size()];
  return 0;
 }
};
bool accessible(uint32_t){return true;}
bool deny(uint32_t r){return r!=4;}
bool precious(uint32_t r){return r==0;}
bool preciousMiddle(uint32_t r){return r==4;}
struct I2C: cirrus::transport::I2CController {
 unsigned calls=0;size_t maxWriteBytes()const override{return 8;}size_t maxReadBytes()const override{return 12;}
 IOReturn transfer(const cirrus::transport::I2CEndpoint& ep,const uint8_t*,size_t,uint8_t*,size_t) override {
  assert(ep.address==0x355&&ep.tenBit);++calls;return 0;
 }
};
struct Hooks: AmplifierLifecycleHooks {
 int failure=0,cleanupFailure=0;unsigned cleanups=0;
 BringUpDisposition pause=BringUpDisposition::Continue;
 BringUpDisposition disposition() const override {return !history.empty()&&history.back()==4?pause:BringUpDisposition::Continue;}
 std::vector<int> history;
 int call(int id){history.push_back(id);return failure==id?-100-id:0;}
 IOReturn identify()override{return call(1);}IOReturn authorizeBoard()override{return call(2);}
 IOReturn initialize()override{return call(3);}IOReturn loadFirmware()override{return call(4);}
 IOReturn verifyReady()override{return call(5);}IOReturn startPlayback()override{return call(6);}
 IOReturn stopPlayback()override{return call(7);}IOReturn safeShutdown()override{++cleanups;history.push_back(8);return cleanupFailure;}
};
Hooks instance;unsigned destroyed=0;
AmplifierLifecycleHooks* factory(const cirrus::platform::AmplifierDriverContext&){return &instance;}
void destroy(AmplifierLifecycleHooks* hooks){assert(hooks==&instance);++destroyed;}
unsigned heapCreated=0,heapDestroyed=0,heapCleanup=0;
int failFactory=-1,failFirmware=-1,failTick=-1;bool waitFirmware=false;
bool resourceReady=false;IOReturn retryError=0;unsigned resourcePolls=0;
struct HeapHooks:Hooks {
 const cirrus::platform::AmplifierDriverContext* metadata;
 IOReturn identify() override {assert(metadata->actualDeviceHid&&metadata->bindingIndex<2);return Hooks::identify();}
 HeapHooks(const cirrus::platform::AmplifierDriverContext& context):metadata(&context){++heapCreated;if(int(context.bindingIndex)==failFirmware)failure=4;if(waitFirmware&&context.bindingIndex==1)pause=BringUpDisposition::Wait;}
 ~HeapHooks() override {++heapDestroyed;}
 IOReturn prepareBringUpRetry(uint64_t)override{++resourcePolls;if(retryError)return retryError;if(!resourceReady)return kIOReturnNotReady;pause=BringUpDisposition::Continue;return 0;}
 IOReturn runtimeTick(const PlaybackContext&) override {return int(metadata->bindingIndex)==failTick?-300:0;}
 IOReturn safeShutdown() override {++heapCleanup;return Hooks::safeShutdown();}
};
AmplifierLifecycleHooks* heapFactory(const cirrus::platform::AmplifierDriverContext& context){return int(context.bindingIndex)==failFactory?nullptr:new HeapHooks(context);}
void heapDestroy(AmplifierLifecycleHooks* hooks){delete hooks;}
int main(){
 for(unsigned width=1;width<=4;++width)for(auto endian:{RegisterEndian::Big,RegisterEndian::Little}){
  Bus bus;RegisterMapConfig config;config.addressBytes=2;config.valueBytes=uint8_t(width);
  config.stride=8;config.maxRegister=0xfff8;config.addressEndian=endian;config.valueEndian=endian;
  RegisterMap map(bus,config,true);uint32_t value=0;assert(map.read(0x1230,&value));
  uint32_t expected=0;for(unsigned i=0;i<width;++i)expected|=uint32_t(bus.response[i])<<(8*(endian==RegisterEndian::Big?width-1-i:i));
  assert(value==expected&&map.write(0x1230,expected));
  auto& frame=bus.frames.back();assert(frame.size()==2+width&&frame[0]==(endian==RegisterEndian::Big?0x12:0x30));
  for(unsigned i=0;i<width;++i)assert(frame[2+i]==bus.response[i]);
  if(width<4)assert(!map.write(0,1U<<(width*8))&&map.lastReturn()==kIOReturnBadArgument);
 }
 Bus bus;bus.limit=6;RegisterMapConfig c;c.addressBytes=1;c.valueBytes=2;c.stride=4;c.maxRegister=0xfc;
 RegisterMap map(bus,c,true);uint8_t bytes[12]{};assert(map.bulkRead(0,bytes,12));
 assert(bus.frames.size()==2&&bus.frames[0][0]==0&&bus.frames[1][0]==12);
 unsigned n=bus.calls;assert(!map.bulkRead(0xf8,bytes,6)&&bus.calls==n);
 c.readable=deny;RegisterMap limited(bus,c,true);assert(!limited.bulkRead(0,bytes,4)&&bus.calls==n);
 c.readable=nullptr;c.precious=precious;RegisterMap diag(bus,c,true);uint32_t value=0;
 assert(!diag.readForDiagnostic(0,&value)&&bus.calls==n);assert(diag.read(0,&value));
 // Diagnostic bulk validates every register before issuing any chunk.
 RegisterMapConfig diagnosticConfig=c;diagnosticConfig.precious=preciousMiddle;
 RegisterMap diagnosticBulk(bus,diagnosticConfig,true);n=bus.calls;
 RegisterIO& diagnosticPort=diagnosticBulk;
 assert(!diagnosticPort.bulkReadForDiagnostic(0,bytes,6)&&bus.calls==n);
 assert(diagnosticPort.bulkReadForDiagnostic(8,bytes,4)&&bus.calls==n+1);
 c.noIncrementReadable=precious;RegisterMap fifo(bus,c);bus.frames.clear();
 assert(fifo.noIncrementRead(0,bytes,12)&&bus.frames.size()==2&&bus.frames[0][0]==0&&bus.frames[1][0]==0);
 n=bus.calls;assert(!fifo.noIncrementRead(8,bytes,4)&&bus.calls==n);
 assert(!diag.updateBits(0,0xff,1)&&bus.calls==n);
 RegisterMap readonly(bus,c);n=bus.calls;assert(!readonly.write(0,1)&&bus.calls==n);
 Lock lock;bus.lock=&lock;RegisterMap locked(bus,c,true,&lock);
 assert(locked.updateBits(8,0xff,0x55)&&lock.calls==1&&!lock.held);bus.lock=nullptr;
 bus.failAt=bus.calls+1;value=0xdeadbeef;assert(!map.read(0,&value)&&value==0xdeadbeef&&map.lastReturn()==-99&&map.lastError()==-99);bus.failAt=0;
 // Eight-byte headers and a large controller limit still fit the fixed frame.
 Bus large;RegisterMapConfig pad;pad.paddingBytes=4;RegisterMap padded(large,pad,true);
 uint8_t payload[520]{};assert(padded.bulkWrite(0,payload,sizeof(payload)));
 assert(large.frames.size()==3&&large.frames[0].size()==260&&large.frames[1][2]==0&&large.frames[1][3]==252);
 RegisterMapConfig bad=pad;bad.valueBytes=0;RegisterMap invalid(large,bad,true);
 n=large.calls;assert(!invalid.bulkRead(0,payload,4)&&large.calls==n);
 I2C i2c;cirrus::transport::I2CRegisterBus port(&i2c,{0x355,true});
 assert(port.maxPayloadBytes(2,true)==12&&port.maxPayloadBytes(2,false)==6);
 assert(port.command(payload,2,payload,4)==0&&i2c.calls==1);
 cirrus::transport::I2CRegisterBus invalidAddress(&i2c,{0x80,false});
 assert(invalidAddress.command(payload,2,payload,4)==kIOReturnBadArgument&&i2c.calls==1);
 using namespace cirrus::platform;
 AmplifierBinding bindings[2];bindings[0].endpoint={BusTransportType::SPI,"\\_SB.SPI1",0,false,1,1000000,0};
 bindings[0].siliconId=0x35a40;bindings[0].tuningResource="board-left";
 bindings[1]=bindings[0];bindings[1].endpoint.spiChipSelect=0;bindings[1].channelIndex=1;
 BoardIdentity identity{0x103c,0x89c6,"CSC3551"};BoardConfiguration board{identity,bindings,2,true,true,true};
 assert(board.permits(identity));auto wrong=identity;wrong.subsystemDevice=0x8ca4;assert(!board.permits(wrong));
 bindings[1].endpoint.spiChipSelect=1;assert(!board.permits(identity));bindings[1].endpoint.spiChipSelect=0;
 board.powerVerified=false;assert(!board.permits(identity));
 cirrus::firmware::FirmwareTarget target{0x35a40,0x103c,0x89c6,0,0,"board-left"};
 cirrus::firmware::FirmwareBundle fw{target,cirrus::firmware::FirmwareFormat::WMFW,payload,4,payload,4};
 assert(fw.matches(target));auto wrongFW=target;wrongFW.channelIndex=1;assert(!fw.matches(wrongFW));
 fw.program=nullptr;assert(!fw.matches(target));fw.format=cirrus::firmware::FirmwareFormat::None;fw.programBytes=0;assert(fw.matches(target));
 board.powerVerified=true;fw.format=cirrus::firmware::FirmwareFormat::WMFW;fw.program=payload;fw.programBytes=4;
 AmplifierDriverRegistration definition{"test-fixture","CSC3551",0x35a40,3,{}, {},factory,destroy};
 AmplifierDriverRegistry registry;assert(!registry.add(definition));
 definition.i2cFormat.readable=definition.i2cFormat.writable=accessible;
 definition.spiFormat.readable=definition.spiFormat.writable=accessible;
 assert(registry.add(definition)&&!registry.add(definition));
 AmplifierDriverContext context{map,bindings[0].endpoint,board,identity,0,fw};
 const AmplifierDriverRegistration* selected=nullptr;auto* created=registry.create(context,selected);
 assert(created==&instance&&selected==&definition);selected->destroy(created);assert(destroyed==1);
 context.actualBoard=wrong;assert(!registry.create(context,selected)&&selected==&definition);
 assert(!registry.find("CSC3551",0x35a40,BusTransportType::SoundWire));
 for(int failure=1;failure<=7;++failure){
  Hooks hooks;hooks.failure=failure;AmplifierLifecycle life(hooks);
  int ret=life.bringUp();if(failure>=6){assert(ret==0);ret=life.startPlayback();if(failure==7){assert(ret==0);ret=life.stopPlayback();}}
  assert(ret==-100-failure&&life.status().stage==AmplifierStage::Faulted);
  assert(hooks.cleanups==(failure>=3?1U:0U));unsigned count=hooks.history.size();
  assert(life.bringUp()==kIOReturnNotReady&&hooks.history.size()==count);
  life.powerLost();hooks.failure=0;assert(life.bringUp()==0&&life.status().firstFailure==-100-failure);
 }
 // Production owner adapter must not verify/play or clean up a pending/debug stop.
 for(auto deferred:{BringUpDisposition::Wait,BringUpDisposition::DebugHalt}) {
  unsigned inits=0,uploads=0,verifies=0,cleanups=0;
  auto progress=BringUpDisposition::Continue;
  auto init=[&]()->IOReturn{++inits;return 0;};
  auto fw=[&]()->IOReturn{++uploads;progress=deferred;return 0;};
  auto verify=[&]()->IOReturn{++verifies;return 0;};
  auto cleanup=[&]()->IOReturn{++cleanups;return 0;};
  auto disposition=[&](){return progress;};
  auto status=runAmplifierBringUp(init,fw,verify,cleanup,disposition);
  assert(status.stage==(deferred==BringUpDisposition::Wait?AmplifierStage::Waiting:AmplifierStage::DebugHalted));
  assert(inits==1&&uploads==1&&!verifies&&!cleanups&&!status.failures);
  // Re-enter only after the owner has reset/revalidated the device.
  progress=BringUpDisposition::Continue;
  auto readyFw=[&]()->IOReturn{++uploads;return 0;};
  status=runAmplifierBringUp(init,readyFw,verify,cleanup,disposition);
  assert(status.stage==AmplifierStage::Ready&&verifies==1&&!cleanups);
  auto failed=[&]()->IOReturn{return -333;};
  status=runAmplifierBringUp(init,failed,verify,cleanup,disposition);
  assert(status.stage==AmplifierStage::Faulted&&status.firstFailureStage==AmplifierStage::Firmware);
  assert(status.firstFailure==-333&&verifies==1&&cleanups==1);
 }
 Hooks hooks;AmplifierLifecycle life(hooks);assert(life.bringUp()==0&&life.startPlayback()==0);
 assert(life.suspend()==0&&life.status().stage==AmplifierStage::Suspended);
 assert(life.resume()==0&&life.status().stage==AmplifierStage::Playing);
 assert(hooks.history==std::vector<int>({1,2,3,4,5,6,7,8,1,2,3,4,5,6}));
 assert(life.detach()==0&&life.status().stage==AmplifierStage::Detached);
 Hooks left,right;AmplifierLifecycle l(left),r(right);AmplifierLifecycle* members[]={&l,&r};AmplifierGroup group;
 assert(group.configure(members,2)&&!group.configure(members,2));
 right.failure=4;assert(group.bringUp()==-104&&group.stage()==AmplifierStage::Faulted);
 assert(left.cleanups==1&&right.cleanups==1&&l.status().stage==AmplifierStage::Detached);
 group.powerLost();right.failure=0;assert(group.bringUp()==0);right.failure=6;
 assert(group.startPlayback()==-106&&left.history[left.history.size()-2]==7&&left.history.back()==8);
 group.powerLost();right.failure=0;assert(group.bringUp()==0&&group.startPlayback()==0);
 assert(group.suspend()==0&&group.resume()==0&&group.stage()==AmplifierStage::Playing);
 assert(group.firstFailure()==-104&&group.detach()==0);
 for(auto pause:{BringUpDisposition::Wait,BringUpDisposition::DebugHalt}) {
  Hooks a,b;b.pause=pause;AmplifierLifecycle al(a),bl(b);AmplifierLifecycle* pair[]={&al,&bl};AmplifierGroup pending;
  assert(pending.configure(pair,2)&&pending.bringUp()==kIOReturnNotReady);
  assert(pending.stage()==(pause==BringUpDisposition::Wait?AmplifierStage::Waiting:AmplifierStage::DebugHalted));
  assert(!pending.firstFailure()&&!a.cleanups&&!b.cleanups);
  assert(pending.startPlayback()==kIOReturnNotReady&&al.status().stage==AmplifierStage::Ready);
  assert(pending.detach()==0&&a.cleanups==1&&b.cleanups==1);
  b.pause=BringUpDisposition::Continue;assert(pending.bringUp()==0&&pending.startPlayback()==0&&pending.suspend()==0);
  b.pause=pause;assert(pending.resume()==kIOReturnNotReady&&!pending.firstFailure());
  assert(al.status().stage==AmplifierStage::Ready&&bl.status().stage!=AmplifierStage::Playing);
  assert(pending.detach()==0);
 }
 // Fictional mixed-silicon fixtures: no ASUS/HP board support is registered.
 AmplifierBinding mixed[2];
 mixed[0].endpoint={BusTransportType::SPI,"\\_SB.SPI1",0,false,0,1000000,0};
 mixed[0].siliconId=0x1111;mixed[0].tuningResource="fixture-left";mixed[0].acpiHid="SPIAMP";
 mixed[1].endpoint={BusTransportType::I2C,"\\_SB.I2C1",0x40,false,0,0,0};
 mixed[1].siliconId=0x2222;mixed[1].channelIndex=1;mixed[1].tuningResource="fixture-right";mixed[1].acpiHid="I2CAMP";
 BoardIdentity machine{0x1234,0x5678,"BOARD"};BoardConfiguration multiBoard{machine,mixed,2,true,true,true};
 cirrus::firmware::FirmwareTarget lt{0x1111,0x1234,0x5678,0,0,"fixture-left"},rt{0x2222,0x1234,0x5678,0,1,"fixture-right"};
 cirrus::firmware::FirmwareBundle lf{lt,cirrus::firmware::FirmwareFormat::None,nullptr,0,nullptr,0},rf{rt,cirrus::firmware::FirmwareFormat::None,nullptr,0,nullptr,0};
 AmplifierDriverRegistration spiAmp{"fixture-spi","SPIAMP",0x1111,2,{}, {},heapFactory,heapDestroy};
 AmplifierDriverRegistration i2cAmp{"fixture-i2c","I2CAMP",0x2222,1,{}, {},heapFactory,heapDestroy};
 spiAmp.spiFormat.readable=spiAmp.spiFormat.writable=accessible;
 i2cAmp.i2cFormat.valueBytes=2;i2cAmp.i2cFormat.readable=i2cAmp.i2cFormat.writable=accessible;
 AmplifierDriverRegistry multiRegistry;assert(multiRegistry.add(spiAmp)&&multiRegistry.add(i2cAmp));
 RegisterMap secondPort(bus,c,true);
 AmplifierDriverContext contexts[]={{map,mixed[0].endpoint,multiBoard,machine,0,lf,"SPIAMP"},{secondPort,mixed[1].endpoint,multiBoard,machine,1,rf,"I2CAMP"}};
 AmplifierManager runtime;
 assert(runtime.bind(multiRegistry,contexts,1)==kIOReturnNotPermitted&&!heapCreated);
 AmplifierDriverContext duplicateContexts[]={contexts[0],contexts[0]};assert(runtime.bind(multiRegistry,duplicateContexts,2)==kIOReturnBadArgument&&!heapCreated);
 assert(!strcmp(runtime.bindFailure().reason,"DUPLICATE_BINDING_ENDPOINT_OR_PORT")&&runtime.bindFailure().index==1);
 AmplifierDriverContext sharedPorts[]={{map,mixed[0].endpoint,multiBoard,machine,0,lf,"SPIAMP"},{map,mixed[1].endpoint,multiBoard,machine,1,rf,"I2CAMP"}};
 assert(runtime.bind(multiRegistry,sharedPorts,2)==kIOReturnBadArgument&&!heapCreated);
 contexts[1].actualDeviceHid="WRONG";assert(runtime.bind(multiRegistry,contexts,2)==kIOReturnNotPermitted);
 assert(heapCreated==heapDestroyed&&!heapCleanup&&!runtime.bound());contexts[1].actualDeviceHid="I2CAMP";
 auto bindTemporary=[&](){AmplifierDriverContext local[]={contexts[0],contexts[1]};return runtime.bind(multiRegistry,local,2);};
 assert(bindTemporary()==0&&runtime.count()==2&&!strcmp(runtime.bindFailure().reason,"NONE"));
 assert(runtime.registration(0)==&spiAmp&&runtime.registration(1)==&i2cAmp);
 assert(runtime.startPlayback()==kIOReturnNotReady);
 assert(runtime.bringUp()==0&&runtime.startPlayback()==0&&runtime.suspend()==0&&runtime.resume()==0);
 assert(runtime.stage()==AmplifierStage::Playing&&runtime.unbind()==0&&heapCreated==heapDestroyed);
 assert(runtime.releasedStatus(0)->stage==AmplifierStage::Detached);
 // Synchronous future drivers stop when the shared audio route disappears.
 assert(runtime.bind(multiRegistry,contexts,2)==0&&runtime.bringUp()==0&&runtime.startPlayback()==0);
 PlaybackContext silent{};
 assert(runtime.runtimeTick(0,silent)==0);runtime.refreshPlaybackState();
 assert(runtime.status(0)->stage==AmplifierStage::Ready&&runtime.stage()==AmplifierStage::Playing);
 assert(runtime.runtimeTick(1,silent)==0);runtime.refreshPlaybackState();
 assert(runtime.stage()==AmplifierStage::Ready&&runtime.unbind()==0);
 // Health errors retain their original cause, shut down peers and block retries.
 assert(runtime.bind(multiRegistry,contexts,2)==0&&runtime.bringUp()==0&&runtime.startPlayback()==0);
 failTick=0;assert(runtime.runtimeTick(0,silent)==-300&&runtime.stage()==AmplifierStage::Faulted);
 assert(runtime.status(0)->firstFailure==-300&&runtime.status(0)->cleanupResult==0);
 assert(runtime.status(1)->stage==AmplifierStage::Detached);
 unsigned afterFaultCleanup=heapCleanup;
 assert(runtime.runtimeTick(1,silent)==kIOReturnNotReady&&heapCleanup==afterFaultCleanup);
 assert(runtime.unbind()==kIOReturnNotReady&&heapCleanup==afterFaultCleanup);failTick=-1;
 // Shipping service waits for a fresh host route after wake.
 AmplifierManager freshRoute(false);
 assert(freshRoute.bind(multiRegistry,contexts,2)==0&&freshRoute.bringUp()==0&&freshRoute.startPlayback()==0);
 assert(freshRoute.suspend()==0&&freshRoute.resume()==0&&freshRoute.stage()==AmplifierStage::Ready);
 assert(freshRoute.unbind()==0);
 // Deferred plans need an explicit driver contract, never a silent no-DSP fallback.
 lf.format=cirrus::firmware::FirmwareFormat::Deferred;
 unsigned beforeDeferred=heapCreated;
 assert(runtime.bind(multiRegistry,contexts,2)==kIOReturnNotPermitted&&heapCreated==beforeDeferred);
 spiAmp.supportsDeferredFirmware=true;
 assert(runtime.bind(multiRegistry,contexts,2)==0&&runtime.bringUp()==0&&runtime.unbind()==0);
 spiAmp.supportsDeferredFirmware=false;lf.format=cirrus::firmware::FirmwareFormat::None;
 failFactory=1;unsigned cleanupsBefore=heapCleanup;
 assert(runtime.bind(multiRegistry,contexts,2)==kIOReturnNotPermitted&&!runtime.bound());
 assert(heapCreated==heapDestroyed&&heapCleanup==cleanupsBefore);failFactory=-1;
 failFirmware=1;assert(runtime.bind(multiRegistry,contexts,2)==0&&runtime.bringUp()==-104);
 assert(runtime.stage()==AmplifierStage::Faulted&&runtime.unbind()==kIOReturnNotReady);
 assert(runtime.releasedStatus(1)->firstFailure==-104&&heapCreated==heapDestroyed);failFirmware=-1;
 waitFirmware=true;assert(runtime.bind(multiRegistry,contexts,2)==0&&runtime.bringUp()==kIOReturnNotReady);
 assert(runtime.stage()==AmplifierStage::Waiting&&runtime.startPlayback()==kIOReturnNotReady);
 const auto waitingCreated=heapCreated,waitingCleanups=heapCleanup;
 assert(runtime.retryBringUp(1)==kIOReturnNotReady && heapCreated==waitingCreated && heapCleanup==waitingCleanups);
 resourceReady=true;assert(runtime.retryBringUp(2)==0 && runtime.stage()==AmplifierStage::Ready);
 assert(heapCreated==waitingCreated && resourcePolls==2);resourceReady=false;
 assert(runtime.unbind()==0&&heapCreated==heapDestroyed);waitFirmware=false;
 waitFirmware=true;assert(runtime.bind(multiRegistry,contexts,2)==0 && runtime.bringUp()==kIOReturnNotReady);
 retryError=-333;assert(runtime.retryBringUp(3)==-333 && runtime.stage()==AmplifierStage::Faulted);
 assert(runtime.status(1)->firstFailure==-333 && runtime.status(0)->stage==AmplifierStage::Detached);
 const auto failedCleanup=heapCleanup;assert(runtime.retryBringUp(4)==kIOReturnNotReady && heapCleanup==failedCleanup);
 assert(runtime.unbind()==kIOReturnNotReady && heapCreated==heapDestroyed);retryError=0;waitFirmware=false;

 {
  AmplifierManager automatic;assert(automatic.bind(multiRegistry,contexts,2)==0&&automatic.bringUp()==0);
 }
 assert(heapCreated==heapDestroyed);
 // A mixed board need not invent one shared amplifier HID as machine identity.
 multiBoard.identity.acpiHid=nullptr;contexts[0].actualBoard.acpiHid=contexts[1].actualBoard.acpiHid=nullptr;
 assert(runtime.bind(multiRegistry,contexts,2)==0&&runtime.bringUp()==0&&runtime.unbind()==0);
 assert(heapCreated==heapDestroyed);
 Hooks broken;AmplifierLifecycle fault(broken);assert(fault.bringUp()==0);broken.cleanupFailure=-200;
 assert(fault.suspend()==-200&&broken.cleanups==1&&fault.status().cleanupResult==-200);
}
''')
    for flags in ([],['-fsanitize=address,undefined','-fno-omit-frame-pointer']):
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*flags,'-I'+str(p),'-I'+str(ROOT/'CirrusAudioFixup'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)
print('PASS configurable register formats, policy, lock/RMW, endpoint/firmware identity and lifecycle rollback')
