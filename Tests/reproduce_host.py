from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CODEC = ROOT / 'CirrusAudioFixup/Firmware/WMFW'
DEVICE = ROOT / 'CirrusAudioFixup/Devices/CS35L41/Resources'
with tempfile.TemporaryDirectory(prefix='cirrus-host-check-') as directory:
    tmp = Path(directory)
    (tmp / 'IOKit').mkdir()
    (tmp / 'IOKit/IOLib.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cassert>
using UInt8 = uint8_t;
using IOReturn = int;
using std::min;
constexpr int kIOReturnError = -1;
#define CIRRUS_LOG(...) ((void)0)
#define CIRRUS_ERR(...) ((void)0)
#define OSSwapLittleToHostInt32(x) (x)
inline size_t strlcpy(char* d,const char* s,size_t n) {
    size_t l=strlen(s); if(n) { size_t c=l<n-1?l:n-1; memcpy(d,s,c); d[c]=0; } return l;
}
inline void* IOMalloc(size_t n) { return malloc(n); }
inline void* IOMallocData(size_t n) { return malloc(n); }
inline void IOFree(void* p,size_t) { free(p); }
inline void IOFreeData(void* p,size_t) { free(p); }
inline void IOSleep(unsigned) {}
inline uint64_t mach_absolute_time() { static uint64_t t=0; return ++t; }
inline void absolutetime_to_nanoseconds(uint64_t t,uint64_t* n) { *n=t; }
struct OSObject {};
struct OSNumber : OSObject { unsigned unsigned32BitValue() { return 0; } };
constexpr int TRACE_OTHER = 0;
''', encoding='utf-8')
    uploader = (CODEC / 'FirmwareUploader.hpp').read_text(encoding='utf-8')
    uploader = uploader.replace('#include "Core/RegisterIO.hpp"', '#include "fake.hpp"').replace('#include "Firmware/WMFW/WMFWParser.hpp"', '#include "WMFWParser.hpp"')
    (tmp / 'uploader.hpp').write_text(uploader, encoding='utf-8')
    resources = (DEVICE / 'Firmware.hpp').read_text(encoding='utf-8')
    arrays = '\n'.join(re.findall(r'const uint8_t \w+\[\s*\d*\s*\] = \{.*?\};', resources, re.S))
    (tmp / 'arrays.hpp').write_text(arrays, encoding='utf-8')
    header = (ROOT / 'CirrusAudioFixup/CirrusAudioFixup.hpp').read_text(encoding='utf-8')
    resource_type = header[header.index('struct FirmwareResource {'):header.index('class CirrusAudioFixup :')]
    table = resources[resources.index('const FirmwareResource cs35l41Firmware[]'):]
    (tmp / 'resources.hpp').write_text(resource_type + table, encoding='utf-8')
    (tmp / 'fake.hpp').write_text(r'''
#pragma once
#include <IOKit/IOLib.h>
#include <map>
#include "Core/RegisterIO.hpp"
struct CS35L41Amp { const char* name="host"; };
class CirrusAudioFixup : public cirrus::core::RegisterIO {
public:
    std::map<uint32_t,uint8_t> memory;
    unsigned reads=0,writes=0,failRead=0,failWrite=0,failWriteEnd=0,corruptRead=0;
    OSObject* getProperty(const char*) { return nullptr; }
    
    bool read(uint32_t, uint32_t*) override { return false; }
    bool write(uint32_t, uint32_t) override { return false; }
    bool updateBits(uint32_t, uint32_t, uint32_t) override { return false; }
    bool pollBit(uint32_t, uint32_t, uint32_t, uint32_t) override { return false; }

    bool bulkRead(uint32_t r,uint8_t* p,size_t n) override {
        assert(n && n<=252 && !(n&3) && !(r&3));
        if (++reads==failRead) return false;
        for(unsigned i=0;i<n;i++) p[i]=memory[r+i];
        if(reads==corruptRead) p[0]^=1;
        return true;
    }
    bool bulkWrite(uint32_t r,const uint8_t* p,size_t n) override {
        assert(n && n<=252 && !(n&3) && !(r&3));
        ++writes;
        if(writes>=failWrite && writes<=failWriteEnd) {
            memory[r]=p[0];
            return false;
        }
        for(unsigned i=0;i<n;i++) memory[r+i]=p[i];
        return true;
    }
};
''', encoding='utf-8')
    command = ['g++', '-std=c++17', '-O0', '-g', '-I'+str(tmp), '-I'+str(CODEC), '-I'+str(ROOT/'CirrusAudioFixup'),
               str(Path(__file__).with_name('host_regression.cpp')), '-o', str(tmp/'check.exe')]
    subprocess.run(command, check=True)
    subprocess.run([str(tmp/'check.exe')], check=True)
