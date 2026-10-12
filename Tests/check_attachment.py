"""Attachment evidence, CPU metadata and registered debug policy boundaries."""
from pathlib import Path
import subprocess,tempfile
from driver_kernel_fixture import install
ROOT=Path(__file__).resolve().parents[1]
CPP=r'''
#include "Diagnostics/AttachmentState.hpp"
#include "Support/CPUIdentity.hpp"
#include "Platform/AmplifierDriverRegistry.hpp"
#include <cassert>
using namespace cirrus;
int main(){
    diagnostics::AttachmentState state;
    diagnostics::AttachmentState invalid;assert(invalid.record(nullptr,nullptr,-99,0));assert(!strcmp(invalid.first.stage,"UNKNOWN") && !strcmp(invalid.first.reason,"UNSPECIFIED_FAILURE"));
    assert(state.record("COMMUNICATION","PROVIDER_UNSUPPORTED",-77,1));
    assert(state.record("CLEANUP","RELEASE_FAILED",-88,2));
    assert(!strcmp(state.first.reason,"PROVIDER_UNSUPPORTED") && state.first.result==-77 && state.first.index==1);
    assert(!strcmp(state.latest.reason,"RELEASE_FAILED") && state.latest.result==-88 && state.latest.index==2);
    while(state.failureCount<31)assert(!state.record("CLEANUP","RELEASE_FAILED",-88,2));
    assert(state.record("CLEANUP","RELEASE_FAILED",-88,2) && state.failureCount==32);
    assert(state.record("CLEANUP","RELEASE_FAILED",-89,2));
    assert(state.record("CLEANUP","RELEASE_FAILED",-89,3));
    assert(state.first.result==-77);
    assert(state.record("CONTROLLER_PM","RESUME_FAILED",-90,1,"CONTROLLER_SLOT") && !strcmp(state.latest.indexKind,"CONTROLLER_SLOT") && !strcmp(state.first.indexKind,"ENDPOINT"));
    state.record("CLEANUP","RELEASE_FAILED",-89,3);
    state.failureCount=UINT32_MAX;assert(!state.record("CLEANUP","RELEASE_FAILED",-89,3) && state.failureCount==UINT32_MAX);
    assert(!strcmp(support::cpuVendorName("GenuineIntel"),"INTEL"));
    assert(!strcmp(support::cpuVendorName("AuthenticAMD"),"AMD"));
    assert(!strcmp(support::cpuVendorName(nullptr),"UNKNOWN") && !strcmp(support::cpuVendorName("GenuineIntelX"),"UNKNOWN"));
    const auto cpu=support::readCPUIdentity();assert(cpu.vendor[12]==0 && cpu.brand[48]==0);
    platform::AmplifierDriverRegistry registry;
    core::RegisterMapConfig format;format.readable=[](uint32_t){return true;};format.writable=format.readable;
    platform::AmplifierDriverRegistration definition{"phase-fixture","AMPFIXTURE",0x1111,1,format,{},
        [](const platform::AmplifierDriverContext&)->core::AmplifierLifecycleHooks*{return nullptr;},[](core::AmplifierLifecycleHooks*){}};
    definition.validDebugPhase=[](const char* phase){return !strcmp(phase,"probe") || !strcmp(phase,"firmware");};
    assert(registry.add(definition));
    assert(!registry.acceptsDebugPhase(nullptr,"probe"));
    assert(!registry.acceptsDebugPhase("AMPFIXTURE",nullptr));
    assert(!registry.acceptsDebugPhase("AMPFIXTURE","") && !registry.acceptsDebugPhase("UNKNOWN","probe"));
    assert(registry.acceptsDebugPhase("AMPFIXTURE","probe") && registry.acceptsDebugPhase("AMPFIXTURE","firmware"));
    assert(!registry.acceptsDebugPhase("AMPFIXTURE","probe-extra"));
}
'''
with tempfile.TemporaryDirectory(prefix='cirrus-attachment-') as directory:
    p=Path(directory);install(p);(p/'attachment.cpp').write_text(CPP)
    for flags in ([],['-fsanitize=address,undefined','-fno-omit-frame-pointer']):
        subprocess.run(['clang++','-std=c++17',*flags,'-I'+str(p),'-I'+str(ROOT/'CirrusAudioFixup'),str(p/'attachment.cpp'),'-o',str(p/'attachment')],check=True)
        subprocess.run([str(p/'attachment')],check=True)
print('PASS attach first/latest native errors, rate limits/saturation, bounded CPU identity and null/unknown debug phase policy')
