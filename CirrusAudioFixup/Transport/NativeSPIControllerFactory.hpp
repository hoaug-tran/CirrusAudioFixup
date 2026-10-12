// Declarative native communication selection; no AMP, OS SDK or silicon fields.
#pragma once
class IOACPIPlatformDevice;
class IOPCIDevice;
class IOService;
namespace cirrus::transport {
class NativeSPIControllerOwner;
struct NativeSPIControllerFailure;
// Return a uniquely owned, bound controller on success; null plus failure on
// rejection. Discovery/owner pointers are borrowed through host shutdown.
using NativeSPIControllerFactory = NativeSPIControllerOwner* (*)(IOACPIPlatformDevice*, IOPCIDevice*, IOService*,
                                                                 NativeSPIControllerFailure&);
}
