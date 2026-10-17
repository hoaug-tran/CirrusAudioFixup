#include "Firmware/WMFW/WMFWFormat.hpp"
#include "Firmware/WMFW/WMFWUploadPlan.hpp"
#include "uploader.hpp"
#include "arrays.hpp"
#include "resources.hpp"
#include <vector>

void putBE(std::vector<uint8_t>& v, unsigned word, uint32_t n) {
    for (unsigned i=0;i<4;i++) v[word*4+i]=uint8_t(n>>(24-i*8));
}
void putLE(std::vector<uint8_t>& v, unsigned pos, uint32_t n) {
    for (unsigned i=0;i<4;i++) v[pos+i]=uint8_t(n>>(i*8));
}
int main() {
    const FirmwareResource* lenovo = nullptr;
    const FirmwareResource* hp = nullptr;
    for (size_t i = 0; i < cs35l41FirmwareCount; ++i) {
        const auto& resource = cs35l41Firmware[i];
        if (resource.subsystemVendor == 0x17AA && resource.subsystemDevice == 0x3847)
            lenovo = &resource;
        if (resource.subsystemVendor == 0x103C && resource.subsystemDevice == 0x89C6)
            hp = &resource;
    }
    assert(lenovo && lenovo->spkid == 1 && lenovo->bin != lenovo->binRight);
    assert(hp && hp->spkid == 0 && hp->bin != hp->binRight && hp->wmfwSize == 34068);
    assert(hp->binSize == 4000 && hp->binRightSize == 4000);
    FirmwareImage fw{};
    auto wmfw=cs35l41_dsp1_spk_prot_17aa3847_wmfw;
    auto wmfwSize=sizeof(cs35l41_dsp1_spk_prot_17aa3847_wmfw);
    assert(WMFWParser::parseWMFW(wmfw,wmfwSize,&fw));
    assert(fw.fw_id==0);
    std::vector<uint8_t> table(64);
    putBE(table,3,0x400A4); putBE(table,4,0x1500);
    putBE(table,5,0xE6); putBE(table,7,0x20); putBE(table,9,1);
    putBE(table,10,0xCD); putBE(table,11,0x1500);
    putBE(table,12,0x94); putBE(table,14,0xE);
    assert(WMFWParser::parseAlgorithmTable(table.data(),table.size(),fw));
    for(auto name:{"HALO_STATE","HALO_HEARTBEAT"}) {
        WMFWControlRef ref{}; uint32_t reg=0;
        assert(WMFWParser::findControl(&fw,name,ref));
        assert(WMFWParser::resolveControl(fw,ref,reg));
        assert(reg==(strcmp(name,"HALO_STATE")==0?0x02800398U:0x0280039CU));
        printf("PASS %s=0x%08X\n",name,reg);
    }
    for(auto name:{"CAL_AMBIENT","CAL_R","CAL_STATUS","CAL_CHECKSUM"}) {
        WMFWControlRef ref{}; uint32_t reg=0;
        assert(WMFWParser::findControl(&fw,name,ref));
        assert(ref.algorithm->id==0xCD && ref.control->type==WMFW_ADSP2_XM && ref.control->len==4);
        assert(WMFWParser::resolveControl(fw,ref,reg));
        printf("PASS calibration metadata %s=0x%08X\n",name,reg);
    }
    for(unsigned side=0;side<2;side++) {
        auto bin=side?cs35l41_dsp1_spk_prot_17aa3847_spkid1_r0_bin:cs35l41_dsp1_spk_prot_17aa3847_spkid1_l0_bin;
        assert(WMFWParser::parseBIN(bin,4984,&fw));
        MappedImage mapped{};
        assert(WMFWMemoryMapper::mapCoefficients(fw,mapped));
        assert(mapped.regionCount==2);
        for(unsigned i=0;i<2;i++) {
            UploadPlan p{};
            assert(WMFWUploadPlanner::generatePlan(i,mapped.regions[i],{252,true,true},p));
            unsigned expected=i?0x034016B0:0x02C0015C;
            assert(p.transactions[0].dspRegister==expected);
            unsigned bytes=0;
            for(unsigned j=0;j<p.transactionCount;j++) {
                assert(p.transactions[j].dspRegister==expected+bytes);
                assert(p.transactions[j].payload==mapped.regions[i].data.begin+bytes);
                bytes+=p.transactions[j].size;
            }
            assert(bytes==mapped.regions[i].size);
            printf("PASS %s BIN region %u=0x%08X bytes=%u\n",side?"R":"L",i,expected,bytes);
        }
        for(size_t n=0;n<4984;n++) {
            if(WMFWParser::parseBIN(bin,n,&fw)) {
                assert(n>=16 && fw.coefficientCount>0);
            } else assert(fw.coefficientCount==0 && fw.total_coeff_payload_bytes==0);
        }
        assert(!WMFWParser::parseBIN(bin,200,&fw));
        assert(fw.coefficientCount==0);
        std::vector<uint8_t> bad(bin,bin+4984);
        bad[18]=0x12; bad[19]=0x01;
        assert(!WMFWParser::parseBIN(bad.data(),bad.size(),&fw));
    }
    std::vector<uint8_t> bad(wmfw,wmfw+wmfwSize);
    bad.push_back(0);
    assert(!WMFWParser::parseWMFW(bad.data(),bad.size(),&fw));
    assert(fw.regionCount==0 && fw.wmfwControlCount==0);
    bad.assign(wmfw,wmfw+wmfwSize);
    putLE(bad,44,0xFFFFFFFF);
    assert(!WMFWParser::parseWMFW(bad.data(),bad.size(),&fw));
    bad.assign(wmfw,wmfw+40);
    for(unsigned i=0;i<33;i++) {
        size_t p=bad.size(); bad.resize(p+8);
        putLE(bad,unsigned(p),0xFE000000);
    }
    assert(!WMFWParser::parseWMFW(bad.data(),bad.size(),&fw));
    assert(!WMFWParser::parseAlgorithmTable(table.data(),63,fw));
    assert(fw.algorithmCount==0);
    putBE(table,9,32);
    assert(!WMFWParser::parseAlgorithmTable(table.data(),table.size(),fw));
    std::vector<uint8_t> payload(504,0xAB);
    MappedRegion r{RegionType::XM_UNPACKED,0,0x02800000,504,{payload.data(),504}};
    UploadPlan plan{};
    assert(WMFWUploadPlanner::generatePlan(0,r,{252,true,true},plan));
    assert(plan.transactionCount==2);
    CS35L41Amp amp;
    for(unsigned mode=0;mode<6;mode++) {
        CirrusAudioFixup bus;
        for(unsigned i=0;i<504;i++) bus.memory[r.dspRegister+i]=0x55;
        if(mode==1) bus.failRead=1;
        if(mode==2) { bus.failWrite=2; bus.failWriteEnd=3; }
        if(mode==3) bus.failRead=4;
        if(mode==4) bus.corruptRead=4;
        if(mode==5) { bus.failWrite=2; bus.failWriteEnd=100; }
        bool success=WMFWUploader::upload(amp.name,bus,plan);
        assert(success==(mode==0));
        if(mode==1) assert(bus.writes==0);
    }
    MappedRegion r_pm{RegionType::PM_PACKED,0,0x03800000,504,{payload.data(),504}};
    UploadPlan plan_pm{};
    assert(WMFWUploadPlanner::generatePlan(0,r_pm,{252,true,true},plan_pm));
    CirrusAudioFixup bus_pm;
    assert(WMFWUploader::upload(amp.name,bus_pm,plan_pm));
    assert(bus_pm.reads==0 && bus_pm.writes==2);
    for(unsigned i=0;i<504;i++) assert(bus_pm.memory[r_pm.dspRegister+i]==0xAB);
    MappedImage mapped{}; UploadSession session{}; CirrusAudioFixup bus;
    mapped.regionCount=33;
    for(unsigned i=0;i<33;i++) mapped.regions[i]=r;
    assert(!WMFWUploadScheduler::run(amp.name,bus,mapped,session));
    assert(!session.complete && bus.reads==0 && bus.writes==0);
    mapped.regionCount=32;
    assert(WMFWUploadScheduler::run(amp.name,bus,mapped,session));
    assert(session.complete && session.passCount==32);
    r.dspRegister=0xFFFFFFFC;
    assert(!WMFWUploadPlanner::generatePlan(0,r,{252,true,true},plan));
    assert(plan.transactionCount==0);
    r.dspRegister=0x02800001;
    assert(!WMFWUploadPlanner::generatePlan(0,r,{252,true,true},plan));
    r.dspRegister=0x02800000;
    assert(!WMFWUploadPlanner::generatePlan(0,r,{0,true,true},plan));
    assert(!WMFWUploadPlanner::generatePlan(0,r,{256,true,true},plan));
    r.data.begin=nullptr;
    assert(!WMFWUploadPlanner::generatePlan(0,r,{252,true,true},plan));
    puts("PASS malformed parser, planner boundaries, 32/33-region preflight, backup/write/read/CRC/restore faults");
}
