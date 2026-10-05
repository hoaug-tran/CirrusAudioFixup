#pragma once

#if defined(__APPLE__)
#include <IOKit/IOLib.h>
#ifndef IOMallocData
#define IOMallocData(size) IOMalloc(size)
#define IOFreeData(ptr, size) IOFree(ptr, size)
#endif
#else
#include <stdio.h>
#define IOLog(fmt, ...) printf(fmt, ##__VA_ARGS__)
#endif

extern bool gCirrusDebug;

#define CIRRUS_LOG_PREFIX "CirrusAudioFixup: "

#ifndef CIRRUS_LOG
#define CIRRUS_LOG(fmt, ...)                                                                                                               \
    do {                                                                                                                                   \
        if (gCirrusDebug)                                                                                                                  \
            IOLog(CIRRUS_LOG_PREFIX fmt "\n", ##__VA_ARGS__);                                                                              \
    } while (0)
#endif

#ifndef CIRRUS_ERR
#define CIRRUS_ERR(fmt, ...) IOLog(CIRRUS_LOG_PREFIX "ERROR: " fmt "\n", ##__VA_ARGS__)
#endif
