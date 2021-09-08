#include "connection.h"
#include "transaction_participant.h"

#include <yt/yt/ytlib/hive/cell_directory.h>
#include <yt/yt/ytlib/hive/cell_directory_synchronizer.h>
#include <yt/yt/ytlib/hive/transaction_participant_service_proxy.h>

#include <yt/yt/ytlib/hydra/hydra_service_proxy.h>

#include <yt/yt/client/hive/transaction_participant.h>

#include <yt/yt/client/api/connection.h>

#include <yt/yt/core/rpc/dispatcher.h>

namespace NYT::NApi::NNative {

using namespace NConcurrency;
using namespace NElection;
using namespace NHiveClient;
using namespace NHydra;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

class TTransactionParticipant
    : public ITransactionParticipant
{
public:
    TTransactionParticipant(
        TCellDirectoryPtr cellDirectory,
        TCellDirectorySynchronizerPtr cellDirectorySynchronizer,
        ITimestampProviderPtr timestampProvider,
        IConnectionPtr connection,
        TCellId cellId,
        const TTransactionParticipantOptions& options)
        : CellDirectory_(std::move(cellDirectory))
        , CellDirectorySynchronizer_(std::move(cellDirectorySynchronizer))
        , TimestampProvider_(std::move(timestampProvider))
        , Connection_(std::move(connection))
        , CellId_(cellId)
        , Options_(options)
    { }

    TCellId GetCellId() const override
    {
        return CellId_;
    }

    const ITimestampProviderPtr& GetTimestampProvider() const override
    {
        return TimestampProvider_;
    }

    ETransactionParticipantState GetState() const override
    {
        if (CellDirectory_->IsCellUnregistered(CellId_)) {
            return ETransactionParticipantState::Unregistered;
        }
        if (Connection_ && Connection_->IsTerminated()) {
            return ETransactionParticipantState::Invalidated;
        }
        return ETransactionParticipantState::Valid;
    }

    TFuture<void> PrepareTransaction(
        TTransactionId transactionId,
        TTimestamp prepareTimestamp,
        const std::vector<TCellId>& cellIdsToSyncWith,
        const NRpc::TAuthenticationIdentity& identity) override
    {
        return SendRequest<TTransactionParticipantServiceProxy::TReqPrepareTransaction>(
            [=] (TTransactionParticipantServiceProxy* proxy) {
                VERIFY_THREAD_AFFINITY_ANY();

                auto req = proxy->PrepareTransaction();
                req->SetResponseHeavy(true);
                PrepareRequest(req);
                NRpc::SetAuthenticationIdentity(req, identity);
                ToProto(req->mutable_transaction_id(), transactionId);
                req->set_prepare_timestamp(prepareTimestamp);
                ToProto(req->mutable_cell_ids_to_sync_with(), cellIdsToSyncWith);
                return req;
            });
    }

    TFuture<void> CommitTransaction(
        TTransactionId transactionId,
        TTimestamp commitTimestamp,
        const NRpc::TAuthenticationIdentity& identity) override
    {
        return SendRequest<TTransactionParticipantServiceProxy::TReqCommitTransaction>(
            [=] (TTransactionParticipantServiceProxy* proxy) {
                VERIFY_THREAD_AFFINITY_ANY();

                auto req = proxy->CommitTransaction();
                req->SetResponseHeavy(true);
                PrepareRequest(req);
                NRpc::SetAuthenticationIdentity(req, identity);
                ToProto(req->mutable_transaction_id(), transactionId);
                req->set_commit_timestamp(commitTimestamp);
                return req;
            });
    }

    TFuture<void> AbortTransaction(
        TTransactionId transactionId,
        const NRpc::TAuthenticationIdentity& identity) override
    {
        return SendRequest<TTransactionParticipantServiceProxy::TReqAbortTransaction>(
            [=] (TTransactionParticipantServiceProxy* proxy) {
                VERIFY_THREAD_AFFINITY_ANY();

                auto req = proxy->AbortTransaction();
                req->SetResponseHeavy(true);
                PrepareRequest(req);
                NRpc::SetAuthenticationIdentity(req, identity);
                ToProto(req->mutable_transaction_id(), transactionId);
                return req;
            });
    }

    TFuture<void> CheckAvailability() override
    {
        return GetChannel().Apply(BIND([=, this_ = MakeStrong(this)] (const NRpc::IChannelPtr& channel) {
            THydraServiceProxy proxy(channel);
            auto req = proxy.Poke();
            PrepareRequest(req);
            return req->Invoke().template As<void>();
        }));
    }

private:
    const TCellDirectoryPtr CellDirectory_;
    const TCellDirectorySynchronizerPtr CellDirectorySynchronizer_;
    const ITimestampProviderPtr TimestampProvider_;
    const IConnectionPtr Connection_;
    const TCellId CellId_;
    const TTransactionParticipantOptions Options_;


    template <class TRequest>
    TFuture<void> SendRequest(std::function<TIntrusivePtr<TRequest>(TTransactionParticipantServiceProxy*)> builder)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return BIND([=, this_ = MakeStrong(this)] {
            return GetChannel().Apply(BIND([=] (const NRpc::IChannelPtr& channel) {
                TTransactionParticipantServiceProxy proxy(channel);
                auto req = builder(&proxy);
                return req->Invoke().template As<void>();
            }));
        })
            .AsyncVia(NRpc::TDispatcher::Get()->GetHeavyInvoker())
            .Run();
    }

    void PrepareRequest(const TIntrusivePtr<NRpc::TClientRequest>& request)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        request->SetTimeout(Options_.RpcTimeout);
    }

    TFuture<NRpc::IChannelPtr> GetChannel()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto channel = CellDirectory_->FindChannel(CellId_);
        if (channel) {
            return MakeFuture(channel);
        }
        if (!CellDirectorySynchronizer_) {
            return MakeNoChannelError();
        }
        return CellDirectorySynchronizer_->Sync().Apply(BIND([=, this_ = MakeStrong(this)] () {
            auto channel = CellDirectory_->FindChannel(CellId_);
            if (channel) {
                return MakeFuture(channel);
            }
            return MakeNoChannelError();
        }));
    }

    TFuture<NRpc::IChannelPtr> MakeNoChannelError()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return MakeFuture<NRpc::IChannelPtr>(TError(
            NRpc::EErrorCode::Unavailable,
            "No such participant cell %v",
            CellId_));
    }
};

////////////////////////////////////////////////////////////////////////////////

ITransactionParticipantPtr CreateTransactionParticipant(
    TCellDirectoryPtr cellDirectory,
    TCellDirectorySynchronizerPtr cellDirectorySynchronizer,
    ITimestampProviderPtr timestampProvider,
    IConnectionPtr connection,
    TCellId cellId,
    const TTransactionParticipantOptions& options)
{
    return New<TTransactionParticipant>(
        std::move(cellDirectory),
        std::move(cellDirectorySynchronizer),
        std::move(timestampProvider),
        std::move(connection),
        cellId,
        options);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
