#include "fair_share_strategy.h"
#include "fair_share_tree.h"
#include "fair_share_tree_element.h"
#include "persistent_scheduler_state.h"
#include "public.h"
#include "scheduler_strategy.h"
#include "scheduling_context.h"
#include "scheduling_segment_manager.h"
#include "fair_share_strategy_operation_controller.h"

#include <yt/yt/server/lib/scheduler/config.h>
#include <yt/yt/server/lib/scheduler/helpers.h>
#include <yt/yt/server/lib/scheduler/resource_metering.h>

#include <yt/yt/ytlib/scheduler/job_resources_helpers.h>
#include <yt/yt/ytlib/scheduler/helpers.h>

#include <yt/yt/client/security_client/acl.h>

#include <yt/yt/core/concurrency/async_rw_lock.h>
#include <yt/yt/core/concurrency/periodic_executor.h>
#include <yt/yt/core/concurrency/thread_pool.h>

#include <yt/yt/core/misc/algorithm_helpers.h>
#include <yt/yt/core/misc/finally.h>
#include <yt/yt/core/misc/atomic_object.h>

#include <yt/yt/core/profiling/profile_manager.h>
#include <yt/yt/core/profiling/timing.h>

#include <yt/yt/core/ytree/service_combiner.h>
#include <yt/yt/core/ytree/virtual.h>


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
        std::vector<IInvokerPtr> feasibleInvokers)
        : Config(std::move(config))
        , Host(host)
        , FeasibleInvokers(std::move(feasibleInvokers))
        , Logger(StrategyLogger)
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
            Host->GetFairShareLoggingInvoker(),
            BIND(&TFairShareStrategy::OnFairShareLogging, MakeWeak(this)),
            Config->FairShareLogPeriod);

        AccumulatedUsageLoggingExecutor_ = New<TPeriodicExecutor>(
            Host->GetFairShareLoggingInvoker(),
            BIND(&TFairShareStrategy::OnLogAccumulatedUsage, MakeWeak(this)),
            Config->AccumulatedUsageLogPeriod);

        MinNeededJobResourcesUpdateExecutor_ = New<TPeriodicExecutor>(
            Host->GetControlInvoker(EControlQueue::FairShareStrategy),
            BIND(&TFairShareStrategy::OnMinNeededJobResourcesUpdate, MakeWeak(this)),
            Config->MinNeededResourcesUpdatePeriod);

        ResourceMeteringExecutor_ = New<TPeriodicExecutor>(
            Host->GetControlInvoker(EControlQueue::Metering),
            BIND(&TFairShareStrategy::OnBuildResourceMetering, MakeWeak(this)),
            Config->ResourceMeteringPeriod);

        ResourceUsageUpdateExecutor_ = New<TPeriodicExecutor>(
            Host->GetFairShareUpdateInvoker(),
            BIND(&TFairShareStrategy::OnUpdateResourceUsages, MakeWeak(this)),
            Config->ResourceUsageSnapshotUpdatePeriod);
    }

    void OnUpdateResourceUsages()
    {
        auto idToTree = SnapshottedIdToTree_.Load();
        for (const auto& [_, tree] : idToTree) {
            tree->UpdateResourceUsages();
        }
    }

    void OnMasterHandshake(const TMasterHandshakeResult& result) override
    {
        LastMeteringStatisticsUpdateTime_ = result.LastMeteringLogTime;
        ConnectionTime_ = TInstant::Now();
    }

    void OnMasterConnected() override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        FairShareProfilingExecutor_->Start();
        FairShareUpdateExecutor_->Start();
        FairShareLoggingExecutor_->Start();
        AccumulatedUsageLoggingExecutor_->Start();
        MinNeededJobResourcesUpdateExecutor_->Start();
        ResourceMeteringExecutor_->Start();
        ResourceUsageUpdateExecutor_->Start();
    }

    void OnMasterDisconnected() override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        FairShareProfilingExecutor_->Stop();
        FairShareUpdateExecutor_->Stop();
        FairShareLoggingExecutor_->Stop();
        AccumulatedUsageLoggingExecutor_->Stop();
        MinNeededJobResourcesUpdateExecutor_->Stop();
        ResourceMeteringExecutor_->Stop();
        ResourceUsageUpdateExecutor_->Stop();

        OperationIdToOperationState_.clear();
        SnapshottedIdToTree_.Exchange(THashMap<TString, IFairShareTreePtr>());
        IdToTree_.clear();
        Initialized_ = false;
        DefaultTreeId_.reset();
        NodeIdToDescriptor_.clear();
        NodeAddresses_.clear();
        NodeIdsPerTree_.clear();
        NodeIdsWithoutTree_.clear();
    }

    void OnFairShareProfiling()
    {
        OnFairShareProfilingAt(TInstant::Now());
    }

    void OnFairShareUpdate()
    {
        OnFairShareUpdateAt(TInstant::Now());
    }

    void OnMinNeededJobResourcesUpdate()
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        YT_LOG_INFO("Starting min needed job resources update");

        for (const auto& [operationId, state] : OperationIdToOperationState_) {
            auto maybeUnschedulableReason = state->GetHost()->CheckUnschedulable();
            if (!maybeUnschedulableReason || maybeUnschedulableReason == EUnschedulableReason::NoPendingJobs) {
                state->GetController()->UpdateMinNeededJobResources();
            }
        }

        YT_LOG_INFO("Min needed job resources successfully updated");
    }

    void OnFairShareLogging()
    {
        OnFairShareLoggingAt(TInstant::Now());
    }

    void OnLogAccumulatedUsage()
    {
        VERIFY_INVOKER_AFFINITY(Host->GetFairShareLoggingInvoker());

        TForbidContextSwitchGuard contextSwitchGuard;

        auto idToTree = SnapshottedIdToTree_.Load();
        for (const auto& [_, tree] : idToTree) {
            tree->LogAccumulatedUsage();
        }
    }

    TFuture<void> ScheduleJobs(const ISchedulingContextPtr& schedulingContext) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        const auto& nodeDescriptor = schedulingContext->GetNodeDescriptor();
        auto tree = FindTreeForNode(nodeDescriptor.Address, nodeDescriptor.Tags);
        if (!tree) {
            return VoidFuture;
        }

        return tree->ScheduleJobs(schedulingContext);
    }

    void PreemptJobsGracefully(const ISchedulingContextPtr& schedulingContext) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        const auto& nodeDescriptor = schedulingContext->GetNodeDescriptor();
        auto tree = FindTreeForNode(nodeDescriptor.Address, nodeDescriptor.Tags);
        if (tree) {
            tree->PreemptJobsGracefully(schedulingContext);
        }
    }

    void RegisterOperation(
        IOperationStrategyHost* operation,
        std::vector<TString>* unknownTreeIds,
        TPoolTreeControllerSettingsMap* poolTreeControllerSettingsMap) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        YT_VERIFY(unknownTreeIds->empty());

        auto treeIdToPoolNameMap = GetOperationPools(operation->GetRuntimeParameters());
        for (const auto& [treeId, _] : treeIdToPoolNameMap) {
            if (!FindTree(treeId)) {
                unknownTreeIds->push_back(treeId);
            }
        }
        for (const auto& treeId : *unknownTreeIds) {
            treeIdToPoolNameMap.erase(treeId);
        }
        auto state = New<TFairShareStrategyOperationState>(operation, Config, Host->GetNodeShardInvokers().size());
        state->TreeIdToPoolNameMap() = std::move(treeIdToPoolNameMap);

        YT_VERIFY(OperationIdToOperationState_.insert(
            std::make_pair(operation->GetId(), state)).second);

        auto runtimeParameters = operation->GetRuntimeParameters();
        for (const auto& [treeId, poolName] : state->TreeIdToPoolNameMap()) {
            const auto& treeParams = GetOrCrash(runtimeParameters->SchedulingOptionsPerPoolTree, treeId);
            GetTree(treeId)->RegisterOperation(state, operation->GetStrategySpecForTree(treeId), treeParams);
        }

        for (const auto& [treeName, poolName] : state->TreeIdToPoolNameMap()) {
            auto tree = GetTree(treeName);
            poolTreeControllerSettingsMap->emplace(
                treeName,
                TPoolTreeControllerSettings{
                    .SchedulingTagFilter = tree->GetNodesFilter(),
                    .Tentative = GetSchedulingOptionsPerPoolTree(state->GetHost(), treeName)->Tentative,
                    .Probing = GetSchedulingOptionsPerPoolTree(state->GetHost(), treeName)->Probing,
                    .MainResource = tree->GetConfig()->MainResource,
                });
        }
    }

    void UnregisterOperation(IOperationStrategyHost* operation) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& state = GetOperationState(operation->GetId());
        for (const auto& [treeId, poolName] : state->TreeIdToPoolNameMap()) {
            DoUnregisterOperationFromTree(state, treeId);
        }

        EraseOrCrash(OperationIdToOperationState_, operation->GetId());
    }

    void UnregisterOperationFromTree(TOperationId operationId, const TString& treeId) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& state = GetOperationState(operationId);

        YT_VERIFY(state->TreeIdToPoolNameMap().contains(treeId));

        DoUnregisterOperationFromTree(state, treeId);

        EraseOrCrash(state->TreeIdToPoolNameMap(), treeId);

        YT_LOG_INFO("Operation removed from a tree (OperationId: %v, TreeId: %v)", operationId, treeId);
    }

    void DoUnregisterOperationFromTree(const TFairShareStrategyOperationStatePtr& operationState, const TString& treeId)
    {
        auto tree = GetTree(treeId);
        tree->UnregisterOperation(operationState);
        tree->ProcessActivatableOperations();
    }

    void DisableOperation(IOperationStrategyHost* operation) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto operationId = operation->GetId();
        const auto& state = GetOperationState(operationId);
        for (const auto& [treeId, poolName] : state->TreeIdToPoolNameMap()) {
            if (auto tree = GetTree(treeId);
                tree->HasOperation(operationId))
            {
                tree->DisableOperation(state);
            }
        }
        state->SetEnabled(false);
    }

    void UpdatePoolTrees(const INodePtr& poolTreesNode, const TPersistentStrategyStatePtr& persistentStrategyState) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        std::vector<TOperationId> orphanedOperationIds;
        std::vector<TOperationId> changedOperationIds;
        TError error;
        {
            // No context switches allowed while fair share trees update.
            TForbidContextSwitchGuard contextSwitchGuard;

            YT_LOG_INFO("Updating pool trees");

            if (poolTreesNode->GetType() != NYTree::ENodeType::Map) {
                error = TError(EErrorCode::WatcherHandlerFailed, "Pool trees node has invalid type")
                    << TErrorAttribute("expected_type", NYTree::ENodeType::Map)
                    << TErrorAttribute("actual_type", poolTreesNode->GetType());
                THROW_ERROR(error);
            }

            auto poolsMap = poolTreesNode->AsMap();

            std::vector<TError> errors;

            // Collect trees to add and remove.
            THashSet<TString> treeIdsToAdd;
            THashSet<TString> treeIdsToRemove;
            THashSet<TString> treeIdsWithChangedFilter;
            THashMap<TString, TSchedulingTagFilter> treeIdToFilter;
            CollectTreeChanges(poolsMap, &treeIdsToAdd, &treeIdsToRemove, &treeIdsWithChangedFilter, &treeIdToFilter);

            YT_LOG_INFO("Pool trees collected to update (TreeIdsToAdd: %v, TreeIdsToRemove: %v, TreeIdsWithChangedFilter: %v)",
                treeIdsToAdd,
                treeIdsToRemove,
                treeIdsWithChangedFilter);

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
                error = TError(EErrorCode::WatcherHandlerFailed, "Error updating pool trees")
                    << std::move(errors);
                THROW_ERROR(error);
            }

            // Check that after adding or removing trees each node will belong exactly to one tree.
            // Check is skipped if trees configuration did not change.
            bool shouldCheckConfiguration = !treeIdsToAdd.empty() || !treeIdsToRemove.empty() || !treeIdsWithChangedFilter.empty();

            if (shouldCheckConfiguration && !CheckTreesConfiguration(treeIdToFilter, &errors)) {
                error = TError(EErrorCode::WatcherHandlerFailed, "Error updating pool trees")
                    << std::move(errors);
                THROW_ERROR(error);
            }

            // Update configs and pools structure of all trees.
            // NB: it updates already existing trees inplace.
            std::vector<TString> updatedTreeIds;
            UpdateTreesConfigs(poolsMap, idToTree, &errors, &updatedTreeIds);

            // Update node at scheduler.
            UpdateNodesOnChangedTrees(idToTree, treeIdsToAdd, treeIdsToRemove);

            // Remove trees from operation states.
            RemoveTreesFromOperationStates(treeIdsToRemove, &orphanedOperationIds, &changedOperationIds);

            // Updating default fair-share tree and global tree map.
            DefaultTreeId_ = defaultTreeId;
            std::swap(IdToTree_, idToTree);
            if (!Initialized_) {
                YT_VERIFY(persistentStrategyState);
                InitPersistentStrategyState(persistentStrategyState);
                Initialized_ = true;
            }

            // Setting alerts.
            if (!errors.empty()) {
                error = TError(EErrorCode::WatcherHandlerFailed, "Error updating pool trees")
                    << std::move(errors);
            } else {
                if (!updatedTreeIds.empty() || !treeIdsToRemove.empty() || !treeIdsToAdd.empty()) {
                    Host->LogEventFluently(&SchedulerEventLogger, ELogEventType::PoolsInfo)
                        .Item("pools").DoMapFor(IdToTree_, [&] (TFluentMap fluent, const auto& value) {
                            const auto& treeId = value.first;
                            const auto& tree = value.second;
                            fluent
                                .Item(treeId).Do(BIND(&IFairShareTree::BuildStaticPoolsInformation, tree));
                        });
                }
                YT_LOG_INFO("Pool trees updated");
            }
        }

        // Invokes operation node flushes.
        FlushOperationNodes(changedOperationIds);

        // Perform abort of orphaned operations one by one.
        AbortOrphanedOperations(orphanedOperationIds);

        THROW_ERROR_EXCEPTION_IF_FAILED(error);
    }

    TError UpdateUserToDefaultPoolMap(const THashMap<TString, TString>& userToDefaultPoolMap) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        std::vector<TError> errors;
        for (const auto& [_, tree] : IdToTree_) {
            auto error = tree->ValidateUserToDefaultPoolMap(userToDefaultPoolMap);
            if (!error.IsOK()) {
                errors.push_back(error);
            }
        }

        TError result;
        if (!errors.empty()) {
            result = TError("Error updating mapping from user to default parent pool")
                << std::move(errors);
        } else {
            for (const auto& [_, tree] : IdToTree_) {
                tree->ActualizeEphemeralPoolParents(userToDefaultPoolMap);
            }
        }

        Host->SetSchedulerAlert(ESchedulerAlertType::UpdateUserToDefaultPoolMap, result);
        return result;
    }

    bool IsInitialized() override
    {
        return Initialized_;
    }

    void BuildOperationProgress(TOperationId operationId, TFluentMap fluent) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        if (!FindOperationState(operationId)) {
            return;
        }

        DoBuildOperationProgress(&IFairShareTree::BuildOperationProgress, operationId, fluent);
    }

    void BuildBriefOperationProgress(TOperationId operationId, TFluentMap fluent) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        if (!FindOperationState(operationId)) {
            return;
        }

        DoBuildOperationProgress(&IFairShareTree::BuildBriefOperationProgress, operationId, fluent);
    }

    std::vector<std::pair<TOperationId, TError>> GetHungOperations() override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        std::vector<std::pair<TOperationId, TError>> result;
        for (const auto& [operationId, operationState] : OperationIdToOperationState_) {
            if (operationState->TreeIdToPoolNameMap().empty()) {
                // This operation is orphaned and will be aborted.
                continue;
            }

            bool hasNonHungTree = false;
            TError operationError("Operation scheduling is hanged in all trees");

            for (const auto& treePoolPair : operationState->TreeIdToPoolNameMap()) {
                const auto& treeName = treePoolPair.first;
                auto error = GetTree(treeName)->CheckOperationIsHung(
                    operationId,
                    Config->OperationHangupSafeTimeout,
                    Config->OperationHangupMinScheduleJobAttempts,
                    Config->OperationHangupDeactivationReasons,
                    Config->OperationHangupDueToLimitingAncestorSafeTimeout);
                if (error.IsOK()) {
                    hasNonHungTree = true;
                    break;
                } else {
                    operationError.MutableInnerErrors()->push_back(error);
                }
            }

            if (!hasNonHungTree) {
                result.emplace_back(operationId, operationError);
            }
        }
        return result;
    }

    void UpdateConfig(const TFairShareStrategyConfigPtr& config) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        Config = config;

        for (const auto& [treeId, tree] : IdToTree_) {
            tree->UpdateControllerConfig(Config);
        }

        for (const auto& [operationId, operationState] : OperationIdToOperationState_) {
            operationState->UpdateConfig(Config);
        }

        FairShareProfilingExecutor_->SetPeriod(Config->FairShareProfilingPeriod);
        FairShareUpdateExecutor_->SetPeriod(Config->FairShareUpdatePeriod);
        FairShareLoggingExecutor_->SetPeriod(Config->FairShareLogPeriod);
        AccumulatedUsageLoggingExecutor_->SetPeriod(Config->AccumulatedUsageLogPeriod);
        MinNeededJobResourcesUpdateExecutor_->SetPeriod(Config->MinNeededResourcesUpdatePeriod);
        ResourceMeteringExecutor_->SetPeriod(Config->ResourceMeteringPeriod);
        ResourceUsageUpdateExecutor_->SetPeriod(Config->ResourceUsageSnapshotUpdatePeriod);
    }

    void BuildOperationInfoForEventLog(const IOperationStrategyHost* operation, TFluentMap fluent) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& operationState = GetOperationState(operation->GetId());
        const auto& pools = operationState->TreeIdToPoolNameMap();
        auto accumulatedUsagePerTree = ExtractAccumulatedUsageForLogging(operation->GetId());

        fluent
            .DoIf(DefaultTreeId_.operator bool(), [&] (TFluentMap fluent) {
                auto it = pools.find(*DefaultTreeId_);
                if (it != pools.end()) {
                    fluent
                        .Item("pool").Value(it->second.GetPool());
                }
            })
            .Item("scheduling_info_per_tree").DoMapFor(pools, [&] (TFluentMap fluent, const auto& pair) {
                const auto& [treeId, poolName] = pair;
                auto tree = GetTree(treeId);
                fluent
                    .Item(treeId).BeginMap()
                        .Do(std::bind(&IFairShareTree::BuildOperationAttributes, tree, operation->GetId(), std::placeholders::_1))
                    .EndMap();
            })
            .Item("accumulated_resource_usage_per_tree").Value(accumulatedUsagePerTree);
    }

    void ApplyOperationRuntimeParameters(IOperationStrategyHost* operation) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto state = GetOperationState(operation->GetId());
        const auto runtimeParameters = operation->GetRuntimeParameters();

        auto newPools = GetOperationPools(operation->GetRuntimeParameters());

        YT_VERIFY(newPools.size() == state->TreeIdToPoolNameMap().size());

        for (const auto& [treeId, oldPool] : state->TreeIdToPoolNameMap()) {
            const auto& newPool = GetOrCrash(newPools, treeId);
            auto tree = GetTree(treeId);
            if (oldPool.GetPool() != newPool.GetPool()) {
                tree->ChangeOperationPool(operation->GetId(), state, newPool);
            }

            const auto& treeParams = GetOrCrash(runtimeParameters->SchedulingOptionsPerPoolTree, treeId);
            tree->UpdateOperationRuntimeParameters(operation->GetId(), treeParams);
        }
        state->TreeIdToPoolNameMap() = newPools;
    }

    void InitOperationRuntimeParameters(
        const TOperationRuntimeParametersPtr& runtimeParameters,
        const TOperationSpecBasePtr& spec,
        const TString& user,
        EOperationType operationType) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto poolTrees = ParsePoolTrees(spec, operationType);
        for (const auto& poolTreeDescription : poolTrees) {
            auto treeParams = New<TOperationFairShareTreeRuntimeParameters>();
            auto specIt = spec->SchedulingOptionsPerPoolTree.find(poolTreeDescription.Name);
            if (specIt != spec->SchedulingOptionsPerPoolTree.end()) {
                treeParams->Weight = spec->Weight ? spec->Weight : specIt->second->Weight;
                treeParams->Pool = GetTree(poolTreeDescription.Name)->CreatePoolName(spec->Pool ? spec->Pool : specIt->second->Pool, user);
                treeParams->ResourceLimits = spec->ResourceLimits->IsNonTrivial() ? spec->ResourceLimits : specIt->second->ResourceLimits;
            } else {
                treeParams->Weight = spec->Weight;
                treeParams->Pool = GetTree(poolTreeDescription.Name)->CreatePoolName(spec->Pool, user);
                treeParams->ResourceLimits = spec->ResourceLimits;
            }
            treeParams->Tentative = poolTreeDescription.Tentative;
            treeParams->Probing = poolTreeDescription.Probing;
            YT_VERIFY(runtimeParameters->SchedulingOptionsPerPoolTree.emplace(poolTreeDescription.Name, std::move(treeParams)).second);
        }
    }

    void UpdateRuntimeParameters(
        const TOperationRuntimeParametersPtr& origin,
        const TOperationRuntimeParametersUpdatePtr& update,
        const TString& user) override
    {
        YT_VERIFY(origin);

        for (auto& [poolTree, treeParams] : origin->SchedulingOptionsPerPoolTree) {
            std::optional<TString> newPoolName = update->Pool;
            auto treeUpdateIt = update->SchedulingOptionsPerPoolTree.find(poolTree);
            if (treeUpdateIt != update->SchedulingOptionsPerPoolTree.end()) {
                newPoolName = treeUpdateIt->second->Pool;
                treeParams = UpdateFairShareTreeRuntimeParameters(treeParams, treeUpdateIt->second);
            }

            // NB: root level attributes has higher priority.
            if (update->Weight) {
                treeParams->Weight = *update->Weight;
            }
            if (newPoolName) {
                treeParams->Pool = GetTree(poolTree)->CreatePoolName(*newPoolName, user);
            }
        }
    }

    TFuture<void> ValidateOperationRuntimeParameters(
        IOperationStrategyHost* operation,
        const TOperationRuntimeParametersPtr& runtimeParameters,
        bool validatePools) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& state = GetOperationState(operation->GetId());

        for (const auto& [treeId, schedulingOptions] : runtimeParameters->SchedulingOptionsPerPoolTree) {
            auto poolTrees = state->TreeIdToPoolNameMap();
            if (poolTrees.find(treeId) == poolTrees.end()) {
                THROW_ERROR_EXCEPTION("Pool tree %Qv was not configured for this operation", treeId);
            }
        }

        if (validatePools) {
            return ValidateOperationPoolsCanBeUsed(operation, runtimeParameters);
        } else {
            return VoidFuture;
        }
    }

    void ValidatePoolLimits(
        IOperationStrategyHost* operation,
        const TOperationRuntimeParametersPtr& runtimeParameters) override
    {
        ValidateMaxRunningOperationsCountOnPoolChange(operation, runtimeParameters);

        auto poolLimitViolations = GetPoolLimitViolations(operation, runtimeParameters);
        if (!poolLimitViolations.empty()) {
            THROW_ERROR poolLimitViolations.begin()->second;
        }
    }

    IYPathServicePtr GetOrchidService() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto dynamicOrchidService = New<TCompositeMapService>();
        dynamicOrchidService->AddChild("pool_trees", New<TPoolTreeService>(this));
        return dynamicOrchidService;
    }

    void BuildOrchid(TFluentMap fluent) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        THashMap<TString, std::vector<TExecNodeDescriptor>> descriptorsPerPoolTree;
        for (const auto& [treeId, poolTree] : IdToTree_) {
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

        // Snapshot list of treeIds.
        std::vector<TString> treeIds;
        treeIds.reserve(std::size(IdToTree_));
        for (auto [treeId, _] : IdToTree_) {
            treeIds.push_back(treeId);
        }

        fluent
            // COMAPT(ignat)
            .OptionalItem("default_fair_share_tree", DefaultTreeId_)
            .OptionalItem("default_pool_tree", DefaultTreeId_)
            .Item("last_metering_statistics_update_time").Value(LastMeteringStatisticsUpdateTime_)
            .Item("scheduling_info_per_pool_tree").DoMapFor(treeIds, [&] (TFluentMap fluent, const TString& treeId) {
                auto tree = FindTree(treeId);
                if (!tree) {
                    return;
                }
                // descriptorsPerPoolTree and treeIds are consistent.
                const auto& treeNodeDescriptors = GetOrCrash(descriptorsPerPoolTree, treeId);
                fluent
                    .Item(treeId).BeginMap()
                        .Do(BIND(&TFairShareStrategy::BuildTreeOrchid, MakeStrong(this), tree, treeNodeDescriptors))
                    .EndMap();
            });
    }

    void ApplyJobMetricsDelta(TOperationIdToOperationJobMetrics operationIdToOperationJobMetrics) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        TForbidContextSwitchGuard contextSwitchGuard;

        auto idToTree = SnapshottedIdToTree_.Load();

        THashMap<TString, THashMap<TOperationId, TJobMetrics>> treeIdToJobMetricDeltas;

        for (auto& [operationId, metricsPerTree] : operationIdToOperationJobMetrics) {
            for (auto& metrics : metricsPerTree) {
                auto treeIt = idToTree.find(metrics.TreeId);
                if (treeIt == idToTree.end()) {
                    continue;
                }

                const auto& state = GetOperationState(operationId);
                if (state->GetHost()->IsTreeErased(metrics.TreeId)) {
                    continue;
                }

                treeIdToJobMetricDeltas[metrics.TreeId].emplace(operationId, std::move(metrics.Metrics));
            }
        }

        for (auto& [treeId, jobMetricsPerOperation] : treeIdToJobMetricDeltas) {
            GetOrCrash(idToTree, treeId)->ApplyJobMetricsDelta(std::move(jobMetricsPerOperation));
        }
    }

    TFuture<void> ValidateOperationStart(const IOperationStrategyHost* operation) override
    {
        return ValidateOperationPoolsCanBeUsed(operation, operation->GetRuntimeParameters());
    }

    THashMap<TString, TError> GetPoolLimitViolations(
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

        for (const auto& [treeId, pool] : pools) {
            auto tree = GetTree(treeId);
            tree->ValidatePoolLimitsOnPoolChange(operation, pool);
        }
    }

    void OnFairShareProfilingAt(TInstant /*now*/) override
    {
        VERIFY_INVOKER_AFFINITY(Host->GetFairShareProfilingInvoker());

        TForbidContextSwitchGuard contextSwitchGuard;

        auto idToTree = SnapshottedIdToTree_.Load();
        for (const auto& [treeId, tree] : idToTree) {
            tree->ProfileFairShare();
        }
    }

    // NB: This function is public for testing purposes.
    void OnFairShareUpdateAt(TInstant now) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        YT_LOG_INFO("Starting fair share update");

        std::vector<std::pair<TString, IFairShareTreePtr>> idToTree(IdToTree_.begin(), IdToTree_.end());
        std::sort(
            idToTree.begin(),
            idToTree.end(),
            [] (const auto& lhs, const auto& rhs) {
                return lhs.second->GetOperationCount() > rhs.second->GetOperationCount();
            });

        std::vector<TFuture<std::tuple<TString, TError, IFairShareTreePtr>>> futures;
        for (const auto& [treeId, tree] : idToTree) {
            futures.push_back(tree->OnFairShareUpdateAt(now).Apply(BIND([treeId = treeId] (const std::pair<IFairShareTreePtr, TError>& pair) {
                const auto& [updatedTree, error] = pair;
                return std::make_tuple(treeId, error, updatedTree);
            })));
        }

        auto resultsOrError = WaitFor(AllSucceeded(futures));
        if (!resultsOrError.IsOK()) {
            Host->Disconnect(resultsOrError);
            return;
        }

        if (auto delay = Config->StrategyTestingOptions->DelayInsideFairShareUpdate) {
            TDelayedExecutor::WaitForDuration(*delay);
        }

        THashMap<TString, IFairShareTreePtr> snapshottedIdToTree;
        std::vector<TError> errors;

        const auto& results = resultsOrError.Value();
        for (const auto& [treeId, error, updatedTree] : results) {
            snapshottedIdToTree.emplace(treeId, updatedTree);
            if (!error.IsOK()) {
                errors.push_back(error);
            }
        }

        {
            // NB(eshcherbin): Make sure that snapshotted mapping in strategy and snapshots in trees are updated atomically.
            // This is necessary to maintain consistency between strategy and trees.
            TForbidContextSwitchGuard guard;

            for (const auto& [_, tree] : snapshottedIdToTree) {
                tree->FinishFairShareUpdate();
            }
            SnapshottedIdToTree_.Exchange(std::move(snapshottedIdToTree));
        }

        if (!errors.empty()) {
            auto error = TError("Found pool configuration issues during fair share update")
                << std::move(errors);
            Host->SetSchedulerAlert(ESchedulerAlertType::UpdateFairShare, error);
        } else {
            Host->SetSchedulerAlert(ESchedulerAlertType::UpdateFairShare, TError());
        }

        Host->InvokeStoringStrategyState(BuildStrategyState());

        YT_LOG_INFO("Fair share successfully updated");
    }

    void OnFairShareEssentialLoggingAt(TInstant now) override
    {
        VERIFY_INVOKER_AFFINITY(Host->GetFairShareLoggingInvoker());

        TForbidContextSwitchGuard contextSwitchGuard;

        auto idToTree = SnapshottedIdToTree_.Load();
        for (const auto& [_, tree] : idToTree) {
            tree->EssentialLogFairShareAt(now);
        }
    }

    void OnFairShareLoggingAt(TInstant now) override
    {
        VERIFY_INVOKER_AFFINITY(Host->GetFairShareLoggingInvoker());

        TForbidContextSwitchGuard contextSwitchGuard;

        auto idToTree = SnapshottedIdToTree_.Load();
        for (const auto& [_, tree] : idToTree) {
            tree->LogFairShareAt(now);
        }
    }

    THashMap<TString, TResourceVolume> ExtractAccumulatedUsageForLogging(TOperationId operationId)
    {
        THashMap<TString, TResourceVolume> treeIdToUsage;
        const auto& state = GetOperationState(operationId);
        for (const auto& [treeId, _] : state->TreeIdToPoolNameMap()) {
            treeIdToUsage.emplace(treeId, GetTree(treeId)->ExtractAccumulatedUsageForLogging(operationId));
        }
        return treeIdToUsage;
    }

    void ProcessJobUpdates(
        const std::vector<TJobUpdate>& jobUpdates,
        std::vector<std::pair<TOperationId, TJobId>>* successfullyUpdatedJobs,
        std::vector<TJobId>* jobsToAbort) override
    {
        VERIFY_THREAD_AFFINITY_ANY();
        YT_VERIFY(successfullyUpdatedJobs->empty());
        YT_VERIFY(jobsToAbort->empty());

        YT_LOG_DEBUG("Processing job updates in strategy (UpdateCount: %v)",
            jobUpdates.size());

        auto idToTree = SnapshottedIdToTree_.Load();

        THashSet<TJobId> jobsToPostpone;

        for (const auto& job : jobUpdates) {
            auto treeIt = idToTree.find(job.TreeId);
            switch (job.Status) {
                case EJobUpdateStatus::Running: {
                    if (treeIt == idToTree.end()) {
                        // Job is orphaned (does not belong to any tree), aborting it.
                        jobsToAbort->push_back(job.JobId);
                    } else {
                        const auto& tree = treeIt->second;

                        bool shouldAbortJob = false;
                        tree->ProcessUpdatedJob(
                            job.OperationId,
                            job.JobId,
                            job.JobResources,
                            job.JobDataCenter,
                            job.JobInfinibandCluster,
                            &shouldAbortJob);

                        if (shouldAbortJob) {
                            jobsToAbort->push_back(job.JobId);
                            // NB(eshcherbin): We want the node shard to send us a job finished update,
                            // this is why we have to postpone the job here. This is very ad-hoc, but I hope it'll
                            // soon be rewritten as a part of the new GPU scheduler. See: YT-15062.
                            jobsToPostpone.insert(job.JobId);
                        }
                    }
                    break;
                }
                case EJobUpdateStatus::Finished: {
                    if (treeIt == idToTree.end()) {
                        // Job is finished but tree does not exist, nothing to do.
                        YT_LOG_DEBUG("Dropping job update since pool tree is missing (OperationId: %v, JobId: %v)",
                            job.OperationId,
                            job.JobId);
                        continue;
                    }
                    const auto& tree = treeIt->second;
                    if (!tree->ProcessFinishedJob(job.OperationId, job.JobId)) {
                        YT_LOG_DEBUG("Postpone job update since operation is disabled or missing in snapshot (OperationId: %v, JobId: %v)",
                            job.OperationId,
                            job.JobId);
                        jobsToPostpone.insert(job.JobId);
                    }
                    break;
                }
                default:
                    YT_ABORT();
            }
        }

        for (const auto& job : jobUpdates) {
            if (!jobsToPostpone.contains(job.JobId)) {
                successfullyUpdatedJobs->push_back({job.OperationId, job.JobId});
            }
        }
    }

    void RegisterJobsFromRevivedOperation(TOperationId operationId, const std::vector<TJobPtr>& jobs) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        THashMap<TString, std::vector<TJobPtr>> jobsByTreeId;

        for (const auto& job : jobs) {
            jobsByTreeId[job->GetTreeId()].push_back(job);
        }

        for (const auto& [treeId, jobs] : jobsByTreeId) {
            auto tree = FindTree(treeId);
            // NB: operation can be missing in tree since ban.
            if (tree && tree->HasOperation(operationId)) {
                tree->RegisterJobsFromRevivedOperation(operationId, jobs);
            } else {
                YT_LOG_INFO("Jobs are not registered in tree since operation is missing (OperationId: %v, TreeId: %v)",
                    operationId,
                    treeId);
            }
        }
    }

    void EnableOperation(IOperationStrategyHost* host) override
    {
        auto operationId = host->GetId();
        const auto& state = GetOperationState(operationId);
        state->SetEnabled(true);
        for (const auto& [treeId, poolName] : state->TreeIdToPoolNameMap()) {
            if (auto tree = GetTree(treeId);
                tree->HasRunningOperation(operationId))
            {
                tree->EnableOperation(state);
            }
        }
        auto maybeUnschedulableReason = host->CheckUnschedulable();
        if (!maybeUnschedulableReason || maybeUnschedulableReason == EUnschedulableReason::NoPendingJobs) {
            state->GetController()->UpdateMinNeededJobResources();
        }
    }

    TFuture<void> RegisterOrUpdateNode(
        TNodeId nodeId,
        const TString& nodeAddress,
        const TBooleanFormulaTags& tags) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return BIND(&TFairShareStrategy::DoRegisterOrUpdateNode, MakeStrong(this))
            .AsyncVia(Host->GetControlInvoker(EControlQueue::NodeTracker))
            .Run(nodeId, nodeAddress, tags);
    }

    void UnregisterNode(TNodeId nodeId, const TString& nodeAddress) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        Host->GetControlInvoker(EControlQueue::NodeTracker)->Invoke(
            BIND([this, this_ = MakeStrong(this), nodeId, nodeAddress] {
                // NOTE: If node is unregistered from node shard before it becomes online
                // then its id can be missing in the map.
                auto it = NodeIdToDescriptor_.find(nodeId);
                if (it == NodeIdToDescriptor_.end()) {
                    YT_LOG_WARNING("Node is not registered at strategy (Address: %v)", nodeAddress);
                } else {
                    if (auto treeId = it->second.TreeId) {
                        auto& treeNodeIds = GetOrCrash(NodeIdsPerTree_, *treeId);
                        EraseOrCrash(treeNodeIds, nodeId);
                    }

                    EraseOrCrash(NodeAddresses_, nodeAddress);
                    NodeIdToDescriptor_.erase(it);

                    YT_LOG_INFO("Node unregistered from strategy (Address: %v)", nodeAddress);
                }
                NodeIdsWithoutTree_.erase(nodeId);
            }));
    }

    const THashMap<TString, THashSet<TNodeId>>& GetNodeIdsPerTree() const override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        return NodeIdsPerTree_;
    }

    TString ChooseBestSingleTreeForOperation(TOperationId operationId, TJobResources newDemand) override
    {
        auto idToTree = SnapshottedIdToTree_.Load();

        // NB(eshcherbin):
        // First, we ignore all trees in which the new operation is not marked running.
        // Then for every candidate pool we model the case if the new operation is assigned to it:
        // 1) We add the pool's current demand share and the operation's demand share to get the model demand share.
        // 2) We calculate reserveShare, defined as (promisedFairShare - modelDemandShare).
        // Finally, we choose the pool with the maximum of MinComponent(reserveShare) over all trees.
        // More precisely, we compute MinComponent ignoring the resource types which are absent in the tree (e.g. GPU).
        TString bestTree;
        auto bestReserveRatio = std::numeric_limits<double>::lowest();
        std::vector<TString> emptyTrees;
        for (const auto& [treeId, poolName] : GetOperationState(operationId)->TreeIdToPoolNameMap()) {
            YT_VERIFY(idToTree.contains(treeId));
            auto tree = idToTree[treeId];

            if (!tree->IsSnapshottedOperationRunningInTree(operationId)) {
                continue;
            }

            auto totalResourceLimits = tree->GetSnapshottedTotalResourceLimits();
            if (totalResourceLimits == TJobResources()) {
                emptyTrees.push_back(treeId);
                continue;
            }

            // If pool is not present in the tree (e.g. due to poor timings or if it is an ephemeral pool),
            // then its demand and guaranteed resources ratio are considered to be zero.
            TResourceVector currentDemandShare;
            TResourceVector promisedFairShare;
            if (auto poolStateSnapshot = tree->GetMaybeStateSnapshotForPool(poolName.GetPool())) {
                currentDemandShare = poolStateSnapshot->DemandShare;
                promisedFairShare = poolStateSnapshot->PromisedFairShare;
            }

            auto newDemandShare = TResourceVector::FromJobResources(newDemand, totalResourceLimits);
            auto modelDemandShare = newDemandShare + currentDemandShare;
            auto reserveShare = promisedFairShare - modelDemandShare;

            // TODO(eshcherbin): Perhaps we need to add a configurable main resource for each tree and compare the shares of this resource.
            auto currentReserveRatio = std::numeric_limits<double>::max();
            #define XX(name, Name) \
                if (totalResourceLimits.Get##Name() > 0) { \
                    currentReserveRatio = std::min(currentReserveRatio, reserveShare[EJobResourceType::Name]); \
                }
            ITERATE_JOB_RESOURCES(XX)
            #undef XX

            // TODO(eshcherbin): This is rather verbose. Consider removing when well tested in production.
            YT_LOG_DEBUG(
                "Considering candidate single tree for operation ("
                "OperationId: %v, TreeId: %v, TotalResourceLimits: %v, "
                "NewDemandShare: %.6g, CurrentDemandShare: %.6g, ModelDemandShare: %.6g, "
                "PromisedFairShare: %.6g, ReserveShare: %.6g, CurrentReserveRatio: %v)",
                operationId,
                treeId,
                FormatResources(totalResourceLimits),
                newDemandShare,
                currentDemandShare,
                modelDemandShare,
                promisedFairShare,
                reserveShare,
                currentReserveRatio);

            if (currentReserveRatio > bestReserveRatio) {
                bestTree = treeId;
                bestReserveRatio = currentReserveRatio;
            }
        }

        YT_VERIFY(bestTree || !emptyTrees.empty());

        if (bestTree) {
            YT_LOG_DEBUG("Chose best single tree for operation (OperationId: %v, BestTree: %v, BestReserveRatio: %v, EmptyCandidateTrees: %v)",
                operationId,
                bestTree,
                bestReserveRatio,
                emptyTrees);
        } else {
            bestTree = emptyTrees[0];

            YT_LOG_DEBUG(
                "Found no best single non-empty tree for operation; choosing first found empty tree"
                "(OperationId: %v, BestTree: %v, EmptyCandidateTrees: %v)",
                operationId,
                bestTree,
                emptyTrees);
        }

        return bestTree;
    }

    TError InitOperationSchedulingSegment(TOperationId operationId) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto state = GetOperationState(operationId);

        bool hasModuleAwareSegment = false;
        for (const auto& [treeId, _] : state->TreeIdToPoolNameMap()) {
            auto segment = GetTree(treeId)->InitOperationSchedulingSegment(operationId);
            hasModuleAwareSegment |= IsModuleAwareSchedulingSegment(segment);
        }

        if (hasModuleAwareSegment && state->TreeIdToPoolNameMap().size() > 1) {
            std::vector<TString> treeIds;
            for (const auto& [treeId, _] : state->TreeIdToPoolNameMap()) {
                treeIds.push_back(treeId);
            }

            return TError(
                "Scheduling in several trees is forbidden for operations in module-aware scheduling segments, "
                "specify a single tree or use the \"schedule_in_single_tree\" spec option")
                << TErrorAttribute("tree_ids", treeIds);
        }

        return TError();
    }

    TStrategySchedulingSegmentsState GetStrategySchedulingSegmentsState() const override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        TStrategySchedulingSegmentsState result;
        for (const auto& [treeId, tree] : IdToTree_) {
            YT_VERIFY(result.TreeStates.emplace(treeId, tree->GetSchedulingSegmentsState()).second);
        }

        return result;
    }

    THashMap<TString, TOperationIdWithSchedulingSegmentModuleList> GetOperationSchedulingSegmentModuleUpdates() const override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        THashMap<TString, TOperationIdWithSchedulingSegmentModuleList> result;
        for (const auto& [treeId, tree] : IdToTree_) {
            auto updates = tree->GetOperationSchedulingSegmentModuleUpdates();
            if (!updates.empty()) {
                YT_VERIFY(result.emplace(treeId, std::move(updates)).second);
            }
        }

        return result;
    }

    void ScanPendingOperations() override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        for (const auto& [_, tree] : IdToTree_) {
            tree->TryRunAllPendingOperations();
        }
    }

    TFuture<void> GetFullFairShareUpdateFinished() override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        return FairShareUpdateExecutor_->GetExecutedEvent();
    }

    void OnBuildResourceMetering()
    {
        DoBuildResourceMeteringAt(TInstant::Now());
    }

    TPersistentStrategyStatePtr BuildStrategyState()
    {
        auto result = New<TPersistentStrategyState>();
        for (const auto& [treeId, tree] : IdToTree_) {
            result->TreeStates[treeId] = tree->BuildPersistentTreeState();
        }
        return result;
    }

    TCachedJobPreemptionStatuses GetCachedJobPreemptionStatusesForNode(
        const TString& nodeAddress,
        const TBooleanFormulaTags& nodeTags) const override
    {
        if (auto tree = FindTreeForNode(nodeAddress, nodeTags)) {
            return tree->GetCachedJobPreemptionStatuses();
        }
        return {};
    }

private:
    class TPoolTreeService;
    friend class TPoolTreeService;

    TFairShareStrategyConfigPtr Config;
    ISchedulerStrategyHost* const Host;

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

    const std::vector<IInvokerPtr> FeasibleInvokers;

    mutable NLogging::TLogger Logger;

    TPeriodicExecutorPtr FairShareProfilingExecutor_;
    TPeriodicExecutorPtr FairShareUpdateExecutor_;
    TPeriodicExecutorPtr FairShareLoggingExecutor_;
    TPeriodicExecutorPtr AccumulatedUsageLoggingExecutor_;
    TPeriodicExecutorPtr MinNeededJobResourcesUpdateExecutor_;
    TPeriodicExecutorPtr ResourceMeteringExecutor_;
    TPeriodicExecutorPtr ResourceUsageUpdateExecutor_;

    THashMap<TOperationId, TFairShareStrategyOperationStatePtr> OperationIdToOperationState_;

    using TFairShareTreeMap = THashMap<TString, IFairShareTreePtr>;
    TFairShareTreeMap IdToTree_;
    bool Initialized_ = false;

    std::optional<TString> DefaultTreeId_;

    // NB(eshcherbin): Note that these fair share tree mapping are only *snapshot* of actual mapping.
    // We should not expect that the set of trees or their structure in the snapshot are the same as
    // in the current |IdToTree_| map. Snapshots could be a little bit behind.
    TAtomicObject<THashMap<TString, IFairShareTreePtr>> SnapshottedIdToTree_;

    TInstant ConnectionTime_;
    TInstant LastMeteringStatisticsUpdateTime_;
    TMeteringMap MeteringStatistics_;

    struct TStrategyExecNodeDescriptor
    {
        TBooleanFormulaTags Tags;
        TString Address;
        std::optional<TString> TreeId;
    };

    THashMap<TNodeId, TStrategyExecNodeDescriptor> NodeIdToDescriptor_;
    THashSet<TString> NodeAddresses_;
    THashMap<TString, THashSet<TNodeId>> NodeIdsPerTree_;
    THashSet<TNodeId> NodeIdsWithoutTree_;

    struct TPoolTreeDescription
    {
        TString Name;
        bool Tentative = false;
        bool Probing = false;
    };

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
                result.push_back(TPoolTreeDescription{ .Name = treeName });
            }
        } else {
            if (!DefaultTreeId_) {
                THROW_ERROR_EXCEPTION(
                    NScheduler::EErrorCode::PoolTreesAreUnspecified,
                    "Failed to determine fair-share tree for operation since "
                    "valid pool trees are not specified and default fair-share tree is not configured");
            }
            result.push_back(TPoolTreeDescription{ .Name = *DefaultTreeId_ });
        }

        if (result.empty()) {
            THROW_ERROR_EXCEPTION(
                NScheduler::EErrorCode::PoolTreesAreUnspecified,
                "No pool trees are specified for operation");
        }

        // Data shuffling shouldn't be launched in tentative trees.
        for (const auto& treeId : tentativePoolTrees) {
            if (auto tree = FindTree(treeId)) {
                auto nonTentativeOperationTypesInTree = tree->GetConfig()->NonTentativeOperationTypes;
                const auto& noTentativePoolOperationTypes = nonTentativeOperationTypesInTree
                    ? *nonTentativeOperationTypesInTree
                    : Config->OperationsWithoutTentativePoolTrees;
                if (noTentativePoolOperationTypes.find(operationType) == noTentativePoolOperationTypes.end()) {
                    result.push_back(TPoolTreeDescription{
                        .Name = treeId,
                        .Tentative = true
                    });
                }
            } else {
                if (!spec->TentativeTreeEligibility->IgnoreMissingPoolTrees) {
                    THROW_ERROR_EXCEPTION("Pool tree %Qv not found", treeId);
                }
            }
        }

        if (spec->ProbingPoolTree) {
            for (const auto& desc : result) {
                if (desc.Name == *spec->ProbingPoolTree) {
                    THROW_ERROR_EXCEPTION("Probing pool tree must not be in regular or tentative pool tree lists")
                        << TErrorAttribute("pool_tree", desc.Name)
                        << TErrorAttribute("is_tentative", desc.Tentative);
                }
            }

            if (auto tree = FindTree(spec->ProbingPoolTree.value())) {
                result.push_back(TPoolTreeDescription{
                    .Name = *spec->ProbingPoolTree,
                    .Probing = true
                });
            } else {
                THROW_ERROR_EXCEPTION("Probing pool tree %Qv not found", spec->ProbingPoolTree.value());
            }
        }

        return result;
    }

    IFairShareTreePtr FindTreeForNode(const TString& nodeAddress, const TBooleanFormulaTags& nodeTags) const
    {
        IFairShareTreePtr matchingTree;

        auto idToTree = SnapshottedIdToTree_.Load();
        for (const auto& [treeId, tree] : idToTree) {
            if (tree->GetSnapshottedConfig()->NodesFilter.CanSchedule(nodeTags)) {
                if (matchingTree) {
                    // Found second matching tree, skip scheduling.
                    YT_LOG_INFO("Node belong to multiple fair-share trees (Address: %v)",
                        nodeAddress);
                    return nullptr;
                }
                matchingTree = tree;
            }
        }

        if (!matchingTree) {
            YT_LOG_INFO("Node does not belong to any fair-share tree (Address: %v)",
                nodeAddress);
            return nullptr;
        }

        return matchingTree;
    }

    TFuture<void> ValidateOperationPoolsCanBeUsed(const IOperationStrategyHost* operation, const TOperationRuntimeParametersPtr& runtimeParameters)
    {
        if (IdToTree_.empty()) {
            THROW_ERROR_EXCEPTION("Scheduler strategy does not have configured fair-share trees");
        }

        auto spec = operation->GetStrategySpec();
        auto pools = GetOperationPools(runtimeParameters);

        if (pools.size() > 1 && !spec->SchedulingTagFilter.IsEmpty()) {
            THROW_ERROR_EXCEPTION(
                "Scheduling tag filter cannot be specified for operations "
                "to be scheduled in multiple fair-share trees");
        }

        for (const auto& [_, poolName]: pools) {
            ValidatePoolName(poolName.GetSpecifiedPoolName());
        }

        std::vector<TFuture<void>> futures;

        for (const auto& [treeId, pool] : pools) {
            auto tree = GetTree(treeId);
            futures.push_back(tree->ValidateOperationPoolsCanBeUsed(operation, pool));
        }

        return AllSucceeded(futures);
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
        return GetOrCrash(OperationIdToOperationState_, operationId);
    }

    IFairShareTreePtr FindTree(const TString& id) const
    {
        auto treeIt = IdToTree_.find(id);
        return treeIt != IdToTree_.end() ? treeIt->second : nullptr;
    }

    IFairShareTreePtr GetTree(const TString& id) const
    {
        auto tree = FindTree(id);
        YT_VERIFY(tree);
        return tree;
    }

    void DoBuildOperationProgress(
        void (IFairShareTree::*method)(TOperationId operationId, TFluentMap fluent) const,
        TOperationId operationId,
        TFluentMap fluent)
    {
        const auto& state = GetOperationState(operationId);
        const auto& pools = state->TreeIdToPoolNameMap();

        fluent
            .Item("scheduling_info_per_pool_tree")
                .DoMapFor(pools, [&] (TFluentMap fluent, const std::pair<TString, TPoolName>& value) {
                    const auto& treeId = value.first;
                    auto tree = GetTree(treeId);

                    fluent
                        .Item(treeId).BeginMap()
                            .Do(BIND(method, tree, operationId))
                        .EndMap();
                });
    }

    void OnOperationRunningInTree(IFairShareTree* tree, TOperationId operationId) const
    {
        YT_VERIFY(tree->HasRunningOperation(operationId));

        auto state = GetOperationState(operationId);
        Host->MarkOperationAsRunningInStrategy(operationId);

        if (state->GetEnabled()) {
            tree->EnableOperation(state);
        }
    }

    TFairShareStrategyTreeConfigPtr ParsePoolTreeConfig(const INodePtr& poolTreeNode, const INodePtr& commonConfig) const
    {
        const auto& attributes = poolTreeNode->Attributes();
        auto ysonConfig = attributes.FindYson(TreeConfigAttributeName);

        if (!commonConfig) {
            return ysonConfig
                ? ConvertTo<TFairShareStrategyTreeConfigPtr>(ysonConfig)
                : ConvertTo<TFairShareStrategyTreeConfigPtr>(attributes.ToMap());
        }

        return ysonConfig
            ? ConvertTo<TFairShareStrategyTreeConfigPtr>(PatchNode(commonConfig, ConvertToNode(ysonConfig)))
            : ConvertTo<TFairShareStrategyTreeConfigPtr>(PatchNode(commonConfig, attributes.ToMap()));
    }

    TFairShareStrategyTreeConfigPtr BuildConfig(const IMapNodePtr& poolTreesMap, const TString& treeId) const
    {
        struct TPoolTreesTemplateConfigInfoView
        {
            TStringBuf name;
            const TPoolTreesTemplateConfig* config;
        };

        const auto& poolTreeAttributes = poolTreesMap->GetChildOrThrow(treeId);

        std::vector<TPoolTreesTemplateConfigInfoView> matchedTemplateConfigs;

        for (const auto& [name, value] : Config->TemplatePoolTreeConfigMap) {
            if (value->Filter && NRe2::TRe2::FullMatch(treeId.data(), *value->Filter)) {
                matchedTemplateConfigs.push_back({name, value.Get()});
            }
        }

        if (matchedTemplateConfigs.empty()) {
            return ParsePoolTreeConfig(poolTreeAttributes, /* commonConfig */ nullptr);
        }

        std::sort(
            std::begin(matchedTemplateConfigs),
            std::end(matchedTemplateConfigs),
            [] (const TPoolTreesTemplateConfigInfoView first, const TPoolTreesTemplateConfigInfoView second) {
                return first.config->Priority < second.config->Priority;
            });

        INodePtr compiledConfig = GetEphemeralNodeFactory()->CreateMap();
        for (const auto& config : matchedTemplateConfigs) {
            compiledConfig = PatchNode(compiledConfig, config.config->Config);
        }

        return ParsePoolTreeConfig(poolTreeAttributes, compiledConfig);
    }

    void CollectTreeChanges(
        const IMapNodePtr& poolsMap,
        THashSet<TString>* treesToAdd,
        THashSet<TString>* treesToRemove,
        THashSet<TString>* treeIdsWithChangedFilter,
        THashMap<TString, TSchedulingTagFilter>* treeIdToFilter) const
    {
        for (const auto& key : poolsMap->GetKeys()) {
            if (IdToTree_.find(key) == IdToTree_.end()) {
                treesToAdd->insert(key);
                try {
                    auto config = ParsePoolTreeConfig(poolsMap->FindChild(key), /* commonConfig */ nullptr);
                    treeIdToFilter->emplace(key, config->NodesFilter);
                } catch (const std::exception&) {
                    // Do nothing, alert will be set later.
                    continue;
                }
            }
        }

        for (const auto& [treeId, tree] : IdToTree_) {
            auto child = poolsMap->FindChild(treeId);
            if (!child) {
                treesToRemove->insert(treeId);
                continue;
            }

            try {
                auto config = ParsePoolTreeConfig(child, /* commonConfig */ nullptr);
                treeIdToFilter->emplace(treeId, config->NodesFilter);

                if (config->NodesFilter != tree->GetNodesFilter()) {
                    treeIdsWithChangedFilter->insert(treeId);
                }
            } catch (const std::exception&) {
                // Do nothing, alert will be set later.
                continue;
            }
        }
    }

    TFairShareTreeMap ConstructUpdatedTreeMap(
        const IMapNodePtr& poolTreesMap,
        const THashSet<TString>& treesToAdd,
        const THashSet<TString>& treesToRemove,
        std::vector<TError>* errors)
    {
        TFairShareTreeMap trees;

        for (const auto& treeId : treesToAdd) {
            TFairShareStrategyTreeConfigPtr treeConfig;
            try {
                treeConfig = BuildConfig(poolTreesMap, treeId);
            } catch (const std::exception& ex) {
                auto error = TError("Error parsing configuration of tree %Qv", treeId)
                    << ex;
                errors->push_back(error);
                YT_LOG_WARNING(error);
                continue;
            }

            auto tree = CreateFairShareTree(treeConfig, Config, Host, FeasibleInvokers, treeId);
            tree->SubscribeOperationRunning(BIND(
                &TFairShareStrategy::OnOperationRunningInTree,
                Unretained(this),
                Unretained(tree.Get())));

            trees.emplace(treeId, tree);
        }

        for (const auto& [treeId, tree] : IdToTree_) {
            if (treesToRemove.find(treeId) == treesToRemove.end()) {
                trees.emplace(treeId, tree);
            }
        }

        return trees;
    }

    bool CheckTreesConfiguration(const THashMap<TString, TSchedulingTagFilter>& treeIdToFilter, std::vector<TError>* errors) const
    {
        THashMap<TNodeId, std::vector<TString>> nodeIdToTreeSet;
        for (const auto& [nodeId, descriptor] : NodeIdToDescriptor_) {
            for (const auto& [treeId, filter] : treeIdToFilter) {
                if (filter.CanSchedule(descriptor.Tags)) {
                    nodeIdToTreeSet[nodeId].push_back(treeId);
                }
            }
        }

        for (const auto& [nodeId, treeIds] : nodeIdToTreeSet) {
            if (treeIds.size() > 1) {
                errors->emplace_back(
                    TError("Cannot update fair-share trees since there is node that belongs to multiple trees")
                        << TErrorAttribute("node_id", nodeId)
                        << TErrorAttribute("matched_trees", treeIds)
                        << TErrorAttribute("node_address", GetOrCrash(NodeIdToDescriptor_, nodeId).Address));
                return false;
            }
        }

        return true;
    }

    void UpdateTreesConfigs(
        const IMapNodePtr& poolTreesMap,
        const TFairShareTreeMap& trees,
        std::vector<TError>* errors,
        std::vector<TString>* updatedTreeIds) const
    {
        for (const auto& [treeId, tree] : trees) {
            auto child = poolTreesMap->GetChildOrThrow(treeId);

            bool treeConfigChanged = false;
            try {
                const auto& config = BuildConfig(poolTreesMap, treeId);
                treeConfigChanged = tree->UpdateConfig(config);
            } catch (const std::exception& ex) {
                auto error = TError("Failed to configure tree %Qv, defaults will be used", treeId)
                    << ex;
                errors->push_back(error);
                continue;
            }

            auto updateResult = tree->UpdatePools(child, treeConfigChanged);
            if (!updateResult.Error.IsOK()) {
                errors->push_back(updateResult.Error);
            }
            if (updateResult.Updated) {
                updatedTreeIds->push_back(treeId);
            }
        }
    }

    void RemoveTreesFromOperationStates(
        const THashSet<TString>& treesToRemove,
        std::vector<TOperationId>* orphanedOperationIds,
        std::vector<TOperationId>* operationsToFlush)
    {
        if (treesToRemove.empty()) {
            return;
        }

        THashMap<TOperationId, THashSet<TString>> operationIdToTreeSet;
        THashMap<TString, THashSet<TOperationId>> treeIdToOperationSet;

        for (const auto& [operationId, operationState] : OperationIdToOperationState_) {
            const auto& poolsMap = operationState->TreeIdToPoolNameMap();
            for (const auto& [treeId, pool] : poolsMap) {
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
                EraseOrCrash(state->TreeIdToPoolNameMap(), treeId);

                auto& treeSet = GetOrCrash(operationIdToTreeSet, operationId);
                EraseOrCrash(treeSet, treeId);
            }
        }

        for (const auto& [operationId, treeSet] : operationIdToTreeSet) {
            const auto& state = GetOperationState(operationId);

            std::vector<TString> treeIdsToErase;
            for (auto [treeId, options] : state->GetHost()->GetRuntimeParameters()->SchedulingOptionsPerPoolTree) {
                if (treeSet.find(treeId) == treeSet.end()) {
                    treeIdsToErase.push_back(treeId);
                }
            }

            if (!treeIdsToErase.empty()) {
                YT_LOG_INFO("Removing operation from deleted trees (OperationId: %v, TreeIds: %v)",
                    operationId,
                    treeIdsToErase);

                state->GetHost()->EraseTrees(treeIdsToErase);
                operationsToFlush->push_back(operationId);
            }

            if (treeSet.empty()) {
                orphanedOperationIds->push_back(operationId);
            }
        }
    }

    void AbortOrphanedOperations(const std::vector<TOperationId>& operationIds)
    {
        for (auto operationId : operationIds) {
            if (OperationIdToOperationState_.find(operationId) != OperationIdToOperationState_.end()) {
                Host->AbortOperation(
                    operationId,
                    TError("No suitable fair-share trees to schedule operation"));
            }
        }
    }

    void FlushOperationNodes(const std::vector<TOperationId>& operationIds)
    {
        for (auto operationId : operationIds) {
            Host->FlushOperationNode(operationId);
        }
    }

    void DoRegisterOrUpdateNode(
        TNodeId nodeId,
        const TString& nodeAddress,
        const TBooleanFormulaTags& tags)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        std::vector<TString> treeIds;
        for (const auto& [treeId, tree] : IdToTree_) {
            if (tree->GetNodesFilter().CanSchedule(tags)) {
                treeIds.push_back(treeId);
            }
        }

        std::optional<TString> treeId;
        if (treeIds.size() == 0) {
            NodeIdsWithoutTree_.insert(nodeId);
        } else if (treeIds.size() == 1) {
            NodeIdsWithoutTree_.erase(nodeId);
            treeId = treeIds[0];
        } else {
            THROW_ERROR_EXCEPTION("Node belongs to more than one fair share tree")
                    << TErrorAttribute("matched_pool_trees", treeIds);
        }

        auto it = NodeIdToDescriptor_.find(nodeId);
        if (it == NodeIdToDescriptor_.end()) {
            THROW_ERROR_EXCEPTION_IF(NodeAddresses_.contains(nodeAddress),
                "Duplicate node address found (Address: %v, NewNodeId: %v)",
                nodeAddress,
                nodeId);

            EmplaceOrCrash(
                NodeIdToDescriptor_,
                nodeId,
                TStrategyExecNodeDescriptor{
                    .Tags = tags,
                    .Address = nodeAddress,
                    .TreeId = treeId,
                });
            NodeAddresses_.insert(nodeAddress);

            if (treeId) {
                auto& treeNodeIds = GetOrCrash(NodeIdsPerTree_, *treeId);
                InsertOrCrash(treeNodeIds, nodeId);
            }

            YT_LOG_INFO("Node is registered at strategy (NodeId: %v, Address: %v, Tags: %v, TreeId: %v)",
                nodeId,
                nodeAddress,
                tags,
                treeId);
        } else {
            auto& currentDescriptor = it->second;
            if (treeId != currentDescriptor.TreeId) {
                OnNodeChangedFairShareTree(nodeId, treeId);
            }

            currentDescriptor.Tags = tags;
            currentDescriptor.Address = nodeAddress;

            YT_LOG_INFO("Node was updated at scheduler (NodeId: %v, Address: %v, Tags: %v, TreeId: %v)",
                nodeId,
                nodeAddress,
                tags,
                treeId);
        }

        ProcessNodesWithoutPoolTreeAlert();
    }

    void UpdateNodesOnChangedTrees(
        const TFairShareTreeMap& idToTree,
        const THashSet<TString> treeIdsToAdd,
        const THashSet<TString> treeIdsToRemove)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        for (const auto& treeId : treeIdsToAdd) {
            EmplaceOrCrash(NodeIdsPerTree_, treeId, THashSet<TNodeId>{});
        }

        // NB(eshcherbin): |OnNodeChangedFairShareTree| requires both trees to be present in |NodeIdsPerTree_|.
        // This is why we add new trees before this cycle and remove old trees after it.
        for (const auto& [nodeId, descriptor] : NodeIdToDescriptor_) {
            std::optional<TString> newTreeId;
            for (const auto& [treeId, tree] : idToTree) {
                if (tree->GetNodesFilter().CanSchedule(descriptor.Tags)) {
                    YT_VERIFY(!newTreeId);
                    newTreeId = treeId;
                }
            }
            if (newTreeId) {
                NodeIdsWithoutTree_.erase(nodeId);
            } else {
                NodeIdsWithoutTree_.insert(nodeId);
            }
            if (newTreeId != descriptor.TreeId) {
                OnNodeChangedFairShareTree(nodeId, newTreeId);
            }
        }

        for (const auto& treeId : treeIdsToRemove) {
            const auto& treeNodeIds = GetOrCrash(NodeIdsPerTree_, treeId);
            YT_VERIFY(treeNodeIds.empty());
            NodeIdsPerTree_.erase(treeId);
        }

        ProcessNodesWithoutPoolTreeAlert();
    }

    void OnNodeChangedFairShareTree(
        TNodeId nodeId,
        const std::optional<TString>& newTreeId)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto it = NodeIdToDescriptor_.find(nodeId);
        YT_VERIFY(it != NodeIdToDescriptor_.end());

        auto& currentDescriptor = it->second;
        YT_VERIFY(newTreeId != currentDescriptor.TreeId);

        YT_LOG_INFO("Node has changed pool tree (NodeId: %v, Address: %v, OldTreeId: %v, NewTreeId: %v)",
            nodeId,
            currentDescriptor.Address,
            currentDescriptor.TreeId,
            newTreeId);

        if (auto oldTreeId = currentDescriptor.TreeId) {
            auto& oldTreeNodeIds = GetOrCrash(NodeIdsPerTree_, *oldTreeId);
            EraseOrCrash(oldTreeNodeIds, nodeId);
        }
        if (newTreeId) {
            auto& newTreeNodeIds = GetOrCrash(NodeIdsPerTree_, *newTreeId);
            InsertOrCrash(newTreeNodeIds, nodeId);
        }

        currentDescriptor.TreeId = newTreeId;

        Host->AbortJobsAtNode(nodeId, EAbortReason::NodeFairShareTreeChanged);
    }

    void ProcessNodesWithoutPoolTreeAlert()
    {
        if (NodeIdsWithoutTree_.empty()) {
            Host->SetSchedulerAlert(ESchedulerAlertType::NodesWithoutPoolTree, TError());
            return;
        }

        std::vector<TString> nodeAddresses;
        int nodeCount = 0;
        bool truncated = false;
        for (auto nodeId : NodeIdsWithoutTree_) {
            ++nodeCount;
            if (nodeCount > MaxNodesWithoutPoolTreeToAlert) {
                truncated = true;
                break;
            }

            nodeAddresses.push_back(GetOrCrash(NodeIdToDescriptor_, nodeId).Address);
        }

        Host->SetSchedulerAlert(
            ESchedulerAlertType::NodesWithoutPoolTree,
            TError("Found nodes that do not match any pool tree")
                << TErrorAttribute("node_addresses", nodeAddresses)
                << TErrorAttribute("truncated", truncated)
                << TErrorAttribute("node_count", NodeIdsWithoutTree_.size()));
    }

    void BuildTreeOrchid(
        const IFairShareTreePtr& tree,
        const std::vector<TExecNodeDescriptor>& descriptors,
        TFluentMap fluent)
    {
        TJobResources resourceLimits;
        for (const auto& descriptor : descriptors) {
            resourceLimits += descriptor.ResourceLimits;
        }

        fluent
            .Item("user_to_ephemeral_pools").Do(BIND(&IFairShareTree::BuildUserToEphemeralPoolsInDefaultPool, tree))
            .Item("config").Value(tree->GetConfig())
            .Item("resource_limits").Value(resourceLimits)
            .Item("resource_usage").Value(Host->GetResourceUsage(tree->GetNodesFilter()))
            .Item("node_count").Value(descriptors.size())
            .Item("node_addresses").BeginList()
                .DoFor(descriptors, [&] (TFluentList fluent, const auto& descriptor) {
                    fluent
                        .Item().Value(descriptor.Address);
                })
            .EndList()
            // This part is asynchronous.
            .Item("fair_share_info").BeginMap()
                .Do(BIND(&IFairShareTree::BuildFairShareInfo, tree))
            .EndMap();
    }

    virtual void DoBuildResourceMeteringAt(TInstant now)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        TMeteringMap newStatistics;

        auto idToTree = SnapshottedIdToTree_.Load();
        if (idToTree.empty()) {
            // It usually means that scheduler just started and snapshotted mapping are not build yet.
            return;
        }

        for (const auto& [_, tree] : idToTree) {
            TMeteringMap newStatisticsPerTree;
            THashMap<TString, TString> customMeteringTags;
            tree->BuildResourceMetering(&newStatisticsPerTree, &customMeteringTags);

            for (auto& [key, value] : newStatisticsPerTree) {
                auto it = MeteringStatistics_.find(key);
                // NB: we are going to have some accumulated values for metering.
                if (it != MeteringStatistics_.end()) {
                    TMeteringStatistics delta(
                        value.StrongGuaranteeResources(),
                        value.ResourceFlow(),
                        value.BurstGuaranteeResources(),
                        value.AllocatedResources(),
                        value.AccumulatedResourceUsage());
                    Host->LogResourceMetering(
                        key,
                        delta,
                        customMeteringTags,
                        ConnectionTime_,
                        LastMeteringStatisticsUpdateTime_,
                        now);
                } else {
                    Host->LogResourceMetering(
                        key,
                        value,
                        customMeteringTags,
                        ConnectionTime_,
                        LastMeteringStatisticsUpdateTime_,
                        now);
                }
                newStatistics.emplace(std::move(key), std::move(value));
            }
        }

        LastMeteringStatisticsUpdateTime_ = now;
        MeteringStatistics_.swap(newStatistics);

        try {
            WaitFor(Host->UpdateLastMeteringLogTime(now))
                .ThrowOnError();
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex, "Failed to update last metering log time");
        }
    }

    void InitPersistentStrategyState(const TPersistentStrategyStatePtr& persistentStrategyState)
    {
        YT_LOG_INFO("Initializing persistent strategy state %v",
            ConvertToYsonString(persistentStrategyState, EYsonFormat::Text));
        for (auto& [treeId, treeState] : persistentStrategyState->TreeStates) {
            auto treeIt = IdToTree_.find(treeId);
            if (treeIt != IdToTree_.end()) {
                treeIt->second->InitPersistentTreeState(treeState);
            } else {
                YT_LOG_INFO("Unknown tree %Qv; skipping its persistent state %Qv",
                    treeId,
                    ConvertToYsonString(treeState, EYsonFormat::Text));
            }
        }
    }

    class TPoolTreeService
        : public TVirtualMapBase
    {
    public:
        explicit TPoolTreeService(TIntrusivePtr<TFairShareStrategy> strategy)
            : Strategy_{std::move(strategy)}
        { }

    private:
        i64 GetSize() const final
        {
            VERIFY_INVOKERS_AFFINITY(Strategy_->FeasibleInvokers);
            return std::ssize(Strategy_->IdToTree_);
        }

        std::vector<TString> GetKeys(const i64 limit) const final
        {
            VERIFY_INVOKERS_AFFINITY(Strategy_->FeasibleInvokers);

            if (limit == 0) {
                return {};
            }

            std::vector<TString> keys;
            keys.reserve(std::min(limit, std::ssize(Strategy_->IdToTree_)));
            for (const auto& [id, tree] : Strategy_->IdToTree_) {
                keys.push_back(id);
                if (std::ssize(keys) == limit) {
                    break;
                }
            }

            return keys;
        }

        IYPathServicePtr FindItemService(const TStringBuf treeId) const final
        {
            VERIFY_INVOKERS_AFFINITY(Strategy_->FeasibleInvokers);

            const auto treeIterator = Strategy_->IdToTree_.find(treeId);
            if (treeIterator == std::cend(Strategy_->IdToTree_)) {
                return nullptr;
            }

            const auto& tree = treeIterator->second;

            return tree->GetOrchidService();
        }

        const TIntrusivePtr<TFairShareStrategy> Strategy_;
    };
};

////////////////////////////////////////////////////////////////////////////////

ISchedulerStrategyPtr CreateFairShareStrategy(
    TFairShareStrategyConfigPtr config,
    ISchedulerStrategyHost* host,
    std::vector<IInvokerPtr> feasibleInvokers)
{
    return New<TFairShareStrategy>(std::move(config), host, std::move(feasibleInvokers));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
