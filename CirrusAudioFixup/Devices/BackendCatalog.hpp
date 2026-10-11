// Add verified silicon backends here. IOService lifecycle/playback dispatch
// does not need a new model switch or a new transport branch per amplifier.
#pragma once
#include "Devices/CS35L41/ServiceBackend.hpp"
#include "Transport/CS35L41SPITransport.hpp"
namespace cirrus::devices {
constexpr bool cs35l41RegisterAccess(uint32_t reg){return !(reg&3) && reg<=0x7ffffffc;}
constexpr core::RegisterMapConfig cs35l41RegisterFormat(bool spi) {
    auto format=spi?transport::CS35L41SPITransport::config():core::RegisterMapConfig{};
    format.maxRegister=0x7ffffffc;format.readable=cs35l41RegisterAccess;format.writable=cs35l41RegisterAccess;
    return format;
}
inline constexpr platform::BackendDefinition kBackendDefinitions[]={
    {"cs35l41-clsa0100","CLSA0100",0x35a40,3,cs35l41RegisterFormat(false),cs35l41RegisterFormat(true),cs35l41::createServiceBackend,cs35l41::destroyServiceBackend,true},
    {"cs35l41-clsa0101","CLSA0101",0x35a40,3,cs35l41RegisterFormat(false),cs35l41RegisterFormat(true),cs35l41::createServiceBackend,cs35l41::destroyServiceBackend,true},
    {"cs35l41-csc3551","CSC3551",0x35a40,3,cs35l41RegisterFormat(false),cs35l41RegisterFormat(true),cs35l41::createServiceBackend,cs35l41::destroyServiceBackend,true},
};
inline bool registerBackends(platform::BackendRegistry& registry) {
    for(const auto& definition:kBackendDefinitions)if(!registry.add(definition))return false;
    return true;
}
}
