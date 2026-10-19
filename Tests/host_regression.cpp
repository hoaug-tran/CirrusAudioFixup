#include "Firmware/WMFW/WMFWFormat.hpp"
#include "Firmware/WMFW/WMFWUploadPlan.hpp"
#include "uploader.hpp"
#include "arrays.hpp"
#include "resources.hpp"
#include "Devices/Amplifiers/CS35L41/Resources/FirmwareSelection.hpp"
#include <vector>

void putBE(std::vector<uint8_t>& v, unsigned word, uint32_t n) {
    for (unsigned i=0;i<4;i++) v[word*4+i]=uint8_t(n>>(24-i*8));
}
void putLE(std::vector<uint8_t>& v, unsigned pos, uint32_t n) {
    for (unsigned i=0;i<4;i++) v[pos+i]=uint8_t(n>>(i*8));
}
int main() {
    using cirrus::devices::amplifiers::cs35l41::selectFirmwareResource;
    {
        using cirrus::devices::amplifiers::cs35l41::selectChannelCoefficients;
        uint8_t left[4]{}, right[4]{};FirmwareResource resource{};
        resource.bin=left;resource.binSize=sizeof(left);size_t bytes=7;
        assert(selectChannelCoefficients(resource,false,bytes)==left&&bytes==4);
        assert(!selectChannelCoefficients(resource,true,bytes)&&bytes==0);
        resource.binRight=right;resource.binRightSize=3;
        assert(!selectChannelCoefficients(resource,true,bytes)&&bytes==0);
        resource.binRightSize=4;
        assert(selectChannelCoefficients(resource,true,bytes)==right&&bytes==4);
        resource.isDummy=true;
        assert(!selectChannelCoefficients(resource,false,bytes)&&bytes==0);
    }
    {
        // Physical speaker 0 is a real identity, never an implicit wildcard.
        FirmwareResource physicalZero{};
        physicalZero.subsystemVendor=0x1234;physicalZero.subsystemDevice=0x5678;
        assert(selectFirmwareResource(&physicalZero,1,0x1234,0x5678,0)==&physicalZero);
        assert(!selectFirmwareResource(&physicalZero,1,0x1234,0x5678,1));
        assert(!selectFirmwareResource(&physicalZero,1,0x1234,0x5678,UINT32_MAX));
    }
    {
        FirmwareResource catalog[3]{};
        catalog[0].subsystemVendor=catalog[1].subsystemVendor=catalog[2].subsystemVendor=0x1234;
        catalog[0].subsystemDevice=catalog[1].subsystemDevice=catalog[2].subsystemDevice=0x5678;
        catalog[1].spkid=1;catalog[2].spkid=2;
        catalog[0].genericSpeaker=true;
        assert(selectFirmwareResource(catalog,3,0x1234,0x5678,1)==&catalog[1]);
        assert(selectFirmwareResource(catalog,3,0x1234,0x5678,7)==&catalog[0]);
        assert(!selectFirmwareResource(catalog,3,0x17aa,0x5678,1));
        std::swap(catalog[0],catalog[2]);
        assert(selectFirmwareResource(catalog,3,0x1234,0x5678,1)==&catalog[1]);
        catalog[0].spkid=1;assert(!selectFirmwareResource(catalog,3,0x1234,0x5678,1));
    }
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
    assert(hp && hp->genericSpeaker && hp->bin != hp->binRight && hp->wmfwSize == 34068);
    assert(hp->binSize == 4000 && hp->binRightSize == 4000);
    {
        FirmwareImage captured{};
        assert(WMFWParser::parseWMFW(hp->wmfw,hp->wmfwSize,&captured));
        // Actual unpacked XM table in the supplied HP Linux regmap. It has two
        // algorithms and a different firmware XM base from the Lenovo fixture.
        const uint32_t words[]={0x40000,0x30000,2,0x400a4,0x2b01,0x1f4,0xe,0,0,2,
                                0xcd,0x1d3f01,0x94,0x50,0xe,0x7bc,0xf20b,0x100,0x176,0x4c,0,0};
        std::vector<uint8_t> actual(sizeof(words));
        for(unsigned i=0;i<sizeof(words)/sizeof(words[0]);++i)putBE(actual,i,words[i]);
        assert(WMFWParser::parseAlgorithmTable(actual.data(),actual.size(),captured));
        assert(captured.firmwareId==0x400a4&&captured.haloFirmwareVersion==0x2b01&&captured.algorithmTotal==2);
        for(auto name:{"HALO_STATE","HALO_HEARTBEAT","CAL_AMBIENT","CAL_R","CAL_STATUS","CAL_CHECKSUM"}) {
            WMFWControlRef ref{};uint32_t reg=0;
            assert(WMFWParser::findControl(&captured,name,ref));
            assert(ref.control->type==WMFW_ADSP2_XM&&ref.control->len==4);
            assert(WMFWParser::resolveControl(captured,ref,reg));
            printf("PASS captured HP %s=0x%08X\n",name,reg);
        }
        for(unsigned side=0;side<2;++side){
            assert(WMFWParser::parseBIN(side?hp->binRight:hp->bin,side?hp->binRightSize:hp->binSize,&captured));
            MappedImage mapped{};assert(WMFWMemoryMapper::mapCoefficients(captured,mapped));
            assert(mapped.regionCount>0);
        }
    }
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
