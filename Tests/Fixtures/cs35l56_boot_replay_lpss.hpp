#pragma once
#include "ReplayOS.hpp"
#include "Transport/SPI/Intel/IntelSPIChipSelects.hpp"
namespace cirrus::transport::intel {
// Hardware boundary: D0 arrival and SPI replies are scripted, not real LPSS MMIO.
class IntelLPSSSPIController final:public SPIController {
 IntelSPIChipSelects selects;
 bool pending=false,ready=false;
public:
 IntelLPSSSPIController(){++liveControllers;}
 ~IntelLPSSSPIController(){--liveControllers;}
 IOReturn bind(IOPCIDevice* pci,IOService*){assert(pci);return continueBind();}
 IOReturn continueBind(){pending=nowNs<powerAtNs;return pending?kIOReturnNotReady:kIOReturnSuccess;}
 bool powerPending()const{return pending;}
 const char* bindFailureReason()const{return pending?"WAITING_POWER":"READY";}
 uint32_t initialCapabilities()const{return 0xF003;}uint32_t initialStatus()const{return 0xF004;}
 uint32_t initialClock()const{return 0;}uint32_t preparedClock()const{return 0;}uint32_t initialClockGate()const{return 0;}
 IOReturn configureChipSelects(IOACPIPlatformDevice* target,IOService* owner){
  ++csBinds;auto result=selects.bind(target,owner);ready=result==0;return result;
 }
 IOReturn prepareGPIOGroup(IOACPIPlatformDevice* source,const char* property){return selects.prepareGPIOGroup(source,property);}
 IOReturn writeGPIOGroup(bool value){++writes;return selects.writeGPIOGroup(value);}
 IOReturn readGPIO(IOACPIPlatformDevice* source,const char* property,size_t index,uint32_t& value){return selects.readGPIO(source,property,index,value);}
 void suspend(){ready=false;}IOReturn resume(){return kIOReturnSuccess;}
 size_t maxTransferBytes()const override{return 262;}
 IOReturn exchange(const SPIEndpoint& endpoint,const uint8_t* tx,uint8_t* rx,size_t bytes)override{
  assert(ready&&!pending&&!stopped&&endpoint.chipSelect<2&&endpoint.mode==0&&endpoint.clockHz==4000000);
  assert(nowNs>=pciAtNs&&nowNs>=powerAtNs&&callbackDone);
  assert(bytes==10&&(tx[0]&0x80)&&!tx[4]&&!tx[5]);
  const uint32_t reg=uint32_t(tx[0]&0x7f)<<24|uint32_t(tx[1])<<16|uint32_t(tx[2])<<8|tx[3];
  uint32_t value=0;
  if(reg==0){++idReads;if(nowNs<bootReadyAtNs)++earlyIdReads;value=nowNs>=bootReadyAtNs?0x35a56:0;}
  else if(reg==4)value=0xb0;
  else {assert(reg==0x28021e0);value=nowNs>=bootReadyAtNs?2:0;}
  if(!rx){++writes;return kIOReturnNotPermitted;}
  auto* cs=selects.select(endpoint.chipSelect);if(cs)assert(cs->setActive(true)==0);
  memset(rx,0,bytes);for(unsigned i=0;i<4;++i)rx[6+i]=uint8_t(value>>(24-8*i));++frames;
  if(cs)assert(cs->setActive(false)==0);
  return kIOReturnSuccess;
 }
};
}
