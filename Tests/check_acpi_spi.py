"""Exercise bounded production _CRS SPI decoding and malformed firmware data."""
from pathlib import Path
import tempfile, subprocess
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cirrus-acpi-spi-') as directory:
    p=Path(directory); (p/'IOKit').mkdir()
    (p/'IOKit/IOLib.h').write_text('#pragma once\nusing IOReturn=int; constexpr int kIOReturnBadArgument=-1,kIOReturnSuccess=0;\n')
    (p/'test.cpp').write_text(r'''
#include <cassert>
#include <vector>
#include <cstring>
#include "Transport/ACPISPIResources.hpp"
using namespace cirrus::transport;
std::vector<uint8_t> resource(unsigned cs=1,unsigned mode=0) {
    std::vector<uint8_t> d={0x8e,0,0,1,0,2,2,0,0,1,9,0,0x40,0x42,0x0f,0,8,
                           uint8_t(mode&1),uint8_t(mode>>1),uint8_t(cs),0};
    const char path[]="\\_SB.PC00.SPI1";
    d.insert(d.end(),path,path+sizeof(path));
    d[1]=uint8_t(d.size()-3); return d;
}
void end(std::vector<uint8_t>& d) { d.push_back(0x79); d.push_back(0); }
int main() {
    ACPISPIResources out{};
    for(unsigned mode=0;mode<4;++mode) {
        auto d=resource(2,mode); end(d);
        assert(parseACPISPIResources(d.data(),d.size(),out)==ACPISPIResult::Success);
        assert(out.count==1 && out.endpoints[0].endpoint.chipSelect==2);
        assert(out.endpoints[0].endpoint.clockHz==1000000 && out.endpoints[0].endpoint.mode==mode);
        assert(!strcmp(out.endpoints[0].source,"\\_SB.PC00.SPI1"));
        for(size_t i=0;i<d.size();++i)
            assert(parseACPISPIResources(d.data(),i,out)==ACPISPIResult::Malformed);
    }
    auto d=resource(); auto second=resource(0); d.insert(d.end(),second.begin(),second.end()); end(d);
    assert(parseACPISPIResources(d.data(),d.size(),out)==ACPISPIResult::Success && out.count==2);
    auto previous=out;
    for(auto index : {3,6,7,9,16}) {
        auto bad=resource(); bad[index]=0xff; end(bad);
        assert(parseACPISPIResources(bad.data(),bad.size(),out)==ACPISPIResult::Unsupported);
        assert(!memcmp(&previous,&out,sizeof(out)));
    }
    for(auto index : {10,17,18,12,13,14,15}) {
        auto bad=resource();
        if(index>=12 && index<=15) for(int j=12;j<=15;++j) bad[j]=0;
        else bad[index]=0xff;
        end(bad);
        assert(parseACPISPIResources(bad.data(),bad.size(),out)==ACPISPIResult::Malformed);
    }
    auto v2=resource(); v2[3]=2; v2[6]=6; end(v2);
    assert(parseACPISPIResources(v2.data(),v2.size(),out)==ACPISPIResult::Success && out.endpoints[0].shared);
    auto bad=resource(); bad.back()='X'; end(bad);
    assert(parseACPISPIResources(bad.data(),bad.size(),out)==ACPISPIResult::Malformed);
    bad=resource(); bad[22]=0; end(bad);
    assert(parseACPISPIResources(bad.data(),bad.size(),out)==ACPISPIResult::Malformed);
    bad=resource(); end(bad); bad.push_back(0);
    assert(parseACPISPIResources(bad.data(),bad.size(),out)==ACPISPIResult::Malformed);
    std::vector<uint8_t> empty={0x79,0};
    assert(parseACPISPIResources(empty.data(),empty.size(),out)==ACPISPIResult::NoSPI && out.count==0);
    bad.clear(); for(size_t i=0;i<=ACPISPIResources::kMaxEndpoints;++i) { auto r=resource(i); bad.insert(bad.end(),r.begin(),r.end()); } end(bad);
    assert(parseACPISPIResources(bad.data(),bad.size(),out)==ACPISPIResult::TooManyEndpoints);
    bad.clear();for(size_t i=0;i<ACPISPIResources::kMaxEndpoints;++i){auto r=resource(i);bad.insert(bad.end(),r.begin(),r.end());}end(bad);
    assert(parseACPISPIResources(bad.data(),bad.size(),out)==ACPISPIResult::Success && out.count==ACPISPIResources::kMaxEndpoints);
    d=resource(); end(d); uint8_t sum=0; for(auto b:d) sum=uint8_t(sum+b); d.back()=uint8_t(0-sum);
    assert(parseACPISPIResources(d.data(),d.size(),out)==ACPISPIResult::Success);
    d.back()^=0x80;
    assert(parseACPISPIResources(d.data(),d.size(),out)==ACPISPIResult::Malformed);
    // Sweep arbitrary buffers for bounds/undefined behavior under sanitizers.
    uint32_t rng=1;
    for(size_t n=1;n<512;++n) { std::vector<uint8_t> random(n);
        for(auto& b:random) { rng=rng*1664525+1013904223; b=uint8_t(rng>>24); }
        parseACPISPIResources(random.data(),random.size(),out);
    }
}
''')
    for flags in ([],['-fsanitize=address,undefined','-fno-omit-frame-pointer']):
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*flags,'-I'+str(p),'-I'+str(ROOT/'CirrusAudioFixup'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)
print('PASS ACPI SPI resource decoding and bounded failures')
