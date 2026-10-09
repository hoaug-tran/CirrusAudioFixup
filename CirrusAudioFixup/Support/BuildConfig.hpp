//
// BuildConfig.hpp
// CirrusAudioFixup build identity and default logging policy.
//
// Debug and Release use the same hardware checks and recovery limits.
// Only the default verbosity and compiler settings differ.
// See LICENSE for distribution terms.
//

#pragma once

namespace cirrus::support {

#if defined(DEBUG) && DEBUG
inline constexpr bool kDefaultVerboseLogging = true;
inline constexpr const char* kBuildConfiguration = "Debug";
#else
inline constexpr bool kDefaultVerboseLogging = false;
inline constexpr const char* kBuildConfiguration = "Release";
#endif

}
