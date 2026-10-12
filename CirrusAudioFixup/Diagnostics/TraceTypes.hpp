// Register operation provenance, independent of IOKit and silicon status.
#pragma once
#include <stdint.h>
namespace cirrus::diagnostics {
enum class TraceSource : uint32_t { Probe = 0, Dump, Consistency, Firmware, Playback, Other };

inline constexpr TraceSource TRACE_PROBE = TraceSource::Probe;
inline constexpr TraceSource TRACE_DUMP = TraceSource::Dump;
inline constexpr TraceSource TRACE_CONSISTENCY = TraceSource::Consistency;
inline constexpr TraceSource TRACE_FIRMWARE = TraceSource::Firmware;
inline constexpr TraceSource TRACE_PLAYBACK = TraceSource::Playback;
inline constexpr TraceSource TRACE_OTHER = TraceSource::Other;

}
