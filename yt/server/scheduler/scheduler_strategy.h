#pragma once

#include "public.h"
#include "event_log.h"
#include "job_resources.h"

#include <yt/ytlib/node_tracker_client/node.pb.h>

#include <yt/core/actions/signal.h>

#include <yt/core/yson/public.h>

#include <yt/core/ytree/permission.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

struct ISchedulerStrategyHost
    : public virtual IEventLogHost
{
    virtual ~ISchedulerStrategyHost() = default;

    virtual TJobResources GetTotalResourceLimits() = 0;
    virtual TJobResources GetMainNodesResourceLimits() = 0;
    virtual TJobResources GetResourceLimits(const TSchedulingTagFilter& filter) = 0;

    virtual void ActivateOperation(const TOperationId& operationId) = 0;

    virtual int GetExecNodeCount() const = 0;
    virtual int GetTotalNodeCount() const = 0;
    virtual TExecNodeDescriptorListPtr GetExecNodeDescriptors(const TSchedulingTagFilter& filter) const = 0;

    virtual void ValidatePoolPermission(
        const NYPath::TYPath& path,
        const Stroka& user,
        NYTree::EPermission permission) const = 0;

    virtual void SetSchedulerAlert(EAlertType alertType, const TError& alert) = 0;
};

////////////////////////////////////////////////////////////////////////////////

struct TUpdatedJob
{
    TUpdatedJob(const TOperationId& operationId, const TJobId& jobId, const TJobResources& delta)
        : OperationId(operationId)
        , JobId(jobId)
        , Delta(delta)
    { }

    TOperationId OperationId;
    TJobId JobId;
    TJobResources Delta;
};

struct TCompletedJob
{
    TCompletedJob(const TOperationId& operationId, const TJobId& jobId)
        : OperationId(operationId)
        , JobId(jobId)
    { }

    TOperationId OperationId;
    TJobId JobId;
};

////////////////////////////////////////////////////////////////////


struct ISchedulerStrategy
    : public virtual TRefCounted
{
    virtual TFuture<void> ScheduleJobs(const ISchedulingContextPtr& schedulingContext) = 0;

    //! Starts periodic updates and logging.
    virtual void StartPeriodicActivity() = 0;

    //! Called periodically to build new tree snapshot.
    virtual void OnFairShareUpdateAt(TInstant now) = 0;

    //! Called periodically to log scheduling tree state.
    virtual void OnFairShareLoggingAt(TInstant now) = 0;

    //! Resets memoized state.
    virtual void ResetState() = 0;

    //! Validates that operation can be started.
    /*!
     *  In particular, the following checks are performed:
     *  1) Limits for the number of concurrent operations are validated.
     *  2) Pool permissions are validated.
     */
    virtual TFuture<void> ValidateOperationStart(const TOperationPtr& operation) = 0;

    //! Validates that operation can be registered without errors.
    /*!
     *  Checks limits for the number of concurrent operations.
     *
     *  The implementation must be synchronous.
     */
    virtual void ValidateOperationCanBeRegistered(const TOperationPtr& operation) = 0;

    //! Register operation in strategy.
    /*!
     *  The implementation must throw no exceptions.
     */
    virtual void RegisterOperation(const TOperationPtr& operation) = 0;

    //! Unregister operation in strategy.
    /*!
     *  The implementation must throw no exceptions.
     */
    virtual void UnregisterOperation(const TOperationPtr& operation) = 0;

    virtual void ProcessUpdatedAndCompletedJobs(
        const std::vector<TUpdatedJob>& updatedJobs,
        const std::vector<TCompletedJob>& completedJobs) = 0;

    virtual void ApplyJobMetricsDelta(
        const TOperationId& operationId,
        const TJobMetrics& jobMetricsDelta) = 0;

    virtual void UpdatePools(const NYTree::INodePtr& poolsNode) = 0;

    virtual void UpdateOperationRuntimeParams(
        const TOperationPtr& operation,
        const NYTree::INodePtr& update) = 0;

    //! Updates current config used by strategy.
    virtual void UpdateConfig(const TFairShareStrategyConfigPtr& config) = 0;

    //! Builds a YSON structure containing a set of attributes to be assigned to operation's node
    //! in Cypress during creation.
    virtual void BuildOperationAttributes(
        const TOperationId& operationId,
        NYson::IYsonConsumer* consumer) = 0;

    //! Builds a YSON map fragment with strategy specific information about operation
    //! that used for event log.
    virtual void BuildOperationInfoForEventLog(
        const TOperationPtr& operation,
        NYson::IYsonConsumer* consumer) = 0;

    //! Builds a YSON structure reflecting operation's progress.
    //! This progress is periodically pushed into Cypress and is also displayed via Orchid.
    virtual void BuildOperationProgress(
        const TOperationId& operationId,
        NYson::IYsonConsumer* consumer) = 0;

    //! Similar to #BuildOperationProgress but constructs a reduced version to be used by UI.
    virtual void BuildBriefOperationProgress(
        const TOperationId& operationId,
        NYson::IYsonConsumer* consumer) = 0;

    //! Builds a YSON structure reflecting the state of the scheduler to be displayed in Orchid.
    virtual void BuildOrchid(NYson::IYsonConsumer* consumer) = 0;

    //! Provides a string describing operation status and statistics.
    virtual Stroka GetOperationLoggingProgress(const TOperationId& operationId) = 0;

    //! Called for a just initialized operation to construct its brief spec
    //! to be used by UI.
    virtual void BuildBriefSpec(
        const TOperationId& operationId,
        NYson::IYsonConsumer* consumer) = 0;
};

DEFINE_REFCOUNTED_TYPE(ISchedulerStrategy)

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
