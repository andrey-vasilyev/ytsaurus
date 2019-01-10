#include "private.h"
#include "operations_cleaner.h"
#include "config.h"
#include "operation.h"
#include "bootstrap.h"
#include "helpers.h"

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/api/native/client.h>
#include <yt/ytlib/api/native/connection.h>

#include <yt/client/api/transaction.h>
#include <yt/client/api/operation_archive_schema.h>

#include <yt/client/security_client/public.h>

#include <yt/client/table_client/row_buffer.h>

#include <yt/core/actions/cancelable_context.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/nonblocking_batch.h>
#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/async_semaphore.h>

#include <yt/core/profiling/profiler.h>

#include <yt/core/utilex/random.h>

#include <yt/core/ytree/fluent.h>

namespace NYT::NScheduler {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NObjectClient;
using namespace NApi;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NProfiling;

static const NLogging::TLogger Logger("OperationsCleaner");

////////////////////////////////////////////////////////////////////////////////

struct TOrderedByIdTag
{ };

struct TOrderedByStartTimeTag
{ };

struct TOperationAliasesTag
{ };

////////////////////////////////////////////////////////////////////////////////

void TArchiveOperationRequest::InitializeFromOperation(const TOperationPtr& operation)
{
    Id = operation->GetId();
    StartTime = operation->GetStartTime();
    FinishTime = *operation->GetFinishTime();
    State = operation->GetState();
    AuthenticatedUser = operation->GetAuthenticatedUser();
    OperationType = operation->GetType();
    Spec = ConvertToYsonString(operation->GetSpec());
    Result = operation->BuildResultString();
    Events = ConvertToYsonString(operation->Events());
    Alerts = operation->BuildAlertsString();
    BriefSpec = operation->BriefSpec();
    RuntimeParameters = ConvertToYsonString(operation->GetRuntimeParameters(), EYsonFormat::Binary);
    Alias = operation->Alias();
    SlotIndexPerPoolTree = ConvertToYsonString(operation->GetSlotIndices(), EYsonFormat::Binary);

    const auto& attributes = operation->ControllerAttributes();
    const auto& initializationAttributes = attributes.InitializeAttributes;
    if (initializationAttributes) {
        UnrecognizedSpec = initializationAttributes->UnrecognizedSpec;
        FullSpec = initializationAttributes->FullSpec;
    }
}

const std::vector<TString>& TArchiveOperationRequest::GetAttributeKeys()
{
    // Keep the stuff below synchronized with InitializeFromAttributes method.
    static const std::vector<TString> attributeKeys = {
        "key",
        "start_time",
        "finish_time",
        "state",
        "authenticated_user",
        "operation_type",
        "progress",
        "brief_progress",
        "spec",
        "brief_spec",
        "result",
        "events",
        "alerts",
        "full_spec",
        "unrecognized_spec",
        "runtime_parameters",
        "alias",
        "slot_index_per_pool_tree",
    };

    return attributeKeys;
}

const std::vector<TString>& TArchiveOperationRequest::GetProgressAttributeKeys()
{
    static const std::vector<TString> attributeKeys = {
        "progress",
        "brief_progress",
    };

    return attributeKeys;
}

void TArchiveOperationRequest::InitializeFromAttributes(const IAttributeDictionary& attributes)
{
    Id = TOperationId::FromString(attributes.Get<TString>("key"));
    StartTime = attributes.Get<TInstant>("start_time");
    FinishTime = attributes.Get<TInstant>("finish_time");
    State = attributes.Get<EOperationState>("state");
    AuthenticatedUser = attributes.Get<TString>("authenticated_user");
    OperationType = attributes.Get<EOperationType>("operation_type");
    Progress = attributes.FindYson("progress");
    BriefProgress = attributes.FindYson("brief_progress");
    Spec = attributes.GetYson("spec");
    BriefSpec = attributes.FindYson("brief_spec");
    Result = attributes.GetYson("result");
    Events = attributes.GetYson("events");
    Alerts = attributes.GetYson("alerts");
    FullSpec = attributes.FindYson("full_spec");
    UnrecognizedSpec = attributes.FindYson("unrecognized_spec");
    RuntimeParameters = attributes.FindYson("runtime_parameters");
    Alias = ConvertTo<TOperationSpecBasePtr>(Spec)->Alias;
    SlotIndexPerPoolTree = attributes.FindYson("slot_index_per_pool_tree");
}

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

std::vector<TString> GetPools(const TYsonString& runtimeParameters)
{
    if (!runtimeParameters) {
        return {};
    }

    auto schedulingOptionsNode = ConvertToNode(runtimeParameters)->AsMap()->FindChild("scheduling_options_per_pool_tree");
    if (!schedulingOptionsNode) {
        return {};
    }

    std::vector<TString> pools;
    for (const auto& pair : schedulingOptionsNode->AsMap()->GetChildren()) {
        auto poolNode = pair.second->AsMap()->FindChild("pool");
        if (poolNode) { // COMPAT(renadeen): remove this check when everything will be on 19.4
            pools.push_back(poolNode->GetValue<TString>());
        }
    }

    return pools;
}

TString GetFilterFactors(const TArchiveOperationRequest& request)
{
    auto dropYPathAttributes = [] (const TString& path) -> TString {
        try {
            auto parsedPath = NYPath::TRichYPath::Parse(path);
            return parsedPath.GetPath();
        } catch (const std::exception& ex) {
            return "";
        }
    };

    auto specMapNode = ConvertToNode(request.Spec)->AsMap();

    std::vector<TString> parts;
    parts.push_back(ToString(request.Id));
    parts.push_back(request.AuthenticatedUser);
    parts.push_back(FormatEnum(request.State));
    parts.push_back(FormatEnum(request.OperationType));
    if (auto node = specMapNode->FindChild("annotations")) {
        parts.push_back(ConvertToYsonString(node, EYsonFormat::Text).GetData());
    }

    for (const auto& key : {"pool", "title"}) {
        auto node = specMapNode->FindChild(key);
        if (node && node->GetType() == ENodeType::String) {
            parts.push_back(node->AsString()->GetValue());
        }
    }

    for (const auto& key : {"input_table_paths", "output_table_paths"}) {
        auto node = specMapNode->FindChild(key);
        if (node && node->GetType() == ENodeType::List) {
            auto child = node->AsList()->FindChild(0);
            if (child && child->GetType() == ENodeType::String) {
                auto path = dropYPathAttributes(child->AsString()->GetValue());
                if (!path.empty()) {
                    parts.push_back(path);
                }
            }
        }
    }

    for (const auto& key : {"output_table_path", "table_path"}) {
        auto node = specMapNode->FindChild(key);
        if (node && node->GetType() == ENodeType::String) {
            auto path = dropYPathAttributes(node->AsString()->GetValue());
            if (!path.empty()) {
                parts.push_back(path);
            }
        }
    }

    if (request.RuntimeParameters) {
        auto pools = GetPools(request.RuntimeParameters);
        parts.insert(parts.end(), pools.begin(), pools.end());
    }

    auto result = JoinToString(parts.begin(), parts.end(), AsStringBuf(" "));
    return to_lower(result);
}

bool HasFailedJobs(const TYsonString& briefProgress)
{
    auto jobsNode = ConvertToNode(briefProgress)->AsMap()->FindChild("jobs");
    return jobsNode && jobsNode->AsMap()->GetChild("failed")->GetValue<i64>() > 0;
}

TUnversionedRow BuildOrderedByIdTableRow(
    const TRowBufferPtr& rowBuffer,
    const TArchiveOperationRequest& request,
    const TOrderedByIdTableDescriptor::TIndex& index,
    int version)
{
    // All any and string values passed to MakeUnversioned* functions MUST be alive till
    // they are captured in row buffer (they are not owned by unversioned value or builder).
    auto state = FormatEnum(request.State);
    auto operationType = FormatEnum(request.OperationType);
    auto filterFactors = GetFilterFactors(request);

    TUnversionedRowBuilder builder;
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[0], index.IdHi));
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[1], index.IdLo));
    builder.AddValue(MakeUnversionedStringValue(state, index.State));
    builder.AddValue(MakeUnversionedStringValue(request.AuthenticatedUser, index.AuthenticatedUser));
    builder.AddValue(MakeUnversionedStringValue(operationType, index.OperationType));
    if (request.Progress) {
        builder.AddValue(MakeUnversionedAnyValue(request.Progress.GetData(), index.Progress));
    }
    if (request.BriefProgress) {
        builder.AddValue(MakeUnversionedAnyValue(request.BriefProgress.GetData(), index.BriefProgress));
    }
    builder.AddValue(MakeUnversionedAnyValue(request.Spec.GetData(), index.Spec));
    if (request.BriefSpec) {
        builder.AddValue(MakeUnversionedAnyValue(request.BriefSpec.GetData(), index.BriefSpec));
    }
    builder.AddValue(MakeUnversionedInt64Value(request.StartTime.MicroSeconds(), index.StartTime));
    builder.AddValue(MakeUnversionedInt64Value(request.FinishTime.MicroSeconds(), index.FinishTime));
    builder.AddValue(MakeUnversionedStringValue(filterFactors, index.FilterFactors));
    builder.AddValue(MakeUnversionedAnyValue(request.Result.GetData(), index.Result));
    builder.AddValue(MakeUnversionedAnyValue(request.Events.GetData(), index.Events));
    builder.AddValue(MakeUnversionedAnyValue(request.Alerts.GetData(), index.Alerts));
    if (version >= 17) {
        if (request.UnrecognizedSpec) {
            builder.AddValue(MakeUnversionedAnyValue(request.UnrecognizedSpec.GetData(), index.UnrecognizedSpec));
        }
        if (request.FullSpec) {
            builder.AddValue(MakeUnversionedAnyValue(request.FullSpec.GetData(), index.FullSpec));
        }
    }

    if (version >= 22 && request.RuntimeParameters) {
        builder.AddValue(MakeUnversionedAnyValue(request.RuntimeParameters.GetData(), index.RuntimeParameters));
    }

    if (version >= 27 && request.SlotIndexPerPoolTree) {
        builder.AddValue(MakeUnversionedAnyValue(request.SlotIndexPerPoolTree.GetData(), index.SlotIndexPerPoolTree));
    }

    if (version >= 29 && request.Annotations) {
        builder.AddValue(MakeUnversionedAnyValue(request.Annotations.GetData(), index.Annotations));
    }

    return rowBuffer->Capture(builder.GetRow());
}

TUnversionedRow BuildOrderedByStartTimeTableRow(
    const TRowBufferPtr& rowBuffer,
    const TArchiveOperationRequest& request,
    const TOrderedByStartTimeTableDescriptor::TIndex& index,
    int version)
{
    // All any and string values passed to MakeUnversioned* functions MUST be alive till
    // they are captured in row buffer (they are not owned by unversioned value or builder).
    auto state = FormatEnum(request.State);
    auto operationType = FormatEnum(request.OperationType);
    auto filterFactors = GetFilterFactors(request);
    auto pools = ConvertToYsonString(GetPools(request.RuntimeParameters));

    TUnversionedRowBuilder builder;
    builder.AddValue(MakeUnversionedInt64Value(request.StartTime.MicroSeconds(), index.StartTime));
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[0], index.IdHi));
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[1], index.IdLo));
    builder.AddValue(MakeUnversionedStringValue(operationType, index.OperationType));
    builder.AddValue(MakeUnversionedStringValue(state, index.State));
    builder.AddValue(MakeUnversionedStringValue(request.AuthenticatedUser, index.AuthenticatedUser));
    builder.AddValue(MakeUnversionedStringValue(filterFactors, index.FilterFactors));

    if (version >= 24) {
        if (pools) {
            builder.AddValue(MakeUnversionedAnyValue(pools.GetData(), index.Pools));
        }
        if (request.BriefProgress) {
            builder.AddValue(MakeUnversionedBooleanValue(HasFailedJobs(request.BriefProgress), index.HasFailedJobs));
        }
    }

    return rowBuffer->Capture(builder.GetRow());
}

TUnversionedRow BuildOperationAliasesTableRow(
    const TRowBufferPtr& rowBuffer,
    const TArchiveOperationRequest& request,
    const TOperationAliasesTableDescriptor::TIndex& index,
    int /* version */)
{
    // All any and string values passed to MakeUnversioned* functions MUST be alive till
    // they are captured in row buffer (they are not owned by unversioned value or builder).

    TUnversionedRowBuilder builder;
    builder.AddValue(MakeUnversionedStringValue(*request.Alias, index.Alias));
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[0], index.OperationIdHi));
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[1], index.OperationIdLo));

    return rowBuffer->Capture(builder.GetRow());
}

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

class TOperationsCleaner::TImpl
    : public TRefCounted
{
public:
    DEFINE_SIGNAL(void(const std::vector<TArchiveOperationRequest>&), OperationsArchived);

    TImpl(
        TOperationsCleanerConfigPtr config,
        IOperationsCleanerHost* host,
        TBootstrap* bootstrap)
        : Config_(std::move(config))
        , Bootstrap_(bootstrap)
        , Host_(host)
        , RemoveBatcher_(Config_->RemoveBatchSize, Config_->RemoveBatchTimeout)
        , ArchiveBatcher_(Config_->ArchiveBatchSize, Config_->ArchiveBatchTimeout)
        , Client_(Bootstrap_->GetMasterClient()->GetNativeConnection()
            ->CreateNativeClient(TClientOptions(NSecurityClient::OperationsCleanerUserName)))
    { }

    void Start()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        DoStart(/* fetchFinishedOperations */ false);
    }

    void Stop()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        DoStop();
    }

    void UpdateConfig(const TOperationsCleanerConfigPtr& config)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        bool enable = Config_->Enable;
        bool enableArchivation = Config_->EnableArchivation;
        Config_ = config;

        if (enable != Config_->Enable) {
            if (Config_->Enable) {
                DoStart(/* fetchFinishedOperations */ true);
            } else {
                DoStop();
            }
        }

        if (enableArchivation != Config_->EnableArchivation) {
            if (Config_->EnableArchivation) {
                DoStartArchivation();
            } else {
                DoStopArchivation();
            }
        }

        YT_LOG_INFO("Operations cleaner config updated (Enable: %v, EnableArchivation: %v)",
            Config_->Enable,
            Config_->EnableArchivation);
    }

    void SubmitForArchivation(TArchiveOperationRequest request)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!IsEnabled()) {
            return;
        }

        auto id = request.Id;

        // Can happen if scheduler reported operation and archiver was turned on and
        // fetched the same operation from Cypress.
        if (OperationMap_.find(id) != OperationMap_.end()) {
            return;
        }

        auto deadline = request.FinishTime + Config_->CleanDelay;

        ArchiveTimeToOperationIdMap_.emplace(deadline, id);
        YCHECK(OperationMap_.emplace(id, std::move(request)).second);

        YT_LOG_DEBUG("Operation submitted for archivation (OperationId: %v, ArchivationStartTime: %v)",
            id,
            deadline);
    }

    void SubmitForRemoval(TRemoveOperationRequest request)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!IsEnabled()) {
            return;
        }

        EnqueueForRemoval(request.Id);
        YT_LOG_DEBUG("Operation submitted for removal (OperationId: %v)", request.Id);
    }

    void SetArchiveVersion(int version)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        ArchiveVersion_ = version;
    }

    bool IsEnabled() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return Enabled_;
    }

    void BuildOrchid(TFluentMap fluent) const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        fluent
            .Item("enable").Value(IsEnabled())
            .Item("enable_archivation").Value(IsArchivationEnabled())
            .Item("remove_pending").Value(RemovePendingCounter_.GetCurrent())
            .Item("archive_pending").Value(ArchivePendingCounter_.GetCurrent())
            .Item("submitted_count").Value(ArchiveTimeToOperationIdMap_.size());
    }

private:
    TOperationsCleanerConfigPtr Config_;
    const TBootstrap* const Bootstrap_;
    IOperationsCleanerHost* const Host_;

    TPeriodicExecutorPtr AnalysisExecutor_;

    TCancelableContextPtr CancelableContext_;
    IInvokerPtr CancelableControlInvoker_;

    int ArchiveVersion_ = -1;

    bool Enabled_ = false;
    bool ArchivationEnabled_ = false;

    TDelayedExecutorCookie ArchivationStartCookie_;

    std::multimap<TInstant, TOperationId> ArchiveTimeToOperationIdMap_;
    THashMap<TOperationId, TArchiveOperationRequest> OperationMap_;

    TNonblockingBatch<TOperationId> RemoveBatcher_;
    TNonblockingBatch<TOperationId> ArchiveBatcher_;

    NNative::IClientPtr Client_;

    TProfiler Profiler = {"/operations_cleaner"};

    TSimpleGauge RemovePendingCounter_ {"/remove_pending"};
    TSimpleGauge ArchivePendingCounter_ {"/archive_pending"};
    TMonotonicCounter ArchivedCounter_ {"/archived"};
    TMonotonicCounter RemovedCounter_ {"/removed"};
    TMonotonicCounter CommittedDataWeightCounter_ {"/committed_data_weight"};
    TMonotonicCounter ArchiveErrorCounter_ {"/archive_errors"};
    TMonotonicCounter RemoveErrorCounter_ {"/remove_errors"};

    TAggregateGauge AnalyzeOperationsTimeCounter_ = {"/analyze_operations_time"};
    TAggregateGauge OperationsRowsPreparationTimeCounter_ = {"/operations_rows_preparation_time"};

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

private:
    IInvokerPtr GetInvoker() const
    {
        return CancelableControlInvoker_;
    }

    void ScheduleArchiveOperations()
    {
        GetInvoker()->Invoke(BIND(&TImpl::ArchiveOperations, MakeStrong(this)));
    }

    void DoStart(bool fetchFinishedOperations)
    {
        if (Config_->Enable && !Enabled_) {
            Enabled_ = true;

            YCHECK(!CancelableContext_);
            CancelableContext_ = New<TCancelableContext>();
            CancelableControlInvoker_ = CancelableContext_->CreateInvoker(
                Bootstrap_->GetControlInvoker(EControlQueue::OperationsCleaner));

            AnalysisExecutor_ = New<TPeriodicExecutor>(
                CancelableControlInvoker_,
                BIND(&TImpl::OnAnalyzeOperations, MakeWeak(this)),
                Config_->AnalysisPeriod);

            AnalysisExecutor_->Start();

            GetInvoker()->Invoke(BIND(&TImpl::RemoveOperations, MakeStrong(this)));

            ScheduleArchiveOperations();
            DoStartArchivation();

            // If operations cleaner was disabled during scheduler runtime and then
            // enabled then we should fetch all finished operation since scheduler did not
            // reported them.
            if (fetchFinishedOperations) {
                GetInvoker()->Invoke(BIND(&TImpl::FetchFinishedOperations, MakeStrong(this)));
            }

            YT_LOG_INFO("Operations cleaner started");
        }
    }

    void DoStartArchivation()
    {
        if (Config_->Enable && Config_->EnableArchivation && !ArchivationEnabled_) {
            ArchivationEnabled_ = true;
            TDelayedExecutor::CancelAndClear(ArchivationStartCookie_);
            Host_->SetSchedulerAlert(ESchedulerAlertType::OperationsArchivation, TError());

            YT_LOG_INFO("Operations archivation started");
        }
    }

    void DoStopArchivation()
    {
        if (!ArchivationEnabled_) {
            return;
        }

        ArchivationEnabled_ = false;
        TDelayedExecutor::CancelAndClear(ArchivationStartCookie_);
        Host_->SetSchedulerAlert(ESchedulerAlertType::OperationsArchivation, TError());

        YT_LOG_INFO("Operations archivation stopped");
    }

    void DoStop()
    {
        if (!Enabled_) {
            return;
        }

        Enabled_ = false;

        CancelableContext_->Cancel();
        CancelableContext_.Reset();
        CancelableControlInvoker_ = nullptr;

        AnalysisExecutor_->Stop();
        AnalysisExecutor_.Reset();

        TDelayedExecutor::CancelAndClear(ArchivationStartCookie_);

        DoStopArchivation();

        ArchiveBatcher_.Drop();
        RemoveBatcher_.Drop();
        ArchiveTimeToOperationIdMap_.clear();
        OperationMap_.clear();
        Profiler.Update(ArchivePendingCounter_, 0);
        Profiler.Update(RemovePendingCounter_, 0);

        YT_LOG_INFO("Operations cleaner stopped");
    }

    void OnAnalyzeOperations()
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        YT_LOG_INFO("Analyzing operations submitted for archivation (SubmittedOperationCount: %v)",
            ArchiveTimeToOperationIdMap_.size());

        if (ArchiveTimeToOperationIdMap_.empty()) {
            YT_LOG_INFO("No operations submitted for archivation");
            return;
        }

        auto now = TInstant::Now();

        int retainedCount = 0;
        int enqueuedForArchivationCount = 0;
        THashMap<TString, int> operationCountPerUser;

        auto canArchive = [&] (const auto& request) {
            if (retainedCount >= Config_->HardRetainedOperationCount) {
                return true;
            }

            if (now - request.FinishTime > Config_->MaxOperationAge) {
                return true;
            }

            if (!IsOperationWithUserJobs(request.OperationType) &&
                request.State == EOperationState::Completed)
            {
                return true;
            }

            if (operationCountPerUser[request.AuthenticatedUser] >= Config_->MaxOperationCountPerUser) {
                return true;
            }

            // TODO(asaitgalin): Consider only operations without stderrs?
            if (retainedCount >= Config_->SoftRetainedOperationCount &&
                request.State != EOperationState::Failed)
            {
                return true;
            }

            return false;
        };

        // Analyze operations with expired grace timeout, from newest to oldest.
        PROFILE_AGGREGATED_TIMING(AnalyzeOperationsTimeCounter_) {
            auto it = ArchiveTimeToOperationIdMap_.lower_bound(now);
            while (it != ArchiveTimeToOperationIdMap_.begin()) {
                --it;

                auto operationId = it->second;
                const auto& request = GetRequest(operationId);
                if (canArchive(request)) {
                    it = ArchiveTimeToOperationIdMap_.erase(it);
                    CleanOperation(operationId);
                    enqueuedForArchivationCount += 1;
                } else {
                    retainedCount += 1;
                    operationCountPerUser[request.AuthenticatedUser] += 1;
                }
            }
        }

        YT_LOG_INFO("Finished analyzing operations submitted for archivation "
            "(RetainedCount: %v, EnqueuedForArchivationCount: %v)",
            retainedCount,
            enqueuedForArchivationCount);
    }

    void EnqueueForRemoval(TOperationId operationId)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YT_LOG_DEBUG("Operation enqueued for removal (OperationId: %v)", operationId);
        Profiler.Increment(RemovePendingCounter_, 1);
        RemoveBatcher_.Enqueue(operationId);
    }

    void EnqueueForArchivation(TOperationId operationId)
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        YT_LOG_DEBUG("Operation enqueued for archivation (OperationId: %v)", operationId);
        Profiler.Increment(ArchivePendingCounter_, 1);
        ArchiveBatcher_.Enqueue(operationId);
    }

    void CleanOperation(TOperationId operationId)
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        if (IsArchivationEnabled()) {
            EnqueueForArchivation(operationId);
        } else {
            EnqueueForRemoval(operationId);
        }
    }

    void TryArchiveOperations(const std::vector<TOperationId>& operationIds)
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        int version = ArchiveVersion_;
        if (version == -1) {
            THROW_ERROR_EXCEPTION("Unknown operations archive version");
        }

        auto asyncTransaction = Client_->StartTransaction(
            ETransactionType::Tablet, TTransactionStartOptions{});
        auto transaction = WaitFor(asyncTransaction)
            .ValueOrThrow();

        YT_LOG_DEBUG("Operations archivation transaction started (TransactionId: %v, OperationCount: %v)",
            transaction->GetId(),
            operationIds.size());

        i64 orderedByIdRowsDataWeight = 0;
        i64 orderedByStartTimeRowsDataWeight = 0;
        i64 operationAliasesRowsDataWeight = 0;

        PROFILE_AGGREGATED_TIMING(OperationsRowsPreparationTimeCounter_) {
            // ordered_by_id table rows
            {
                TOrderedByIdTableDescriptor desc;
                auto rowBuffer = New<TRowBuffer>(TOrderedByIdTag{});
                std::vector<TUnversionedRow> rows;
                rows.reserve(operationIds.size());

                for (const auto& operationId : operationIds) {
                    const auto& request = GetRequest(operationId);
                    auto row = NDetail::BuildOrderedByIdTableRow(rowBuffer, request, desc.Index, version);
                    rows.push_back(row);
                    orderedByIdRowsDataWeight += GetDataWeight(row);
                }

                transaction->WriteRows(
                    GetOperationsArchiveOrderedByIdPath(),
                    desc.NameTable,
                    MakeSharedRange(std::move(rows), std::move(rowBuffer)));
            }

            // ordered_by_start_time rows
            {
                TOrderedByStartTimeTableDescriptor desc;
                auto rowBuffer = New<TRowBuffer>(TOrderedByStartTimeTag{});
                std::vector<TUnversionedRow> rows;
                rows.reserve(operationIds.size());

                for (const auto& operationId : operationIds) {
                    const auto& request = GetRequest(operationId);
                    auto row = NDetail::BuildOrderedByStartTimeTableRow(rowBuffer, request, desc.Index, version);
                    rows.push_back(row);
                    orderedByStartTimeRowsDataWeight += GetDataWeight(row);
                }

                transaction->WriteRows(
                    GetOperationsArchiveOrderedByStartTimePath(),
                    desc.NameTable,
                    MakeSharedRange(std::move(rows), std::move(rowBuffer)));
            }

            // operation_aliases rows
            if (ArchiveVersion_ >= 26) {
                TOperationAliasesTableDescriptor desc;
                auto rowBuffer = New<TRowBuffer>(TOperationAliasesTag{});
                std::vector<TUnversionedRow> rows;
                rows.reserve(operationIds.size());

                for (const auto& operationId : operationIds) {
                    const auto& request = GetRequest(operationId);

                    if (request.Alias) {
                        auto row = NDetail::BuildOperationAliasesTableRow(rowBuffer, request, desc.Index, version);
                        rows.emplace_back(row);
                        operationAliasesRowsDataWeight += GetDataWeight(row);
                    }
                }

                transaction->WriteRows(
                    GetOperationsArchiveOperationAliasesPath(),
                    desc.NameTable,
                    MakeSharedRange(std::move(rows), std::move(rowBuffer)));
            }
        }

        i64 totalDataWeight = orderedByIdRowsDataWeight + orderedByStartTimeRowsDataWeight;

        YT_LOG_DEBUG("Started committing archivation transaction (TransactionId: %v, OperationCount: %v, OrderedByIdRowsDataWeight: %v, "
            "OrderedByStartTimeRowsDataWeight: %v, TotalDataWeight: %v)",
            transaction->GetId(),
            operationIds.size(),
            orderedByIdRowsDataWeight,
            orderedByStartTimeRowsDataWeight,
            totalDataWeight);

        WaitFor(transaction->Commit())
            .ThrowOnError();

        YT_LOG_DEBUG("Finished committing archivation transaction (TransactionId: %v)", transaction->GetId());

        Profiler.Increment(CommittedDataWeightCounter_, totalDataWeight);
        Profiler.Increment(ArchivedCounter_, operationIds.size());
    }

    bool IsArchivationEnabled() const
    {
        return IsEnabled() && ArchivationEnabled_;
    }

    void ArchiveOperations()
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        auto batch = WaitFor(ArchiveBatcher_.DequeueBatch())
            .ValueOrThrow();

        if (!batch.empty()) {
            while (IsArchivationEnabled()) {
                try {
                    TryArchiveOperations(batch);
                    break;
                } catch (const std::exception& ex) {
                    YT_LOG_WARNING(ex, "Failed to archive operations (PendingCount: %v)",
                        ArchivePendingCounter_.GetCurrent());
                    Profiler.Increment(ArchiveErrorCounter_, 1);
                }

                if (ArchivePendingCounter_.GetCurrent() > Config_->MaxOperationCountEnqueuedForArchival) {
                    TemporarilyDisableArchivation();
                    break;
                } else {
                    auto sleepDelay = Config_->MinArchivationRetrySleepDelay +
                        RandomDuration(Config_->MaxArchivationRetrySleepDelay - Config_->MinArchivationRetrySleepDelay);
                    TDelayedExecutor::WaitForDuration(sleepDelay);
                }
            }

            std::vector<TArchiveOperationRequest> archivedOperationRequests;
            archivedOperationRequests.reserve(batch.size());
            for (const auto& operationId : batch) {
                auto it = OperationMap_.find(operationId);
                YCHECK(it != OperationMap_.end());
                archivedOperationRequests.emplace_back(std::move(it->second));
                OperationMap_.erase(it);
                EnqueueForRemoval(operationId);
            }

            OperationsArchived_.Fire(archivedOperationRequests);

            Profiler.Increment(ArchivePendingCounter_, -batch.size());
        }

        ScheduleArchiveOperations();
    }

    void RemoveOperations()
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        auto batch = WaitFor(RemoveBatcher_.DequeueBatch())
            .ValueOrThrow();

        if (!batch.empty()) {
            YT_LOG_DEBUG("Removing operations from Cypress (OperationCount: %v)", batch.size());

            std::vector<TOperationId> failedOperationIds;
            std::vector<TOperationId> operationIdsToRemove;

            {
                auto channel = Client_->GetMasterChannelOrThrow(
                    EMasterChannelKind::Follower,
                    PrimaryMasterCellTag);

                TObjectServiceProxy proxy(channel);
                auto batchReq = proxy.ExecuteBatch();

                for (const auto& operationId : batch) {
                    auto req = TYPathProxy::Get(GetOperationPath(operationId) + "/@lock_count");
                    batchReq->AddRequest(req, "get_lock_count");
                }

                auto batchRspOrError = WaitFor(batchReq->Invoke());

                if (batchRspOrError.IsOK()) {
                    const auto& batchRsp = batchRspOrError.Value();
                    auto rsps = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_lock_count");
                    YCHECK(rsps.size() == batch.size());

                    for (int index = 0; index < rsps.size(); ++index) {
                        bool isLocked = false;
                        const auto rsp = rsps[index];
                        if (rsp.IsOK()) {
                            auto lockCountNode = ConvertToNode(TYsonString(rsp.Value()->value()));
                            if (lockCountNode->AsUint64()->GetValue() > 0) {
                                isLocked = true;
                            }
                        }

                        const auto& operationId = batch[index];
                        if (isLocked) {
                            failedOperationIds.push_back(operationId);
                        } else {
                            operationIdsToRemove.push_back(operationId);
                        }
                    }
                } else {
                    YT_LOG_WARNING(
                        batchRspOrError,
                        "Failed to get lock count for operations from Cypress (OperationCount: %v)",
                        batch.size());

                    failedOperationIds = batch;
                }
            }

            if (!operationIdsToRemove.empty()) {
                auto channel = Client_->GetMasterChannelOrThrow(
                    EMasterChannelKind::Leader,
                    PrimaryMasterCellTag);

                TObjectServiceProxy proxy(channel);
                auto batchReq = proxy.ExecuteBatch();

                for (const auto& operationId : operationIdsToRemove) {
                    auto req = TYPathProxy::Remove(GetOperationPath(operationId));
                    req->set_recursive(true);
                    batchReq->AddRequest(req, "remove_operation");
                }

                auto batchRspOrError = WaitFor(batchReq->Invoke());
                if (batchRspOrError.IsOK()) {
                    const auto& batchRsp = batchRspOrError.Value();
                    auto rsps = batchRsp->GetResponses<TYPathProxy::TRspRemove>("remove_operation");
                    YCHECK(rsps.size() == operationIdsToRemove.size());

                    for (int index = 0; index < operationIdsToRemove.size(); ++index) {
                        const auto& operationId = operationIdsToRemove[index];

                        auto rsp = rsps[index];
                        if (!rsp.IsOK()) {
                            YT_LOG_DEBUG(
                                rsp,
                                "Failed to remove finished operation from Cypress (OperationId: %v)",
                                operationId);

                            failedOperationIds.push_back(operationId);
                        }
                    }
                } else {
                    YT_LOG_WARNING(
                        batchRspOrError,
                        "Failed to remove finished operations from Cypress (OperationCount: %v)",
                        operationIdsToRemove.size());

                    for (const auto& operationId : operationIdsToRemove) {
                        failedOperationIds.push_back(operationId);
                    }
                }
            }

            YCHECK(batch.size() >= failedOperationIds.size());
            int removedCount = batch.size() - failedOperationIds.size();
            YT_LOG_DEBUG("Successfully removed %v operations from Cypress", removedCount);

            Profiler.Increment(RemovedCounter_, removedCount);
            Profiler.Increment(RemoveErrorCounter_, failedOperationIds.size());

            for (const auto& operationId : failedOperationIds) {
                RemoveBatcher_.Enqueue(operationId);
            }

            Profiler.Increment(RemovePendingCounter_, -removedCount);
        }

        auto callback = BIND(&TImpl::RemoveOperations, MakeStrong(this))
            .Via(GetInvoker());

        TDelayedExecutor::Submit(callback, RandomDuration(Config_->MaxRemovalSleepDelay));
    }

    void TemporarilyDisableArchivation()
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        DoStopArchivation();

        auto enableCallback = BIND(&TImpl::DoStartArchivation, MakeStrong(this))
            .Via(GetInvoker());

        ArchivationStartCookie_ = TDelayedExecutor::Submit(
            enableCallback,
            Config_->ArchivationEnableDelay);

        auto enableTime = TInstant::Now() + Config_->ArchivationEnableDelay;

        Host_->SetSchedulerAlert(
            ESchedulerAlertType::OperationsArchivation,
            TError("Max enqueued operations limit reached; archivation is temporarily disabled")
            << TErrorAttribute("enable_time", enableTime));

        YT_LOG_INFO("Archivation is temporarily disabled (EnableTime: %v)", enableTime);
    }

    void FetchFinishedOperations()
    {
        try {
            DoFetchFinishedOperations();
        } catch (const std::exception& ex) {
            // NOTE(asaitgalin): Maybe disconnect? What can we do here?
            YT_LOG_WARNING(ex, "Failed to fetch finished operations from Cypress");
        }
    }

    void DoFetchFinishedOperations()
    {
        YT_LOG_INFO("Fetching all finished operations from Cypress");

        auto createBatchRequest = BIND([this] {
            auto channel = Client_->GetMasterChannelOrThrow(
                EMasterChannelKind::Follower, PrimaryMasterCellTag);

            TObjectServiceProxy proxy(channel);
            return proxy.ExecuteBatch();
        });

        auto listOperationsResult = ListOperations(createBatchRequest);

        // Remove some operations.
        for (const auto& operation : listOperationsResult.OperationsToRemove) {
            SubmitForRemoval({operation});
        }

        auto operations = FetchOperationsFromCypressForCleaner(
            listOperationsResult.OperationsToArchive,
            createBatchRequest,
            Config_->FetchBatchSize);

        // NB: needed for us to store the latest operation for each alias in operation_aliases archive table.
        std::sort(operations.begin(), operations.end(), [] (const auto& lhs, const auto& rhs) {
            return lhs.FinishTime < rhs.FinishTime;
        });

        for (auto& operation : operations) {
            SubmitForArchivation(std::move(operation));
        }

        YT_LOG_INFO("Fetched and processed all finished operations");
    }

    const TArchiveOperationRequest& GetRequest(TOperationId operationId) const
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        auto it = OperationMap_.find(operationId);
        YCHECK(it != OperationMap_.end());
        return it->second;
    }
};

////////////////////////////////////////////////////////////////////////////////

TOperationsCleaner::TOperationsCleaner(
    TOperationsCleanerConfigPtr config,
    IOperationsCleanerHost* host,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(std::move(config), host, bootstrap))
{ }

void TOperationsCleaner::Start()
{
    Impl_->Start();
}

void TOperationsCleaner::Stop()
{
    Impl_->Stop();
}

void TOperationsCleaner::SubmitForArchivation(TArchiveOperationRequest request)
{
    Impl_->SubmitForArchivation(std::move(request));
}

void TOperationsCleaner::SubmitForRemoval(TRemoveOperationRequest request)
{
    Impl_->SubmitForRemoval(std::move(request));
}

void TOperationsCleaner::UpdateConfig(const TOperationsCleanerConfigPtr& config)
{
    Impl_->UpdateConfig(config);
}

void TOperationsCleaner::SetArchiveVersion(int version)
{
    Impl_->SetArchiveVersion(version);
}

bool TOperationsCleaner::IsEnabled() const
{
    return Impl_->IsEnabled();
}

void TOperationsCleaner::BuildOrchid(TFluentMap fluent) const
{
    Impl_->BuildOrchid(fluent);
}

DELEGATE_SIGNAL(TOperationsCleaner, void(const std::vector<TArchiveOperationRequest>& requests), OperationsArchived, *Impl_);

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

template <class TForwardIt, class TFunctor, class TSize>
void SplitAndApply(
    TForwardIt begin,
    TForwardIt end,
    TFunctor functor,
    TSize chunkSize = 1)
{
    if (chunkSize < 1) {
        throw std::invalid_argument("Chunk size must be greater than zero");
    }

    auto current = begin;
    while (current != end) {
        auto distance = std::distance(current, end);
        auto it = current;
        if (distance < chunkSize) {
            std::advance(it, distance);
        } else {
            std::advance(it, chunkSize);
        }
        functor(current, it);
        current = it;
    }
}

template <class T, class TFunctor, class TSize>
void SplitAndApply(
    const std::vector<T>& vec,
    TFunctor functor,
    TSize chunkSize = 1)
{
    SplitAndApply(
        vec.begin(),
        vec.end(),
        [functor] (typename std::vector<T>::const_iterator begin, typename std::vector<T>::const_iterator end) {
            std::vector<T> tmp(begin, end);
            functor(tmp);
        },
        chunkSize);
}

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

std::vector<TArchiveOperationRequest> FetchOperationsFromCypressForCleaner(
    const std::vector<TOperationId>& operationIds,
    TCallback<TObjectServiceProxy::TReqExecuteBatchPtr()> createBatchRequest,
    int batchSize)
{
    using NYT::ToProto;

    std::vector<TArchiveOperationRequest> result;

    auto fetchBatch = [&] (const std::vector<TOperationId>& batch) {
        auto batchReq = createBatchRequest();

        for (const auto& operationId : batch) {
            auto req = TYPathProxy::Get(GetOperationPath(operationId) + "/@");
            ToProto(req->mutable_attributes()->mutable_keys(), TArchiveOperationRequest::GetAttributeKeys());
            batchReq->AddRequest(req, "get_op_attributes");
        }

        auto rspOrError = WaitFor(batchReq->Invoke());
        auto error = GetCumulativeError(rspOrError);
        THROW_ERROR_EXCEPTION_IF_FAILED(error, "Error requesting operations attributes for archivation");

        auto rsps = rspOrError.Value()->GetResponses<TYPathProxy::TRspGet>("get_op_attributes");
        YCHECK(batch.size() == rsps.size());

        for (int index = 0; index < batch.size(); ++index) {
            auto attributes = ConvertToAttributes(TYsonString(rsps[index].Value()->value()));
            YCHECK(TOperationId::FromString(attributes->Get<TString>("key")) == batch[index]);

            TArchiveOperationRequest req;
            req.InitializeFromAttributes(*attributes);
            result.push_back(req);
        }
    };

    NDetail::SplitAndApply(operationIds, fetchBatch, batchSize);

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
