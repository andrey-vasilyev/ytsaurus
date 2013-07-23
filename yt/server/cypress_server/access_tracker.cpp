#include "stdafx.h"
#include "access_tracker.h"
#include "node.h"
#include "config.h"
#include "cypress_manager.h"
#include "private.h"

#include <ytlib/profiling/timing.h>

#include <server/cell_master/bootstrap.h>
#include <server/cell_master/meta_state_facade.h>

#include <server/object_server/object_manager.h>

namespace NYT {
namespace NCypressServer {

using namespace NTransactionServer;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = CypressServerLogger;

////////////////////////////////////////////////////////////////////////////////

TAccessTracker::TAccessTracker(
    TCypressManagerConfigPtr config,
    NCellMaster::TBootstrap* bootstrap)
    : Config(config)
    , Bootstrap(bootstrap)
{ }

void TAccessTracker::StartFlush()
{
    VERIFY_THREAD_AFFINITY(StateThread);

    YCHECK(!FlushInvoker);
    FlushInvoker = New<TPeriodicInvoker>(
        Bootstrap->GetMetaStateFacade()->GetEpochInvoker(),
        BIND(&TAccessTracker::OnFlush, MakeWeak(this)),
        Config->AccessStatisticsFlushPeriod,
        EPeriodicInvokerMode::Manual);
    FlushInvoker->Start();
}

void TAccessTracker::StopFlush()
{
    VERIFY_THREAD_AFFINITY(StateThread);

    if (FlushInvoker) {
        FlushInvoker->Stop();
        FlushInvoker.Reset();
    }

    Reset();
}

void TAccessTracker::SetModified(
    TCypressNodeBase* trunkNode,
    TTransaction* transaction)
{
    VERIFY_THREAD_AFFINITY(StateThread);
    YCHECK(trunkNode->IsTrunk());

    // Failure here means that the node wasn't indeed locked,
    // which is strange given that we're about to mark it as modified.
    TVersionedNodeId versionedId(trunkNode->GetId(), GetObjectId(transaction));
    auto cypressManager = Bootstrap->GetCypressManager();
    auto* node = cypressManager->GetNode(versionedId);

    auto objectManager = Bootstrap->GetObjectManager();
    auto* mutationContext = Bootstrap
        ->GetMetaStateFacade()
        ->GetManager()
        ->GetMutationContext();

    node->SetModificationTime(mutationContext->GetTimestamp());
    node->SetRevision(mutationContext->GetVersion().ToRevision());
}

void TAccessTracker::SetAccessed(TCypressNodeBase* trunkNode)
{
    VERIFY_THREAD_AFFINITY(StateThread);
    YCHECK(trunkNode->IsTrunk());

    auto* update = trunkNode->GetAccessStatisticsUpdate();
    if (!update) {
        update = UpdateAccessStatisticsRequest.add_updates();
        ToProto(update->mutable_node_id(), trunkNode->GetId());
        trunkNode->SetAccessStatisticsUpdate(update);
        NodesWithAccessStatisticsUpdate.push_back(trunkNode);
    }

    auto objectManager = Bootstrap->GetObjectManager();
    auto now = NProfiling::CpuInstantToInstant(NProfiling::GetCpuInstant());
    update->set_access_time(now.MicroSeconds());
    update->set_access_counter_delta(update->access_counter_delta() + 1);
}

void TAccessTracker::Reset()
{
    FOREACH (auto* node, NodesWithAccessStatisticsUpdate) {
        node->SetAccessStatisticsUpdate(nullptr);
    }    

    UpdateAccessStatisticsRequest.Clear();
    NodesWithAccessStatisticsUpdate.clear();
}

void TAccessTracker::OnFlush()
{
    VERIFY_THREAD_AFFINITY(StateThread);

    if (NodesWithAccessStatisticsUpdate.empty()) {
        FlushInvoker->ScheduleNext();
        return;
    }

    LOG_DEBUG("Starting access statistics commit for %d nodes",
        UpdateAccessStatisticsRequest.updates_size());

    auto metaStateFacade = Bootstrap->GetMetaStateFacade();
    auto invoker = metaStateFacade->GetEpochInvoker();
    Bootstrap
        ->GetCypressManager()
        ->CreateUpdateAccessStatisticsMutation(UpdateAccessStatisticsRequest)
        ->OnSuccess(BIND(&TAccessTracker::OnCommitSucceeded, MakeWeak(this)).Via(invoker))
        ->OnError(BIND(&TAccessTracker::OnCommitFailed, MakeWeak(this)).Via(invoker))
        ->PostCommit();

    Reset();
}

void TAccessTracker::OnCommitSucceeded()
{
    LOG_DEBUG("Access statistics commit succeeded");

    FlushInvoker->ScheduleOutOfBand();
    FlushInvoker->ScheduleNext();
}

void TAccessTracker::OnCommitFailed(const TError& error)
{
    LOG_ERROR(error, "Access statistics commit failed");

    FlushInvoker->ScheduleNext();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
