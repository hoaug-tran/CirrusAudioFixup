//
// Logging.hpp
// Kernel logging and the host-test logging shim.
// Routine logging is optional; failures remain visible in both builds.
// Keep register sweeps behind explicit diagnostic options.
// See LICENSE for distribution terms.
//

#pragma once

#include "Support/BuildConfig.hpp"

#if defined(__APPLE__) && (defined(KERNEL) || defined(_KERNEL) || defined(__KERNEL__))
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

// Keep failures visible in either build. Routine messages follow the runtime
// verbosity flag, so Release can still provide useful evidence with -cirrusdbg.
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
