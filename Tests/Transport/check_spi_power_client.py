"""Run the production PM evidence helper with a deterministic callback-drain seam.

The seam models PMstop's SDK drain contract; it does not validate Apple's PM
scheduler or prove that a target PCI domain will grant power.
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
import shutil
import subprocess
import tempfile

from host_build import sanitizer_flags

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'CirrusAudioFixup/Transport/SPI/SPIPowerClient.cpp'
with tempfile.TemporaryDirectory(prefix='cirrus-pm-client-') as directory:
    tmp = Path(directory)
    for folder in ['IOKit', 'kern', 'mach']:
        (tmp / folder).mkdir()
    (tmp / 'IOKit/IOLib.h').write_text(r'''
#pragma once
#include <cassert>
#include <cstdint>
#include <mutex>
inline bool failLock=false;
inline unsigned locks=0;
using IOLock=std::mutex;
inline IOLock* IOLockAlloc(){if(failLock)return nullptr;++locks;return new IOLock;}
inline void IOLockFree(IOLock* p){--locks;delete p;}
inline void IOLockLock(IOLock* p){p->lock();}
inline void IOLockUnlock(IOLock* p){p->unlock();}
''')
    (tmp / 'IOKit/IOService.h').write_text(r'''
#pragma once
#include "IOLib.h"
#include <functional>
#include <condition_variable>
#include <thread>
using IOReturn=int;
constexpr IOReturn kIOReturnSuccess=0,IOPMAckImplied=0;
class OSDictionary {};
#define OSDeclareDefaultStructors(T) public: T()=default; ~T() override=default;
#define OSDefineMetaClassAndStructors(T,B)
inline bool failInit=false;
inline unsigned live=0,stops=0,baseCompletions=0;
class IOService {
public:
 unsigned refs=1; uint64_t id=99; IOService* provider=nullptr;
 std::thread worker; std::mutex gate; std::condition_variable cv;
 bool inCompletion=false,drain=false,stopped=false;
 std::function<void()> drainCheck;
 IOService(){++live;}
 virtual ~IOService(){assert(!worker.joinable());--live;}
 virtual bool init(OSDictionary* =nullptr){return !failInit;}
 virtual void free(){}
 void retain(){++refs;}
 void release(){if(!--refs){free();delete this;}}
 uint64_t getRegistryEntryID(){return id;}
 bool attach(IOService* p){provider=p;p->retain();return true;}
 void detach(IOService* p){assert(provider==p);provider=nullptr;p->release();}
 virtual IOReturn setPowerState(unsigned long,IOService*){return 0;}
 virtual void powerChangeDone(unsigned long){
  ++baseCompletions;
  std::unique_lock<std::mutex> l(gate);inCompletion=true;cv.notify_all();
  cv.wait(l,[&]{return drain;});
 }
 void launch(){worker=std::thread([this]{
  assert(setPowerState(1,this)==IOPMAckImplied);powerChangeDone(0);
 });}
 void waitForCompletion(){std::unique_lock<std::mutex> l(gate);
  cv.wait(l,[&]{return inCompletion;});}
 void PMstop(){
  ++stops;
  // Owner cancellation must already be visible and its evidence lock released.
  if(drainCheck)drainCheck();
  {std::lock_guard<std::mutex> l(gate);drain=true;cv.notify_all();}
  if(worker.joinable())worker.join();
  stopped=true;
 }
};
''')
    (tmp / 'kern/clock.h').write_text(r'''
#pragma once
#include <cstdint>
inline void absolutetime_to_nanoseconds(uint64_t t,uint64_t* ns){*ns=t;}
''')
    (tmp / 'mach/mach_time.h').write_text(r'''
#pragma once
#include <atomic>
#include <cstdint>
inline std::atomic<uint64_t> nowNs{1000000};
inline uint64_t mach_absolute_time(){return nowNs.fetch_add(1000);}
''')
    (tmp / 'kern/thread.h').write_text(r'''
#pragma once
#include <atomic>
#include <cstdint>
#include <thread>
using thread_t=uint64_t;
inline thread_t current_thread(){static std::atomic<uint64_t> next{1};
 thread_local uint64_t id=next.fetch_add(1);return id;}
inline uint64_t thread_tid(thread_t t){return t;}
''')
    (tmp / 'test.cpp').write_text(r'''
#include "Transport/SPI/SPIPowerClient.hpp"
#include <cstring>
int main(){
 auto* parent=new IOService;parent->id=123;
 auto* client=new CirrusSPIPowerClient;assert(client->init());
 assert(client->attach(parent));client->identifyParent(parent);
 using M=CirrusSPIPowerClient::Milestone;
 for(auto m:{M::Initialized,M::Registered,M::Joined,M::Vote})
  client->recordMilestone(m,0,m==M::Vote?1:0);
 // bounded invalid phase
 client->recordMilestone(M::Count,-1);
 auto before=client->snapshot();assert(before.parentId==123);
 assert(!before.cancelled);
 assert(before.setState.count==0&&before.completed.count==0);
 for(auto& e:before.milestones)assert(e.count==1&&e.uptimeUs&&e.threadId);
 const auto ownerThread=before.milestones[0].threadId;
 // Invoke actual production overrides through IOService virtual dispatch.
 client->launch();client->waitForCompletion();
 auto observed=client->snapshot();
 assert(observed.setState.count==1&&observed.setState.ordinal==1);
 assert(observed.setState.deviceId==client->id);
 assert(observed.setState.threadId!=ownerThread);
 assert(observed.completed.count==1&&observed.completed.ordinal==0);
 assert(observed.completed.threadId==observed.setState.threadId);
 assert(observed.completed.uptimeUs>=observed.setState.uptimeUs);
 assert(baseCompletions==1);
 // Owner initiates cancellation while completion remains in flight. PMstop
 // must see cancellation and join that callback before detach/release.
 bool ownerAlive=true;
 client->drainCheck=[&]{assert(ownerAlive);assert(client->snapshot().cancelled);};
 client->cancelAndStop(true);assert(client->stopped&&!client->worker.joinable());
 auto cancelled=client->snapshot();assert(cancelled.cancelledUs);
 client->recordMilestone(M::Vote,-1,0);client->identifyParent(nullptr);
 // defensive suppression after cancellation
 client->setPowerState(0,client);
 auto after=client->snapshot();
 assert(after.parentId==123&&after.setState.count==observed.setState.count);
 assert(after.milestones[3].count==1&&after.milestones[3].ordinal==1);
 client->cancelAndStop(true);assert(stops==1);
 // no callback/owner retain cycle
 assert(client->refs==1&&parent->refs==2);
 client->drainCheck={};ownerAlive=false;
 client->detach(parent);client->release();assert(parent->refs==1&&live==1);
 parent->release();assert(live==0&&locks==0);
 // Cleanup also works before PMinit, and after allocation/init failures.
 client=new CirrusSPIPowerClient;assert(client->init());
 client->cancelAndStop(false);client->release();assert(stops==1);
 failLock=true;client=new CirrusSPIPowerClient;assert(!client->init());
 client->release();failLock=false;
 failInit=true;client=new CirrusSPIPowerClient;assert(!client->init());
 client->release();assert(live==0&&locks==0);
}
''')
    for flags in sanitizer_flags():
        exe = tmp / 'pm-client-test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-DKERNEL', *flags, '-I', str(tmp),
                        '-I', str(ROOT / 'CirrusAudioFixup'), str(tmp / 'test.cpp'),
                        str(SOURCE), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)

    clang = shutil.which('clang++')
    if clang:
        common = [clang, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsyntax-only']
        subprocess.run([*common, str(SOURCE)], check=True)
        sdk = ROOT / 'MacKernelSDK/Headers'
        if sdk.is_dir():
            subprocess.run([*common, '-target', 'x86_64-apple-darwin',
                            '-DKERNEL', '-DKERNEL_PRIVATE', '-DAPPLE', '-D__APPLE__',
                            '-nostdinc', '-isystem', str(sdk), '-isystem', str(sdk / 'i386'),
                            str(SOURCE)], check=True)
        else:
            print('SKIP target syntax: MacKernelSDK headers unavailable')
    else:
        print('SKIP syntax checks: clang++ unavailable')
print('PASS production SPI PM helper: callbacks, bounded evidence, cancellation drain, ownership and failure cleanup')
