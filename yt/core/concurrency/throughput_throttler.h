#pragma once

#include "public.h"

#include <yt/core/actions/future.h>

#include <yt/core/logging/log.h>

#include <yt/core/profiling/profiler.h>

#include <yt/core/ypath/public.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NConcurrency {

////////////////////////////////////////////////////////////////////////////////
   
//! Enables throttling sync and async operations.
/*!
 *  This interface and its implementations are vastly inspired by the "token bucket" algorithm and
 *  |DataTransferThrottler| class from Hadoop.
 */
struct IThroughputThrottler
    : public virtual TRefCounted
{
    //! Assuming that we are about to transfer #count bytes,
    //! returns a future that is set when enough time has passed
    //! to ensure proper bandwidth utilization.
    /*!
     *  \note Thread affinity: any
     */
    virtual TFuture<void> Throttle(i64 count) = 0;

    //! Tries to acquire #count bytes for transfer.
    //! Returns |true| if the request could be served without overdraft.
    /*!
     *  \note Thread affinity: any
     */
    virtual bool TryAcquire(i64 count) = 0;

    //! Unconditionally acquires #count bytes for transfer.
    //! This requires could easily lead to overdraft.
    /*!
     *  \note Thread affinity: any
     */
    virtual void Acquire(i64 count) = 0;

    //! Returns |true| if the throttling limit has been exceeded.
    /*!
     *  \note Thread affinity: any
     */
    virtual bool IsOverdraft() const = 0;
};

DEFINE_REFCOUNTED_TYPE(IThroughputThrottler)

//! Returns a throttler from #config.
IThroughputThrottlerPtr CreateLimitedThrottler(
    TThroughputThrottlerConfigPtr config,
    const NLogging::TLogger& logger = NLogging::TLogger(),
    const NProfiling::TProfiler& profiler = NProfiling::TProfiler());

//! Returns a throttler that imposes no throughput limit.
IThroughputThrottlerPtr GetUnlimitedThrottler();

////////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT

