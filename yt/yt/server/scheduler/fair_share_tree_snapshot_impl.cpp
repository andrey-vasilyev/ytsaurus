#include "fair_share_tree_snapshot_impl.h"


namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////
TFairShareTreeSnapshotImpl::TFairShareTreeSnapshotImpl(
    TRootElementPtr rootElement,
    TNonOwningOperationElementMap enabledOperationIdToElement,
    TNonOwningOperationElementMap disabledOperationIdToElement,
    TNonOwningPoolMap poolNameToElement,
    TFairShareStrategyTreeConfigPtr treeConfig,
    TFairShareStrategyOperationControllerConfigPtr controllerConfig,
    bool coreProfilingCompatibilityEnabled,
    TTreeSchedulingSegmentsState schedulingSegmentsState)
    : RootElement_(std::move(rootElement))
    , EnabledOperationMap_(std::move(enabledOperationIdToElement))
    , DisabledOperationMap_(std::move(disabledOperationIdToElement))
    , PoolMap_(std::move(poolNameToElement))
    , TreeConfig_(std::move(treeConfig))
    , ControllerConfig_(std::move(controllerConfig))
    , CoreProfilingCompatibilityEnabled_(coreProfilingCompatibilityEnabled)
    , SchedulingSegmentsState_(std::move(schedulingSegmentsState))
{ }

TPool* TFairShareTreeSnapshotImpl::FindPool(const TString& poolName) const
{
    auto it = PoolMap_.find(poolName);
    return it != PoolMap_.end() ? it->second : nullptr;
}

TOperationElement* TFairShareTreeSnapshotImpl::FindEnabledOperationElement(TOperationId operationId) const
{
    auto it = EnabledOperationMap_.find(operationId);
    return it != EnabledOperationMap_.end() ? it->second : nullptr;
}

TOperationElement* TFairShareTreeSnapshotImpl::FindDisabledOperationElement(TOperationId operationId) const
{
    auto it = DisabledOperationMap_.find(operationId);
    return it != DisabledOperationMap_.end() ? it->second : nullptr;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
