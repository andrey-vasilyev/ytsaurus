#include "fair_share_tree.h"

#include <yt/core/misc/finally.h>

#include <yt/core/profiling/scoped_timer.h>

namespace NYT {
namespace NScheduler {

using namespace NConcurrency;
using namespace NNodeTrackerClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////

static const auto& Logger = SchedulerLogger;
static const auto& Profiler = SchedulerProfiler;

////////////////////////////////////////////////////////////////////

static const double RatioComputationPrecision = std::numeric_limits<double>::epsilon();
static const double RatioComparisonPrecision = sqrt(RatioComputationPrecision);

////////////////////////////////////////////////////////////////////

TJobResources ToJobResources(const TResourceLimitsConfigPtr& config)
{
    auto perTypeLimits = InfiniteJobResources();
    if (config->UserSlots) {
        perTypeLimits.SetUserSlots(*config->UserSlots);
    }
    if (config->Cpu) {
        perTypeLimits.SetCpu(*config->Cpu);
    }
    if (config->Network) {
        perTypeLimits.SetNetwork(*config->Network);
    }
    if (config->Memory) {
        perTypeLimits.SetMemory(*config->Memory);
    }
    return perTypeLimits;
}

////////////////////////////////////////////////////////////////////

TFairShareContext::TFairShareContext(
    const ISchedulingContextPtr& schedulingContext,
    int treeSize)
    : SchedulingContext(schedulingContext)
    , DynamicAttributesList(treeSize)
{ }

TDynamicAttributes& TFairShareContext::DynamicAttributes(ISchedulerElement* element)
{
    int index = element->GetTreeIndex();
    YCHECK(index < DynamicAttributesList.size());
    return DynamicAttributesList[index];
}

const TDynamicAttributes& TFairShareContext::DynamicAttributes(ISchedulerElement* element) const
{
    int index = element->GetTreeIndex();
    YCHECK(index < DynamicAttributesList.size());
    return DynamicAttributesList[index];
}

////////////////////////////////////////////////////////////////////

TSchedulerElementBaseFixedState::TSchedulerElementBaseFixedState(const TJobResources& totalResourceLimits)
    : ResourceDemand_(ZeroJobResources())
    , ResourceLimits_(InfiniteJobResources())
    , MaxPossibleResourceUsage_(ZeroJobResources())
    , TotalResourceLimits_(totalResourceLimits)
{ }

////////////////////////////////////////////////////////////////////

TSchedulerElementBaseSharedState::TSchedulerElementBaseSharedState()
    : ResourceUsage_(ZeroJobResources())
{ }

TJobResources TSchedulerElementBaseSharedState::GetResourceUsage()
{
    TReaderGuard guard(ResourceUsageLock_);

    return ResourceUsage_;
}

void TSchedulerElementBaseSharedState::IncreaseResourceUsage(const TJobResources& delta)
{
    TWriterGuard guard(ResourceUsageLock_);

    ResourceUsage_ += delta;
}

double TSchedulerElementBaseSharedState::GetResourceUsageRatio(
    EResourceType dominantResource,
    double dominantResourceLimit)
{
    TReaderGuard guard(ResourceUsageLock_);

    if (dominantResourceLimit == 0) {
        return 1.0;
    }
    return GetResource(ResourceUsage_, dominantResource) / dominantResourceLimit;
}

bool TSchedulerElementBaseSharedState::GetAlive() const
{
    return Alive_;
}

void TSchedulerElementBaseSharedState::SetAlive(bool alive)
{
    Alive_ = alive;
}

////////////////////////////////////////////////////////////////////

int TSchedulerElementBase::EnumerateNodes(int startIndex)
{
    TreeIndex_ = startIndex++;
    return startIndex;
}

int TSchedulerElementBase::GetTreeIndex() const
{
    return TreeIndex_;
}

void TSchedulerElementBase::Update(TDynamicAttributesList& dynamicAttributesList)
{
    UpdateBottomUp(dynamicAttributesList);
    UpdateTopDown(dynamicAttributesList);
}

void TSchedulerElementBase::UpdateBottomUp(TDynamicAttributesList& dynamicAttributesList)
{
    UpdateAttributes();
    dynamicAttributesList[this->GetTreeIndex()].Active = true;
    UpdateDynamicAttributes(dynamicAttributesList);
}

void TSchedulerElementBase::UpdateTopDown(TDynamicAttributesList& dynamicAttributesList)
{ }

void TSchedulerElementBase::UpdateDynamicAttributes(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(IsActive(dynamicAttributesList));
    dynamicAttributesList[this->GetTreeIndex()].SatisfactionRatio = ComputeLocalSatisfactionRatio();
    dynamicAttributesList[this->GetTreeIndex()].Active = IsAlive();
}

void TSchedulerElementBase::PrescheduleJob(TFairShareContext& context, bool starvingOnly)
{
    UpdateDynamicAttributes(context.DynamicAttributesList);
}

const TSchedulableAttributes& TSchedulerElementBase::Attributes() const
{
    return Attributes_;
}

TSchedulableAttributes& TSchedulerElementBase::Attributes()
{
    return Attributes_;
}

void TSchedulerElementBase::UpdateAttributes()
{
    // Choose dominant resource types, compute max share ratios, compute demand ratios.
    const auto& demand = ResourceDemand();
    auto usage = GetResourceUsage();
    auto totalLimits = Host_->GetTotalResourceLimits();

    auto maxPossibleResourceUsage = Min(totalLimits, MaxPossibleResourceUsage_);

    if (usage == ZeroJobResources()) {
        Attributes_.DominantResource = GetDominantResource(demand, totalLimits);
    } else {
        Attributes_.DominantResource = GetDominantResource(usage, totalLimits);
    }

    i64 dominantDemand = GetResource(demand, Attributes_.DominantResource);
    i64 dominantUsage = GetResource(usage, Attributes_.DominantResource);
    i64 dominantLimit = GetResource(totalLimits, Attributes_.DominantResource);

    Attributes_.DemandRatio =
        dominantLimit == 0 ? 1.0 : (double) dominantDemand / dominantLimit;

    double usageRatio =
        dominantLimit == 0 ? 1.0 : (double) dominantUsage / dominantLimit;

    Attributes_.DominantLimit = dominantLimit;

    Attributes_.MaxPossibleUsageRatio = GetMaxShareRatio();
    if (usageRatio > RatioComputationPrecision) {
        // In this case we know pool resource preferences and can take them into account.
        // We find maximum number K such that Usage * K < Limit and use it to estimate
        // maximum dominant resource usage.
        Attributes_.MaxPossibleUsageRatio = std::min(
            GetMinResourceRatio(maxPossibleResourceUsage, usage) * usageRatio,
            Attributes_.MaxPossibleUsageRatio);
    } else {
        // In this case we have no information about pool resource preferences, so just assume
        // that it uses all resources equally.
        Attributes_.MaxPossibleUsageRatio = std::min(
            Attributes_.DemandRatio,
            Attributes_.MaxPossibleUsageRatio);
    }
}

TNullable<Stroka> TSchedulerElementBase::GetNodeTag() const
{
    return Null;
}

bool TSchedulerElementBase::IsActive(const TDynamicAttributesList& dynamicAttributesList) const
{
    return dynamicAttributesList[GetTreeIndex()].Active;
}

bool TSchedulerElementBase::IsAlive() const
{
    return SharedState_->GetAlive();
}

void TSchedulerElementBase::SetAlive(bool alive)
{
    SharedState_->SetAlive(alive);
}

TCompositeSchedulerElement* TSchedulerElementBase::GetParent() const
{
    return Parent_;
}

void TSchedulerElementBase::SetParent(TCompositeSchedulerElement* parent)
{
    Parent_ = parent;
}

int TSchedulerElementBase::GetPendingJobCount() const
{
    return PendingJobCount_;
}

ESchedulableStatus TSchedulerElementBase::GetStatus() const
{
    return ESchedulableStatus::Normal;
}

bool TSchedulerElementBase::GetStarving() const
{
    return Starving_;
}

void TSchedulerElementBase::SetStarving(bool starving)
{
    Starving_ = starving;
}

const TJobResources& TSchedulerElementBase::ResourceDemand() const
{
    return ResourceDemand_;
}

const TJobResources& TSchedulerElementBase::ResourceLimits() const
{
    return ResourceLimits_;
}

const TJobResources& TSchedulerElementBase::MaxPossibleResourceUsage() const
{
    return MaxPossibleResourceUsage_;
}

TJobResources TSchedulerElementBase::GetResourceUsage() const
{
    return SharedState_->GetResourceUsage();
}

double TSchedulerElementBase::GetResourceUsageRatio() const
{
    return SharedState_->GetResourceUsageRatio(
        Attributes_.DominantResource,
        Attributes_.DominantLimit);
}

void TSchedulerElementBase::IncreaseLocalResourceUsage(const TJobResources& delta)
{
    SharedState_->IncreaseResourceUsage(delta);
}

TSchedulerElementBase::TSchedulerElementBase(
    ISchedulerStrategyHost* host,
    TFairShareStrategyConfigPtr strategyConfig)
    : TSchedulerElementBaseFixedState(host->GetTotalResourceLimits())
    , Host_(host)
    , StrategyConfig_(strategyConfig)
    , SharedState_(New<TSchedulerElementBaseSharedState>())
{ }

TSchedulerElementBase::TSchedulerElementBase(const TSchedulerElementBase& other)
    : TSchedulerElementBaseFixedState(other)
    , Host_(nullptr)
    , StrategyConfig_(CloneYsonSerializable(other.StrategyConfig_))
    , SharedState_(other.SharedState_)
{ }

double TSchedulerElementBase::ComputeLocalSatisfactionRatio() const
{
    double minShareRatio = Attributes_.AdjustedMinShareRatio;
    double fairShareRatio = Attributes_.FairShareRatio;
    double usageRatio = GetResourceUsageRatio();

    // Check for corner cases.
    if (fairShareRatio < RatioComputationPrecision) {
        return std::numeric_limits<double>::max();
    }

    if (minShareRatio > RatioComputationPrecision && usageRatio < minShareRatio) {
        // Needy element, negative satisfaction.
        return usageRatio / minShareRatio - 1.0;
    } else {
        // Regular element, positive satisfaction.
        return usageRatio / fairShareRatio;
    }
}

ESchedulableStatus TSchedulerElementBase::GetStatus(double defaultTolerance) const
{
    double usageRatio = GetResourceUsageRatio();
    double demandRatio = Attributes_.DemandRatio;

    double tolerance =
        demandRatio < Attributes_.FairShareRatio + RatioComparisonPrecision
        ? 1.0
        : defaultTolerance;

    if (usageRatio > Attributes_.FairShareRatio * tolerance - RatioComparisonPrecision) {
        return ESchedulableStatus::Normal;
    }

    return usageRatio < Attributes_.AdjustedMinShareRatio
           ? ESchedulableStatus::BelowMinShare
           : ESchedulableStatus::BelowFairShare;
}

void TSchedulerElementBase::CheckForStarvation(
    TDuration minSharePreemptionTimeout,
    TDuration fairSharePreemptionTimeout,
    TInstant now)
{
    auto status = GetStatus();
    switch (status) {
        case ESchedulableStatus::BelowMinShare:
            if (!BelowFairShareSince_) {
                BelowFairShareSince_ = now;
            } else if (BelowFairShareSince_.Get() < now - minSharePreemptionTimeout) {
                SetStarving(true);
            }
            break;

        case ESchedulableStatus::BelowFairShare:
            if (!BelowFairShareSince_) {
                BelowFairShareSince_ = now;
            } else if (BelowFairShareSince_.Get() < now - fairSharePreemptionTimeout) {
                SetStarving(true);
            }
            break;

        case ESchedulableStatus::Normal:
            BelowFairShareSince_ = Null;
            SetStarving(false);
            break;

        default:
            YUNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////

TCompositeSchedulerElementFixedState::TCompositeSchedulerElementFixedState()
    : RunningOperationCount_(0)
    , OperationCount_(0)
{ }

////////////////////////////////////////////////////////////////////

TCompositeSchedulerElement::TCompositeSchedulerElement(
    ISchedulerStrategyHost* host,
    TFairShareStrategyConfigPtr strategyConfig)
    : TSchedulerElementBase(host, strategyConfig)
{ }

TCompositeSchedulerElement::TCompositeSchedulerElement(const TCompositeSchedulerElement& other)
    : TSchedulerElementBase(other)
    , TCompositeSchedulerElementFixedState(other)
{
    for (const auto& child : other.Children) {
        auto childClone = child->Clone();
        childClone->SetParent(this);
        Children.insert(childClone);
    }
    for (const auto& child : other.DisabledChildren) {
        auto childClone = child->Clone();
        childClone->SetParent(this);
        DisabledChildren.insert(childClone);
    }
}

int TCompositeSchedulerElement::EnumerateNodes(int startIndex)
{
    startIndex = TSchedulerElementBase::EnumerateNodes(startIndex);
    for (const auto& child : Children) {
        startIndex = child->EnumerateNodes(startIndex);
    }
    return startIndex;
}

void TCompositeSchedulerElement::UpdateBottomUp(TDynamicAttributesList& dynamicAttributesList)
{
    Attributes_.BestAllocationRatio = 0.0;
    PendingJobCount_ = 0;
    ResourceDemand_ = ZeroJobResources();
    auto maxPossibleChildrenResourceUsage_ = ZeroJobResources();
    for (const auto& child : Children) {
        child->UpdateBottomUp(dynamicAttributesList);

        Attributes_.BestAllocationRatio = std::max(
            Attributes_.BestAllocationRatio,
            child->Attributes().BestAllocationRatio);

        PendingJobCount_ += child->GetPendingJobCount();
        ResourceDemand_ += child->ResourceDemand();
        maxPossibleChildrenResourceUsage_ += child->MaxPossibleResourceUsage();
    }
    MaxPossibleResourceUsage_ = Min(maxPossibleChildrenResourceUsage_, ResourceLimits_);
    TSchedulerElementBase::UpdateBottomUp(dynamicAttributesList);
}

void TCompositeSchedulerElement::UpdateTopDown(TDynamicAttributesList& dynamicAttributesList)
{
    switch (Mode_) {
        case ESchedulingMode::Fifo:
            // Easy case -- the first child get everything, others get none.
            UpdateFifo(dynamicAttributesList);
            break;

        case ESchedulingMode::FairShare:
            // Hard case -- compute fair shares using fit factor.
            UpdateFairShare(dynamicAttributesList);
            break;

        default:
            YUNREACHABLE();
    }

    UpdatePreemptionSettingsLimits();

    // Propagate updates to children.
    for (const auto& child : Children) {
        UpdateChildPreemptionSettings(child);
        child->UpdateTopDown(dynamicAttributesList);
    }
}

double TCompositeSchedulerElement::GetFairShareStarvationToleranceLimit() const
{
    return 1.0;
}

TDuration TCompositeSchedulerElement::GetMinSharePreemptionTimeoutLimit() const
{
    return TDuration::Zero();
}

TDuration TCompositeSchedulerElement::GetFairSharePreemptionTimeoutLimit() const
{
    return TDuration::Zero();
}

void TCompositeSchedulerElement::UpdatePreemptionSettingsLimits()
{
    if (Parent_) {
        AdjustedFairShareStarvationToleranceLimit_ = std::min(
            GetFairShareStarvationToleranceLimit(),
            Parent_->AdjustedFairShareStarvationToleranceLimit());

        AdjustedMinSharePreemptionTimeoutLimit_ = std::max(
            GetMinSharePreemptionTimeoutLimit(),
            Parent_->AdjustedMinSharePreemptionTimeoutLimit());

        AdjustedFairSharePreemptionTimeoutLimit_ = std::max(
            GetFairSharePreemptionTimeoutLimit(),
            Parent_->AdjustedFairSharePreemptionTimeoutLimit());
    }
}

void TCompositeSchedulerElement::UpdateChildPreemptionSettings(const ISchedulerElementPtr& child)
{
    auto& childAttributes = child->Attributes();

    childAttributes.AdjustedFairShareStarvationTolerance = std::min(
        child->GetFairShareStarvationTolerance(),
        AdjustedFairShareStarvationToleranceLimit_);

    childAttributes.AdjustedMinSharePreemptionTimeout = std::max(
        child->GetMinSharePreemptionTimeout(),
        AdjustedMinSharePreemptionTimeoutLimit_);

    childAttributes.AdjustedFairSharePreemptionTimeout = std::max(
        child->GetFairSharePreemptionTimeout(),
        AdjustedFairSharePreemptionTimeoutLimit_);
}

void TCompositeSchedulerElement::UpdateDynamicAttributes(TDynamicAttributesList& dynamicAttributesList)
{
    YCHECK(IsActive(dynamicAttributesList));
    auto& attributes = dynamicAttributesList[this->GetTreeIndex()];

    if (!IsAlive()) {
        attributes.Active = false;
        return;
    }

    // Compute local satisfaction ratio.
    attributes.SatisfactionRatio = ComputeLocalSatisfactionRatio();
    // Start times bubble up from leaf nodes with operations.
    attributes.MinSubtreeStartTime = TInstant::Max();
    // Adjust satisfaction ratio using children.
    // Declare the element passive if all children are passive.
    attributes.Active = false;
    attributes.BestLeafDescendant = nullptr;

    while (auto bestChild = GetBestActiveChild(dynamicAttributesList)) {
        const auto& bestChildAttributes = dynamicAttributesList[bestChild->GetTreeIndex()];
        auto childBestLeafDescendant = bestChildAttributes.BestLeafDescendant;
        if (!childBestLeafDescendant->IsAlive()) {
            bestChild->UpdateDynamicAttributes(dynamicAttributesList);
            if (!bestChildAttributes.Active) {
                continue;
            }
            childBestLeafDescendant = bestChildAttributes.BestLeafDescendant;
        }

        // We need to evaluate both MinSubtreeStartTime and SatisfactionRatio
        // because parent can use different scheduling mode.
        attributes.MinSubtreeStartTime = std::min(
            attributes.MinSubtreeStartTime,
            bestChildAttributes.MinSubtreeStartTime);

        attributes.SatisfactionRatio = std::min(
            attributes.SatisfactionRatio,
            bestChildAttributes.SatisfactionRatio);

        attributes.BestLeafDescendant = childBestLeafDescendant;
        attributes.Active = true;
        break;
    }
}

void TCompositeSchedulerElement::PrescheduleJob(TFairShareContext& context, bool starvingOnly)
{
    auto& attributes = context.DynamicAttributes(this);

    attributes.Active = true;

    if (!IsAlive()) {
        attributes.Active = false;
        return;
    }

    if (!context.SchedulingContext->CanSchedule(GetNodeTag())) {
        attributes.Active = false;
        return;
    }

    for (const auto& child : Children) {
        // If pool is starving, any child will do.
        if (Starving_) {
            child->PrescheduleJob(context, false);
        } else {
            child->PrescheduleJob(context, starvingOnly);
        }
    }

    TSchedulerElementBase::PrescheduleJob(context, starvingOnly);
}

bool TCompositeSchedulerElement::ScheduleJob(TFairShareContext& context)
{
    auto& attributes = context.DynamicAttributes(this);
    if (!attributes.Active) {
        return false;
    }

    auto bestLeafDescendant = attributes.BestLeafDescendant;
    if (!bestLeafDescendant->IsAlive()) {
        UpdateDynamicAttributes(context.DynamicAttributesList);
        if (!attributes.Active) {
            return false;
        }
        bestLeafDescendant = attributes.BestLeafDescendant;
    }

    // NB: Ignore the child's result.
    bestLeafDescendant->ScheduleJob(context);
    return true;
}

void TCompositeSchedulerElement::IncreaseResourceUsage(const TJobResources& delta)
{
    auto* currentElement = this;
    while (currentElement) {
        currentElement->IncreaseLocalResourceUsage(delta);
        currentElement = currentElement->GetParent();
    }
}

bool TCompositeSchedulerElement::IsRoot() const
{
    return false;
}

void TCompositeSchedulerElement::AddChild(const ISchedulerElementPtr& child, bool enabled)
{
    if (enabled) {
        YCHECK(Children.insert(child).second);
    } else {
        YCHECK(DisabledChildren.insert(child).second);
    }
}

void TCompositeSchedulerElement::EnableChild(const ISchedulerElementPtr& child)
{
    auto it = DisabledChildren.find(child);
    YCHECK(it != DisabledChildren.end());
    Children.insert(child);
    DisabledChildren.erase(it);
}

void TCompositeSchedulerElement::RemoveChild(const ISchedulerElementPtr& child)
{
    bool foundInChildren = (Children.find(child) != Children.end());
    bool foundInDisabledChildren = (DisabledChildren.find(child) != DisabledChildren.end());
    YCHECK((foundInChildren && !foundInDisabledChildren) || (!foundInChildren && foundInDisabledChildren));
    if (foundInChildren) {
        Children.erase(child);
    } else {
        DisabledChildren.erase(child);
    }
}

bool TCompositeSchedulerElement::IsEmpty() const
{
    return Children.empty() && DisabledChildren.empty();
}

// Given a non-descending continuous |f|, |f(0) = 0|, and a scalar |a|,
// computes |x \in [0,1]| s.t. |f(x) = a|.
// If |f(1) < a| then still returns 1.
template <class F>
static double BinarySearch(const F& f, double a)
{
    if (f(1) < a) {
        return 1.0;
    }

    double lo = 0.0;
    double hi = 1.0;
    while (hi - lo > RatioComputationPrecision) {
        double x = (lo + hi) / 2.0;
        if (f(x) < a) {
            lo = x;
        } else {
            hi = x;
        }
    }
    return (lo + hi) / 2.0;
}

template <class TGetter, class TSetter>
void TCompositeSchedulerElement::ComputeByFitting(
    const TGetter& getter,
    const TSetter& setter,
    double sum)
{
    auto getSum = [&] (double fitFactor) -> double {
        double sum = 0.0;
        for (const auto& child : Children) {
            sum += getter(fitFactor, child);
        }
        return sum;
    };

    // Run binary search to compute fit factor.
    double fitFactor = BinarySearch(getSum, sum);

    // Compute actual min shares from fit factor.
    for (const auto& child : Children) {
        double value = getter(fitFactor, child);
        setter(child, value);
    }
}

void TCompositeSchedulerElement::UpdateFifo(TDynamicAttributesList& dynamicAttributesList)
{
    // TODO(acid): This code shouldn't use active children.
    const auto& bestChild = GetBestActiveChildFifo(dynamicAttributesList);
    for (const auto& child : Children) {
        auto& childAttributes = child->Attributes();
        if (child == bestChild) {
            childAttributes.AdjustedMinShareRatio = std::min(
                childAttributes.DemandRatio,
                Attributes_.AdjustedMinShareRatio);
            childAttributes.FairShareRatio = std::min(
                childAttributes.DemandRatio,
                Attributes_.FairShareRatio);
        } else {
            childAttributes.AdjustedMinShareRatio = 0.0;
            childAttributes.FairShareRatio = 0.0;
        }
    }
}

void TCompositeSchedulerElement::UpdateFairShare(TDynamicAttributesList& dynamicAttributesList)
{
    // Compute min shares.
    // Compute min weight.
    double minShareSum = 0.0;
    double minWeight = 1.0;
    for (const auto& child : Children) {
        auto& childAttributes = child->Attributes();
        double result = child->GetMinShareRatio();
        // Never give more than can be used.
        result = std::min(result, childAttributes.MaxPossibleUsageRatio);
        // Never give more than we can allocate.
        result = std::min(result, childAttributes.BestAllocationRatio);
        childAttributes.AdjustedMinShareRatio = result;
        minShareSum += result;

        if (child->GetWeight() > RatioComputationPrecision) {
            minWeight = std::min(minWeight, child->GetWeight());
        }
    }

    // Normalize min shares, if needed.
    if (minShareSum > Attributes_.AdjustedMinShareRatio) {
        double fitFactor = Attributes_.AdjustedMinShareRatio / minShareSum;
        for (const auto& child : Children) {
            auto& childAttributes = child->Attributes();
            childAttributes.AdjustedMinShareRatio *= fitFactor;
        }
    }

    // Compute fair shares.
    ComputeByFitting(
        [&] (double fitFactor, const ISchedulerElementPtr& child) -> double {
            const auto& childAttributes = child->Attributes();
            double result = fitFactor * child->GetWeight() / minWeight;
            // Never give less than promised by min share.
            result = std::max(result, childAttributes.AdjustedMinShareRatio);
            // Never give more than can be used.
            result = std::min(result, childAttributes.MaxPossibleUsageRatio);
            // Never give more than we can allocate.
            result = std::min(result, childAttributes.BestAllocationRatio);
            return result;
        },
        [&] (const ISchedulerElementPtr& child, double value) {
            auto& attributes = child->Attributes();
            attributes.FairShareRatio = value;
        },
        Attributes_.FairShareRatio);
}

ISchedulerElementPtr TCompositeSchedulerElement::GetBestActiveChild(const TDynamicAttributesList& dynamicAttributesList) const
{
    switch (Mode_) {
        case ESchedulingMode::Fifo:
            return GetBestActiveChildFifo(dynamicAttributesList);
        case ESchedulingMode::FairShare:
            return GetBestActiveChildFairShare(dynamicAttributesList);
        default:
            YUNREACHABLE();
    }
}

ISchedulerElementPtr TCompositeSchedulerElement::GetBestActiveChildFifo(const TDynamicAttributesList& dynamicAttributesList) const
{
    auto isBetter = [this, &dynamicAttributesList] (const ISchedulerElementPtr& lhs, const ISchedulerElementPtr& rhs) -> bool {
        for (auto parameter : FifoSortParameters_) {
            switch (parameter) {
                case EFifoSortParameter::Weight:
                    if (lhs->GetWeight() != rhs->GetWeight()) {
                        return lhs->GetWeight() > rhs->GetWeight();
                    }
                    break;
                case EFifoSortParameter::StartTime: {
                    const auto& lhsStartTime = dynamicAttributesList[lhs->GetTreeIndex()].MinSubtreeStartTime;
                    const auto& rhsStartTime = dynamicAttributesList[rhs->GetTreeIndex()].MinSubtreeStartTime;
                    if (lhsStartTime != rhsStartTime) {
                        return lhsStartTime < rhsStartTime;
                    }
                    break;
                }
                case EFifoSortParameter::PendingJobCount: {
                    int lhsPendingJobCount = lhs->GetPendingJobCount();
                    int rhsPendingJobCount = rhs->GetPendingJobCount();
                    if (lhsPendingJobCount != rhsPendingJobCount) {
                        return lhsPendingJobCount < rhsPendingJobCount;
                    }
                    break;
                }
                default:
                    YUNREACHABLE();
            }
        }
        return false;
    };

    ISchedulerElement* bestChild = nullptr;
    for (const auto& child : Children) {
        if (child->IsActive(dynamicAttributesList)) {
            if (bestChild && isBetter(bestChild, child))
                continue;

            bestChild = child.Get();
        }
    }
    return bestChild;
}

ISchedulerElementPtr TCompositeSchedulerElement::GetBestActiveChildFairShare(const TDynamicAttributesList& dynamicAttributesList) const
{
    ISchedulerElement* bestChild = nullptr;
    double bestChildSatisfactionRatio = std::numeric_limits<double>::max();
    for (const auto& child : Children) {
        if (child->IsActive(dynamicAttributesList)) {
            double childSatisfactionRatio = dynamicAttributesList[child->GetTreeIndex()].SatisfactionRatio;
            if (!bestChild || childSatisfactionRatio < bestChildSatisfactionRatio)
            {
                bestChild = child.Get();
                bestChildSatisfactionRatio = childSatisfactionRatio;
            }
        }
    }
    return bestChild;
}

////////////////////////////////////////////////////////////////////

TPoolFixedState::TPoolFixedState(const Stroka& id)
    : Id_(id)
{ }

////////////////////////////////////////////////////////////////////

TPool::TPool(
    ISchedulerStrategyHost* host,
    const Stroka& id,
    TFairShareStrategyConfigPtr strategyConfig)
    : TCompositeSchedulerElement(host, strategyConfig)
    , TPoolFixedState(id)
{
    SetDefaultConfig();
}

TPool::TPool(const TPool& other)
    : TCompositeSchedulerElement(other)
    , TPoolFixedState(other)
    , Config_(CloneYsonSerializable(other.Config_))
{ }

bool TPool::IsDefaultConfigured() const
{
    return DefaultConfigured_;
}

TPoolConfigPtr TPool::GetConfig()
{
    return Config_;
}

void TPool::SetConfig(TPoolConfigPtr config)
{
    DoSetConfig(config);
    DefaultConfigured_ = false;
}

void TPool::SetDefaultConfig()
{
    DoSetConfig(New<TPoolConfig>());
    DefaultConfigured_ = true;
}

Stroka TPool::GetId() const
{
    return Id_;
}

double TPool::GetWeight() const
{
    return Config_->Weight;
}

double TPool::GetMinShareRatio() const
{
    return Config_->MinShareRatio;
}

double TPool::GetMaxShareRatio() const
{
    return Config_->MaxShareRatio;
}

ESchedulableStatus TPool::GetStatus() const
{
    return TSchedulerElementBase::GetStatus(Attributes_.AdjustedFairShareStarvationTolerance);
}

double TPool::GetFairShareStarvationTolerance() const
{
    return Config_->FairShareStarvationTolerance.Get(Parent_->Attributes().AdjustedFairShareStarvationTolerance);
}

TDuration TPool::GetMinSharePreemptionTimeout() const
{
    return Config_->MinSharePreemptionTimeout.Get(Parent_->Attributes().AdjustedMinSharePreemptionTimeout);
}

TDuration TPool::GetFairSharePreemptionTimeout() const
{
    return Config_->FairSharePreemptionTimeout.Get(Parent_->Attributes().AdjustedFairSharePreemptionTimeout);
}

double TPool::GetFairShareStarvationToleranceLimit() const
{
    return Config_->FairShareStarvationToleranceLimit.Get(StrategyConfig_->FairShareStarvationToleranceLimit);
}

TDuration TPool::GetMinSharePreemptionTimeoutLimit() const
{
    return Config_->MinSharePreemptionTimeoutLimit.Get(StrategyConfig_->MinSharePreemptionTimeoutLimit);
}

TDuration TPool::GetFairSharePreemptionTimeoutLimit() const
{
    return Config_->FairSharePreemptionTimeoutLimit.Get(StrategyConfig_->FairSharePreemptionTimeoutLimit);
}

void TPool::SetStarving(bool starving)
{
    if (starving && !GetStarving()) {
        TSchedulerElementBase::SetStarving(true);
        LOG_INFO("Pool is now starving (PoolId: %v, Status: %v)",
            GetId(),
            GetStatus());
    } else if (!starving && GetStarving()) {
        TSchedulerElementBase::SetStarving(false);
        LOG_INFO("Pool is no longer starving (PoolId: %v)",
            GetId());
    }
}

void TPool::CheckForStarvation(TInstant now)
{
    TSchedulerElementBase::CheckForStarvation(
        Attributes_.AdjustedMinSharePreemptionTimeout,
        Attributes_.AdjustedFairSharePreemptionTimeout,
        now);
}

TNullable<Stroka> TPool::GetNodeTag() const
{
    return Config_->SchedulingTag;
}

void TPool::UpdateBottomUp(TDynamicAttributesList& dynamicAttributesList)
{
    ResourceLimits_ = ComputeResourceLimits();
    TCompositeSchedulerElement::UpdateBottomUp(dynamicAttributesList);
}

int TPool::GetMaxRunningOperationCount() const
{
    return Config_->MaxRunningOperationCount.Get(StrategyConfig_->MaxRunningOperationCountPerPool);
}

int TPool::GetMaxOperationCount() const
{
    return Config_->MaxOperationCount.Get(StrategyConfig_->MaxOperationCountPerPool);
}

ISchedulerElementPtr TPool::Clone()
{
    return New<TPool>(*this);
}

void TPool::DoSetConfig(TPoolConfigPtr newConfig)
{
    Config_ = newConfig;
    FifoSortParameters_ = Config_->FifoSortParameters;
    Mode_ = Config_->Mode;
}

TJobResources TPool::ComputeResourceLimits() const
{
    auto resourceLimits = Host_->GetResourceLimits(GetNodeTag()) * Config_->MaxShareRatio;
    auto perTypeLimits = ToJobResources(Config_->ResourceLimits);
    return Min(resourceLimits, perTypeLimits);
}

////////////////////////////////////////////////////////////////////

TOperationElementFixedState::TOperationElementFixedState(TOperationPtr operation)
    : OperationId_(operation->GetId())
    , Operation_(operation)
{ }

////////////////////////////////////////////////////////////////////

TOperationElementSharedState::TOperationElementSharedState()
    : NonpreemptableResourceUsage_(ZeroJobResources())
{ }

void TOperationElementSharedState::IncreaseJobResourceUsage(const TJobId& jobId, const TJobResources& resourcesDelta)
{
    TWriterGuard guard(JobPropertiesMapLock_);

    IncreaseJobResourceUsage(JobPropertiesMap_.at(jobId), resourcesDelta);
}

void TOperationElementSharedState::UpdatePreemptableJobsList(double fairShareRatio, const TJobResources& totalResourceLimits)
{
    TWriterGuard guard(JobPropertiesMapLock_);

    auto dominantResource = GetDominantResource(NonpreemptableResourceUsage_, totalResourceLimits);
    i64 dominantLimit = GetResource(totalResourceLimits, dominantResource);

    auto getNonpreemptableUsageRatio = [&] (const TJobResources& extraResources) -> double {
        i64 usage = GetResource(NonpreemptableResourceUsage_ + extraResources, dominantResource);
        return dominantLimit == 0 ? 1.0 : (double) usage / dominantLimit;
    };

    // Remove nonpreemptable jobs exceeding the fair share.
    while (!NonpreemptableJobs_.empty()) {
        if (getNonpreemptableUsageRatio(ZeroJobResources()) <= fairShareRatio) {
            break;
        }

        auto jobId = NonpreemptableJobs_.back();
        auto& jobProperties = JobPropertiesMap_.at(jobId);
        YCHECK(!jobProperties.Preemptable);

        NonpreemptableJobs_.pop_back();
        NonpreemptableResourceUsage_ -= jobProperties.ResourceUsage;

        PreemptableJobs_.push_front(jobId);

        jobProperties.Preemptable = true;
        jobProperties.JobIdListIterator = PreemptableJobs_.begin();
    }

    // Add more nonpreemptable jobs until filling up the fair share.
    while (!PreemptableJobs_.empty()) {
        auto jobId = PreemptableJobs_.front();
        auto& jobProperties = JobPropertiesMap_.at(jobId);
        YCHECK(jobProperties.Preemptable);

        if (getNonpreemptableUsageRatio(jobProperties.ResourceUsage) > fairShareRatio) {
            break;
        }

        PreemptableJobs_.pop_front();

        NonpreemptableJobs_.push_back(jobId);
        NonpreemptableResourceUsage_ += jobProperties.ResourceUsage;

        jobProperties.Preemptable = false;
        jobProperties.JobIdListIterator = --NonpreemptableJobs_.end();
    }
}

bool TOperationElementSharedState::IsJobExisting(const TJobId& jobId) const
{
    TReaderGuard guard(JobPropertiesMapLock_);

    return JobPropertiesMap_.find(jobId) != JobPropertiesMap_.end();
}

bool TOperationElementSharedState::IsJobPreemptable(const TJobId& jobId) const
{
    TReaderGuard guard(JobPropertiesMapLock_);

    return JobPropertiesMap_.at(jobId).Preemptable;
}

int TOperationElementSharedState::GetPreemptableJobCount() const
{
    TReaderGuard guard(JobPropertiesMapLock_);

    return PreemptableJobs_.size();
}

void TOperationElementSharedState::AddJob(const TJobId& jobId, const TJobResources resourceUsage)
{
    TWriterGuard guard(JobPropertiesMapLock_);

    PreemptableJobs_.push_back(jobId);

    auto it = JobPropertiesMap_.insert(std::make_pair(
        jobId,
        TJobProperties(true, --PreemptableJobs_.end(), ZeroJobResources())));
    YCHECK(it.second);

    IncreaseJobResourceUsage(it.first->second, resourceUsage);
}

TJobResources TOperationElementSharedState::RemoveJob(const TJobId& jobId)
{
    TWriterGuard guard(JobPropertiesMapLock_);

    auto it = JobPropertiesMap_.find(jobId);
    YCHECK(it != JobPropertiesMap_.end());

    auto& properties = it->second;
    if (properties.Preemptable) {
        PreemptableJobs_.erase(properties.JobIdListIterator);
    } else {
        NonpreemptableJobs_.erase(properties.JobIdListIterator);
    }

    auto resourceUsage = properties.ResourceUsage;
    IncreaseJobResourceUsage(properties, -resourceUsage);

    JobPropertiesMap_.erase(it);

    return resourceUsage;
}

bool TOperationElementSharedState::IsBlocked(
    TInstant now,
    int MaxConcurrentScheduleJobCalls,
    const TDuration& ScheduleJobFailBackoffTime) const
{
    TReaderGuard guard(ConcurrentScheduleJobCallsLock_);

    return IsBlockedImpl(now, MaxConcurrentScheduleJobCalls, ScheduleJobFailBackoffTime);
}

bool TOperationElementSharedState::TryStartScheduleJob(
    TInstant now,
    int maxConcurrentScheduleJobCalls,
    TDuration scheduleJobFailBackoffTime)
{
    TWriterGuard guard(ConcurrentScheduleJobCallsLock_);

    if (IsBlockedImpl(now, maxConcurrentScheduleJobCalls, scheduleJobFailBackoffTime)) {
        return false;
    }

    BackingOff_ = false;
    ++ConcurrentScheduleJobCalls_;
    return true;
}

void TOperationElementSharedState::FinishScheduleJob()
{
    TWriterGuard guard(ConcurrentScheduleJobCallsLock_);

    --ConcurrentScheduleJobCalls_;
}

void TOperationElementSharedState::EnableScheduleJobBackoff(TInstant now)
{
    TWriterGuard guard(ConcurrentScheduleJobCallsLock_);

    BackingOff_ = true;
    LastScheduleJobFailTime_ = now;
}

bool TOperationElementSharedState::IsBlockedImpl(
    TInstant now,
    int MaxConcurrentScheduleJobCalls,
    const TDuration& ScheduleJobFailBackoffTime) const
{
    return ConcurrentScheduleJobCalls_ >= MaxConcurrentScheduleJobCalls ||
        (BackingOff_ && LastScheduleJobFailTime_ + ScheduleJobFailBackoffTime > now);
}

void TOperationElementSharedState::IncreaseJobResourceUsage(TJobProperties& properties, const TJobResources& resourcesDelta)
{
    properties.ResourceUsage += resourcesDelta;
    if (!properties.Preemptable) {
        NonpreemptableResourceUsage_ += resourcesDelta;
    }
}

////////////////////////////////////////////////////////////////////

TOperationElement::TOperationElement(
    TFairShareStrategyConfigPtr strategyConfig,
    TStrategyOperationSpecPtr spec,
    TOperationRuntimeParamsPtr runtimeParams,
    ISchedulerStrategyHost* host,
    TOperationPtr operation)
    : TSchedulerElementBase(host, strategyConfig)
    , TOperationElementFixedState(operation)
    , Spec_(spec)
    , RuntimeParams_(runtimeParams)
    , SharedState_(New<TOperationElementSharedState>())
{ }

TOperationElement::TOperationElement(const TOperationElement& other)
    : TSchedulerElementBase(other)
    , TOperationElementFixedState(other)
    , Spec_(CloneYsonSerializable(other.Spec_))
    , RuntimeParams_(CloneYsonSerializable(other.RuntimeParams_))
    , SharedState_(other.SharedState_)
{ }

double TOperationElement::GetFairShareStarvationTolerance() const
{
    return Spec_->FairShareStarvationTolerance.Get(Parent_->Attributes().AdjustedFairShareStarvationTolerance);
}

TDuration TOperationElement::GetMinSharePreemptionTimeout() const
{
    return Spec_->MinSharePreemptionTimeout.Get(Parent_->Attributes().AdjustedMinSharePreemptionTimeout);
}

TDuration TOperationElement::GetFairSharePreemptionTimeout() const
{
    return Spec_->FairSharePreemptionTimeout.Get(Parent_->Attributes().AdjustedFairSharePreemptionTimeout);
}

void TOperationElement::UpdateBottomUp(TDynamicAttributesList& dynamicAttributesList)
{
    TSchedulerElementBase::UpdateBottomUp(dynamicAttributesList);

    ResourceDemand_ = ComputeResourceDemand();
    ResourceLimits_ = ComputeResourceLimits();
    MaxPossibleResourceUsage_ = ComputeMaxPossibleResourceUsage();
    PendingJobCount_ = ComputePendingJobCount();

    auto totalLimits = Host_->GetTotalResourceLimits();
    auto allocationLimits = GetAdjustedResourceLimits(
        ResourceDemand_,
        totalLimits,
        Host_->GetExecNodeCount());

    i64 dominantLimit = GetResource(totalLimits, Attributes_.DominantResource);
    i64 dominantAllocationLimit = GetResource(allocationLimits, Attributes_.DominantResource);

    Attributes_.BestAllocationRatio =
        dominantLimit == 0 ? 1.0 : (double) dominantAllocationLimit / dominantLimit;
}

void TOperationElement::UpdateDynamicAttributes(TDynamicAttributesList& dynamicAttributesList)
{
    auto& attributes = dynamicAttributesList[this->GetTreeIndex()];
    attributes.Active = true;
    attributes.BestLeafDescendant = this;
    attributes.MinSubtreeStartTime = Operation_->GetStartTime();

    TSchedulerElementBase::UpdateDynamicAttributes(dynamicAttributesList);
}

void TOperationElement::PrescheduleJob(TFairShareContext& context, bool starvingOnly)
{
    auto& attributes = context.DynamicAttributes(this);

    attributes.Active = true;

    if (!IsAlive()) {
        attributes.Active = false;
        return;
    }

    if (!context.SchedulingContext->CanSchedule(GetNodeTag())) {
        attributes.Active = false;
        return;
    }

    if (starvingOnly && !Starving_) {
        attributes.Active = false;
        return;
    }

    if (IsBlocked(context.SchedulingContext->GetNow())) {
        attributes.Active = false;
        return;
    }

    TSchedulerElementBase::PrescheduleJob(context, starvingOnly);
}

bool TOperationElement::ScheduleJob(TFairShareContext& context)
{
    YCHECK(IsActive(context.DynamicAttributesList));

    auto updateAncestorsAttributes = [&] () {
        auto* parent = GetParent();
        while (parent) {
            parent->UpdateDynamicAttributes(context.DynamicAttributesList);
            parent = parent->GetParent();
        }
    };

    auto disableOperationElement = [&] () {
        context.DynamicAttributes(this).Active = false;
        updateAncestorsAttributes();
    };

    auto now = context.SchedulingContext->GetNow();
    if (IsBlocked(now))
    {
        disableOperationElement();
        return false;
    }

    if (!SharedState_->TryStartScheduleJob(
        now,
        StrategyConfig_->MaxConcurrentControllerScheduleJobCalls,
        StrategyConfig_->ControllerScheduleJobFailBackoffTime))
    {
        disableOperationElement();
        return false;
    }

    auto scheduleJobGuard = Finally([&] {
        SharedState_->FinishScheduleJob();
    });

    NProfiling::TScopedTimer timer;
    auto scheduleJobResult = DoScheduleJob(context);
    auto scheduleJobDuration = timer.GetElapsed();
    context.TotalScheduleJobDuration += scheduleJobDuration;
    context.ExecScheduleJobDuration += scheduleJobResult->Duration;

    for (auto reason : TEnumTraits<EScheduleJobFailReason>::GetDomainValues()) {
        context.FailedScheduleJob[reason] += scheduleJobResult->Failed[reason];
    }

    if (!scheduleJobResult->JobStartRequest) {
        disableOperationElement();
        Operation_->UpdateControllerTimeStatistics("/schedule_job/fail", scheduleJobDuration);

        if (scheduleJobResult->Failed[EScheduleJobFailReason::NotEnoughResources] == 0 &&
            scheduleJobResult->Failed[EScheduleJobFailReason::NoLocalJobs] == 0)
        {
            LOG_DEBUG("Failed to schedule job, backing off (OperationId: %v, Reasons: %v)",
                OperationId_,
                scheduleJobResult->Failed);
            SharedState_->EnableScheduleJobBackoff(context.SchedulingContext->GetNow());
        }

        return false;
    }

    const auto& jobStartRequest = scheduleJobResult->JobStartRequest.Get();
    context.SchedulingContext->ResourceUsage() += jobStartRequest.ResourceLimits;
    OnJobStarted(jobStartRequest.Id, jobStartRequest.ResourceLimits);
    auto job = context.SchedulingContext->StartJob(Operation_, jobStartRequest);
    context.JobToOperationElement[job] = this;

    UpdateDynamicAttributes(context.DynamicAttributesList);
    updateAncestorsAttributes();
    Operation_->UpdateControllerTimeStatistics("/schedule_job/success", scheduleJobDuration);

    // TODO(acid): Check hierarchical resource usage here.

    return true;
}

Stroka TOperationElement::GetId() const
{
    return ToString(OperationId_);
}

double TOperationElement::GetWeight() const
{
    return RuntimeParams_->Weight;
}

double TOperationElement::GetMinShareRatio() const
{
    return Spec_->MinShareRatio;
}

double TOperationElement::GetMaxShareRatio() const
{
    return Spec_->MaxShareRatio;
}

TNullable<Stroka> TOperationElement::GetNodeTag() const
{
    return Spec_->SchedulingTag;
}

ESchedulableStatus TOperationElement::GetStatus() const
{
    if (!Operation_->IsSchedulable()) {
        return ESchedulableStatus::Normal;
    }

    if (GetPendingJobCount() == 0) {
        return ESchedulableStatus::Normal;
    }

    return TSchedulerElementBase::GetStatus(Attributes_.AdjustedFairShareStarvationTolerance);
}

void TOperationElement::SetStarving(bool starving)
{
    if (starving && !GetStarving()) {
        TSchedulerElementBase::SetStarving(true);
        LOG_INFO("Operation is now starving (OperationId: %v, Status: %v)",
            GetId(),
            GetStatus());
    } else if (!starving && GetStarving()) {
        TSchedulerElementBase::SetStarving(false);
        LOG_INFO("Operation is no longer starving (OperationId: %v)",
            GetId());
    }
}

void TOperationElement::CheckForStarvation(TInstant now)
{
    auto minSharePreemptionTimeout = Attributes_.AdjustedMinSharePreemptionTimeout;
    auto fairSharePreemptionTimeout = Attributes_.AdjustedFairSharePreemptionTimeout;

    double jobCountRatio = GetPendingJobCount() / StrategyConfig_->JobCountPreemptionTimeoutCoefficient;

    if (jobCountRatio < 1.0) {
        minSharePreemptionTimeout *= jobCountRatio;
        fairSharePreemptionTimeout *= jobCountRatio;
    }

    TSchedulerElementBase::CheckForStarvation(
        minSharePreemptionTimeout,
        fairSharePreemptionTimeout,
        now);
}

bool TOperationElement::HasStarvingParent() const
{
    auto* parent = GetParent();
    while (parent) {
        if (parent->GetStarving()) {
            return true;
        }
        parent = parent->GetParent();
    }
    return false;
}

void TOperationElement::IncreaseResourceUsage(const TJobResources& delta)
{
    IncreaseLocalResourceUsage(delta);
    GetParent()->IncreaseResourceUsage(delta);
}

void TOperationElement::IncreaseJobResourceUsage(const TJobId& jobId, const TJobResources& resourcesDelta)
{
    IncreaseResourceUsage(resourcesDelta);
    SharedState_->IncreaseJobResourceUsage(jobId, resourcesDelta);
    SharedState_->UpdatePreemptableJobsList(Attributes_.FairShareRatio, TotalResourceLimits_);
}

bool TOperationElement::IsJobExisting(const TJobId& jobId) const
{
    return SharedState_->IsJobExisting(jobId);
}

bool TOperationElement::IsJobPreemptable(const TJobId& jobId) const
{
    return SharedState_->IsJobPreemptable(jobId);
}

int TOperationElement::GetPreemptableJobCount() const
{
    return SharedState_->GetPreemptableJobCount();
}

void TOperationElement::OnJobStarted(const TJobId& jobId, const TJobResources& resourceUsage)
{
    SharedState_->AddJob(jobId, resourceUsage);
    IncreaseResourceUsage(resourceUsage);
}

void TOperationElement::OnJobFinished(const TJobId& jobId)
{
    auto resourceUsage = SharedState_->RemoveJob(jobId);
    IncreaseResourceUsage(-resourceUsage);
}

ISchedulerElementPtr TOperationElement::Clone()
{
    return New<TOperationElement>(*this);
}

bool TOperationElement::IsBlocked(TInstant now) const
{
    return !Operation_->IsSchedulable() ||
        GetPendingJobCount() == 0 ||
        SharedState_->IsBlocked(
            now,
            StrategyConfig_->MaxConcurrentControllerScheduleJobCalls,
            StrategyConfig_->ControllerScheduleJobFailBackoffTime);
}

TJobResources TOperationElement::GetHierarchicalResourceLimits(const TFairShareContext& context) const
{
    const auto& schedulingContext = context.SchedulingContext;

    // Bound limits with node free resources.
    auto limits =
        schedulingContext->ResourceLimits()
        - schedulingContext->ResourceUsage()
        + schedulingContext->ResourceUsageDiscount();

    // Bound limits with pool free resources.
    auto* parent = GetParent();
    while (parent) {
        auto parentLimits =
            parent->ResourceLimits()
            - parent->GetResourceUsage()
            + context.DynamicAttributes(parent).ResourceUsageDiscount;

        limits = Min(limits, parentLimits);
        parent = parent->GetParent();
    }

    // Bound limits with operation free resources.
    limits = Min(limits, ResourceLimits() - GetResourceUsage());

    return limits;
}

TScheduleJobResultPtr TOperationElement::DoScheduleJob(TFairShareContext& context)
{
    auto jobLimits = GetHierarchicalResourceLimits(context);
    auto controller = Operation_->GetController();

    auto scheduleJobResultFuture = BIND(&IOperationController::ScheduleJob, controller)
        .AsyncVia(controller->GetCancelableInvoker())
        .Run(context.SchedulingContext, jobLimits);

    auto scheduleJobResultFutureWithTimeout = scheduleJobResultFuture
        .WithTimeout(StrategyConfig_->ControllerScheduleJobTimeLimit);

    auto scheduleJobResultWithTimeoutOrError = std::move(WaitFor(scheduleJobResultFutureWithTimeout));

    if (!scheduleJobResultWithTimeoutOrError.IsOK()) {
        auto scheduleJobResult = New<TScheduleJobResult>();
        if (scheduleJobResultWithTimeoutOrError.GetCode() == NYT::EErrorCode::Timeout) {
            LOG_WARNING("Controller is scheduling for too long, aborting ScheduleJob");
            ++scheduleJobResult->Failed[EScheduleJobFailReason::Timeout];
            // If ScheduleJob was not canceled we need to abort created job.
            scheduleJobResultFuture.Subscribe(
                BIND([=] (const TErrorOr<TScheduleJobResultPtr>& scheduleJobResultOrError) {
                    if (scheduleJobResultOrError.IsOK()) {
                        const auto& scheduleJobResult = scheduleJobResultOrError.Value();
                        if (scheduleJobResult->JobStartRequest) {
                            const auto& jobId = scheduleJobResult->JobStartRequest->Id;
                            LOG_WARNING("Aborting late job (JobId: %v, OperationId: %v)",
                                jobId,
                                OperationId_);
                            controller->OnJobAborted(
                                std::make_unique<TAbortedJobSummary>(
                                    jobId,
                                    EAbortReason::SchedulingTimeout));
                        }
                    }
            }));
        }
        return scheduleJobResult;
    }

    return scheduleJobResultWithTimeoutOrError.Value();
}

TJobResources TOperationElement::ComputeResourceDemand() const
{
    if (Operation_->IsSchedulable()) {
        const auto& controller = Operation_->GetController();
        return GetResourceUsage() + controller->GetNeededResources();
    }
    return ZeroJobResources();
}

TJobResources TOperationElement::ComputeResourceLimits() const
{
    auto maxShareLimits = Host_->GetResourceLimits(GetNodeTag()) * Spec_->MaxShareRatio;
    auto perTypeLimits = ToJobResources(Spec_->ResourceLimits);
    return Min(maxShareLimits, perTypeLimits);
}

TJobResources TOperationElement::ComputeMaxPossibleResourceUsage() const
{
    return Min(ResourceLimits(), ResourceDemand());
}

int TOperationElement::ComputePendingJobCount() const
{
    return Operation_->GetController()->GetPendingJobCount();
}

////////////////////////////////////////////////////////////////////

TRootElement::TRootElement(
    ISchedulerStrategyHost* host,
    TFairShareStrategyConfigPtr strategyConfig)
    : TCompositeSchedulerElement(host, strategyConfig)
{
    Attributes_.FairShareRatio = 1.0;
    Attributes_.AdjustedMinShareRatio = 1.0;
    Mode_ = ESchedulingMode::FairShare;
    Attributes_.AdjustedFairShareStarvationTolerance = GetFairShareStarvationTolerance();
    Attributes_.AdjustedMinSharePreemptionTimeout = GetMinSharePreemptionTimeout();
    Attributes_.AdjustedFairSharePreemptionTimeout = GetFairSharePreemptionTimeout();
    AdjustedFairShareStarvationToleranceLimit_ = GetFairShareStarvationToleranceLimit();
    AdjustedMinSharePreemptionTimeoutLimit_ = GetMinSharePreemptionTimeoutLimit();
    AdjustedFairSharePreemptionTimeoutLimit_ = GetFairSharePreemptionTimeoutLimit();
}

void TRootElement::Update(TDynamicAttributesList& dynamicAttributesList)
{
    TreeSize_ = TCompositeSchedulerElement::EnumerateNodes(0);
    dynamicAttributesList.assign(TreeSize_, TDynamicAttributes());
    TCompositeSchedulerElement::Update(dynamicAttributesList);
}

bool TRootElement::IsRoot() const
{
    return true;
}

TNullable<Stroka> TRootElement::GetNodeTag() const
{
    return Null;
}

Stroka TRootElement::GetId() const
{
    return Stroka(RootPoolName);
}

double TRootElement::GetWeight() const
{
    return 1.0;
}

double TRootElement::GetMinShareRatio() const
{
    return 0.0;
}

double TRootElement::GetMaxShareRatio() const
{
    return 1.0;
}

double TRootElement::GetFairShareStarvationTolerance() const
{
    return StrategyConfig_->FairShareStarvationTolerance;
}

TDuration TRootElement::GetMinSharePreemptionTimeout() const
{
    return StrategyConfig_->MinSharePreemptionTimeout;
}

TDuration TRootElement::GetFairSharePreemptionTimeout() const
{
    return StrategyConfig_->FairSharePreemptionTimeout;
}

void TRootElement::CheckForStarvation(TInstant now)
{
    YUNREACHABLE();
}

int TRootElement::GetMaxRunningOperationCount() const
{
    return StrategyConfig_->MaxRunningOperationCount;
}

int TRootElement::GetMaxOperationCount() const
{
    return StrategyConfig_->MaxOperationCount;
}

ISchedulerElementPtr TRootElement::Clone()
{
    return New<TRootElement>(*this);
}

TRootElementPtr TRootElement::CloneRoot()
{
    return New<TRootElement>(*this);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
