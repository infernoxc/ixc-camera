#pragma once

// TraceLogging for code running inside the Windows Frame Server service. There is no file
// logging here: the service account can't write to the user's profile, and the events cost
// nothing unless a trace session is listening. Capture with scripts/trace-vcam.ps1.

#include <windows.h>
#include <TraceLoggingProvider.h>
#include <winmeta.h>

TRACELOGGING_DECLARE_PROVIDER(g_ixcTraceProvider);

#define IXC_TRACE(name, ...) TraceLoggingWrite(g_ixcTraceProvider, name, __VA_ARGS__)
#define IXC_TRACE_HR(name, hr) \
    TraceLoggingWrite(g_ixcTraceProvider, name, TraceLoggingLevel(WINEVENT_LEVEL_ERROR), TraceLoggingHResult(hr, "hr"))
