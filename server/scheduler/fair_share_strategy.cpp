#include "fair_share_strategy.h"
#include "fair_share_tree.h"
#include "fair_share_tree_element.h"
#include "public.h"
#include "scheduler_strategy.h"
#include "scheduling_context.h"
#include "fair_share_strategy_operation_controller.h"

#include <yt/server/lib/scheduler/config.h>

#include <yt/ytlib/scheduler/job_resources.h>

#include <yt/ytlib/security_client/acl.h>

#include <yt/core/concurrency/async_rw_lock.h>
#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/thread_pool.h>

#include <yt/core/misc/algorithm_helpers.h>
#include <yt/core/misc/finally.h>

#include <yt/core/profiling/profile_manager.h>
#include <yt/core/profiling/timing.h>
#include <yt/core/profiling/metrics_accumulator.h>

namespace NYT::NScheduler {

using namespace NConcurrency;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NYson;
using namespace NYTree;
using namespace NProfiling;
using namespace NControllerAgent;
using namespace NSecurityClient;

////////////////////////////////////////////////////////////////////////////////

class TFairShareStrategy
    : public ISchedulerStrategy
{
public:
    TFairShareStrategy(
        TFairShareStrategyConfigPtr config,
        ISchedulerStrategyHost* host,
        const std::vector<IInvokerPtr>& feasibleInvokers)
        : Config(config)
        , Host(host)
        , FeasibleInvokers(feasibleInvokers)
        , Logger(SchedulerLogger)
    {
        FairShareProfilingExecutor_ = New<TPeriodicExecutor>(
            Host->GetFairShareProfilingInvoker(),
            BIND(&TFairShareStrategy::OnFairShareProfiling, MakeWeak(this)),
            Config->FairShareProfilingPeriod);

        FairShareUpdateExecutor_ = New<TPeriodicExecutor>(
            Host->GetControlInvoker(EControlQueue::FairShareStrategy),
            BIND(&TFairShareStrategy::OnFairShareUpdate, MakeWeak(this)),
            Config->FairShareUpdatePeriod);

        FairShareLoggingExecutor_ = New<TPeriodicExecutor>(
            Host->GetControlInvoker(EControlQueue::FairShareStrategy),
            BIND(&TFairShareStrategy::OnFairShareLogging, MakeWeak(this)),
            Config->FairShareLogPeriod);

        MinNeededJobResourcesUpdateExecutor_ = New<TPeriodicExecutor>(
            Host->GetControlInvoker(EControlQueue::FairShareStrategy),
            BIND(&TFairShareStrategy::OnMinNeededJobResourcesUpdate, MakeWeak(this)),
            Config->MinNeededResourcesUpdatePeriod);
    }

    virtual void OnMasterConnected() override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        FairShareProfilingExecutor_->Start();
        FairShareLoggingExecutor_->Start();
        FairShareUpdateExecutor_->Start();
        MinNeededJobResourcesUpdateExecutor_->Start();
    }

    virtual void OnMasterDisconnected() override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        FairShareProfilingExecutor_->Stop();
        FairShareLoggingExecutor_->Stop();
        FairShareUpdateExecutor_->Stop();
        MinNeededJobResourcesUpdateExecutor_->Stop();

        OperationIdToOperationState_.clear();
        IdToTree_.clear();

        DefaultTreeId_.reset();

        {
            TWriterGuard guard(TreeIdToSnapshotLock_);
            TreeIdToSnapshot_.clear();
        }
    }

    void OnFairShareProfiling()
    {
        OnFairShareProfilingAt(TInstant::Now());
    }

    void OnFairShareUpdate()
    {
        OnFairShareUpdateAt(TInstant::Now());
    }

    void OnMinNeededJobResourcesUpdate() override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        YT_LOG_INFO("Starting min needed job resources update");

        for (const auto& pair : OperationIdToOperationState_) {
            const auto& state = pair.second;
            if (state->GetHost()->IsSchedulable()) {
                state->GetController()->UpdateMinNeededJobResources();
            }
        }

        YT_LOG_INFO("Min needed job resources successfully updated");
    }

    void OnFairShareLogging()
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        OnFairShareLoggingAt(TInstant::Now());
    }

    virtual TFuture<void> ScheduleJobs(const ISchedulingContextPtr& schedulingContext) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto snapshot = FindTreeSnapshotByNodeDescriptor(schedulingContext->GetNodeDescriptor());
        if (!snapshot) {
            return VoidFuture;
        }

        return snapshot->ScheduleJobs(schedulingContext);
    }

    virtual void PreemptJobsGracefully(const ISchedulingContextPtr& schedulingContext) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto snapshot = FindTreeSnapshotByNodeDescriptor(schedulingContext->GetNodeDescriptor());
        if (snapshot) {
            snapshot->PreemptJobsGracefully(schedulingContext);
        }
    }

    virtual void RegisterOperation(IOperationStrategyHost* operation) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto spec = ParseSpec(operation);

        auto state = CreateFairShareStrategyOperationState(operation);

        YT_VERIFY(OperationIdToOperationState_.insert(
            std::make_pair(operation->GetId(), state)).second);

        auto runtimeParameters = operation->GetRuntimeParameters();

        for (const auto& pair : state->TreeIdToPoolNameMap()) {
            const auto& treeId = pair.first;
            const auto& tree = GetTree(pair.first);

            auto paramsIt = runtimeParameters->SchedulingOptionsPerPoolTree.find(treeId);
            YT_VERIFY(paramsIt != runtimeParameters->SchedulingOptionsPerPoolTree.end());

            if (tree->RegisterOperation(state, spec, paramsIt->second)) {
                ActivateOperations({operation->GetId()});
            }
        }
    }

    virtual void UnregisterOperation(IOperationStrategyHost* operation) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& state = GetOperationState(operation->GetId());
        for (const auto& pair : state->TreeIdToPoolNameMap()) {
            const auto& treeId = pair.first;
            DoUnregisterOperationFromTree(state, treeId);
        }

        YT_VERIFY(OperationIdToOperationState_.erase(operation->GetId()) == 1);
    }

    virtual void UnregisterOperationFromTree(TOperationId operationId, const TString& treeId) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& state = GetOperationState(operationId);
        if (!state->TreeIdToPoolNameMap().contains(treeId)) {
            YT_LOG_INFO("Operation to be removed from a tentative tree was not found in that tree (OperationId: %v, TreeId: %v)",
                operationId,
                treeId);
            return;
        }

        DoUnregisterOperationFromTree(state, treeId);

        state->EraseTree(treeId);

        YT_LOG_INFO("Operation removed from a tentative tree (OperationId: %v, TreeId: %v)", operationId, treeId);
    }

    void DoUnregisterOperationFromTree(const TFairShareStrategyOperationStatePtr& operationState, const TString& treeId)
    {
        auto tree = GetTree(treeId);
        tree->UnregisterOperation(operationState);
        ActivateOperations(tree->RunWaitingOperations());
    }

    virtual void DisableOperation(IOperationStrategyHost* operation) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& state = GetOperationState(operation->GetId());
        for (const auto& pair : state->TreeIdToPoolNameMap()) {
            const auto& treeId = pair.first;
            GetTree(treeId)->DisableOperation(state);
        }
    }

    virtual void UpdatePoolTrees(const INodePtr& poolTreesNode) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        YT_LOG_INFO("Updating pool trees");

        if (poolTreesNode->GetType() != NYTree::ENodeType::Map) {
            auto error = TError("Pool trees node has invalid type")
                << TErrorAttribute("expected_type", NYTree::ENodeType::Map)
                << TErrorAttribute("actual_type", poolTreesNode->GetType());
            YT_LOG_WARNING(error);
            Host->SetSchedulerAlert(ESchedulerAlertType::UpdatePools, error);
            return;
        }

        auto poolsMap = poolTreesNode->AsMap();

        std::vector<TError> errors;

        // Collect trees to add and remove.
        THashSet<TString> treeIdsToAdd;
        THashSet<TString> treeIdsToRemove;
        CollectTreesToAddAndRemove(poolsMap, &treeIdsToAdd, &treeIdsToRemove);

        // Populate trees map. New trees are not added to global map yet.
        auto idToTree = ConstructUpdatedTreeMap(
            poolsMap,
            treeIdsToAdd,
            treeIdsToRemove,
            &errors);

        // Check default tree pointer. It should point to some valid tree,
        // otherwise pool trees are not updated.
        auto defaultTreeId = poolsMap->Attributes().Find<TString>(DefaultTreeAttributeName);

        if (defaultTreeId && idToTree.find(*defaultTreeId) == idToTree.end()) {
            errors.emplace_back("Default tree is missing");
            auto error = TError("Error updating pool trees")
                << std::move(errors);
            Host->SetSchedulerAlert(ESchedulerAlertType::UpdatePools, error);
            return;
        }

        // Check that after adding or removing trees each node will belong exactly to one tree.
        // Check is skipped if trees configuration did not change.
        bool skipTreesConfigurationCheck = treeIdsToAdd.empty() && treeIdsToRemove.empty();

        if (!skipTreesConfigurationCheck) {
            if (!CheckTreesConfiguration(idToTree, &errors)) {
                auto error = TError("Error updating pool trees")
                    << std::move(errors);
                Host->SetSchedulerAlert(ESchedulerAlertType::UpdatePools, error);
                return;
            }
        }

        // Update configs and pools structure of all trees.
        std::vector<TString> updatedTreeIds;
        UpdateTreesConfigs(poolsMap, idToTree, &errors, &updatedTreeIds);

        // Abort orphaned operations.
        AbortOrphanedOperations(treeIdsToRemove);

        // Updating default fair-share tree and global tree map.
        DefaultTreeId_ = defaultTreeId;
        std::swap(IdToTree_, idToTree);

        // Setting alerts.
        if (!errors.empty()) {
            auto error = TError("Error updating pool trees")
                << std::move(errors);
            Host->SetSchedulerAlert(ESchedulerAlertType::UpdatePools, error);
        } else {
            Host->SetSchedulerAlert(ESchedulerAlertType::UpdatePools, TError());
            if (!updatedTreeIds.empty() || !treeIdsToRemove.empty() || !treeIdsToAdd.empty()) {
                Host->LogEventFluently(ELogEventType::PoolsInfo)
                    .Item("pools").DoMapFor(IdToTree_, [&] (TFluentMap fluent, const auto& value) {
                        const auto& treeId = value.first;
                        const auto& tree = value.second;
                        fluent
                            .Item(treeId).Do(BIND(&TFairShareTree::BuildStaticPoolsInformation, tree));
                    });
            }
            YT_LOG_INFO("Pool trees updated");
        }
    }

    virtual void BuildOperationAttributes(TOperationId operationId, TFluentMap fluent) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& state = GetOperationState(operationId);
        const auto& pools = state->TreeIdToPoolNameMap();

        if (DefaultTreeId_ && pools.find(*DefaultTreeId_) != pools.end()) {
            GetTree(*DefaultTreeId_)->BuildOperationAttributes(operationId, fluent);
        }
    }

    virtual void BuildOperationProgress(TOperationId operationId, TFluentMap fluent) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        if (!FindOperationState(operationId)) {
            return;
        }

        DoBuildOperationProgress(&TFairShareTree::BuildOperationProgress, operationId, fluent);
    }

    virtual void BuildBriefOperationProgress(TOperationId operationId, TFluentMap fluent) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        if (!FindOperationState(operationId)) {
            return;
        }

        DoBuildOperationProgress(&TFairShareTree::BuildBriefOperationProgress, operationId, fluent);
    }

    virtual TPoolTreeControllerSettingsMap GetOperationPoolTreeControllerSettingsMap(TOperationId operationId) override
    {
        TPoolTreeControllerSettingsMap result;
        const auto& state = GetOperationState(operationId);
        for (const auto& [treeName, poolName] : state->TreeIdToPoolNameMap()) {
            auto tree = GetTree(treeName);
            result.emplace(
                treeName,
                TPoolTreeControllerSettings{
                    .SchedulingTagFilter = tree->GetNodesFilter(),
                    .Tentative = GetSchedulingOptionsPerPoolTree(state->GetHost(), treeName)->Tentative
                });
        }
        return result;
    }

    virtual std::vector<std::pair<TOperationId, TError>> GetUnschedulableOperations() override
    {
        std::vector<std::pair<TOperationId, TError>> result;
        for (const auto& operationStatePair : OperationIdToOperationState_) {
            auto operationId = operationStatePair.first;
            const auto& operationState = operationStatePair.second;

            if (operationState->TreeIdToPoolNameMap().empty()) {
                // This operation is orphaned and will be aborted.
                continue;
            }

            bool hasSchedulableTree = false;
            TError operationError("Operation is unschedulable in all trees");

            for (const auto& treePoolPair : operationState->TreeIdToPoolNameMap()) {
                const auto& treeName = treePoolPair.first;
                auto error = GetTree(treeName)->CheckOperationUnschedulable(
                    operationId,
                    Config->OperationUnschedulableSafeTimeout,
                    Config->OperationUnschedulableMinScheduleJobAttempts,
                    Config->OperationUnschedulableDeactiovationReasons);
                if (error.IsOK()) {
                    hasSchedulableTree = true;
                    break;
                } else {
                    operationError.InnerErrors().push_back(error);
                }
            }

            if (!hasSchedulableTree) {
                result.emplace_back(operationId, operationError);
            }
        }
        return result;
    }

    virtual void UpdateConfig(const TFairShareStrategyConfigPtr& config) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        Config = config;

        for (const auto& pair : IdToTree_) {
            const auto& tree = pair.second;
            tree->UpdateControllerConfig(config);
        }

        FairShareProfilingExecutor_->SetPeriod(Config->FairShareProfilingPeriod);
        FairShareUpdateExecutor_->SetPeriod(Config->FairShareUpdatePeriod);
        FairShareLoggingExecutor_->SetPeriod(Config->FairShareLogPeriod);
        MinNeededJobResourcesUpdateExecutor_->SetPeriod(Config->MinNeededResourcesUpdatePeriod);
    }

    virtual void BuildOperationInfoForEventLog(const IOperationStrategyHost* operation, TFluentMap fluent)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& operationState = GetOperationState(operation->GetId());
        const auto& pools = operationState->TreeIdToPoolNameMap();

        fluent
            .DoIf(DefaultTreeId_.operator bool(), [&] (TFluentMap fluent) {
                auto it = pools.find(*DefaultTreeId_);
                if (it != pools.end()) {
                    fluent
                        .Item("pool").Value(it->second.GetPool());
                }
            });
    }

    virtual void ApplyOperationRuntimeParameters(IOperationStrategyHost* operation) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto state = GetOperationState(operation->GetId());
        const auto runtimeParameters = operation->GetRuntimeParameters();

        auto newPools = GetOperationPools(operation->GetRuntimeParameters());

        YT_VERIFY(newPools.size() == state->TreeIdToPoolNameMap().size());

        // Tentative trees can be removed from state, we must apply these changes to new state.
        for (const auto& erasedTree : state->GetHost()->ErasedTrees()) {
            newPools.erase(erasedTree);
        }

        for (const auto& pair : state->TreeIdToPoolNameMap()) {
            const auto& treeId = pair.first;
            const auto& oldPool = pair.second;

            auto newPoolIt = newPools.find(treeId);
            YT_VERIFY(newPoolIt != newPools.end());

            auto tree = GetTree(treeId);
            if (oldPool.GetPool() != newPoolIt->second.GetPool()) {
                tree->ChangeOperationPool(operation->GetId(), state, newPoolIt->second);
                ActivateOperations(tree->RunWaitingOperations());
            }

            auto it = runtimeParameters->SchedulingOptionsPerPoolTree.find(treeId);
            YT_VERIFY(it != runtimeParameters->SchedulingOptionsPerPoolTree.end());
            tree->UpdateOperationRuntimeParameters(operation->GetId(), it->second);
        }
        state->TreeIdToPoolNameMap() = newPools;
    }

    virtual void InitOperationRuntimeParameters(
        const TOperationRuntimeParametersPtr& runtimeParameters,
        const TOperationSpecBasePtr& spec,
        const TSerializableAccessControlList& baseAcl,
        const TString& user,
        EOperationType operationType) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        runtimeParameters->Acl = baseAcl;
        runtimeParameters->Acl.Entries.insert(
            runtimeParameters->Acl.Entries.end(),
            spec->Acl.Entries.begin(),
            spec->Acl.Entries.end());

        auto poolTrees = ParsePoolTrees(spec, operationType);
        for (const auto& poolTreeDescription : poolTrees) {
            auto treeParams = New<TOperationFairShareTreeRuntimeParameters>();
            auto specIt = spec->SchedulingOptionsPerPoolTree.find(poolTreeDescription.Name);
            if (specIt != spec->SchedulingOptionsPerPoolTree.end()) {
                treeParams->Weight = spec->Weight ? spec->Weight : specIt->second->Weight;
                treeParams->Pool = GetTree(poolTreeDescription.Name)->CreatePoolName(spec->Pool ? spec->Pool : specIt->second->Pool, user);
                treeParams->ResourceLimits = spec->ResourceLimits ? spec->ResourceLimits : specIt->second->ResourceLimits;
            } else {
                treeParams->Weight = spec->Weight;
                treeParams->Pool = GetTree(poolTreeDescription.Name)->CreatePoolName(spec->Pool, user);
                treeParams->ResourceLimits = spec->ResourceLimits;
            }
            treeParams->Tentative = poolTreeDescription.Tentative;
            YT_VERIFY(runtimeParameters->SchedulingOptionsPerPoolTree.emplace(poolTreeDescription.Name, std::move(treeParams)).second);
        }
    }

    virtual void ValidateOperationRuntimeParameters(
        IOperationStrategyHost* operation,
        const TOperationRuntimeParametersPtr& runtimeParameters,
        bool validatePools) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& state = GetOperationState(operation->GetId());

        for (const auto& pair : runtimeParameters->SchedulingOptionsPerPoolTree) {
            auto poolTrees = state->TreeIdToPoolNameMap();
            if (poolTrees.find(pair.first) == poolTrees.end()) {
                THROW_ERROR_EXCEPTION("Pool tree %Qv was not configured for this operation", pair.first);
            }
        }

        if (validatePools) {
            ValidateOperationPoolsCanBeUsed(operation, runtimeParameters);
            ValidateMaxRunningOperationsCountOnPoolChange(operation, runtimeParameters);

            auto poolLimitViolations = GetPoolLimitViolations(operation, runtimeParameters);
            if (!poolLimitViolations.empty()) {
                THROW_ERROR poolLimitViolations.begin()->second;
            }
        }
    }

    virtual void BuildOrchid(TFluentMap fluent) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        THashMap<TString, std::vector<TExecNodeDescriptor>> descriptorsPerPoolTree;
        for (const auto& pair : IdToTree_) {
            const auto& treeId = pair.first;
            descriptorsPerPoolTree.emplace(treeId, std::vector<TExecNodeDescriptor>{});
        }

        auto descriptors = Host->CalculateExecNodeDescriptors(TSchedulingTagFilter());
        for (const auto& idDescriptorPair : *descriptors) {
            const auto& descriptor = idDescriptorPair.second;
            if (!descriptor.Online) {
                continue;
            }
            for (const auto& idTreePair : IdToTree_) {
                const auto& treeId = idTreePair.first;
                const auto& tree = idTreePair.second;
                if (tree->GetNodesFilter().CanSchedule(descriptor.Tags)) {
                    descriptorsPerPoolTree[treeId].push_back(descriptor);
                    break;
                }
            }
        }

        fluent
            .DoIf(DefaultTreeId_.operator bool(), [&] (TFluentMap fluent) {
                fluent
                    .Item("default_fair_share_tree").Value(*DefaultTreeId_);
            })
            .Item("scheduling_info_per_pool_tree").DoMapFor(IdToTree_, [&] (TFluentMap fluent, const auto& pair) {
                const auto& treeId = pair.first;
                const auto& tree = pair.second;

                auto it = descriptorsPerPoolTree.find(treeId);
                YT_VERIFY(it != descriptorsPerPoolTree.end());

                fluent
                    .Item(treeId).BeginMap()
                        .Do(BIND(&TFairShareStrategy::BuildTreeOrchid, tree, it->second))
                    .EndMap();
            });
    }

    virtual void ApplyJobMetricsDelta(const TOperationIdToOperationJobMetrics& operationIdToOperationJobMetrics) override
    {
        // TODO(eshcherbin): Change verification. This method is called only in control thread.
        VERIFY_THREAD_AFFINITY_ANY();

        TForbidContextSwitchGuard contextSwitchGuard;

        THashMap<TString, IFairShareTreeSnapshotPtr> snapshots;
        {
            TReaderGuard guard(TreeIdToSnapshotLock_);
            snapshots = TreeIdToSnapshot_;
        }

        for (const auto& pair : operationIdToOperationJobMetrics) {
            auto operationId = pair.first;
            for (const auto& metrics : pair.second) {
                auto snapshotIt = snapshots.find(metrics.TreeId);
                if (snapshotIt == snapshots.end()) {
                    continue;
                }

                const auto& snapshot = snapshotIt->second;
                snapshot->ApplyJobMetricsDelta(operationId, metrics.Metrics);
            }
        }
    }

    virtual TFuture<void> ValidateOperationStart(const IOperationStrategyHost* operation) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        return BIND(&TFairShareStrategy::ValidateOperationPoolsCanBeUsed, Unretained(this))
            .AsyncVia(GetCurrentInvoker())
            .Run(operation, operation->GetRuntimeParameters());
    }

    virtual THashMap<TString, TError> GetPoolLimitViolations(
        const IOperationStrategyHost* operation,
        const TOperationRuntimeParametersPtr& runtimeParameters) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto pools = GetOperationPools(runtimeParameters);

        THashMap<TString, TError> result;

        for (const auto& [treeId, pool] : pools) {
            auto tree = GetTree(treeId);
            try {
                tree->ValidatePoolLimits(operation, pool);
            } catch (TErrorException& ex) {
                result.emplace(treeId, std::move(ex.Error()));
            }
        }

        return result;
    }

    virtual void ValidateMaxRunningOperationsCountOnPoolChange(
        const IOperationStrategyHost* operation,
        const TOperationRuntimeParametersPtr& runtimeParameters)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto pools = GetOperationPools(runtimeParameters);

        for (const auto& pair : pools) {
            auto tree = GetTree(pair.first);
            tree->ValidatePoolLimitsOnPoolChange(operation, pair.second);
        }
    }

    virtual void OnFairShareProfilingAt(TInstant now) override
    {
        VERIFY_INVOKER_AFFINITY(Host->GetFairShareProfilingInvoker());

        TForbidContextSwitchGuard contextSwitchGuard;

        THashMap<TString, IFairShareTreeSnapshotPtr> snapshots;
        {
            TReaderGuard guard(TreeIdToSnapshotLock_);
            snapshots = TreeIdToSnapshot_;
        }

        for (const auto& [treeId, treeSnapshot] : snapshots) {
            treeSnapshot->ProfileFairShare();
        }
    }

    // NB: This function is public for testing purposes.
    virtual void OnFairShareUpdateAt(TInstant now) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        YT_LOG_INFO("Starting fair share update");

        THashMap<TString, TFuture<std::pair<IFairShareTreeSnapshotPtr, TError>>> asyncUpdates;
        for (const auto& [treeId, tree] : IdToTree_) {
            asyncUpdates.emplace(treeId, tree->OnFairShareUpdateAt(now));
        }

        auto result = WaitFor(Combine(asyncUpdates));
        if (!result.IsOK()) {
            Host->Disconnect(result);
            return;
        }

        const auto& updateResults = result.Value();

        THashMap<TString, IFairShareTreeSnapshotPtr> snapshots;
        std::vector<TError> errors;

        for (const auto& [treeId, updateResult] : updateResults) {
            const auto& [snapshot, error] = updateResult;
            snapshots.emplace(treeId, snapshot);
            if (!error.IsOK()) {
                errors.push_back(error);
            }
        }

        {
            TWriterGuard guard(TreeIdToSnapshotLock_);
            std::swap(TreeIdToSnapshot_, snapshots);
            ++SnapshotRevision_;
        }

        if (!errors.empty()) {
            auto error = TError("Found pool configuration issues during fair share update")
                << std::move(errors);
            Host->SetSchedulerAlert(ESchedulerAlertType::UpdateFairShare, error);
        } else {
            Host->SetSchedulerAlert(ESchedulerAlertType::UpdateFairShare, TError());
        }

        YT_LOG_INFO("Fair share successfully updated");
    }

    virtual void OnFairShareEssentialLoggingAt(TInstant now) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        for (const auto& pair : IdToTree_) {
            const auto& tree = pair.second;
            tree->OnFairShareEssentialLoggingAt(now);
        }
    }

    virtual void OnFairShareLoggingAt(TInstant now) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        for (const auto& pair : IdToTree_) {
            const auto& tree = pair.second;
            tree->OnFairShareLoggingAt(now);
        }
    }

    virtual void ProcessJobUpdates(
        const std::vector<TJobUpdate>& jobUpdates,
        std::vector<std::pair<TOperationId, TJobId>>* successfullyUpdatedJobs,
        std::vector<TJobId>* jobsToAbort,
        int* snapshotRevision) override
    {
        VERIFY_THREAD_AFFINITY_ANY();
        YT_VERIFY(successfullyUpdatedJobs->empty());
        YT_VERIFY(jobsToAbort->empty());

        YT_LOG_DEBUG("Processing job updates in strategy (UpdateCount: %v)",
            jobUpdates.size());

        THashMap<TString, IFairShareTreeSnapshotPtr> snapshots;
        {
            TReaderGuard guard(TreeIdToSnapshotLock_);
            snapshots = TreeIdToSnapshot_;
            *snapshotRevision = SnapshotRevision_;
        }

        THashSet<TJobId> jobsToSave;

        for (const auto& job : jobUpdates) {
            switch (job.Status) {
                case EJobUpdateStatus::Running: {
                    auto snapshotIt = snapshots.find(job.TreeId);
                    if (snapshotIt == snapshots.end()) {
                        // Job is orphaned (does not belong to any tree), aborting it.
                        jobsToAbort->push_back(job.JobId);
                    } else {
                        const auto& snapshot = snapshotIt->second;
                        snapshot->ProcessUpdatedJob(job.OperationId, job.JobId, job.Delta);
                    }
                    break;
                }
                case EJobUpdateStatus::Finished: {
                    auto snapshotIt = snapshots.find(job.TreeId);
                    if (snapshotIt == snapshots.end()) {
                        // Job is finished but tree does not exist, nothing to do.
                        continue;
                    }
                    const auto& snapshot = snapshotIt->second;
                    if (snapshot->HasOperation(job.OperationId)) {
                        snapshot->ProcessFinishedJob(job.OperationId, job.JobId);
                    } else if (!job.SnapshotRevision || *job.SnapshotRevision == *snapshotRevision) {
                        jobsToSave.insert(job.JobId);
                    } else {
                        YT_LOG_DEBUG("Dropping finished job (OperationId: %v, JobId: %v)", job.OperationId, job.JobId);
                    }
                    break;
                }
                default:
                    YT_ABORT();
            }
        }

        for (const auto& job : jobUpdates) {
            if (!jobsToSave.contains(job.JobId)) {
                successfullyUpdatedJobs->push_back({job.OperationId, job.JobId});
            }
        }
    }

    virtual void RegisterJobsFromRevivedOperation(TOperationId operationId, const std::vector<TJobPtr>& jobs) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        THashMap<TString, std::vector<TJobPtr>> jobsByTreeId;

        for (const auto& job : jobs) {
            jobsByTreeId[job->GetTreeId()].push_back(job);
        }

        for (const auto& pair : jobsByTreeId) {
            auto tree = FindTree(pair.first);
            // NB: operation can be missing in tree since ban.
            if (tree && tree->HasOperation(operationId)) {
                tree->RegisterJobsFromRevivedOperation(operationId, pair.second);
            }
        }
    }

    virtual void EnableOperation(IOperationStrategyHost* host) override
    {
        auto operationId = host->GetId();
        const auto& state = GetOperationState(operationId);
        for (const auto& pair : state->TreeIdToPoolNameMap()) {
            const auto& treeId = pair.first;
            GetTree(treeId)->EnableOperation(state);
        }
        if (host->IsSchedulable()) {
            state->GetController()->UpdateMinNeededJobResources();
        }
    }

    virtual void ValidateNodeTags(const THashSet<TString>& tags, int* treeCount) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        // Trees this node falls into.
        std::vector<TString> trees;

        for (const auto& pair : IdToTree_) {
            const auto& treeId = pair.first;
            const auto& tree = pair.second;
            if (tree->GetNodesFilter().CanSchedule(tags)) {
                trees.push_back(treeId);
            }
        }

        *treeCount = trees.size();

        if (trees.size() > 1) {
            THROW_ERROR_EXCEPTION("Node belongs to more than one fair-share tree")
                << TErrorAttribute("matched_trees", trees);
        }
    }

private:
    TFairShareStrategyConfigPtr Config;
    ISchedulerStrategyHost* const Host;

    const std::vector<IInvokerPtr> FeasibleInvokers;

    mutable NLogging::TLogger Logger;

    TPeriodicExecutorPtr FairShareProfilingExecutor_;
    TPeriodicExecutorPtr FairShareUpdateExecutor_;
    TPeriodicExecutorPtr FairShareLoggingExecutor_;
    TPeriodicExecutorPtr MinNeededJobResourcesUpdateExecutor_;

    THashMap<TOperationId, TFairShareStrategyOperationStatePtr> OperationIdToOperationState_;

    TFuture<void> ProfilingCompleted_ = VoidFuture;

    using TFairShareTreeMap = THashMap<TString, TFairShareTreePtr>;
    TFairShareTreeMap IdToTree_;

    std::optional<TString> DefaultTreeId_;

    TReaderWriterSpinLock TreeIdToSnapshotLock_;
    THashMap<TString, IFairShareTreeSnapshotPtr> TreeIdToSnapshot_;
    int SnapshotRevision_ = 0;

    struct TPoolTreeDescription
    {
        TString Name;
        bool Tentative;
    };

    TStrategyOperationSpecPtr ParseSpec(const IOperationStrategyHost* operation) const
    {
        try {
            return ConvertTo<TStrategyOperationSpecPtr>(operation->GetSpecString());
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing strategy spec of operation")
                << ex;
        }
    }

    std::vector<TPoolTreeDescription> ParsePoolTrees(const TOperationSpecBasePtr& spec, EOperationType operationType) const
    {
        if (spec->PoolTrees) {
            for (const auto& treeId : *spec->PoolTrees) {
                if (!FindTree(treeId)) {
                    THROW_ERROR_EXCEPTION("Pool tree %Qv not found", treeId);
                }
            }
        }

        THashSet<TString> tentativePoolTrees;
        if (spec->TentativePoolTrees) {
            tentativePoolTrees = *spec->TentativePoolTrees;
        } else if (spec->UseDefaultTentativePoolTrees) {
            tentativePoolTrees = Config->DefaultTentativePoolTrees;
        }

        if (!tentativePoolTrees.empty() && (!spec->PoolTrees || spec->PoolTrees->empty())) {
            THROW_ERROR_EXCEPTION("Regular pool trees must be explicitly specified for tentative pool trees to work properly");
        }

        for (const auto& tentativePoolTree : tentativePoolTrees) {
            if (spec->PoolTrees && spec->PoolTrees->contains(tentativePoolTree)) {
                THROW_ERROR_EXCEPTION("Regular and tentative pool trees must not intersect");
            }
        }

        std::vector<TPoolTreeDescription> result;
        if (spec->PoolTrees) {
            for (const auto& treeName : *spec->PoolTrees) {
                result.push_back(TPoolTreeDescription{
                    .Name = treeName,
                    .Tentative = false
                });
            }
        } else {
            if (!DefaultTreeId_) {
                THROW_ERROR_EXCEPTION("Failed to determine fair-share tree for operation since "
                    "valid pool trees are not specified and default fair-share tree is not configured");
            }
            result.push_back(TPoolTreeDescription{
                .Name = *DefaultTreeId_,
                .Tentative = false
            });
        }

        if (result.empty()) {
            THROW_ERROR_EXCEPTION("No pool trees are specified for operation");
        }

        // Data shuffling shouldn't be launched in tentative trees.
        const auto& noTentativePoolOperationTypes = Config->OperationsWithoutTentativePoolTrees;
        if (noTentativePoolOperationTypes.find(operationType) == noTentativePoolOperationTypes.end()) {
            for (const auto& treeId : tentativePoolTrees) {
                if (FindTree(treeId)) {
                    result.push_back(TPoolTreeDescription{
                        .Name = treeId,
                        .Tentative = true
                    });
                } else {
                    if (!spec->TentativeTreeEligibility->IgnoreMissingPoolTrees) {
                        THROW_ERROR_EXCEPTION("Pool tree %Qv not found", treeId);
                    }
                }
            }
        }

        return result;
    }

    IFairShareTreeSnapshotPtr FindTreeSnapshotByNodeDescriptor(const TExecNodeDescriptor& descriptor) const
    {
        IFairShareTreeSnapshotPtr matchingSnapshot;

        THashMap<TString, IFairShareTreeSnapshotPtr> snapshots;
        {
            TReaderGuard guard(TreeIdToSnapshotLock_);
            snapshots = TreeIdToSnapshot_;
        }

        for (const auto& [treeId, snapshot] : snapshots) {
            if (snapshot->GetNodesFilter().CanSchedule(descriptor.Tags)) {
                // NB: ValidateNodeTags does not guarantee that this check will not success,
                // since node filters of snapshots updated asynchronously.
                if (matchingSnapshot) {
                    // Found second matching snapshot, skip scheduling.
                    YT_LOG_INFO("Node belong to multiple fair-share trees, scheduling skipped (Address: %v)",
                        descriptor.Address);
                    return nullptr;
                }
                matchingSnapshot = snapshot;
            }
        }

        if (!matchingSnapshot) {
            YT_LOG_INFO("Node does not belong to any fair-share tree, scheduling skipped (Address: %v)",
                descriptor.Address);
            return nullptr;
        }

        return matchingSnapshot;
    }

    void ValidateOperationPoolsCanBeUsed(const IOperationStrategyHost* operation, const TOperationRuntimeParametersPtr& runtimeParameters)
    {
        if (IdToTree_.empty()) {
            THROW_ERROR_EXCEPTION("Scheduler strategy does not have configured fair-share trees");
        }

        auto spec = ParseSpec(operation);
        auto pools = GetOperationPools(runtimeParameters);

        if (pools.size() > 1 && !spec->SchedulingTagFilter.IsEmpty()) {
            THROW_ERROR_EXCEPTION(
                "Scheduling tag filter cannot be specified for operations "
                "to be scheduled in multiple fair-share trees");
        }

        std::vector<TFuture<void>> futures;

        for (const auto& pair : pools) {
            auto tree = GetTree(pair.first);
            futures.push_back(tree->ValidateOperationPoolsCanBeUsed(operation, pair.second));
        }

        WaitFor(Combine(futures))
            .ThrowOnError();
    }

    TFairShareStrategyOperationStatePtr FindOperationState(TOperationId operationId) const
    {
        auto it = OperationIdToOperationState_.find(operationId);
        if (it == OperationIdToOperationState_.end()) {
            return nullptr;
        }
        return it->second;
    }

    TFairShareStrategyOperationStatePtr GetOperationState(TOperationId operationId) const
    {
        auto it = OperationIdToOperationState_.find(operationId);
        YT_VERIFY(it != OperationIdToOperationState_.end());
        return it->second;
    }

    TFairShareTreePtr FindTree(const TString& id) const
    {
        auto treeIt = IdToTree_.find(id);
        return treeIt != IdToTree_.end() ? treeIt->second : nullptr;
    }

    TFairShareTreePtr GetTree(const TString& id) const
    {
        auto tree = FindTree(id);
        YT_VERIFY(tree);
        return tree;
    }

    void DoBuildOperationProgress(
        void (TFairShareTree::*method)(TOperationId operationId, TFluentMap fluent),
        TOperationId operationId,
        TFluentMap fluent)
    {
        const auto& state = GetOperationState(operationId);
        const auto& pools = state->TreeIdToPoolNameMap();

        fluent
            .Item("scheduling_info_per_pool_tree")
                .DoMapFor(pools, [&] (TFluentMap fluent, const std::pair<TString, TPoolName>& value) {
                    const auto& treeId = value.first;
                    fluent
                        .Item(treeId).BeginMap()
                            .Do(BIND(method, GetTree(treeId), operationId))
                        .EndMap();
                });
    }

    void ActivateOperations(const std::vector<TOperationId>& operationIds) const
    {
        for (auto operationId : operationIds) {
            const auto& state = GetOperationState(operationId);
            if (!state->GetHost()->GetActivated()) {
                Host->ActivateOperation(operationId);
            }
        }
    }

    void CollectTreesToAddAndRemove(
        const IMapNodePtr& poolsMap,
        THashSet<TString>* treesToAdd,
        THashSet<TString>* treesToRemove) const
    {
        for (const auto& key : poolsMap->GetKeys()) {
            if (IdToTree_.find(key) == IdToTree_.end()) {
                treesToAdd->insert(key);
            }
        }

        for (const auto& pair : IdToTree_) {
            const auto& treeId = pair.first;
            const auto& tree = pair.second;

            auto child = poolsMap->FindChild(treeId);
            if (!child) {
                treesToRemove->insert(treeId);
                continue;
            }

            // Nodes filter update is equivalent to remove-add operation.
            try {
                auto configMap = child->Attributes().ToMap();
                auto config = ConvertTo<TFairShareStrategyTreeConfigPtr>(configMap);

                if (config->NodesFilter != tree->GetNodesFilter()) {
                    treesToRemove->insert(treeId);
                    treesToAdd->insert(treeId);
                }
            } catch (const std::exception&) {
                // Do nothing, alert will be set later.
                continue;
            }
        }
    }

    TFairShareTreeMap ConstructUpdatedTreeMap(
        const IMapNodePtr& poolsMap,
        const THashSet<TString>& treesToAdd,
        const THashSet<TString>& treesToRemove,
        std::vector<TError>* errors) const
    {
        TFairShareTreeMap trees;

        for (const auto& treeId : treesToAdd) {
            TFairShareStrategyTreeConfigPtr treeConfig;
            try {
                auto configMap = poolsMap->GetChild(treeId)->Attributes().ToMap();
                treeConfig = ConvertTo<TFairShareStrategyTreeConfigPtr>(configMap);
            } catch (const std::exception& ex) {
                auto error = TError("Error parsing configuration of tree %Qv", treeId)
                    << ex;
                errors->push_back(error);
                YT_LOG_WARNING(error);
                continue;
            }

            auto tree = New<TFairShareTree>(treeConfig, Config, Host, FeasibleInvokers, treeId);
            trees.emplace(treeId, tree);
        }

        for (const auto& pair : IdToTree_) {
            if (treesToRemove.find(pair.first) == treesToRemove.end()) {
                trees.insert(pair);
            }
        }

        return trees;
    }

    bool CheckTreesConfiguration(const TFairShareTreeMap& trees, std::vector<TError>* errors) const
    {
        THashMap<NNodeTrackerClient::TNodeId, THashSet<TString>> nodeIdToTreeSet;

        for (const auto& pair : trees) {
            const auto& treeId = pair.first;
            const auto& tree = pair.second;
            auto nodes = Host->GetExecNodeIds(tree->GetNodesFilter());

            for (const auto& node : nodes) {
                nodeIdToTreeSet[node].insert(treeId);
            }
        }

        for (const auto& pair : nodeIdToTreeSet) {
            const auto& nodeId = pair.first;
            const auto& treeIds  = pair.second;
            if (treeIds.size() > 1) {
                errors->emplace_back(
                    TError("Cannot update fair-share trees since there is node that belongs to multiple trees")
                        << TErrorAttribute("node_id", nodeId)
                        << TErrorAttribute("matched_trees", treeIds)
                        << TErrorAttribute("node_address", Host->GetExecNodeAddress(nodeId)));
                return false;
            }
        }

        return true;
    }

    void UpdateTreesConfigs(
        const IMapNodePtr& poolsMap,
        const TFairShareTreeMap& trees,
        std::vector<TError>* errors,
        std::vector<TString>* updatedTreeIds) const
    {
        for (const auto& pair : trees) {
            const auto& treeId = pair.first;
            const auto& tree = pair.second;

            auto child = poolsMap->GetChild(treeId);

            try {
                auto configMap = child->Attributes().ToMap();
                auto config = ConvertTo<TFairShareStrategyTreeConfigPtr>(configMap);
                tree->UpdateConfig(config);
            } catch (const std::exception& ex) {
                auto error = TError("Failed to configure tree %Qv, defaults will be used", treeId)
                    << ex;
                errors->push_back(error);
                continue;
            }

            auto updateResult = tree->UpdatePools(child);
            if (!updateResult.Error.IsOK()) {
                errors->push_back(updateResult.Error);
            }
            if (updateResult.Updated) {
                updatedTreeIds->push_back(treeId);
            }
        }
    }

    void AbortOrphanedOperations(const THashSet<TString>& treesToRemove)
    {
        if (treesToRemove.empty()) {
            return;
        }

        THashMap<TOperationId, THashSet<TString>> operationIdToTreeSet;
        THashMap<TString, THashSet<TOperationId>> treeIdToOperationSet;

        for (const auto& pair : OperationIdToOperationState_) {
            auto operationId = pair.first;
            const auto& poolsMap = pair.second->TreeIdToPoolNameMap();

            for (const auto& treeAndPool : poolsMap) {
                const auto& treeId = treeAndPool.first;

                YT_VERIFY(operationIdToTreeSet[operationId].insert(treeId).second);
                YT_VERIFY(treeIdToOperationSet[treeId].insert(operationId).second);
            }
        }

        for (const auto& treeId : treesToRemove) {
            auto it = treeIdToOperationSet.find(treeId);

            // No operations are running in this tree.
            if (it == treeIdToOperationSet.end()) {
                continue;
            }

            // Unregister operations in removed tree and update their tree set.
            for (auto operationId : it->second) {
                const auto& state = GetOperationState(operationId);
                GetTree(treeId)->UnregisterOperation(state);
                YT_VERIFY(state->TreeIdToPoolNameMap().erase(treeId) == 1);

                auto treeSetIt = operationIdToTreeSet.find(operationId);
                YT_VERIFY(treeSetIt != operationIdToTreeSet.end());
                YT_VERIFY(treeSetIt->second.erase(treeId) == 1);
            }
        }

        // Aborting orphaned operations.
        for (const auto& pair : operationIdToTreeSet) {
            auto operationId = pair.first;
            const auto& treeSet = pair.second;
            bool isOperationExists = OperationIdToOperationState_.find(operationId) != OperationIdToOperationState_.end();
            if (treeSet.empty() && isOperationExists) {
                Host->AbortOperation(
                    operationId,
                    TError("No suitable fair-share trees to schedule operation"));
            }
        }
    }

    static void BuildTreeOrchid(
        const TFairShareTreePtr& tree,
        const std::vector<TExecNodeDescriptor>& descriptors,
        TFluentMap fluent)
    {
        TJobResources resourceLimits;
        for (const auto& descriptor : descriptors) {
            resourceLimits += descriptor.ResourceLimits;
        }

        fluent
            .Item("user_to_ephemeral_pools").Do(BIND(&TFairShareTree::BuildUserToEphemeralPoolsInDefaultPool, tree))
            .Item("fair_share_info").BeginMap()
                .Do(BIND(&TFairShareTree::BuildFairShareInfo, tree))
            .EndMap()
            .Do(BIND(&TFairShareTree::BuildOrchid, tree))
            .Item("resource_limits").Value(resourceLimits)
            .Item("node_count").Value(descriptors.size())
            .Item("node_addresses").BeginList()
                .DoFor(descriptors, [&] (TFluentList fluent, const auto& descriptor) {
                    fluent
                        .Item().Value(descriptor.Address);
                })
            .EndList();
    }
};

ISchedulerStrategyPtr CreateFairShareStrategy(
    TFairShareStrategyConfigPtr config,
    ISchedulerStrategyHost* host,
    const std::vector<IInvokerPtr>& feasibleInvokers)
{
    return New<TFairShareStrategy>(config, host, feasibleInvokers);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
