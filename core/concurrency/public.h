#pragma once

#include <yt/core/misc/public.h>

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TActionQueue)
DECLARE_REFCOUNTED_CLASS(TFairShareActionQueue)
DECLARE_REFCOUNTED_CLASS(TThreadPool)
DECLARE_REFCOUNTED_CLASS(TPeriodicExecutor)
DECLARE_REFCOUNTED_CLASS(TAsyncSemaphore)
DECLARE_REFCOUNTED_CLASS(TProfiledAsyncSemaphore)

DECLARE_REFCOUNTED_STRUCT(TDelayedExecutorEntry)
typedef TDelayedExecutorEntryPtr TDelayedExecutorCookie;

DECLARE_REFCOUNTED_CLASS(TThroughputThrottlerConfig)
DECLARE_REFCOUNTED_STRUCT(IThroughputThrottler)
DECLARE_REFCOUNTED_STRUCT(IReconfigurableThroughputThrottler)

DECLARE_REFCOUNTED_STRUCT(IAsyncInputStream)
DECLARE_REFCOUNTED_STRUCT(IAsyncOutputStream)

DECLARE_REFCOUNTED_STRUCT(IAsyncZeroCopyInputStream)
DECLARE_REFCOUNTED_STRUCT(IAsyncZeroCopyOutputStream)

DECLARE_REFCOUNTED_STRUCT(IFairShareThreadPool)

DECLARE_REFCOUNTED_CLASS(TAsyncStreamPipe)

DEFINE_ENUM(ESyncStreamAdapterStrategy,
    (WaitFor)
    (Get)
);

class TAsyncSemaphore;

DEFINE_ENUM(EExecutionStackKind,
    (Small) // 256 Kb (default)
    (Large) //   8 Mb
);

class TExecutionStack;

DECLARE_REFCOUNTED_CLASS(TFiber)

template <class TSignature>
class TCoroutine;

template <class T>
class TNonblockingQueue;

DECLARE_REFCOUNTED_STRUCT(TLeaseEntry)
using TLease = TLeaseEntryPtr;

DECLARE_REFCOUNTED_STRUCT(IPollable)
DECLARE_REFCOUNTED_STRUCT(IPoller)

using TThreadId = size_t;
constexpr size_t InvalidThreadId = 0;

using TFiberId = size_t;
constexpr size_t InvalidFiberId = 0;

using TFairShareThreadPoolTag = TString;

DECLARE_REFCOUNTED_STRUCT(ITwoLevelFairShareThreadPool);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NConcurrency
