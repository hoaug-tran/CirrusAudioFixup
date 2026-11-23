"""Verify the proactive CS35L41/CS35L56 hardware candidate catalog, onboarding tool, and read-only diagnostics."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Fixtures"))
from acpi_kernel_fixture import install
from host_build import host_compiler, sanitizer_flags
from production_source_fixture import method, service_source

ROOT = Path(__file__).resolve().parents[2]

PRODUCTION_SUBSYSTEMS = {
    0x10280B28,
    0x103C89C6,
    0x103C8A31,
    0x10431533,
    0x10431683,
    0x10431B13,
    0x10431C63,
    0x10431C9F,
    0x10431D1F,
    0x17AA382B,
    0x17AA3847,
    0x17AA3877,
    0x17AA38F9,
    0x17AA391E,
}


def verify_catalog_sync_and_json():
    subprocess.run(
        [sys.executable, str(ROOT / "Tools/Hardware/sync_hardware_catalog.py"), "--verify"],
        cwd=ROOT,
        check=True,
    )
    catalog_json = json.loads(
        (ROOT / "docs/hardware/hardware_catalog.json").read_text(encoding="utf-8")
    )
    summary = catalog_json["summary"]
    entries = catalog_json["entries"]
    aliases = catalog_json["aliases"]
    assert summary["total"] == len(entries)
    assert (
        summary["level_3_production_supported"]
        + summary["level_2_technically_supportable"]
        + summary["level_1_recognized_candidate"]
        == summary["total"]
    )
    assert summary["cs35l41"] + summary["cs35l56"] == summary["total"]
    assert summary["aliases"] == len(aliases)

    seen = set()
    l3 = set()
    for entry in entries:
        ssid = int(entry["subsystem_hex"], 16)
        assert ssid not in seen, f"Duplicate catalog entry: {entry['subsystem']}"
        seen.add(ssid)
        assert entry["amp_count"] in (0, 1, 2, 3, 4)
        if entry["level"] >= 2:
            assert entry["amp_count"] == 2
        if entry["level"] == 2:
            assert entry["bus_hint"] == "I2C"
            assert entry["upstream_firmware_available"] is True
            assert len(entry.get("upstream_firmware_files", [])) >= 3
        assert entry["family"] in ("CS35L41", "CS35L56")
        if entry["level"] == 3:
            assert entry["missing_reason"] == "NONE"
            l3.add(ssid)
        else:
            assert entry["missing_reason"] != "NONE"
    assert l3 == PRODUCTION_SUBSYSTEMS

    alias_map = {a["from"]: a["to"] for a in aliases}
    assert alias_map == {"17AA:385C": "17AA:3877", "17AA:3D3B": "17AA:38F9"}


def verify_onboarding_tool():
    tool = str(ROOT / "Tools/Hardware/onboard_board.py")
    out = subprocess.check_output([sys.executable, tool, "--list-ready"], cwd=ROOT, text=True)
    assert "Technically supportable (Level 2) boards: 48" in out
    assert "103C:8A28" in out and "17AA:390B" in out

    insp = subprocess.check_output(
        [sys.executable, tool, "--inspect", "17AA:3D3B"], cwd=ROOT, text=True
    )
    assert "Alias From: 17AA:3D3B -> 17AA:38F9" in insp

    with tempfile.TemporaryDirectory() as td:
        stage_dir = Path(td) / "stage_8a28"
        subprocess.run(
            [sys.executable, tool, "--stage", "103C:8A28", "--out-dir", str(stage_dir)],
            cwd=ROOT,
            check=True,
        )
        manifest = json.loads((stage_dir / "staging_manifest.json").read_text(encoding="utf-8"))
        assert manifest["authorized"] is False
        assert manifest["entry"]["subsystem"] == "103C:8A28"
        assert manifest["board_properties"]["boost_type"] == "internal"
        assert len(manifest["import_commands"]) == 2

        # Downgraded SPI board (1028:0BEB) and quad-amp board (17AA:38C7) must be refused!
        for rejected in ("1028:0BEB", "17AA:38C7", "1043:14D3"):
            res = subprocess.run(
                [sys.executable, tool, "--stage", rejected, "--out-dir", str(Path(td) / "bad")],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            assert res.returncode != 0, f"Expected --stage {rejected} to fail"
            assert "REFUSED:" in res.stderr


def verify_cpp_runtime_and_isolation():
    mocks = r"""
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include "Platform/Board/BoardDiscovery.hpp"
#include "Platform/Board/HardwareCatalog.hpp"
#include "Platform/Board/PlatformProfile.hpp"

struct OSObject {unsigned refs=1;virtual ~OSObject()=default;void retain(){++refs;}void release(){assert(refs>1);--refs;}};
struct OSString:OSObject {std::string value;explicit OSString(const char* s):value(s){}const char* getCStringNoCopy(){return value.c_str();}};
#define OSDynamicCast(t,o) dynamic_cast<t*>(o)
#define OSSafeReleaseNULL(o) do{if(o){o->release();o=nullptr;}}while(0)
constexpr int kIOReturnSuccess=0;
constexpr bool kOSBooleanTrue=true,kOSBooleanFalse=false;
constexpr unsigned kIOPCIConfigSubSystemVendorID=0x2c,kIOPCIConfigSubSystemID=0x2e;
struct IORegistryEntry:OSObject {};
class IOService:public IORegistryEntry {public:OSString* ioName=nullptr;OSObject* getProperty(const char* name){return !strcmp(name,"IOName")?ioName:nullptr;}const char* getName(){return "SPKR";}};
class IOACPIPlatformDevice:public IOService {public:OSString hid{"CSC3551"},sub{"103C8A28"};
 int evaluateObject(const char* name,OSObject** out){*out=!strcmp(name,"_HID")?&hid:!strcmp(name,"_SUB")?&sub:nullptr;if(!*out)return -1;(*out)->retain();return 0;}};
class IOPCIDevice:public IOService {public:uint32_t ssid=0x103c8a28;uint16_t configRead16(unsigned offset){return offset==0x2c?ssid>>16:ssid&0xffff;}};
#include "Support/KernelDeviceCast.hpp"
#define CIRRUS_LOG(...) ((void)0)
class CirrusAudioFixup {
public:
 IOService* mProvider=nullptr;IOPCIDevice* audio=nullptr;
 const cirrus::platform::PlatformProfile* mPlatformProfile=nullptr;
 cirrus::platform::DiscoveredBoard* mDiscoveredBoard=nullptr;
 cirrus::platform::PlatformProfile mDiscoveredProfile{};
 uint32_t mPlatformSubsystemId=0;bool mPlatformAuthorized=false;
 std::map<std::string,std::string> strProps;
 std::map<std::string,uint64_t> numProps;
 std::map<std::string,bool> boolProps;
 IOService* getAudioController(){if(audio)audio->retain();return audio;}
 void setProperty(const char* k,uint64_t v,unsigned){numProps[k]=v;}
 void setProperty(const char* k,const char* v){strProps[k]=v?v:"";}
 void setProperty(const char* k,bool v){boolProps[k]=v;}
 void resolvePlatformProfile();
};
"""
    actual = method(service_source(), "CirrusAudioFixup", "resolvePlatformProfile")
    checks = r"""
int main(){
 using namespace cirrus::platform::catalog;
 static_assert(sizeof(CandidateEntry) == 8, "CandidateEntry must stay compact and pointer-free");

 // 1. Catalog invariants: strictly ascending, zero duplicates.
 assert(kHardwareCatalogCount > 250);
 for(size_t i=0;i<kHardwareCatalogCount;++i){
  const auto& e=kHardwareCatalog[i];
  assert(e.subsystem!=0 && missingReasonName(e.missingReason)!=nullptr && e.ampCount<=4);
  if(e.level!=SupportLevel::RecognizedCandidate) assert(e.ampCount==2);
  if(i>0) assert(kHardwareCatalog[i-1].subsystem < e.subsystem);
  assert(lookupCandidate(e.subsystem)==&e);
 }
 assert(lookupCandidate(0)==nullptr);
 assert(lookupCandidate(0x12345678U)==nullptr);

 // 1b. Alias resolution (Lenovo 17AA:385C -> 17AA:3877, 17AA:3D3B -> 17AA:38F9).
 assert(lookupCandidate(0x17AA385CU) && lookupCandidate(0x17AA385CU)->subsystem == 0x17AA3877U);
 assert(lookupCandidate(0x17AA3D3BU) && lookupCandidate(0x17AA3D3BU)->subsystem == 0x17AA38F9U);

 // 2. All 14 production boards must be ProductionSupported with "NONE".
 const uint32_t l3Ids[]={
  0x10280B28U,0x103C89C6U,0x103C8A31U,0x10431533U,0x10431683U,0x10431B13U,0x10431C63U,
  0x10431C9FU,0x10431D1FU,0x17AA382BU,0x17AA3847U,0x17AA3877U,0x17AA38F9U,0x17AA391EU
 };
 size_t l3Count=0;
 for(size_t i=0;i<kHardwareCatalogCount;++i){
  if(kHardwareCatalog[i].level==SupportLevel::ProductionSupported) ++l3Count;
 }
 assert(l3Count==sizeof(l3Ids)/sizeof(l3Ids[0]));
 for(uint32_t id : l3Ids){
  const auto* e=lookupCandidate(id);
  assert(e && e->level==SupportLevel::ProductionSupported && !strcmp(missingReasonName(e->missingReason),"NONE"));
 }

 // 3. Level 2 CS35L41 candidate (HP Envy 13 103C:8A28) must publish diagnostic, stay unauthorized.
 IOACPIPlatformDevice amp;IOPCIDevice audio;CirrusAudioFixup service;
 service.mProvider=&amp;service.audio=&audio;
 service.resolvePlatformProfile();
 assert(!service.mPlatformAuthorized);
 assert(!service.boolProps["Cirrus_Platform_Authorized"]);
 assert(service.numProps["Cirrus_Candidate_Subsystem"]==0x103C8A28U);
 assert(service.strProps["Cirrus_Candidate_Status"]=="TECHNICALLY_SUPPORTABLE_NO_EMBEDDED_FW");
 assert(service.strProps["Cirrus_Candidate_Family"]=="CS35L41");
 assert(service.numProps["Cirrus_Candidate_Amp_Count"]==2);
 assert(service.strProps["Cirrus_Candidate_Missing"]=="MISSING_EMBEDDED_FW_AND_HW_VALIDATION");
 assert(service.strProps["Cirrus_Candidate_Match_Source"]=="PRIMARY");

 // 4. Level 2 Acer CS35L41 candidate (1025:1826).
 amp.sub.value="10251826";audio.ssid=0x10251826U;
 service.resolvePlatformProfile();
 assert(!service.mPlatformAuthorized);
 assert(service.strProps["Cirrus_Candidate_Status"]=="TECHNICALLY_SUPPORTABLE_NO_EMBEDDED_FW");
 assert(service.strProps["Cirrus_Candidate_Family"]=="CS35L41");

 // 5. Level 2 Lenovo Yoga Book 9i CS35L56 candidate (17AA:390B) must stay unauthorized.
 amp.hid.value="CSC3554";amp.sub.value="17AA390B";audio.ssid=0x17AA390BU;
 service.resolvePlatformProfile();
 assert(!service.mPlatformAuthorized);
 assert(service.strProps["Cirrus_Candidate_Status"]=="TECHNICALLY_SUPPORTABLE_NO_EMBEDDED_FW");
 assert(service.strProps["Cirrus_Candidate_Family"]=="CS35L56");
 assert(service.numProps["Cirrus_Candidate_Amp_Count"]==2);

 // 6. Downgraded SPI candidates (1028:0BEB, 1043:14D3) & ambiguous-bus candidates (103C:8BE9, 1043:1433) must stay Level 1.
 for(uint32_t spiId : {0x10280BEBU, 0x104314D3U}){
  char hex[9];snprintf(hex,sizeof(hex),"%08X",spiId);
  amp.hid.value="CSC3551";amp.sub.value=hex;audio.ssid=spiId;
  service.resolvePlatformProfile();
  assert(!service.mPlatformAuthorized);
  assert(service.strProps["Cirrus_Candidate_Status"]=="RECOGNIZED_CANDIDATE");
  assert(service.strProps["Cirrus_Candidate_Missing"]=="SPI_REQUIRES_NATIVE_ACPI_DSD_AND_FW");
 }
 for(uint32_t ambId : {0x103C8BE9U, 0x10431433U}){
  char hex[9];snprintf(hex,sizeof(hex),"%08X",ambId);
  amp.hid.value="CSC3551";amp.sub.value=hex;audio.ssid=ambId;
  service.resolvePlatformProfile();
  assert(!service.mPlatformAuthorized);
  assert(service.strProps["Cirrus_Candidate_Status"]=="RECOGNIZED_CANDIDATE");
  assert(service.strProps["Cirrus_Candidate_Missing"]=="AMBIGUOUS_BUS_TOPOLOGY");
 }

 // 7. Level 1 quad-amp CS35L41 candidate (Lenovo 17AA:38C7 & Dell 1028:0C4D).
 for(uint32_t quadId : {0x17AA38C7U, 0x10280C4DU}){
  char hex[9];snprintf(hex,sizeof(hex),"%08X",quadId);
  amp.hid.value="CSC3551";amp.sub.value=hex;audio.ssid=quadId;
  service.resolvePlatformProfile();
  assert(!service.mPlatformAuthorized);
  assert(service.strProps["Cirrus_Candidate_Status"]=="RECOGNIZED_CANDIDATE");
  assert(service.strProps["Cirrus_Candidate_Family"]=="CS35L41");
  assert(service.numProps["Cirrus_Candidate_Amp_Count"]==4);
  assert(service.strProps["Cirrus_Candidate_Missing"]=="UNSUPPORTED_QUAD_AMP");
 }

 // 8. PCI SSID fallback for catalog diagnostic MUST NOT weaken ACPI authorization fail-closed rule.
 amp.hid.value="CSC3551";amp.sub.value="INVALID";audio.ssid=0x103C89C6U;
 service.resolvePlatformProfile();
 assert(!service.mPlatformAuthorized);
 assert(!service.boolProps["Cirrus_Platform_Authorized"]);
 assert(service.numProps["Cirrus_Candidate_Subsystem"]==0x103C89C6U);
 assert(service.strProps["Cirrus_Candidate_Status"]=="PRODUCTION_SUPPORTED");
 assert(service.strProps["Cirrus_Candidate_Match_Source"]=="PCI_FALLBACK");

 // 9. Unrecognized and zero SSIDs.
 amp.sub.value="12345678";audio.ssid=0x12345678U;
 service.resolvePlatformProfile();
 assert(!service.mPlatformAuthorized);
 assert(service.strProps["Cirrus_Candidate_Status"]=="UNKNOWN_HARDWARE");
 assert(service.strProps["Cirrus_Candidate_Family"]=="UNKNOWN");
 assert(service.numProps["Cirrus_Candidate_Amp_Count"]==0);
 assert(service.strProps["Cirrus_Candidate_Missing"]=="UNRECOGNIZED_SUBSYSTEM_ID");
 assert(service.strProps["Cirrus_Candidate_Match_Source"]=="NONE");

 amp.sub.value="INVALID";audio.ssid=0;
 service.resolvePlatformProfile();
 assert(!service.mPlatformAuthorized);
 assert(service.strProps["Cirrus_Candidate_Status"]=="UNKNOWN_HARDWARE");
 assert(service.strProps["Cirrus_Candidate_Missing"]=="NO_SUBSYSTEM_ID");
 assert(service.strProps["Cirrus_Candidate_Match_Source"]=="NONE");

 assert(amp.hid.refs==1&&amp.sub.refs==1&&audio.refs==1);
 return 0;
}
"""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        install(tmp)
        (tmp / "IOKit").mkdir(exist_ok=True)
        (tmp / "IOKit/IORegistryEntry.h").write_text("#pragma once\n")
        (tmp / "IOKit/IOLib.h").write_text("#pragma once\n")
        (tmp / "test.cpp").write_text(mocks + actual + checks, encoding="utf-8")
        exe = tmp / "test"
        compiler = host_compiler()
        for flags in sanitizer_flags():
            subprocess.run(
                [
                    compiler,
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    *flags,
                    "-I" + str(tmp),
                    "-I" + str(ROOT / "CirrusAudioFixup"),
                    str(tmp / "test.cpp"),
                    "-o",
                    str(exe),
                ],
                check=True,
            )
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    verify_catalog_sync_and_json()
    verify_onboarding_tool()
    verify_cpp_runtime_and_isolation()
    print(
        "PASS proactive hardware catalog integrity, 3-level classification, onboarding tool and read-only authorization isolation"
    )
