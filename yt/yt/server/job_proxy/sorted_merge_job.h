#pragma once

#include "public.h"

namespace NYT::NJobProxy {

////////////////////////////////////////////////////////////////////////////////

IJobPtr CreateSortedMergeJob(IJobHost* host);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobProxy
