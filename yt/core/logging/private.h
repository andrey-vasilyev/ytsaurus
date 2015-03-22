#pragma once

#include "public.h"

#include <core/profiling/profiler.h>

namespace NYT {
namespace NLogging {

////////////////////////////////////////////////////////////////////////////////

extern const char* const SystemLoggingCategory;
extern const char* const DefaultStderrWriterName;
extern const ELogLevel DefaultStderrMinLevel;
extern NProfiling::TProfiler LoggingProfiler;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(ILogWriter)

////////////////////////////////////////////////////////////////////////////////

} // namespace NLogging
} // namespace NYT
