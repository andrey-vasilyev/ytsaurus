#include "stdafx.h"
#include "tablet_manager.h"
#include "tablet_slot.h"
#include "automaton.h"
#include "tablet.h"
#include "partition.h"
#include "transaction.h"
#include "transaction_manager.h"
#include "config.h"
#include "store_manager.h"
#include "slot_manager.h"
#include "dynamic_memory_store.h"
#include "chunk_store.h"
#include "store_flusher.h"
#include "lookup.h"
#include "private.h"
#include "security_manager.h"
#include "in_memory_manager.h"

#include <core/misc/ring_queue.h>
#include <core/misc/string.h>
#include <core/misc/nullable.h>

#include <core/ytree/fluent.h>

#include <core/compression/codec.h>

#include <ytlib/new_table_client/name_table.h>
#include <ytlib/new_table_client/chunk_meta_extensions.h>

#include <ytlib/tablet_client/config.h>
#include <ytlib/tablet_client/wire_protocol.h>
#include <ytlib/tablet_client/wire_protocol.pb.h>

#include <ytlib/chunk_client/block_cache.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>

#include <ytlib/object_client/helpers.h>

#include <server/misc/memory_usage_tracker.h>

#include <server/hydra/hydra_manager.h>
#include <server/hydra/mutation.h>
#include <server/hydra/mutation_context.h>

#include <server/tablet_node/tablet_manager.pb.h>

#include <server/tablet_server/tablet_manager.pb.h>

#include <server/hive/hive_manager.h>
#include <server/hive/transaction_supervisor.pb.h>

#include <server/data_node/block_store.h>

#include <server/cell_node/bootstrap.h>

namespace NYT {
namespace NTabletNode {

using namespace NCompression;
using namespace NConcurrency;
using namespace NYson;
using namespace NYTree;
using namespace NHydra;
using namespace NCellNode;
using namespace NTabletClient;
using namespace NTabletClient::NProto;
using namespace NTabletNode::NProto;
using namespace NTabletServer::NProto;
using namespace NVersionedTableClient;
using namespace NTransactionClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NObjectClient;
using namespace NHive;
using namespace NHive::NProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletNodeLogger;
static const auto BlockedRowWaitQuantum = TDuration::MilliSeconds(100);

////////////////////////////////////////////////////////////////////////////////

class TTabletManager::TImpl
    : public TTabletAutomatonPart
{
public:
    explicit TImpl(
        TTabletManagerConfigPtr config,
        TTabletSlotPtr slot,
        TBootstrap* bootstrap)
        : TTabletAutomatonPart(
            slot,
            bootstrap)
        , Config_(config)
        , ChangelogCodec_(GetCodec(Config_->ChangelogCodec))
        , TabletMap_(TTabletMapTraits(this))
    {
        VERIFY_INVOKER_THREAD_AFFINITY(Slot_->GetAutomatonInvoker(), AutomatonThread);

        RegisterLoader(
            "TabletManager.Keys",
            BIND(&TImpl::LoadKeys, Unretained(this)));
        RegisterLoader(
            "TabletManager.Values",
            BIND(&TImpl::LoadValues, Unretained(this)));
        RegisterLoader(
            "TabletManager.Async",
            BIND(&TImpl::LoadAsync, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "TabletManager.Keys",
            BIND(&TImpl::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "TabletManager.Values",
            BIND(&TImpl::SaveValues, Unretained(this)));
        RegisterSaver(
            EAsyncSerializationPriority::Default,
            "TabletManager.Async",
            BIND(&TImpl::SaveAsync, Unretained(this)));

        RegisterMethod(BIND(&TImpl::HydraMountTablet, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUnmountTablet, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraRemountTablet, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraSetTabletState, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraFollowerExecuteWrite, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraRotateStore, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraCommitTabletStoresUpdate, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTabletStoresUpdated, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraSplitPartition, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraMergePartitions, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUpdatePartitionSampleKeys, Unretained(this)));
    }

    void Initialize()
    {
        auto transactionManager = Slot_->GetTransactionManager();
        transactionManager->SubscribeTransactionPrepared(BIND(&TImpl::OnTransactionPrepared, MakeStrong(this)));
        transactionManager->SubscribeTransactionCommitted(BIND(&TImpl::OnTransactionCommitted, MakeStrong(this)));
        transactionManager->SubscribeTransactionAborted(BIND(&TImpl::OnTransactionAborted, MakeStrong(this)));
    }


    TTablet* GetTabletOrThrow(const TTabletId& id)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* tablet = FindTablet(id);
        if (!tablet) {
            THROW_ERROR_EXCEPTION("No such tablet %v",
                id);
        }
        return tablet;
    }


    void Read(
        TTabletSnapshotPtr tabletSnapshot,
        TTimestamp timestamp,
        TWireProtocolReader* reader,
        TWireProtocolWriter* writer)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidatePermission(tabletSnapshot, EPermission::Read);

        ValidateReadTimestamp(timestamp);

        while (!reader->IsFinished()) {
            ExecuteSingleRead(
                tabletSnapshot,
                timestamp,
                reader,
                writer);
        }
    }

    void Write(
        TTabletSnapshotPtr tabletSnapshot,
        const TTransactionId& transactionId,
        TWireProtocolReader* reader)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidatePermission(tabletSnapshot, EPermission::Write);

        // NB: No yielding beyond this point.
        // May access tablet and transaction.

        const auto& tabletId = tabletSnapshot->TabletId;
        auto* tablet = GetTabletOrThrow(tabletId);

        auto transactionManager = Slot_->GetTransactionManager();
        auto* transaction = transactionManager->GetTransactionOrThrow(transactionId);

        ValidateTabletMounted(tablet);
        ValidateStoreLimit(tablet);
        ValidateTransactionActive(transaction);
        ValidateMemoryLimit();

        int prelockedCountBefore = PrelockedTransactions_.size();
        auto readerBegin = reader->GetCurrent();

        TError error;
        TNullable<TRowBlockedException> rowBlockedEx;

        while (!reader->IsFinished()) {
            const char* readerCheckpoint = reader->GetCurrent();
            auto rewindReader = [&] () {
                reader->SetCurrent(readerCheckpoint);
            };
            try {
                ExecuteSingleWrite(tablet, transaction, reader, true);
            } catch (const TRowBlockedException& ex) {
                rewindReader();
                rowBlockedEx = ex;
                break;
            } catch (const std::exception& ex) {
                rewindReader();
                error = ex;
                break;
            }
        }

        int prelockedCountAfter = PrelockedTransactions_.size();
        int prelockedCountDelta = prelockedCountAfter - prelockedCountBefore;
        if (prelockedCountDelta > 0) {
            LOG_DEBUG("Rows prelocked (TransactionId: %v, TabletId: %v, RowCount: %v)",
                transactionId,
                tabletId,
                prelockedCountDelta);

            auto readerEnd = reader->GetCurrent();
            auto recordData = reader->Slice(readerBegin, readerEnd);
            auto compressedRecordData = ChangelogCodec_->Compress(recordData);
            auto writeRecord = TTransactionWriteRecord{tabletId, recordData};

            TReqExecuteWrite hydraRequest;
            ToProto(hydraRequest.mutable_transaction_id(), transactionId);
            ToProto(hydraRequest.mutable_tablet_id(), tabletId);
            hydraRequest.set_codec(static_cast<int>(ChangelogCodec_->GetId()));
            hydraRequest.set_compressed_data(ToString(compressedRecordData));
            CreateMutation(Slot_->GetHydraManager(), hydraRequest)
                ->SetAction(BIND(
                    &TImpl::HydraLeaderExecuteWrite,
                    MakeStrong(this),
                    prelockedCountDelta,
                    writeRecord))
                ->Commit();
        }

        // NB: Yielding is now possible.
        // Cannot neither access tablet, nor transaction.

        if (rowBlockedEx) {
            rowBlockedEx->GetStore()->WaitOnBlockedRow(
                rowBlockedEx->GetRow(),
                rowBlockedEx->GetLockMask(),
                rowBlockedEx->GetTimestamp());
        }

        error.ThrowOnError();
    }


    TChunkStorePtr CreateChunkStore(
        const TStoreId& storeId,
        TTablet* tablet,
        const TChunkMeta* chunkMeta)
    {
        auto store = New<TChunkStore>(
            storeId,
            tablet,
            chunkMeta,
            Bootstrap_);
        store->SetInMemoryMode(tablet->GetConfig()->InMemoryMode);
        StartMemoryUsageTracking(store);
        return store;
    }

    TDynamicMemoryStorePtr CreateDynamicMemoryStore(
        const TStoreId& storeId,
        TTablet* tablet)
    {
        auto store = New<TDynamicMemoryStore>(
            Config_,
            storeId,
            tablet);
        store->SubscribeRowBlocked(BIND(
            &TImpl::OnRowBlocked,
            MakeWeak(this),
            Unretained(store.Get()),
            tablet->GetTabletId(),
            Slot_->GetGuardedAutomatonInvoker(EAutomatonThreadQueue::Read)));
        StartMemoryUsageTracking(store);
        return store;
    }

    IStorePtr CreateStore(TTablet* tablet, const TStoreId& storeId)
    {
        switch (TypeFromId(storeId)) {
            case EObjectType::Chunk:
            case EObjectType::ErasureChunk:
                return CreateChunkStore(storeId, tablet, nullptr);

            case EObjectType::DynamicMemoryTabletStore:
                return CreateDynamicMemoryStore(storeId, tablet);

            default:
                YUNREACHABLE();
        }
    }


    void OnRowBlocked(
        IStore* store,
        const TTabletId& tabletId,
        IInvokerPtr invoker,
        TDynamicRow row,
        int lockIndex)
    {
        WaitFor(
            BIND(
                &TImpl::WaitOnBlockedRow,
                MakeStrong(this),
                MakeStrong(store),
                tabletId,
                row,
                lockIndex)
            .AsyncVia(invoker)
            .Run());
    }

    void WaitOnBlockedRow(
        IStorePtr /*store*/,
        const TTabletId& tabletId,
        TDynamicRow row,
        int lockIndex)
    {
        auto* tablet = FindTablet(tabletId);
        if (!tablet)
            return;

        const auto& lock = row.BeginLocks(tablet->GetKeyColumnCount())[lockIndex];
        const auto* transaction = lock.Transaction;
        if (!transaction)
            return;

        LOG_DEBUG("Waiting on blocked row (Key: %v, LockIndex: %v, TabletId: %v, TransactionId: %v)",
            RowToKey(tablet->Schema(), tablet->KeyColumns(), row),
            lockIndex,
            tabletId,
            transaction->GetId());

        WaitFor(transaction->GetFinished().WithTimeout(BlockedRowWaitQuantum));
    }

    void ScheduleStoreRotation(TTablet* tablet)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        const auto& storeManager = tablet->GetStoreManager();
        if (!storeManager->IsRotationPossible())
            return;

        storeManager->ScheduleRotation();

        TReqRotateStore request;
        ToProto(request.mutable_tablet_id(), tablet->GetTabletId());

        CommitTabletMutation(request)
            .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TMutationResponse>& error) {
                if (!error.IsOK()) {
                    LOG_ERROR(error, "Error committing tablet store rotation mutation");
                }
            }));

        LOG_DEBUG("Store rotation scheduled (TabletId: %v)",
            tablet->GetTabletId());
    }


    void BuildOrchidYson(IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        BuildYsonFluently(consumer)
            .DoMapFor(TabletMap_, [&] (TFluentMap fluent, const std::pair<TTabletId, TTablet*>& pair) {
                auto* tablet = pair.second;
                fluent
                    .Item(ToString(tablet->GetTabletId()))
                    .Do(BIND(&TImpl::BuildTabletOrchidYson, Unretained(this), tablet));
            });
    }


    DECLARE_ENTITY_MAP_ACCESSORS(Tablet, TTablet, TTabletId);

private:
    const TTabletManagerConfigPtr Config_;

    ICodec* const ChangelogCodec_;

    class TTabletMapTraits
    {
    public:
        explicit TTabletMapTraits(TImpl* owner)
            : Owner_(owner)
        { }

        std::unique_ptr<TTablet> Create(const TTabletId& id) const
        {
            return std::make_unique<TTablet>(id, Owner_->Slot_);
        }

    private:
        TImpl* const Owner_;

    };

    TEntityMap<TTabletId, TTablet, TTabletMapTraits> TabletMap_;
    yhash_set<TTablet*> UnmountingTablets_;

    TRingQueue<TTransaction*> PrelockedTransactions_;

    yhash_set<TDynamicMemoryStorePtr> OrphanedStores_;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    void SaveKeys(TSaveContext& context) const
    {
        TabletMap_.SaveKeys(context);
    }

    void SaveValues(TSaveContext& context) const
    {
        TabletMap_.SaveValues(context);
    }

    TCallback<void(TSaveContext&)> SaveAsync()
    {
        std::vector<std::pair<TTabletId, TCallback<void(TSaveContext&)>>> capturedTablets;
        for (const auto& pair : TabletMap_) {
            auto* tablet = pair.second;
            capturedTablets.push_back(std::make_pair(tablet->GetTabletId(), tablet->AsyncSave()));
        }

        return BIND(
            [
                =,
                capturedTablets = std::move(capturedTablets)
            ] (TSaveContext& context) {
                using NYT::Save;
                for (const auto& pair : capturedTablets) {
                    Save(context, pair.first);
                    pair.second.Run(context);
                }
            });
    }

    void LoadKeys(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TabletMap_.LoadKeys(context);
    }

    void LoadValues(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TabletMap_.LoadValues(context);
    }

    void LoadAsync(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        SERIALIZATION_DUMP_WRITE(context, "tablets[%v]", TabletMap_.size());
        SERIALIZATION_DUMP_INDENT(context) {
            for (size_t index = 0; index != TabletMap_.size(); ++index) {
                auto tabletId = LoadSuspended<TTabletId>(context);
                auto* tablet = GetTablet(tabletId);
                SERIALIZATION_DUMP_WRITE(context, "%v =>", tabletId);
                SERIALIZATION_DUMP_INDENT(context) {
                    tablet->AsyncLoad(context);
                }
            }
        }
    }


    virtual void OnBeforeSnapshotLoaded() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnBeforeSnapshotLoaded();

        DoClear();
    }

    virtual void OnAfterSnapshotLoaded() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnAfterSnapshotLoaded();

        for (const auto& pair : TabletMap_) {
            auto* tablet = pair.second;
            CreateStoreManager(tablet);
            if (tablet->GetState() >= ETabletState::WaitingForLocks) {
                YCHECK(UnmountingTablets_.insert(tablet).second);
            }
        }

        auto transactionManager = Slot_->GetTransactionManager();
        for (const auto& pair : transactionManager->Transactions()) {
            auto* transaction = pair.second;
            int rowCount = 0;
            for (const auto& record : transaction->WriteLog()) {
                auto* tablet = GetTablet(record.TabletId);
                TWireProtocolReader reader(record.Data);
                while (!reader.IsFinished()) {
                    ExecuteSingleWrite(tablet, transaction, &reader, false);
                    ++rowCount;
                }
            }
            LOG_DEBUG("Transaction write log applied (TransactionId: %v, RowCount: %v)",
                transaction->GetId(),
                rowCount);
        }
    }


    virtual void Clear() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::Clear();

        DoClear();
    }

    void DoClear()
    {
        TabletMap_.Clear();
        UnmountingTablets_.clear();
        OrphanedStores_.clear();
    }


    virtual void OnLeaderRecoveryComplete() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnLeaderRecoveryComplete();

        YCHECK(PrelockedTransactions_.empty());

        for (const auto& pair : TabletMap_) {
            auto* tablet = pair.second;
            StartTabletEpoch(tablet);
        }
    }

    virtual void OnLeaderActive() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnLeaderActive();

        for (const auto& pair : TabletMap_) {
            auto* tablet = pair.second;
            CheckIfFullyUnlocked(tablet);
            CheckIfFullyFlushed(tablet);
        }
    }

    virtual void OnStopLeading() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnStopLeading();

        while (!PrelockedTransactions_.empty()) {
            auto* transaction = PrelockedTransactions_.front();
            PrelockedTransactions_.pop();

            auto rowRef = transaction->PrelockedRows().front();
            transaction->PrelockedRows().pop();

            if (ValidateAndDiscardRowRef(rowRef)) {
                rowRef.Store->GetTablet()->GetStoreManager()->AbortRow(transaction, rowRef);
            }
        }

        // Actually redundant: all prelocked rows were popped above.
        auto transactionManager = Slot_->GetTransactionManager();
        for (const auto& pair : transactionManager->Transactions()) {
            auto* transaction = pair.second;
            transaction->PrelockedRows().clear();
        }

        for (const auto& pair : TabletMap_) {
            auto* tablet = pair.second;
            StopTabletEpoch(tablet);
        }

        OrphanedStores_.clear();
    }


    virtual void OnStartFollowing() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnStartFollowing();

        YUNREACHABLE();
    }

    virtual void OnStopFollowing() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnStopFollowing();

        YUNREACHABLE();
    }


    void HydraMountTablet(const TReqMountTablet& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto tableId = FromProto<TObjectId>(request.table_id());
        auto schema = FromProto<TTableSchema>(request.schema());
        auto keyColumns = FromProto<TKeyColumns>(request.key_columns());
        auto pivotKey = FromProto<TOwningKey>(request.pivot_key());
        auto nextPivotKey = FromProto<TOwningKey>(request.next_pivot_key());
        auto mountConfig = DeserializeTableMountConfig((TYsonString(request.mount_config())), tabletId);
        auto writerOptions = DeserializeTabletWriterOptions(TYsonString(request.writer_options()), tabletId);

        auto tabletHolder = std::make_unique<TTablet>(
            mountConfig,
            writerOptions,
            tabletId,
            tableId,
            Slot_,
            schema,
            keyColumns,
            pivotKey,
            nextPivotKey);

        tabletHolder->CreateInitialPartition();
        tabletHolder->SetState(ETabletState::Mounted);

        auto storeManager = CreateStoreManager(tabletHolder.get());
        storeManager->CreateActiveStore();

        auto* tablet = TabletMap_.Insert(tabletId, std::move(tabletHolder));

        std::vector<std::pair<TOwningKey, int>> chunkBoundaries;

        for (const auto& descriptor : request.chunk_stores()) {
            auto miscExt = GetProtoExtension<NChunkClient::NProto::TMiscExt>(descriptor.chunk_meta().extensions());
            if (!miscExt.eden()) {
                auto boundaryKeysExt = GetProtoExtension<NVersionedTableClient::NProto::TBoundaryKeysExt>(descriptor.chunk_meta().extensions());
                auto minKey = FromProto<TOwningKey>(boundaryKeysExt.min());
                auto maxKey = FromProto<TOwningKey>(boundaryKeysExt.max());
                chunkBoundaries.push_back(std::make_pair(minKey, 1));
                chunkBoundaries.push_back(std::make_pair(maxKey, -1));
            }
        }

        if (!chunkBoundaries.empty()) {
            std::sort(chunkBoundaries.begin(), chunkBoundaries.end());
            std::vector<TOwningKey> pivots{pivotKey};
            int depth = 0;
            for (const auto& boundary : chunkBoundaries) {
                if (boundary.second == 1 && depth == 0 && boundary.first > pivotKey) {
                    pivots.push_back(boundary.first);
                }
                depth += boundary.second;
            }
            YCHECK(tablet->Partitions().size() == 1);
            tablet->SplitPartition(0, pivots);
        }

        for (const auto& descriptor : request.chunk_stores()) {
            auto chunkId = FromProto<TChunkId>(descriptor.store_id());
            auto store = CreateChunkStore(
                chunkId,
                tablet,
                &descriptor.chunk_meta());
            storeManager->AddStore(store);
            if (mountConfig->InMemoryMode != EInMemoryMode::None && mountConfig->InMemoryMode != EInMemoryMode::Disabled) {
                storeManager->ScheduleStorePreload(store);
            }
        }

        SchedulePartitionsSampling(tablet);

        {
            TRspMountTablet response;
            ToProto(response.mutable_tablet_id(), tabletId);
            PostMasterMutation(response);
        }

        if (!IsRecovery()) {
            StartTabletEpoch(tablet);
        }

        LOG_INFO_UNLESS(IsRecovery(), "Tablet mounted (TabletId: %v, TableId: %v, Keys: %v .. %v, StoreCount: %v, PartitionCount: %v)",
            tabletId,
            tableId,
            pivotKey,
            nextPivotKey,
            request.chunk_stores_size(),
            tablet->Partitions().size());
    }

    void HydraUnmountTablet(const TReqUnmountTablet& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet)
            return;

        if (request.force()) {
            LOG_INFO_UNLESS(IsRecovery(), "Tablet is forcefully unmounted (TabletId: %v)",
                tabletId);

            // Just a formality.
            tablet->SetState(ETabletState::Unmounted);

            for (const auto& pair : tablet->Stores()) {
                SetStoreOrphaned(tablet, pair.second);
            }

            const auto& storeManager = tablet->GetStoreManager();
            for (auto store : storeManager->GetLockedStores()) {
                SetStoreOrphaned(tablet, store);
            }

            if (!IsRecovery()) {
                StopTabletEpoch(tablet);
            }

            TabletMap_.Remove(tabletId);
            UnmountingTablets_.erase(tablet); // don't check the result
            return;
        }

        if (tablet->GetState() != ETabletState::Mounted) {
            LOG_INFO_UNLESS(IsRecovery(), "Requested to unmount a tablet in %Qlv state, ignored (TabletId: %v)",
                tablet->GetState(),
                tabletId);
            return;
        }

        LOG_INFO_UNLESS(IsRecovery(), "Unmounting tablet (TabletId: %v)",
            tabletId);

        // Just a formality.
        YCHECK(tablet->GetState() == ETabletState::Mounted);
        tablet->SetState(ETabletState::WaitingForLocks);

        YCHECK(UnmountingTablets_.insert(tablet).second);

        LOG_INFO_IF(IsLeader(), "Waiting for all tablet locks to be released (TabletId: %v)",
            tabletId);

        if (IsLeader()) {
            CheckIfFullyUnlocked(tablet);
        }
    }

    void HydraRemountTablet(const TReqRemountTablet& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet)
            return;

        auto mountConfig = DeserializeTableMountConfig((TYsonString(request.mount_config())), tabletId);
        auto writerOptions = DeserializeTabletWriterOptions(TYsonString(request.writer_options()), tabletId);

        int oldSamplesPerPartition = tablet->GetConfig()->SamplesPerPartition;
        int newSamplesPerPartition = mountConfig->SamplesPerPartition;

        auto storeManager = tablet->GetStoreManager();
        storeManager->Remount(mountConfig, writerOptions);

        if (oldSamplesPerPartition != newSamplesPerPartition) {
            SchedulePartitionsSampling(tablet);
        }

        LOG_INFO_UNLESS(IsRecovery(), "Tablet remounted (TabletId: %v)",
            tabletId);
    }

    void HydraSetTabletState(const TReqSetTabletState& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet)
            return;

        auto requestedState = ETabletState(request.state());

        switch (requestedState) {
            case ETabletState::Flushing: {
                tablet->SetState(ETabletState::Flushing);

                // NB: Flush requests for all other stores must already be on their way.
                RotateStores(tablet, false);

                LOG_INFO_IF(IsLeader(), "Waiting for all tablet stores to be flushed (TabletId: %v)",
                    tabletId);

                if (IsLeader()) {
                    CheckIfFullyFlushed(tablet);
                }
                break;
            }

            case ETabletState::Unmounted: {
                tablet->SetState(ETabletState::Unmounted);

                LOG_INFO_UNLESS(IsRecovery(), "Tablet unmounted (TabletId: %v)",
                    tabletId);

                if (!IsRecovery()) {
                    StopTabletEpoch(tablet);
                }

                TabletMap_.Remove(tabletId);
                YCHECK(UnmountingTablets_.erase(tablet) == 1);

                {
                    TRspUnmountTablet response;
                    ToProto(response.mutable_tablet_id(), tabletId);
                    PostMasterMutation(response);
                }
                break;
            }

            default:
                YUNREACHABLE();
        }
    }

    void HydraLeaderExecuteWrite(int rowCount, const TTransactionWriteRecord& writeRecord)
    {
        for (int index = 0; index < rowCount; ++index) {
            YASSERT(!PrelockedTransactions_.empty());
            auto* transaction = PrelockedTransactions_.front();
            PrelockedTransactions_.pop();

            auto rowRef = transaction->PrelockedRows().front();
            transaction->PrelockedRows().pop();

            if (ValidateAndDiscardRowRef(rowRef)) {
                rowRef.Store->GetTablet()->GetStoreManager()->ConfirmRow(transaction, rowRef);
            }

            transaction->WriteLog().Enqueue(writeRecord);
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Rows confirmed (TabletId: %v, RowCount: %v, WriteRecordSize: %v)",
            writeRecord.TabletId,
            rowCount,
            writeRecord.Data.Size());
    }

    void HydraFollowerExecuteWrite(const TReqExecuteWrite& request) noexcept
    {
        auto transactionId = FromProto<TTransactionId>(request.transaction_id());
        auto transactionManager = Slot_->GetTransactionManager();
        auto* transaction = transactionManager->GetTransaction(transactionId);

        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = GetTablet(tabletId);

        auto codecId = ECodec(request.codec());
        auto* codec = GetCodec(codecId);
        auto compressedRecordData = TSharedRef::FromString(request.compressed_data());
        auto recordData = codec->Decompress(compressedRecordData);
        auto writeRecord = TTransactionWriteRecord{tabletId, recordData};

        TWireProtocolReader reader(recordData);
        int rowCount = 0;
        while (!reader.IsFinished()) {
            ExecuteSingleWrite(tablet, transaction, &reader, false);
            ++rowCount;
        }

        transaction->WriteLog().Enqueue(writeRecord);

        LOG_DEBUG_UNLESS(IsRecovery(), "Rows written (TransactionId: %v, TabletId: %v, RowCount: %v, WriteRecordSize: %v)",
            transactionId,
            tabletId,
            rowCount,
            recordData.Size());
    }

    void HydraRotateStore(const TReqRotateStore& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet || tablet->GetState() != ETabletState::Mounted)
            return;

        RotateStores(tablet, true);
        UpdateTabletSnapshot(tablet);
    }


    void HydraCommitTabletStoresUpdate(const TReqCommitTabletStoresUpdate& commitRequest)
    {
        auto tabletId = FromProto<TTabletId>(commitRequest.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet)
            return;

        std::vector<TStoreId> storeIdsToAdd;
        for (const auto& descriptor : commitRequest.stores_to_add()) {
            storeIdsToAdd.push_back(FromProto<TStoreId>(descriptor.store_id()));
        }

        std::vector<TStoreId> storeIdsToRemove;
        for (const auto& descriptor : commitRequest.stores_to_remove()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            storeIdsToRemove.push_back(storeId);
            auto store = tablet->GetStore(storeId);
            YCHECK(store->GetStoreState() != EStoreState::ActiveDynamic);
            store->SetStoreState(EStoreState::RemoveCommitting);
        }

        LOG_INFO_UNLESS(IsRecovery(), "Committing tablet stores update (TabletId: %v, StoreIdsToAdd: [%v], StoreIdsToRemove: [%v])",
            tabletId,
            JoinToString(storeIdsToAdd),
            JoinToString(storeIdsToRemove));

        auto slot = tablet->GetSlot();
        auto hiveManager = slot->GetHiveManager();

        {
            TReqUpdateTabletStores masterRequest;
            ToProto(masterRequest.mutable_tablet_id(), tabletId);
            masterRequest.mutable_stores_to_add()->MergeFrom(commitRequest.stores_to_add());
            masterRequest.mutable_stores_to_remove()->MergeFrom(commitRequest.stores_to_remove());

            hiveManager->PostMessage(slot->GetMasterMailbox(), masterRequest);
        }

        if (commitRequest.has_transaction_id()) {
            auto transactionId = FromProto<TTransactionId>(commitRequest.transaction_id());

            TReqHydraAbortTransaction masterRequest;
            ToProto(masterRequest.mutable_transaction_id(), transactionId);
            ToProto(masterRequest.mutable_mutation_id(), NRpc::NullMutationId);

            hiveManager->PostMessage(slot->GetMasterMailbox(), masterRequest);
        }
    }

    void HydraOnTabletStoresUpdated(const TRspUpdateTabletStores& response)
    {
        auto tabletId = FromProto<TTabletId>(response.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet)
            return;

        if (response.has_error()) {
            auto error = FromProto<TError>(response.error());
            LOG_WARNING_UNLESS(IsRecovery(), error, "Error updating tablet stores (TabletId: %v)",
                tabletId);

            for (const auto& descriptor : response.stores_to_remove()) {
                auto storeId = FromProto<TStoreId>(descriptor.store_id());
                auto store = tablet->GetStore(storeId);
                YCHECK(store->GetStoreState() == EStoreState::RemoveCommitting);
                BackoffStore(store, EStoreState::RemoveFailed);
            }
            return;
        }

        auto mountConfig = tablet->GetConfig();
        auto inMemoryManager = Bootstrap_->GetInMemoryManager();
        const auto& storeManager = tablet->GetStoreManager();
        std::vector<TStoreId> addedStoreIds;
        for (const auto& descriptor : response.stores_to_add()) {
            auto storeId = FromProto<TChunkId>(descriptor.store_id());
            addedStoreIds.push_back(storeId);
            YCHECK(descriptor.has_chunk_meta());

            auto store = CreateChunkStore(
                storeId,
                tablet,
                &descriptor.chunk_meta());
            storeManager->AddStore(store);

            auto chunkData = inMemoryManager->EvictInterceptedChunkData(storeId);
            if (mountConfig->InMemoryMode != EInMemoryMode::None && mountConfig->InMemoryMode != EInMemoryMode::Disabled) {
                if (!storeManager->TryPreloadStoreFromInterceptedData(store, chunkData)) {
                    storeManager->ScheduleStorePreload(store);
                }
            }

            SchedulePartitionSampling(store->GetPartition());

            TStoreId backingStoreId;
            if (!IsRecovery() && descriptor.has_backing_store_id()) {
                backingStoreId = FromProto<TStoreId>(descriptor.backing_store_id());
                auto backingStore = tablet->GetStore(backingStoreId);
                SetBackingStore(tablet, store, backingStore);
            }

            LOG_DEBUG_UNLESS(IsRecovery(), "Store added (TabletId: %v, StoreId: %v, BackingStoreId: %v)",
                tabletId,
                storeId,
                backingStoreId);
        }

        std::vector<TStoreId> removedStoreIds;
        for (const auto& descriptor : response.stores_to_remove()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            removedStoreIds.push_back(storeId);
            auto store = tablet->GetStore(storeId);
            storeManager->RemoveStore(store);
            SchedulePartitionSampling(store->GetPartition());
            LOG_DEBUG_UNLESS(IsRecovery(), "Store removed (TabletId: %v, StoreId: %v)",
                tabletId,
                storeId);
        }

        LOG_INFO_UNLESS(IsRecovery(), "Tablet stores updated successfully (TabletId: %v, AddedStoreIds: [%v], RemovedStoreIds: [%v])",
            tabletId,
            JoinToString(addedStoreIds),
            JoinToString(removedStoreIds));

        UpdateTabletSnapshot(tablet);
        if (IsLeader()) {
            CheckIfFullyFlushed(tablet);
        }
    }

    void HydraSplitPartition(const TReqSplitPartition& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet)
            return;

        auto partitionId = FromProto<TPartitionId>(request.partition_id());
        auto* partition = tablet->GetPartitionById(partitionId);
        auto pivotKeys = FromProto<TOwningKey>(request.pivot_keys());

        // NB: Set the state back to normal; otherwise if some of the below checks fail, we might get
        // a partition stuck in splitting state forever.
        partition->SetState(EPartitionState::Normal);

        if (tablet->Partitions().size() >= tablet->GetConfig()->MaxPartitionCount)
            return;

        int partitionIndex = partition->GetIndex();
        i64 partitionDataSize = partition->GetUncompressedDataSize();

        tablet->SplitPartition(partitionIndex, pivotKeys);

        auto resultingPartitionIds = JoinToString(ConvertToStrings(
            tablet->Partitions().begin() + partitionIndex,
            tablet->Partitions().begin() + partitionIndex + pivotKeys.size(),
            [] (const std::unique_ptr<TPartition>& partition) {
                return ToString(partition->GetId());
            }));

        LOG_INFO_UNLESS(IsRecovery(), "Splitting partition (TabletId: %v, OriginalPartitionId: %v, ResultingPartitionIds: [%v], DataSize: %v, Keys: %v)",
            tablet->GetTabletId(),
            partitionId,
            resultingPartitionIds,
            partitionDataSize,
            JoinToString(pivotKeys, Stroka(" .. ")));

        // NB: Initial partition is split into new ones with indexes |[partitionIndex, partitionIndex + pivotKeys.size())|.
        SchedulePartitionsSampling(tablet, partitionIndex, partitionIndex + pivotKeys.size());
        UpdateTabletSnapshot(tablet);
    }

    void HydraMergePartitions(const TReqMergePartitions& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet)
            return;

        auto firstPartitionId = FromProto<TPartitionId>(request.partition_id());
        auto* firstPartition = tablet->GetPartitionById(firstPartitionId);

        int firstPartitionIndex = firstPartition->GetIndex();
        int lastPartitionIndex = firstPartitionIndex + request.partition_count() - 1;

        i64 partitionsDataSize = 0;
        for (int index = firstPartitionIndex; index <= lastPartitionIndex; ++index) {
            const auto& partition = tablet->Partitions()[index];
            partitionsDataSize += partition->GetUncompressedDataSize();
            // See HydraSplitPartition.
            // Currently this code is redundant since there's no escape path below,
            // but we prefer to keep it to make things look symmetric.
            partition->SetState(EPartitionState::Normal);
        }

        auto originalPartitionIds = JoinToString(ConvertToStrings(
            tablet->Partitions().begin() + firstPartitionIndex,
            tablet->Partitions().begin() + lastPartitionIndex + 1,
            [] (const std::unique_ptr<TPartition>& partition) {
                return ToString(partition->GetId());
            }));

        tablet->MergePartitions(firstPartitionIndex, lastPartitionIndex);

        LOG_INFO_UNLESS(IsRecovery(), "Merging partitions (TabletId: %v, OriginalPartitionIds: [%v], ResultingPartitionId: %v, DataSize: %v)",
            tablet->GetTabletId(),
            originalPartitionIds,
            tablet->Partitions()[firstPartitionIndex]->GetId(),
            partitionsDataSize);

        // NB: Initial partitions are merged into a single one with index |firstPartitionIndex|.
        SchedulePartitionsSampling(tablet, firstPartitionIndex, firstPartitionIndex + 1);
        UpdateTabletSnapshot(tablet);
    }

    void HydraUpdatePartitionSampleKeys(const TReqUpdatePartitionSampleKeys& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!tablet)
            return;

        auto partitionId = FromProto<TPartitionId>(request.partition_id());
        auto* partition = tablet->FindPartitionById(partitionId);
        if (!partition)
            return;


        auto sampleKeys = New<TKeyList>();
        sampleKeys->Keys = FromProto<TOwningKey>(request.sample_keys());
        partition->SetSampleKeys(sampleKeys);
        YCHECK(sampleKeys->Keys.empty() || sampleKeys->Keys[0] > partition->GetPivotKey());
        UpdateTabletSnapshot(tablet);

        const auto* mutationContext = GetCurrentMutationContext();
        partition->SetSamplingTime(mutationContext->GetTimestamp());

        LOG_INFO_UNLESS(IsRecovery(), "Partition sample keys updated (TabletId: %v, PartitionId: %v, SampleKeyCount: %v)",
            tabletId,
            partition->GetId(),
            sampleKeys->Keys.size());
    }


    void OnTransactionPrepared(TTransaction* transaction)
    {
        auto handleRow = [&] (const TDynamicRowRef& rowRef) {
            // NB: Don't call ValidateAndDiscardRowRef, row refs are just scanned.
            if (ValidateRowRef(rowRef)) {
                rowRef.Store->GetTablet()->GetStoreManager()->PrepareRow(transaction, rowRef);
            }
        };

        for (const auto& rowRef : transaction->LockedRows()) {
            handleRow(rowRef);
        }

        for (auto it = transaction->PrelockedRows().begin();
            it != transaction->PrelockedRows().end();
            transaction->PrelockedRows().move_forward(it))
        {
            handleRow(*it);
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Locked rows prepared (TransactionId: %v, LockedRowCount: %v, PrelockedRowCount: %v)",
            transaction->GetId(),
            transaction->LockedRows().size(),
            transaction->PrelockedRows().size());
    }

    void OnTransactionCommitted(TTransaction* transaction)
    {
        auto handleRow = [&] (const TDynamicRowRef& rowRef) {
            if (ValidateAndDiscardRowRef(rowRef)) {
                rowRef.Store->GetTablet()->GetStoreManager()->CommitRow(transaction, rowRef);
            }
        };

        for (const auto& rowRef : transaction->LockedRows()) {
            handleRow(rowRef);
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Locked rows committed (TransactionId: %v, RowCount: %v)",
            transaction->GetId(),
            transaction->LockedRows().size());

        YCHECK(transaction->PrelockedRows().empty());
        transaction->LockedRows().clear();

        OnTransactionFinished(transaction);
    }

    void OnTransactionAborted(TTransaction* transaction)
    {
        auto handleRow = [&] (const TDynamicRowRef& rowRef) {
            if (ValidateAndDiscardRowRef(rowRef)) {
                rowRef.Store->GetTablet()->GetStoreManager()->AbortRow(transaction, rowRef);
            }
        };

        for (const auto& rowRef : transaction->LockedRows()) {
            handleRow(rowRef);
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Locked rows aborted (TransactionId: %v, RowCount: %v)",
            transaction->GetId(),
            transaction->LockedRows().size());

        YCHECK(transaction->PrelockedRows().empty());
        transaction->LockedRows().clear();

        OnTransactionFinished(transaction);
    }

    void OnTransactionFinished(TTransaction* /*transaction*/)
    {
        if (IsLeader()) {
            for (auto* tablet : UnmountingTablets_) {
                CheckIfFullyUnlocked(tablet);
            }
        }
    }


    void SetStoreOrphaned(TTablet* tablet, IStorePtr store)
    {
        if (store->GetStoreState() == EStoreState::Orphaned)
            return;
        store->SetStoreState(EStoreState::Orphaned);

        if (store->GetType() != EStoreType::DynamicMemory)
            return;
        
        auto dynamicStore = store->AsDynamicMemory();
        int lockCount = dynamicStore->GetLockCount();
        if (lockCount > 0) {
            YCHECK(OrphanedStores_.insert(dynamicStore).second);
            LOG_INFO_UNLESS(IsRecovery(), "Dynamic memory store is orphaned and will be kept (StoreId: %v, TabletId: %v, LockCount: %v)",
                store->GetId(),
                tablet->GetTabletId(),
                lockCount);
        }
    }

    bool ValidateRowRef(const TDynamicRowRef& rowRef)
    {
        auto* store = rowRef.Store;
        return store->GetStoreState() != EStoreState::Orphaned;
    }

    bool ValidateAndDiscardRowRef(const TDynamicRowRef& rowRef)
    {
        auto* store = rowRef.Store;
        if (store->GetStoreState() != EStoreState::Orphaned) {
            return true;
        }

        int lockCount = store->Unlock();
        if (lockCount == 0) {
            LOG_INFO_UNLESS(IsRecovery(), "Store unlocked and will be dropped (StoreId: %v)",
                store->GetId());
            YCHECK(OrphanedStores_.erase(store) == 1);
        }

        return false;
    }


    void ExecuteSingleRead(
        TTabletSnapshotPtr tabletSnapshot,
        TTimestamp timestamp,
        TWireProtocolReader* reader,
        TWireProtocolWriter* writer)
    {
        auto command = reader->ReadCommand();
        switch (command) {
            case EWireProtocolCommand::LookupRows:
                LookupRows(
                    Bootstrap_->GetBoundedConcurrencyReadPoolInvoker(),
                    std::move(tabletSnapshot),
                    timestamp,
                    reader,
                    writer);
                break;

            default:
                THROW_ERROR_EXCEPTION("Unknown read command %v",
                    command);
        }
    }

    void ExecuteSingleWrite(
        TTablet* tablet,
        TTransaction* transaction,
        TWireProtocolReader* reader,
        bool prelock)
    {
        const auto& storeManager = tablet->GetStoreManager();

        TDynamicRowRef rowRef;

        auto command = reader->ReadCommand();
        switch (command) {
            case EWireProtocolCommand::WriteRow: {
                TReqWriteRow req;
                reader->ReadMessage(&req);
                auto row = reader->ReadUnversionedRow();
                rowRef = storeManager->WriteRow(
                    transaction,
                    row,
                    prelock,
                    ELockMode(req.lock_mode()));
                break;
            }

            case EWireProtocolCommand::DeleteRow: {
                TReqDeleteRow req;
                reader->ReadMessage(&req);
                auto key = reader->ReadUnversionedRow();
                rowRef = storeManager->DeleteRow(
                    transaction,
                    key,
                    prelock);
                break;
            }

            default:
                THROW_ERROR_EXCEPTION("Unknown write command %v",
                    command);
        }

        if (prelock) {
            ValidateTabletMounted(tablet);
            ValidateTransactionActive(transaction);
            PrelockedTransactions_.push(transaction);
            transaction->PrelockedRows().push(rowRef);
        }
    }


    void CheckIfFullyUnlocked(TTablet* tablet)
    {
        if (tablet->GetState() != ETabletState::WaitingForLocks)
            return;

        if (tablet->GetStoreManager()->HasActiveLocks())
            return;

        LOG_INFO_UNLESS(IsRecovery(), "All tablet locks released (TabletId: %v)",
            tablet->GetTabletId());

        tablet->SetState(ETabletState::FlushPending);

        TReqSetTabletState request;
        ToProto(request.mutable_tablet_id(), tablet->GetTabletId());
        request.set_state(static_cast<int>(ETabletState::Flushing));

        CommitTabletMutation(request)
            .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TMutationResponse>& error) {
                if (!error.IsOK()) {
                    LOG_ERROR(error, "Error committing tablet state change mutation");
                }
            }));
    }

    void CheckIfFullyFlushed(TTablet* tablet)
    {
        if (tablet->GetState() != ETabletState::Flushing)
            return;

        if (tablet->GetStoreManager()->HasUnflushedStores())
            return;

        LOG_INFO_UNLESS(IsRecovery(), "All tablet stores flushed (TabletId: %v)",
            tablet->GetTabletId());

        tablet->SetState(ETabletState::UnmountPending);

        TReqSetTabletState request;
        ToProto(request.mutable_tablet_id(), tablet->GetTabletId());
        request.set_state(static_cast<int>(ETabletState::Unmounted));

        CommitTabletMutation(request)
            .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TMutationResponse>& error) {
                if (!error.IsOK()) {
                    LOG_ERROR(error, "Error committing tablet state change mutation");
                }
            }));
    }


    void RotateStores(TTablet* tablet, bool createNew)
    {
        tablet->GetStoreManager()->Rotate(createNew);
    }


    TFuture<TMutationResponse> CommitTabletMutation(const ::google::protobuf::MessageLite& message)
    {
        auto mutation = CreateMutation(Slot_->GetHydraManager(), message);
        return BIND(&TMutation::Commit, mutation)
            .AsyncVia(Slot_->GetEpochAutomatonInvoker())
            .Run();
    }

    void PostMasterMutation(const ::google::protobuf::MessageLite& message)
    {
        auto hiveManager = Slot_->GetHiveManager();
        hiveManager->PostMessage(Slot_->GetMasterMailbox(), message);
    }


    TStoreManagerPtr CreateStoreManager(TTablet* tablet)
    {
        auto storeManager = New<TStoreManager>(
            Config_,
            tablet,
            BIND([=] () -> TDynamicMemoryStorePtr {
                auto slot = tablet->GetSlot();
                auto storeId = slot->GenerateId(EObjectType::DynamicMemoryTabletStore);
                return CreateDynamicMemoryStore(storeId, tablet);
            }));
        tablet->SetStoreManager(storeManager);
        return storeManager;
    }

    void StartTabletEpoch(TTablet* tablet)
    {
        tablet->SetLastPartitioningTime(TInstant::Now());

        const auto& storeManager = tablet->GetStoreManager();
        storeManager->StartEpoch(Slot_);

        auto slotManager = Bootstrap_->GetTabletSlotManager();
        slotManager->RegisterTabletSnapshot(tablet);
    }

    void StopTabletEpoch(TTablet* tablet)
    {
        // TODO(babenko): consider moving
        tablet->GetEden()->SetState(EPartitionState::Normal);
        for (const auto& partition : tablet->Partitions()) {
            partition->SetState(EPartitionState::Normal);
        }

        const auto& storeManager = tablet->GetStoreManager();
        storeManager->StopEpoch();

        auto slotManager = Bootstrap_->GetTabletSlotManager();
        slotManager->UnregisterTabletSnapshot(tablet);
    }


    void BackoffStore(IStorePtr store, EStoreState state)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        store->SetStoreState(state);

        auto callback = BIND([=, this_ = MakeStrong(this)] () {
            VERIFY_THREAD_AFFINITY(AutomatonThread);
            store->SetStoreState(store->GetPersistentStoreState());
        });

        if (IsLeader()) {
            TDelayedExecutor::Submit(callback.Via(Slot_->GetEpochAutomatonInvoker()), Config_->ErrorBackoffTime);
        } else {
            callback.Run();
        }
    }


    void SetBackingStore(TTablet* tablet, TChunkStorePtr store, IStorePtr backingStore)
    {
        store->SetBackingStore(backingStore);
        LOG_DEBUG("Backing store set (StoreId: %v, BackingStoreId: %v)",
            store->GetId(),
            backingStore->GetId());

        auto callback = BIND([=, this_ = MakeStrong(this)] () {
            VERIFY_THREAD_AFFINITY(AutomatonThread);
            store->SetBackingStore(nullptr);
            LOG_DEBUG("Backing store released (StoreId: %v)", store->GetId());
        });
        TDelayedExecutor::Submit(
            // NB: Submit the callback via the regular automaton invoker, not the epoch one since
            // we need the store to be released even if the epoch ends.
            callback.Via(tablet->GetSlot()->GetAutomatonInvoker()),
            tablet->GetConfig()->BackingStoreRetentionTime);
    }


    void BuildTabletOrchidYson(TTablet* tablet, IYsonConsumer* consumer)
    {
        BuildYsonFluently(consumer)
            .BeginAttributes()
                .Item("opaque").Value(true)
            .EndAttributes()
            .BeginMap()
                .Item("table_id").Value(tablet->GetTableId())
                .Item("state").Value(tablet->GetState())
                .Item("pivot_key").Value(tablet->GetPivotKey())
                .Item("next_pivot_key").Value(tablet->GetNextPivotKey())
                .Item("eden").Do(BIND(&TImpl::BuildPartitionOrchidYson, Unretained(this), tablet->GetEden()))
                .Item("partitions").DoListFor(tablet->Partitions(), [&] (TFluentList fluent, const std::unique_ptr<TPartition>& partition) {
                    fluent
                        .Item()
                        .Do(BIND(&TImpl::BuildPartitionOrchidYson, Unretained(this), partition.get()));
                })
            .EndMap();
    }

    void BuildPartitionOrchidYson(TPartition* partition, IYsonConsumer* consumer)
    {
        BuildYsonFluently(consumer)
            .BeginMap()
                .Item("id").Value(partition->GetId())
                .Item("state").Value(partition->GetState())
                .Item("pivot_key").Value(partition->GetPivotKey())
                .Item("next_pivot_key").Value(partition->GetNextPivotKey())
                .Item("sample_key_count").Value(partition->GetSampleKeys()->Keys.size())
                .Item("sampling_time").Value(partition->GetSamplingTime())
                .Item("sampling_request_time").Value(partition->GetSamplingRequestTime())
                .Item("uncompressed_data_size").Value(partition->GetUncompressedDataSize())
                .Item("unmerged_row_count").Value(partition->GetUnmergedRowCount())
                .Item("stores").DoMapFor(partition->Stores(), [&] (TFluentMap fluent, const IStorePtr& store) {
                    fluent
                        .Item(ToString(store->GetId()))
                        .Do(BIND(&TImpl::BuildStoreOrchidYson, Unretained(this), store));
                })
            .EndMap();
    }

    void BuildStoreOrchidYson(IStorePtr store, IYsonConsumer* consumer)
    {
        BuildYsonFluently(consumer)
            .BeginAttributes()
                .Item("opaque").Value(true)
            .EndAttributes()
            .BeginMap()
                .Do(BIND(&IStore::BuildOrchidYson, store))
            .EndMap();
    }


    static EMemoryCategory GetMemoryConsumerFromStore(IStorePtr store)
    {
        switch (store->GetType()) {
            case EStoreType::DynamicMemory:
                return EMemoryCategory::TabletDynamic;
            case EStoreType::Chunk:
                return EMemoryCategory::TabletStatic;
            default:
                YUNREACHABLE();
        }
    }

    static void OnStoreMemoryUsageUpdated(NCellNode::TBootstrap* bootstrap, EMemoryCategory consumer, i64 delta)
    {
        auto* tracker = bootstrap->GetMemoryUsageTracker();
        if (delta >= 0) {
            tracker->Acquire(consumer, delta);
        } else {
            tracker->Release(consumer, -delta);
        }
    }

    void StartMemoryUsageTracking(IStorePtr store)
    {
        store->SubscribeMemoryUsageUpdated(BIND(
            &TImpl::OnStoreMemoryUsageUpdated,
            Bootstrap_,
            GetMemoryConsumerFromStore(store)));
    }

    void ValidateMemoryLimit()
    {
        if (Bootstrap_->GetTabletSlotManager()->IsOutOfMemory()) {
            THROW_ERROR_EXCEPTION("Node is out of tablet memory, all writes disabled");
        }
    }

    void ValidateStoreLimit(TTablet* tablet)
    {
        if (tablet->Stores().size() >= tablet->GetConfig()->MaxStoresPerTablet) {
            THROW_ERROR_EXCEPTION("Too many stores in tablet, all writes disabled")
                << TErrorAttribute("tablet_id", tablet->GetTableId())
                << TErrorAttribute("store_limit", tablet->GetConfig()->MaxStoresPerTablet);
        }
    }


    void UpdateTabletSnapshot(TTablet* tablet)
    {
        if (!IsRecovery()) {
            auto slotManager = Bootstrap_->GetTabletSlotManager();
            slotManager->UpdateTabletSnapshot(tablet);
        }
    }


    void SchedulePartitionSampling(TPartition* partition)
    {
        if (!partition->IsEden()) {
            const auto* mutationContext = GetCurrentMutationContext();
            partition->SetSamplingRequestTime(mutationContext->GetTimestamp());
        }
    }

    void SchedulePartitionsSampling(TTablet* tablet, int beginPartitionIndex, int endPartitionIndex)
    {
        const auto* mutationContext = GetCurrentMutationContext();
        for (int index = beginPartitionIndex; index < endPartitionIndex; ++index) {
            tablet->Partitions()[index]->SetSamplingRequestTime(mutationContext->GetTimestamp());
        }
    }

    void SchedulePartitionsSampling(TTablet* tablet)
    {
        SchedulePartitionsSampling(tablet, 0, tablet->Partitions().size());
    }


    void ValidateTabletMounted(TTablet* tablet)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (tablet->GetState() != ETabletState::Mounted) {
            THROW_ERROR_EXCEPTION("Tablet %v is not in \"mounted\" state",
                tablet->GetTabletId());
        }
    }

    void ValidateTransactionActive(TTransaction* transaction)
    {
        if (transaction->GetState() != ETransactionState::Active) {
            transaction->ThrowInvalidState();
        }
    }


    TTableMountConfigPtr DeserializeTableMountConfig(const TYsonString& str, const TTabletId& tabletId)
    {
        try {
            return ConvertTo<TTableMountConfigPtr>(str);
        } catch (const std::exception& ex) {
            LOG_ERROR_UNLESS(IsRecovery(), ex, "Error deserializing tablet mount config (TabletId: %v)",
                 tabletId);
            return New<TTableMountConfig>();
        }
    }

    TTabletWriterOptionsPtr DeserializeTabletWriterOptions(const TYsonString& str, const TTabletId& tabletId)
    {
        try {
            return ConvertTo<TTabletWriterOptionsPtr>(str);
        } catch (const std::exception& ex) {
            LOG_ERROR_UNLESS(IsRecovery(), ex, "Error deserializing writer options (TabletId: %v)",
                 tabletId);
            return New<TTabletWriterOptions>();
        }
    }
};

DEFINE_ENTITY_MAP_ACCESSORS(TTabletManager::TImpl, Tablet, TTablet, TTabletId, TabletMap_)

///////////////////////////////////////////////////////////////////////////////

TTabletManager::TTabletManager(
    TTabletManagerConfigPtr config,
    TTabletSlotPtr slot,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(
        config,
        slot,
        bootstrap))
{ }

TTabletManager::~TTabletManager()
{ }

void TTabletManager::Initialize()
{
    Impl_->Initialize();
}

TTablet* TTabletManager::GetTabletOrThrow(const TTabletId& id)
{
    return Impl_->GetTabletOrThrow(id);
}

void TTabletManager::Read(
    TTabletSnapshotPtr tabletSnapshot,
    TTimestamp timestamp,
    TWireProtocolReader* reader,
    TWireProtocolWriter* writer)
 {
    Impl_->Read(
        std::move(tabletSnapshot),
        timestamp,
        reader,
        writer);
}

void TTabletManager::Write(
    TTabletSnapshotPtr tabletSnapshot,
    const TTransactionId& transactionId,
    TWireProtocolReader* reader)
{
    Impl_->Write(
        std::move(tabletSnapshot),
        transactionId,
        reader);
}

IStorePtr TTabletManager::CreateStore(TTablet* tablet, const TStoreId& storeId)
{
    return Impl_->CreateStore(tablet, storeId);
}

void TTabletManager::ScheduleStoreRotation(TTablet* tablet)
{
    Impl_->ScheduleStoreRotation(tablet);
}

void TTabletManager::BuildOrchidYson(IYsonConsumer* consumer)
{
    Impl_->BuildOrchidYson(consumer);
}

DELEGATE_ENTITY_MAP_ACCESSORS(TTabletManager, Tablet, TTablet, TTabletId, *Impl_)

///////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
