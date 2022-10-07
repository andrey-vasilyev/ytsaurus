#include "replication_reader.h"

#include "private.h"
#include "traffic_meter.h"
#include "block_cache.h"
#include "block_id.h"
#include "chunk_reader.h"
#include "chunk_reader_host.h"
#include "config.h"
#include "data_node_service_proxy.h"
#include "helpers.h"
#include "chunk_reader_allowing_repair.h"
#include "chunk_reader_options.h"
#include "chunk_replica_cache.h"

#include <yt/yt/ytlib/api/native/client.h>
#include <yt/yt/ytlib/api/native/connection.h>

#include <yt/yt/ytlib/chunk_client/chunk_meta_cache.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_statistics.h>
#include <yt/yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/yt/ytlib/chunk_client/replication_reader.h>

#include <yt/yt/ytlib/cypress_client/cypress_ypath_proxy.h>

#include <yt/yt/ytlib/table_client/lookup_reader.h>

#include <yt/yt/ytlib/node_tracker_client/channel.h>
#include <yt/yt/ytlib/node_tracker_client/node_status_directory.h>

#include <yt/yt/ytlib/object_client/object_service_proxy.h>

#include <yt/yt/client/api/config.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/client/rpc/helpers.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/client/table_client/row_buffer.h>
#include <yt/yt/client/table_client/schema.h>
#include <yt/yt/client/table_client/versioned_row.h>
#include <yt/yt/client/table_client/wire_protocol.h>

#include <yt/yt/core/concurrency/action_queue.h>
#include <yt/yt/core/concurrency/delayed_executor.h>
#include <yt/yt/core/concurrency/thread_affinity.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/misc/atomic_object.h>
#include <yt/yt/core/misc/hedging_manager.h>
#include <yt/yt/core/misc/protobuf_helpers.h>
#include <yt/yt/core/misc/string.h>
#include <yt/yt/core/misc/memory_reference_tracker.h>

#include <yt/yt/core/net/local_address.h>

#include <yt/yt/core/ytalloc/memory_zone.h>

#include <yt/yt/core/rpc/hedging_channel.h>

#include <util/generic/algorithm.h>
#include <util/generic/cast.h>
#include <util/generic/ymath.h>

#include <util/random/shuffle.h>

#include <cmath>

namespace NYT::NChunkClient {
namespace {

using namespace NConcurrency;
using namespace NHydra;
using namespace NRpc;
using namespace NApi;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NNodeTrackerClient;
using namespace NChunkClient;
using namespace NNet;
using namespace NYTAlloc;
using namespace NTableClient;

using NNodeTrackerClient::TNodeId;
using NYT::ToProto;
using NYT::FromProto;
using ::ToString;

////////////////////////////////////////////////////////////////////////////////

static const double MaxBackoffMultiplier = 1000.0;

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EPeerType,
    (Peer)
    (Seed)
);

////////////////////////////////////////////////////////////////////////////////

struct TPeer
{
    TPeer() = default;
    TPeer(
        TNodeId nodeId,
        TString address,
        TNodeDescriptor nodeDescriptor,
        EPeerType peerType,
        EAddressLocality locality,
        std::optional<TInstant> nodeSuspicionMarkTime)
        : NodeId(nodeId)
        , Address(std::move(address))
        , NodeDescriptor(std::move(nodeDescriptor))
        , Type(peerType)
        , Locality(locality)
        , NodeSuspicionMarkTime(nodeSuspicionMarkTime)
    { }

    TNodeId NodeId = InvalidNodeId;
    TString Address;
    TNodeDescriptor NodeDescriptor;
    EPeerType Type;
    EAddressLocality Locality;
    std::optional<TInstant> NodeSuspicionMarkTime;
};

void FormatValue(TStringBuilderBase* builder, const TPeer& peer, TStringBuf format)
{
    FormatValue(builder, peer.Address, format);
}

using TPeerList = TCompactVector<TPeer, 3>;

////////////////////////////////////////////////////////////////////////////////

struct TPeerQueueEntry
{
    TPeerQueueEntry(TPeer peer, int banCount)
        : Peer(std::move(peer))
        , BanCount(banCount)
    { }

    TPeer Peer;
    int BanCount = 0;
    ui32 Random = RandomNumber<ui32>();
};

////////////////////////////////////////////////////////////////////////////////

class TSessionBase;
class TReadBlockSetSession;
class TReadBlockRangeSession;
class TGetMetaSession;
class TLookupRowsSession;

DECLARE_REFCOUNTED_CLASS(TReplicationReader)

class TReplicationReader
    : public IChunkReaderAllowingRepair
    , public ILookupReader
{
public:
    TReplicationReader(
        TReplicationReaderConfigPtr config,
        TRemoteReaderOptionsPtr options,
        TChunkReaderHostPtr chunkReaderHost,
        TChunkId chunkId,
        const TChunkReplicaList& seedReplicas)
        : Config_(std::move(config))
        , Options_(std::move(options))
        , Client_(chunkReaderHost->Client)
        , NodeDirectory_(Client_->GetNativeConnection()->GetNodeDirectory())
        , LocalDescriptor_(chunkReaderHost->LocalDescriptor)
        , ChunkId_(chunkId)
        , BlockCache_(chunkReaderHost->BlockCache)
        , ChunkMetaCache_(chunkReaderHost->ChunkMetaCache)
        , TrafficMeter_(chunkReaderHost->TrafficMeter)
        , NodeStatusDirectory_(chunkReaderHost->NodeStatusDirectory)
        , BandwidthThrottler_(chunkReaderHost->BandwidthThrottler)
        , RpsThrottler_(chunkReaderHost->RpsThrottler)
        , Networks_(Client_->GetNativeConnection()->GetNetworks())
        , Logger(ChunkClientLogger.WithTag("ChunkId: %v", ChunkId_))
        , InitialSeeds_(std::move(seedReplicas))
    {
        YT_VERIFY(NodeDirectory_);

        if (!Options_->AllowFetchingSeedsFromMaster && InitialSeeds_.empty()) {
            THROW_ERROR_EXCEPTION(
                NChunkClient::EErrorCode::NoChunkSeedsGiven,
                "Cannot read chunk %v: master seeds retries are disabled and no initial seeds are given",
                ChunkId_);
        }

        YT_LOG_DEBUG("Reader initialized "
            "(InitialSeedReplicas: %v, FetchPromPeers: %v, LocalDescriptor: %v, PopulateCache: %v, "
            "AllowFetchingSeedsFromMaster: %v, Networks: %v)",
            MakeFormattableView(InitialSeeds_, TChunkReplicaAddressFormatter(NodeDirectory_)),
            Config_->FetchFromPeers,
            LocalDescriptor_,
            Config_->PopulateCache,
            Options_->AllowFetchingSeedsFromMaster,
            Networks_);
    }

    TFuture<std::vector<TBlock>> ReadBlocks(
        const TClientChunkReadOptions& options,
        const std::vector<int>& blockIndexes,
        std::optional<i64> estimatedSize) override;

    TFuture<std::vector<TBlock>> ReadBlocks(
        const TClientChunkReadOptions& options,
        int firstBlockIndex,
        int blockCount,
        std::optional<i64> estimatedSize) override;

    TFuture<TRefCountedChunkMetaPtr> GetMeta(
        const TClientChunkReadOptions& options,
        std::optional<int> partitionTag,
        const std::optional<std::vector<int>>& extensionTags) override;

    TFuture<TSharedRef> LookupRows(
        const TClientChunkReadOptions& options,
        TSharedRange<TLegacyKey> lookupKeys,
        TObjectId tableId,
        TRevision revision,
        TTableSchemaPtr tableSchema,
        std::optional<i64> estimatedSize,
        const TColumnFilter& columnFilter,
        TTimestamp timestamp,
        NCompression::ECodec codecId,
        bool produceAllVersions,
        TTimestamp overrideTimestamp,
        bool enablePeerProbing,
        bool enableRejectsIfThrottling) override;

    TChunkId GetChunkId() const override
    {
        return ChunkId_;
    }

    TInstant GetLastFailureTime() const override
    {
        return LastFailureTime_;
    }

    void SetFailed()
    {
        LastFailureTime_ = NProfiling::GetInstant();
    }

    void SetSlownessChecker(TCallback<TError(i64, TDuration)> slownessChecker) override
    {
        SlownessChecker_ = slownessChecker;
    }

    TError RunSlownessChecker(i64 bytesReceived, TInstant startTimestamp)
    {
        if (!SlownessChecker_) {
            return {};
        }
        auto timePassed = TInstant::Now() - startTimestamp;
        return SlownessChecker_(bytesReceived, timePassed);
    }

private:
    friend class TSessionBase;
    friend class TReadBlockSetSession;
    friend class TReadBlockRangeSession;
    friend class TGetMetaSession;
    friend class TLookupRowsSession;

    const TReplicationReaderConfigPtr Config_;
    const TRemoteReaderOptionsPtr Options_;
    const NNative::IClientPtr Client_;
    const TNodeDirectoryPtr NodeDirectory_;
    const TNodeDescriptor LocalDescriptor_;
    const TChunkId ChunkId_;
    const IBlockCachePtr BlockCache_;
    const IClientChunkMetaCachePtr ChunkMetaCache_;
    const TTrafficMeterPtr TrafficMeter_;
    const INodeStatusDirectoryPtr NodeStatusDirectory_;
    const IThroughputThrottlerPtr BandwidthThrottler_;
    const IThroughputThrottlerPtr RpsThrottler_;
    const TNetworkPreferenceList Networks_;

    const NLogging::TLogger Logger;

    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, PeersSpinLock_);
    NHydra::TRevision FreshSeedsRevision_ = NHydra::NullRevision;
    //! Peers returning NoSuchChunk error are banned forever.
    THashSet<TString> BannedForeverPeers_;
    //! Every time peer fails (e.g. time out occurs), we increase ban counter.
    THashMap<TString, int> PeerBanCountMap_;
    //! If AllowFetchingSeedsFromMaster is |true| InitialSeeds_ (if present) are used
    //! until 'DiscardSeeds' is called for the first time.
    //! If AllowFetchingSeedsFromMaster is |false| InitialSeeds_ must be given and cannot be discarded.
    TChunkReplicaList InitialSeeds_;

    std::atomic<TInstant> LastFailureTime_ = TInstant();
    TCallback<TError(i64, TDuration)> SlownessChecker_;


    TFuture<TAllyReplicasInfo> GetReplicasFuture()
    {
        {
            auto guard = Guard(PeersSpinLock_);

            if (!InitialSeeds_.empty()) {
                auto seeds = InitialSeeds_;
                guard.Release();
                return MakeFuture(TAllyReplicasInfo::FromChunkReplicas(seeds));
            }
        }

        YT_VERIFY(Options_->AllowFetchingSeedsFromMaster);
        const auto& chunkReplicaCache = Client_->GetNativeConnection()->GetChunkReplicaCache();
        auto futures = chunkReplicaCache->GetReplicas({DecodeChunkId(ChunkId_).Id});
        YT_VERIFY(futures.size() == 1);
        return futures[0];
    }

    void DiscardSeeds(const TFuture<TAllyReplicasInfo>& future)
    {
        if (!Options_->AllowFetchingSeedsFromMaster) {
            // We're not allowed to ask master for seeds.
            // Better keep the initial ones.
            return;
        }

        {
            auto guard = Guard(PeersSpinLock_);
            InitialSeeds_.clear();
        }

        Client_->GetNativeConnection()->GetChunkReplicaCache()->DiscardReplicas(
            DecodeChunkId(ChunkId_).Id,
            future);
    }

    void OnChunkReplicasLocated(const TAllyReplicasInfo& seedReplicas)
    {
        auto guard = Guard(PeersSpinLock_);

        if (FreshSeedsRevision_ >= seedReplicas.Revision) {
            return;
        }

        FreshSeedsRevision_ = seedReplicas.Revision;

        for (auto replica : seedReplicas.Replicas) {
            const auto* nodeDescriptor = NodeDirectory_->FindDescriptor(replica.GetNodeId());
            if (!nodeDescriptor) {
                YT_LOG_WARNING("Skipping replica with unresolved node id (NodeId: %v)", replica.GetNodeId());
                continue;
            }
            if (auto address = nodeDescriptor->FindAddress(Networks_)) {
                BannedForeverPeers_.erase(*address);
            }
        }
    }

    //! Notifies reader about peer banned inside one of the sessions.
    void OnPeerBanned(const TString& peerAddress)
    {
        auto guard = Guard(PeersSpinLock_);
        auto [it, inserted] = PeerBanCountMap_.emplace(peerAddress, 1);
        if (!inserted) {
            ++it->second;
        }

        if (it->second > Config_->MaxBanCount) {
            BannedForeverPeers_.insert(peerAddress);
        }
    }

    void BanPeerForever(const TString& peerAddress)
    {
        auto guard = Guard(PeersSpinLock_);
        BannedForeverPeers_.insert(peerAddress);
    }

    int GetBanCount(const TString& peerAddress) const
    {
        auto guard = Guard(PeersSpinLock_);
        auto it = PeerBanCountMap_.find(peerAddress);
        return it == PeerBanCountMap_.end() ? 0 : it->second;
    }

    bool IsPeerBannedForever(const TString& peerAddress) const
    {
        if (!Config_->BanPeersPermanently) {
            return false;
        }

        auto guard = Guard(PeersSpinLock_);
        return BannedForeverPeers_.contains(peerAddress);
    }

    void AccountTraffic(i64 transferredByteCount, const TNodeDescriptor& srcDescriptor)
    {
        if (TrafficMeter_) {
            TrafficMeter_->IncrementInboundByteCount(srcDescriptor.GetDataCenter(), transferredByteCount);
        }
    }

    //! Fallback for GetMeta when the chunk meta is missing in ChunkMetaCache or the cache is disabled.
    TFuture<TRefCountedChunkMetaPtr> FetchMeta(
        const TClientChunkReadOptions& options,
        std::optional<int> partitionTag,
        const std::optional<std::vector<int>>& extensionsTags);
};

DEFINE_REFCOUNTED_TYPE(TReplicationReader)

////////////////////////////////////////////////////////////////////////////////

class TSessionBase
    : public TRefCounted
{
protected:
    struct TPeerProbeResult
    {
        bool NetThrottling;
        bool DiskThrottling;
        i64 NetQueueSize;
        i64 DiskQueueSize;
        ::google::protobuf::RepeatedPtrField<NChunkClient::NProto::TPeerDescriptor> PeerDescriptors;
        TAllyReplicasInfo AllyReplicas;
    };

    using TErrorOrPeerProbeResult = TErrorOr<TPeerProbeResult>;

    template <class TRspPtr>
    static TPeerProbeResult ParseProbeResponse(const TRspPtr& rsp)
    {
        return {
            .NetThrottling = rsp->net_throttling(),
            .DiskThrottling = rsp->disk_throttling(),
            .NetQueueSize = rsp->net_queue_size(),
            .DiskQueueSize = rsp->disk_queue_size(),
            .PeerDescriptors = rsp->peer_descriptors(),
            .AllyReplicas = FromProto<TAllyReplicasInfo>(rsp->ally_replicas())
        };
    }

    virtual bool UpdatePeerBlockMap(const TPeerProbeResult& /*probeResult*/)
    {
        // P2P is not supported by default.
        return false;
    }

    //! Reference to the owning reader.
    const TWeakPtr<TReplicationReader> Reader_;

    const TReplicationReaderConfigPtr ReaderConfig_;
    const TRemoteReaderOptionsPtr ReaderOptions_;
    const TChunkId ChunkId_;

    const TClientChunkReadOptions SessionOptions_;

    const INodeStatusDirectoryPtr NodeStatusDirectory_;

    //! The workload descriptor from the config with instant field updated
    //! properly.
    const TWorkloadDescriptor WorkloadDescriptor_;

    //! Translates node ids to node descriptors.
    const TNodeDirectoryPtr NodeDirectory_;

    //! List of the networks to use from descriptor.
    const TNetworkPreferenceList Networks_;

    const IThroughputThrottlerPtr BandwidthThrottler_;
    const IThroughputThrottlerPtr RpsThrottler_;

    NLogging::TLogger Logger;

    //! Zero based retry index (less than |ReaderConfig_->RetryCount|).
    int RetryIndex_ = 0;

    //! Zero based pass index (less than |ReaderConfig_->PassCount|).
    int PassIndex_ = 0;

    //! Seed replicas for the current retry.
    TAllyReplicasInfo SeedReplicas_;

    //! Set of peer addresses banned for the current retry.
    THashSet<TString> BannedPeers_;

    //! List of candidates addresses to try during current pass, prioritized by:
    //! locality, ban counter, random number.
    typedef std::priority_queue<
        TPeerQueueEntry,
        std::vector<TPeerQueueEntry>,
        std::function<bool(const TPeerQueueEntry&, const TPeerQueueEntry&)>> TPeerQueue;
    TPeerQueue PeerQueue_;

    //! Catalogue of peers, seen on current pass.
    THashMap<TString, TPeer> Peers_;

    //! Either fixed priority invoker build upon CompressionPool or fair share thread pool invoker assigned to
    //! compression fair share tag from the workload descriptor.
    IInvokerPtr SessionInvoker_;

    //! The instant this session was started.
    TInstant StartTime_ = TInstant::Now();

    //! The instant current retry was started.
    TInstant RetryStartTime_;

    //! Total number of bytes received in this session; used to detect slow reads.
    i64 TotalBytesReceived_ = 0;

    TSessionBase(
        TReplicationReader* reader,
        const TClientChunkReadOptions& options,
        IThroughputThrottlerPtr bandwidthThrottler,
        IThroughputThrottlerPtr rpsThrottler)
        : Reader_(reader)
        , ReaderConfig_(reader->Config_)
        , ReaderOptions_(reader->Options_)
        , ChunkId_(reader->ChunkId_)
        , SessionOptions_(options)
        , NodeStatusDirectory_(reader->NodeStatusDirectory_)
        , WorkloadDescriptor_(ReaderConfig_->EnableWorkloadFifoScheduling
            ? options.WorkloadDescriptor.SetCurrentInstant()
            : options.WorkloadDescriptor)
        , NodeDirectory_(reader->NodeDirectory_)
        , Networks_(reader->Networks_)
        , BandwidthThrottler_(std::move(bandwidthThrottler))
        , RpsThrottler_(std::move(rpsThrottler))
        , Logger(ChunkClientLogger.WithTag("SessionId: %v, ReadSessionId: %v, ChunkId: %v",
            TGuid::Create(),
            options.ReadSessionId,
            ChunkId_))
    {
        SessionInvoker_ = GetCompressionInvoker(WorkloadDescriptor_);
        if (WorkloadDescriptor_.CompressionFairShareTag) {
            Logger.AddTag("CompressionFairShareTag: %v", WorkloadDescriptor_.CompressionFairShareTag);
        }

        ResetPeerQueue();
    }

    EAddressLocality GetNodeLocality(const TNodeDescriptor& descriptor)
    {
        auto reader = Reader_.Lock();
        auto locality = EAddressLocality::None;

        if (reader) {
            locality = ComputeAddressLocality(descriptor, reader->LocalDescriptor_);
        }
        return locality;
    }

    bool SyncThrottle(const IThroughputThrottlerPtr& throttler, i64 count)
    {
        auto throttlerFuture = throttler->Throttle(count);
        SetSessionFuture(throttlerFuture);

        auto throttleResult = WaitForFast(throttlerFuture);
        if (!throttleResult.IsOK()) {
            auto error = TError(
                NChunkClient::EErrorCode::ReaderThrottlingFailed,
                "Failed to apply throttling in reader")
                << throttleResult;
            OnSessionFailed(true, error);
            return false;
        }

        return true;
    }

    void AsyncThrottle(const IThroughputThrottlerPtr& throttler, i64 count, TClosure onSuccess)
    {
        auto throttlerFuture = throttler->Throttle(count);
        SetSessionFuture(throttlerFuture);
        throttlerFuture
            .Subscribe(BIND([=, this_ = MakeStrong(this), onSuccess = std::move(onSuccess)] (const TError& throttleResult) {
                if (!throttleResult.IsOK()) {
                    auto error = TError(
                        NChunkClient::EErrorCode::ReaderThrottlingFailed,
                        "Failed to apply throttling in reader")
                        << throttleResult;
                    OnSessionFailed(true, error);
                    return;
                }
                onSuccess();
            }).Via(SessionInvoker_));
    }

    void BanPeer(const TString& address, bool forever)
    {
        auto reader = Reader_.Lock();
        if (!reader) {
            return;
        }

        if (forever && !reader->IsPeerBannedForever(address)) {
            YT_LOG_DEBUG("Node is banned until seeds are re-fetched from master (Address: %v)", address);
            reader->BanPeerForever(address);
        }

        if (BannedPeers_.insert(address).second) {
            reader->OnPeerBanned(address);
            YT_LOG_DEBUG("Node is banned for the current retry (Address: %v, BanCount: %v)",
                address,
                reader->GetBanCount(address));
        }
    }

    const TNodeDescriptor& GetPeerDescriptor(const TString& address)
    {
        return GetOrCrash(Peers_, address).NodeDescriptor;
    }

    //! Register peer and install into the peer queue if neccessary.
    bool AddPeer(
        TNodeId nodeId,
        const TString& address,
        const TNodeDescriptor& descriptor,
        EPeerType type,
        std::optional<TInstant> nodeSuspicionMarkTime)
    {
        auto reader = Reader_.Lock();
        if (!reader) {
            return false;
        }

        // TODO(akozhikhov): catch this exception.
        TPeer peer(
            nodeId,
            descriptor.GetAddressOrThrow(Networks_),
            descriptor,
            type,
            GetNodeLocality(descriptor),
            nodeSuspicionMarkTime);
        if (!Peers_.insert({address, peer}).second) {
            // Peer was already handled on current pass.
            return false;
        }

        if (IsPeerBanned(address)) {
            // Peer is banned.
            return false;
        }

        PeerQueue_.push(TPeerQueueEntry(peer, reader->GetBanCount(address)));
        return true;
    }

    //! Reinstall peer in the peer queue.
    void ReinstallPeer(const TString& address)
    {
        auto reader = Reader_.Lock();
        if (!reader || IsPeerBanned(address)) {
            return;
        }

        YT_LOG_DEBUG("Reinstall peer into peer queue (Address: %v)", address);
        const auto& peer = GetOrCrash(Peers_, address);
        PeerQueue_.push(TPeerQueueEntry(peer, reader->GetBanCount(address)));
    }

    bool IsSeed(const TString& address)
    {
        return GetOrCrash(Peers_, address).Type == EPeerType::Seed;
    }

    bool IsPeerBanned(const TString& address)
    {
        auto reader = Reader_.Lock();
        if (!reader) {
            return false;
        }

        return BannedPeers_.find(address) != BannedPeers_.end() || reader->IsPeerBannedForever(address);
    }

    IChannelPtr GetChannel(const TString& address)
    {
        auto reader = Reader_.Lock();
        if (!reader) {
            return nullptr;
        }

        try {
            const auto& channelFactory = reader->Client_->GetChannelFactory();
            // TODO(akozhikhov): Don't catch here.
            return channelFactory->CreateChannel(address);
        } catch (const std::exception& ex) {
            RegisterError(ex);
            BanPeer(address, false);
            return nullptr;
        }
    }

    template <class T>
    void ProcessError(const TErrorOr<T>& rspOrError, const TPeer& peer, const TError& wrappingError)
    {
        auto code = rspOrError.GetCode();
        if (code == NYT::EErrorCode::Canceled) {
            return;
        }

        if (NodeStatusDirectory_ &&
            !peer.NodeSuspicionMarkTime &&
            NodeStatusDirectory_->ShouldMarkNodeSuspicious(rspOrError))
        {
            YT_LOG_WARNING("Node is marked as suspicious (NodeId: %v, NodeAddress: %v, Error: %v)",
                peer.NodeId,
                peer.Address,
                rspOrError);
            NodeStatusDirectory_->UpdateSuspicionMarkTime(
                peer.NodeId,
                peer.Address,
                /* suspicious */ true,
                std::nullopt);
        }

        auto error = wrappingError << rspOrError;
        if (code == NRpc::EErrorCode::Unavailable ||
            code == NRpc::EErrorCode::RequestQueueSizeLimitExceeded ||
            code == NHydra::EErrorCode::InvalidChangelogState)
        {
            YT_LOG_DEBUG(error);
            return;
        }

        BanPeer(peer.Address, code == NChunkClient::EErrorCode::NoSuchChunk);
        RegisterError(error);
    }

    TPeerList PickPeerCandidates(
        const TReplicationReaderPtr& reader,
        int count,
        bool enableEarlyExit,
        std::function<bool(const TString&)> filter = {})
    {
        TPeerList candidates;
        while (!PeerQueue_.empty() && std::ssize(candidates) < count) {
            const auto& top = PeerQueue_.top();
            if (top.BanCount != reader->GetBanCount(top.Peer.Address)) {
                auto queueEntry = top;
                PeerQueue_.pop();
                queueEntry.BanCount = reader->GetBanCount(queueEntry.Peer.Address);
                PeerQueue_.push(queueEntry);
                continue;
            }

            if (!candidates.empty() && enableEarlyExit) {
                if (candidates.front().Type == EPeerType::Peer) {
                    // If we have peer candidate, ask it first.
                    break;
                }

                // Ensure that peers with best locality are always asked first.
                // Locality is compared w.r.t. config options.
                if (ComparePeerLocality(top.Peer, candidates.front()) < 0) {
                    break;
                }
            }

            if ((!filter || filter(top.Peer.Address)) && !IsPeerBanned(top.Peer.Address)) {
                candidates.push_back(top.Peer);
            }

            PeerQueue_.pop();
        }
        return candidates;
    }

    IChannelPtr MakePeersChannel(
        const TPeerList& peers,
        const std::optional<THedgingChannelOptions>& hedgingOptions)
    {
        if (peers.empty()) {
            return nullptr;
        }

        if (peers.size() != 1 && hedgingOptions) {
            return CreateHedgingChannel(
                GetChannel(peers[0].Address),
                GetChannel(peers[1].Address),
                *hedgingOptions);
        }

        return GetChannel(peers[0].Address);
    }

    void NextRetry()
    {
        auto reader = Reader_.Lock();
        if (!reader) {
            return;
        }

        if (IsCanceled()) {
            return;
        }

        if (ShouldStop()) {
            return;
        }

        YT_VERIFY(!SeedsFuture_);

        YT_LOG_DEBUG("Retry started (RetryIndex: %v/%v)",
            RetryIndex_ + 1,
            ReaderConfig_->RetryCount);

        PassIndex_ = 0;
        BannedPeers_.clear();

        RetryStartTime_ = TInstant::Now();

        SeedsFuture_ = reader->GetReplicasFuture();
        SeedsFuture_.Subscribe(BIND_NEW(&TSessionBase::OnGotSeeds, MakeStrong(this))
            .Via(SessionInvoker_));
    }

    void OnRetryFailed()
    {
        DiscardSeeds();

        int retryCount = ReaderConfig_->RetryCount;
        YT_LOG_DEBUG("Retry failed (RetryIndex: %v/%v)",
            RetryIndex_ + 1,
            retryCount);

        ++RetryIndex_;
        if (RetryIndex_ >= retryCount) {
            OnSessionFailed(/*fatal*/ true);
            return;
        }

        TDelayedExecutor::Submit(
            BIND(&TSessionBase::NextRetry, MakeStrong(this))
                .Via(SessionInvoker_),
            GetBackoffDuration(RetryIndex_));
    }

    void MaybeUpdateSeeds(const TAllyReplicasInfo& allyReplicas)
    {
        auto reader = Reader_.Lock();
        if (!reader) {
            return;
        }

        if (!allyReplicas) {
            return;
        }

        // NB: We could have changed current pass seeds,
        // but for the sake of simplicity that will be done upon next retry within standard reading pipeline.
        reader->Client_->GetNativeConnection()->GetChunkReplicaCache()->UpdateReplicas(
            DecodeChunkId(ChunkId_).Id,
            allyReplicas);
    }

    void DiscardSeeds()
    {
        auto reader = Reader_.Lock();
        if (!reader) {
            return;
        }

        YT_VERIFY(SeedsFuture_);
        reader->DiscardSeeds(SeedsFuture_);
        SeedsFuture_.Reset();
    }

    void SetReaderFailed()
    {
        auto reader = Reader_.Lock();
        if (!reader) {
            return;
        }

        reader->SetFailed();
    }

    bool PrepareNextPass()
    {
        if (IsCanceled()) {
            return false;
        }

        YT_LOG_DEBUG("Pass started (PassIndex: %v/%v)",
            PassIndex_ + 1,
            ReaderConfig_->PassCount);

        ResetPeerQueue();
        Peers_.clear();

        const auto& seedReplicas = SeedReplicas_.Replicas;

        std::vector<const TNodeDescriptor*> peerDescriptors;
        std::vector<TNodeId> nodeIds;
        std::vector<TString> peerAddresses;
        peerDescriptors.reserve(seedReplicas.size());
        nodeIds.reserve(seedReplicas.size());
        peerAddresses.reserve(seedReplicas.size());

        for (auto replica : seedReplicas) {
            const auto* descriptor = NodeDirectory_->FindDescriptor(replica.GetNodeId());
            if (!descriptor) {
                RegisterError(TError(
                    NNodeTrackerClient::EErrorCode::NoSuchNode,
                    "Unresolved node id %v in node directory",
                    replica.GetNodeId()));
                continue;
            }

            auto address = descriptor->FindAddress(Networks_);
            if (!address) {
                RegisterError(TError(
                    NNodeTrackerClient::EErrorCode::NoSuchNetwork,
                    "Cannot find any of %v addresses for seed %v",
                    Networks_,
                    descriptor->GetDefaultAddress()));
                OnSessionFailed(/*fatal*/ true);
                return false;
            }

            peerDescriptors.push_back(descriptor);
            nodeIds.push_back(replica.GetNodeId());
            peerAddresses.push_back(*address);
        }

        auto nodeSuspicionMarkTimes = NodeStatusDirectory_
            ? NodeStatusDirectory_->RetrieveSuspicionMarkTimes(nodeIds)
            : std::vector<std::optional<TInstant>>();
        for (int i = 0; i < std::ssize(peerDescriptors); ++i) {
            auto suspicionMarkTime = NodeStatusDirectory_
                ? nodeSuspicionMarkTimes[i]
                : std::nullopt;
            AddPeer(
                nodeIds[i],
                std::move(peerAddresses[i]),
                *peerDescriptors[i],
                EPeerType::Seed,
                suspicionMarkTime);
        }

        if (PeerQueue_.empty()) {
            RegisterError(TError(
                NChunkClient::EErrorCode::NoChunkSeedsKnown,
                "No feasible seeds to start a pass"));
            if (ReaderOptions_->AllowFetchingSeedsFromMaster) {
                OnRetryFailed();
            } else {
                OnSessionFailed(/*fatal*/ true);
            }
            return false;
        }

        return true;
    }

    void OnPassCompleted()
    {
        if (ShouldStop()) {
            return;
        }

        int passCount = ReaderConfig_->PassCount;
        YT_LOG_DEBUG("Pass completed (PassIndex: %v/%v)",
            PassIndex_ + 1,
            passCount);

        ++PassIndex_;
        if (PassIndex_ >= passCount) {
            OnRetryFailed();
            return;
        }

        if (RetryStartTime_ + ReaderConfig_->RetryTimeout < TInstant::Now()) {
            RegisterError(TError(
                EErrorCode::ReaderTimeout,
                "Replication reader retry %v out of %v timed out",
                 RetryIndex_,
                 ReaderConfig_->RetryCount)
                 << TErrorAttribute("retry_start_time", RetryStartTime_)
                 << TErrorAttribute("retry_timeout", ReaderConfig_->RetryTimeout));
            OnRetryFailed();
            return;
        }

        TDelayedExecutor::Submit(
            BIND(&TSessionBase::NextPass, MakeStrong(this))
                .Via(SessionInvoker_),
            GetBackoffDuration(PassIndex_));
    }

    template <class TResponsePtr>
    void BanSeedIfUncomplete(const TResponsePtr& rsp, const TString& address)
    {
        if (IsSeed(address) && !rsp->has_complete_chunk()) {
            YT_LOG_DEBUG("Seed does not contain the chunk (Address: %v)", address);
            BanPeer(address, false);
        }
    }

    void RegisterError(const TError& error, bool raiseAlert = false)
    {
        if (raiseAlert) {
            YT_LOG_ALERT(error);
        } else {
            YT_LOG_ERROR(error);
        }
        InnerErrors_.push_back(error);
    }

    TError BuildCombinedError(const TError& error)
    {
        return error << InnerErrors_;
    }

    virtual void NextPass() = 0;
    virtual void OnSessionFailed(bool fatal) = 0;
    virtual void OnSessionFailed(bool fatal, const TError& error) = 0;

    TPeerList ProbeAndSelectBestPeers(
        const TPeerList& candidates,
        int count,
        const std::vector<int>& blockIndexes)
    {
        if (count <= 0) {
            return {};
        }

        if (candidates.size() <= 1) {
            return {candidates.begin(), candidates.end()};
        }

        auto peerAndProbeResultsOrError = WaitFor(DoProbeAndSelectBestPeers(candidates, blockIndexes));
        YT_VERIFY(peerAndProbeResultsOrError.IsOK());

        return OnPeersProbed(std::move(peerAndProbeResultsOrError.Value()), count);
    }

    TFuture<TPeerList> AsyncProbeAndSelectBestPeers(
        const TPeerList& candidates,
        int count,
        const std::vector<int>& blockIndexes)
    {
        if (count <= 0) {
            return {};
        }
        if (candidates.size() <= 1) {
            return MakeFuture<TPeerList>({candidates.begin(), candidates.end()});
        }

        return DoProbeAndSelectBestPeers(candidates, blockIndexes)
            .Apply(BIND(
                [=, this_ = MakeWeak(this)]
                (const TErrorOr<std::vector<std::pair<TPeer, TErrorOrPeerProbeResult>>>& peerAndProbeResultsOrError)
            {
                YT_VERIFY(peerAndProbeResultsOrError.IsOK());
                return OnPeersProbed(std::move(peerAndProbeResultsOrError.Value()), count);
            }).AsyncVia(SessionInvoker_));
    }

    bool ShouldThrottle(const TString& address, bool condition) const
    {
        return
            (!IsAddressLocal(address) && condition) ||
            ReaderConfig_->EnableLocalThrottling;
    }

    bool IsCanceled() const
    {
        auto guard = Guard(CancelationSpinLock_);
        return CancelationError_.has_value();
    }

    virtual void OnCanceled(const TError& error)
    {
        auto guard = Guard(CancelationSpinLock_);

        if (CancelationError_) {
            return;
        }

        CancelationError_ = error;
        SessionFuture_.Cancel(error);
    }

    void SetSessionFuture(TFuture<void> sessionFuture)
    {
        auto guard = Guard(CancelationSpinLock_);

        if (CancelationError_) {
            sessionFuture.Cancel(*CancelationError_);
            return;
        }

        SessionFuture_ = std::move(sessionFuture);
    }

private:
    //! Errors collected by the session.
    std::vector<TError> InnerErrors_;

    TFuture<TAllyReplicasInfo> SeedsFuture_;

    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, CancelationSpinLock_);
    //! Sets the value upon cancelation.
    std::optional<TError> CancelationError_;
    //! Future of the previous cancellable action within session (e.g. Throttle, GetBlockSet).
    TFuture<void> SessionFuture_ = VoidFuture;


    int ComparePeerLocality(const TPeer& lhs, const TPeer& rhs) const
    {
        if (lhs.Locality > rhs.Locality) {
            if (ReaderConfig_->PreferLocalHost && rhs.Locality < EAddressLocality::SameHost) {
                return 1;
            }

            if (ReaderConfig_->PreferLocalRack && rhs.Locality < EAddressLocality::SameRack) {
                return 1;
            }

            if (ReaderConfig_->PreferLocalDataCenter && rhs.Locality < EAddressLocality::SameDataCenter) {
                return 1;
            }
        } else if (lhs.Locality < rhs.Locality) {
            return -ComparePeerLocality(rhs, lhs);
        }

        return 0;
    }

    int ComparePeerQueueEntries(const TPeerQueueEntry& lhs, const TPeerQueueEntry rhs) const
    {
        int result = ComparePeerLocality(lhs.Peer, rhs.Peer);
        if (result != 0) {
            return result;
        }

        if (lhs.Peer.Type != rhs.Peer.Type) {
            // Prefer Peers to Seeds to make most use of P2P.
            if (lhs.Peer.Type == EPeerType::Peer) {
                return 1;
            } else {
                YT_VERIFY(lhs.Peer.Type == EPeerType::Seed);
                return -1;
            }
        }

        if (lhs.Peer.NodeSuspicionMarkTime || rhs.Peer.NodeSuspicionMarkTime) {
            // Prefer peer that is not suspicious.
            // If both are suspicious prefer one that was suspicious for less time.
            auto lhsMarkTime = lhs.Peer.NodeSuspicionMarkTime.value_or(TInstant::Max());
            auto rhsMarkTime = rhs.Peer.NodeSuspicionMarkTime.value_or(TInstant::Max());
            return lhsMarkTime < rhsMarkTime
                ? -1
                : 1;
        }

        if (lhs.BanCount != rhs.BanCount) {
            // The less - the better.
            return rhs.BanCount - lhs.BanCount;
        }

        if (lhs.Random != rhs.Random) {
            return lhs.Random < rhs.Random ? -1 : 1;
        }

        return 0;
    }

    TDuration GetBackoffDuration(int index) const
    {
        auto backoffMultiplier = std::min(
            std::pow(ReaderConfig_->BackoffTimeMultiplier, index - 1),
            MaxBackoffMultiplier);

        auto backoffDuration = ReaderConfig_->MinBackoffTime * backoffMultiplier;
        backoffDuration = std::min(backoffDuration, ReaderConfig_->MaxBackoffTime);
        return backoffDuration;
    }

    void ResetPeerQueue()
    {
        PeerQueue_ = TPeerQueue([&] (const TPeerQueueEntry& lhs, const TPeerQueueEntry& rhs) {
            return ComparePeerQueueEntries(lhs, rhs) < 0;
        });
    }

    void OnGotSeeds(const TErrorOr<TAllyReplicasInfo>& resultOrError)
    {
        auto reader = Reader_.Lock();
        if (!reader) {
            return;
        }

        if (!resultOrError.IsOK()) {
            if (resultOrError.FindMatching(NChunkClient::EErrorCode::NoSuchChunk)) {
                RegisterError(resultOrError);
            } else {
                DiscardSeeds();
                RegisterError(TError(
                    NChunkClient::EErrorCode::MasterCommunicationFailed,
                    "Error requesting seeds from master")
                    << resultOrError);
            }
            OnSessionFailed(/*fatal*/ true);
            return;
        }

        SeedReplicas_ = resultOrError.Value();
        if (IsErasureChunkPartId(ChunkId_)) {
            auto replicaIndex = ReplicaIndexFromErasurePartId(ChunkId_);
            EraseIf(
                SeedReplicas_.Replicas,
                [&] (const auto& replica) {
                    return replica.GetReplicaIndex() != replicaIndex;
                });
        }

        reader->OnChunkReplicasLocated(SeedReplicas_);

        if (!SeedReplicas_) {
            RegisterError(TError(
                NChunkClient::EErrorCode::ChunkIsLost,
                "Chunk is lost"));
            if (ReaderConfig_->FailOnNoSeeds) {
                DiscardSeeds();
                OnSessionFailed(/*fatal*/ true);
            } else {
                OnRetryFailed();
            }
            return;
        }

        NextPass();
    }

    bool ShouldStop()
    {
        auto reader = Reader_.Lock();
        if (!reader) {
            return true;
        }

        if (StartTime_ + ReaderConfig_->SessionTimeout < TInstant::Now()) {
            RegisterError(TError(
                NChunkClient::EErrorCode::ReaderTimeout,
                "Replication reader session timed out")
                 << TErrorAttribute("session_start_time", StartTime_)
                 << TErrorAttribute("session_timeout", ReaderConfig_->SessionTimeout));
            OnSessionFailed(/*fatal*/ false);
            return true;
        }

        auto error = reader->RunSlownessChecker(TotalBytesReceived_, StartTime_);
        if (!error.IsOK()) {
            RegisterError(TError(
                NChunkClient::EErrorCode::ChunkReadSessionSlow,
                "Read session of chunk %v is slow; may attempt repair",
                ChunkId_)
                << error);
            OnSessionFailed(/*fatal*/ false);
            return true;
        }

        return false;
    }

    template <class TRspPtr>
    static std::pair<TPeer, TErrorOrPeerProbeResult> ParsePeerAndProbeResponse(
        TPeer peer,
        const TErrorOr<TRspPtr>& rspOrError)
    {
        // TODO(akozhikhov): We probably should not ignore HasCompleteChunk field.
        if (rspOrError.IsOK()) {
            return {
                std::move(peer),
                TErrorOrPeerProbeResult(ParseProbeResponse(rspOrError.Value()))
            };
        } else {
            return {
                std::move(peer),
                TErrorOrPeerProbeResult(TError(rspOrError))
            };
        }
    }

    template <class TRspPtr>
    std::pair<TPeer, TErrorOrPeerProbeResult> ParseSuspiciousPeerAndProbeResponse(
        TPeer peer,
        const TErrorOr<TRspPtr>& rspOrError,
        const TFuture<TAllyReplicasInfo>& allyReplicasFuture,
        int totalPeerCount)
    {
        VERIFY_INVOKER_AFFINITY(SessionInvoker_);

        YT_VERIFY(peer.NodeSuspicionMarkTime && NodeStatusDirectory_);

        if (rspOrError.IsOK()) {
            NodeStatusDirectory_->UpdateSuspicionMarkTime(
                peer.NodeId,
                peer.Address,
                /* suspicious */ false,
                peer.NodeSuspicionMarkTime);
            peer.NodeSuspicionMarkTime.reset();

            return {
                std::move(peer),
                TErrorOrPeerProbeResult(ParseProbeResponse(rspOrError.Value()))
            };
        }

        if (ReaderConfig_->SuspiciousNodeGracePeriod && totalPeerCount > 1) {
            auto now = TInstant::Now();
            if (*peer.NodeSuspicionMarkTime + *ReaderConfig_->SuspiciousNodeGracePeriod < now) {
                if (auto reader = Reader_.Lock()) {
                    // NB(akozhikhov): In order not to call DiscardSeeds too often, we implement additional delay.
                    YT_LOG_WARNING("Discarding seeds due to node being suspicious (NodeAddress: %v, SuspicionTime: %v)",
                        peer.Address,
                        now - *peer.NodeSuspicionMarkTime);
                    reader->DiscardSeeds(allyReplicasFuture);
                }
            }
        }

        return {
            std::move(peer),
            TErrorOrPeerProbeResult(TError(rspOrError))
        };
    }

    TFuture<std::pair<TPeer, TErrorOrPeerProbeResult>> ProbePeer(
        const IChannelPtr& channel,
        const TPeer& peer,
        const std::vector<int>& blockIndexes)
    {
        TDataNodeServiceProxy proxy(channel);
        proxy.SetDefaultTimeout(ReaderConfig_->ProbeRpcTimeout);

        auto req = proxy.ProbeBlockSet();
        SetRequestWorkloadDescriptor(req, WorkloadDescriptor_);
        req->SetResponseHeavy(true);
        ToProto(req->mutable_chunk_id(), ChunkId_);
        ToProto(req->mutable_block_indexes(), blockIndexes);
        req->SetAcknowledgementTimeout(std::nullopt);
        req->set_ally_replicas_revision(SeedReplicas_.Revision);

        if (peer.NodeSuspicionMarkTime) {
            return req->Invoke().Apply(BIND(
                [=, this_ = MakeStrong(this), seedsFuture = SeedsFuture_, totalPeerCount = std::ssize(Peers_)]
                (const TDataNodeServiceProxy::TErrorOrRspProbeBlockSetPtr& rspOrError)
                {
                    return ParseSuspiciousPeerAndProbeResponse(
                        std::move(peer),
                        rspOrError,
                        seedsFuture,
                        totalPeerCount);
                })
                .AsyncVia(SessionInvoker_));
        } else {
            return req->Invoke().Apply(BIND([=] (const TDataNodeServiceProxy::TErrorOrRspProbeBlockSetPtr& rspOrError) {
                return ParsePeerAndProbeResponse(std::move(peer), rspOrError);
            }));
        }
    }

    TFuture<std::vector<std::pair<TPeer, TErrorOrPeerProbeResult>>> DoProbeAndSelectBestPeers(
        const TPeerList& candidates,
        const std::vector<int>& blockIndexes)
    {
        // Multiple candidates - send probing requests.
        std::vector<TFuture<std::pair<TPeer, TErrorOrPeerProbeResult>>> asyncResults;
        std::vector<TFuture<std::pair<TPeer, TErrorOrPeerProbeResult>>> asyncSuspiciousResults;
        asyncResults.reserve(candidates.size());
        for (const auto& peer : candidates) {
            auto channel = GetChannel(peer.Address);
            if (!channel) {
                continue;
            }

            if (peer.NodeSuspicionMarkTime) {
                asyncSuspiciousResults.push_back(ProbePeer(
                    channel,
                    peer,
                    blockIndexes));
            } else {
                asyncResults.push_back(ProbePeer(
                    channel,
                    peer,
                    blockIndexes));
            }
        }

        YT_LOG_DEBUG("Gathered candidate peers for probing (Addresses: %v, SuspiciousNodeCount: %v)",
            candidates,
            asyncSuspiciousResults.size());

        if (asyncSuspiciousResults.empty()) {
            return AllSucceeded(std::move(asyncResults));
        } else if (asyncResults.empty()) {
            return AllSucceeded(std::move(asyncSuspiciousResults));
        } else {
            return AllSucceeded(std::move(asyncResults))
                .Apply(BIND(
                [
                    =,
                    asyncSuspiciousResults = std::move(asyncSuspiciousResults)
                ] (const TErrorOr<std::vector<std::pair<TPeer, TErrorOrPeerProbeResult>>>& resultsOrError) {
                    YT_VERIFY(resultsOrError.IsOK());
                    auto results = resultsOrError.Value();
                    int totalCandidateCount = results.size() + asyncSuspiciousResults.size();

                    for (const auto& asyncSuspiciousResult : asyncSuspiciousResults) {
                        auto maybeSuspiciousResult = asyncSuspiciousResult.TryGet();
                        if (!maybeSuspiciousResult) {
                            continue;
                        }

                        YT_VERIFY(maybeSuspiciousResult->IsOK());

                        auto suspiciousResultValue = std::move(maybeSuspiciousResult->Value());
                        if (suspiciousResultValue.second.IsOK()) {
                            results.push_back(std::move(suspiciousResultValue));
                        } else {
                            ProcessError(
                                suspiciousResultValue.second,
                                suspiciousResultValue.first,
                                TError(
                                    NChunkClient::EErrorCode::NodeProbeFailed,
                                    "Error probing suspicious node %v",
                                    suspiciousResultValue.first.Address));
                        }
                    }

                    auto omittedSuspiciousNodeCount = totalCandidateCount - results.size();
                    if (omittedSuspiciousNodeCount > 0) {
                        SessionOptions_.ChunkReaderStatistics->OmittedSuspiciousNodeCount.fetch_add(
                            omittedSuspiciousNodeCount,
                            std::memory_order_relaxed);
                    }

                    return results;
                })
                .AsyncVia(SessionInvoker_));
        }
    }

    TPeerList OnPeersProbed(
        std::vector<std::pair<TPeer, TErrorOrPeerProbeResult>> peerAndProbeResults,
        int count)
    {
        std::vector<std::pair<TPeer, TPeerProbeResult>> peerAndSuccessfulProbeResults;
        bool receivedNewPeers = false;
        for (auto& [peer, probeResultOrError] : peerAndProbeResults) {
            if (!probeResultOrError.IsOK()) {
                ProcessError(
                    probeResultOrError,
                    peer,
                    TError(
                        NChunkClient::EErrorCode::NodeProbeFailed,
                        "Error probing node %v queue length",
                        peer.Address));
                continue;
            }

            auto& probeResult = probeResultOrError.Value();

            if (UpdatePeerBlockMap(probeResult)) {
                receivedNewPeers = true;
            }

            // Exclude throttling peers from current pass.
            if (probeResult.NetThrottling || probeResult.DiskThrottling) {
                YT_LOG_DEBUG("Peer is throttling (Address: %v, NetThrottling: %v, DiskThrottling: %v)",
                    peer.Address,
                    probeResult.NetThrottling,
                    probeResult.DiskThrottling);
                continue;
            }

            peerAndSuccessfulProbeResults.emplace_back(std::move(peer), std::move(probeResult));
        }

        if (peerAndSuccessfulProbeResults.empty()) {
            YT_LOG_DEBUG("All peer candidates were discarded");
            return {};
        }

        if (receivedNewPeers) {
            YT_LOG_DEBUG("P2P was activated");
            for (const auto& [peer, probeResult] : peerAndSuccessfulProbeResults) {
                ReinstallPeer(peer.Address);
            }
            return {};
        }

        SortBy(
            peerAndSuccessfulProbeResults,
            [&] (const auto& peerAndSuccessfulProbeResult) {
                const auto& [peer, probeResult] = peerAndSuccessfulProbeResult;
                return
                    ReaderConfig_->NetQueueSizeFactor * probeResult.NetQueueSize +
                    ReaderConfig_->DiskQueueSizeFactor * probeResult.DiskQueueSize;
            });

        count = std::min(count, static_cast<int>(peerAndSuccessfulProbeResults.size()));
        TPeerList bestPeers;
        for (int index = 0; index < static_cast<int>(peerAndSuccessfulProbeResults.size()); ++index) {
            const auto& [peer, probeResult] = peerAndSuccessfulProbeResults[index];
            if (index < count) {
                bestPeers.push_back(peer);
            } else {
                ReinstallPeer(peer.Address);
            }
        }

        YT_LOG_DEBUG("Best peers selected (Peers: %v)",
            MakeFormattableView(
                MakeRange(peerAndSuccessfulProbeResults.begin(), peerAndSuccessfulProbeResults.begin() + count),
                [] (auto* builder, const auto& peerAndSuccessfulProbeResult) {
                    const auto& [peer, probeResult] = peerAndSuccessfulProbeResult;
                    builder->AppendFormat("{Address: %v, DiskQueueSize: %v, NetQueueSize: %v}",
                        peer.Address,
                        probeResult.DiskQueueSize,
                        probeResult.NetQueueSize);
                }));

        return bestPeers;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TReadBlockSetSession
    : public TSessionBase
{
public:
    TReadBlockSetSession(
        TReplicationReader* reader,
        const TClientChunkReadOptions& options,
        const std::vector<int>& blockIndexes,
        std::optional<i64> estimatedSize,
        IThroughputThrottlerPtr bandwidthThrottler,
        IThroughputThrottlerPtr rpsThrottler)
        : TSessionBase(
            reader,
            options,
            std::move(bandwidthThrottler),
            std::move(rpsThrottler))
        , Options_(options)
        , BlockIndexes_(blockIndexes)
        , EstimatedSize_(estimatedSize)
    {
        YT_LOG_DEBUG("Will read block set (Blocks: %v)",
            blockIndexes);
    }

    ~TReadBlockSetSession()
    {
        Promise_.TrySet(TError(NYT::EErrorCode::Canceled, "Reader destroyed"));
    }

    TFuture<std::vector<TBlock>> Run()
    {
        if (BlockIndexes_.empty()) {
            return MakeFuture(std::vector<TBlock>());
        }
        Promise_.OnCanceled(BIND(&TReadBlockSetSession::OnCanceled, MakeWeak(this)));
        StartTime_ = TInstant::Now();
        NextRetry();
        return Promise_;
    }

private:
    TClientChunkReadOptions Options_;

    //! Block indexes to read during the session.
    const std::vector<int> BlockIndexes_;
    const std::optional<i64> EstimatedSize_;

    //! Promise representing the session.
    const TPromise<std::vector<TBlock>> Promise_ = NewPromise<std::vector<TBlock>>();

    i64 BytesThrottled_ = 0;

    //! Blocks that are fetched so far.
    THashMap<int, TBlock> Blocks_;

    //! Maps peer addresses to block indexes.
    THashMap<TString, THashSet<int>> PeerBlocksMap_;

    //! address -> block_index -> (session_id, iteration).
    THashMap<TNodeId, THashMap<int, NChunkClient::NProto::TP2PBarrier>> P2PDeliveryBarrier_;

    struct TBlockWithCookie
    {
        int BlockIndex;
        std::unique_ptr<ICachedBlockCookie> Cookie;

        TBlockWithCookie(int blockIndex, std::unique_ptr<ICachedBlockCookie> cookie)
            : BlockIndex(blockIndex)
            , Cookie(std::move(cookie))
        { }
    };


    void NextPass() override
    {
        if (!PrepareNextPass()) {
            return;
        }

        PeerBlocksMap_.clear();
        P2PDeliveryBarrier_.clear();
        auto blockIndexes = GetUnfetchedBlockIndexes();
        for (const auto& [address, peer] : Peers_) {
            PeerBlocksMap_[address] = THashSet<int>(blockIndexes.begin(), blockIndexes.end());
        }

        RequestBlocks();
    }

    std::vector<int> GetUnfetchedBlockIndexes()
    {
        std::vector<int> result;
        result.reserve(BlockIndexes_.size());
        for (int blockIndex : BlockIndexes_) {
            if (Blocks_.find(blockIndex) == Blocks_.end()) {
                result.push_back(blockIndex);
            }
        }
        return result;
    }

    void RequestBlocks()
    {
        SessionInvoker_->Invoke(
            BIND(&TReadBlockSetSession::DoRequestBlocks, MakeStrong(this)));
    }

    bool UpdatePeerBlockMap(const TPeerProbeResult& probeResult) override
    {
        if (!ReaderConfig_->FetchFromPeers && !probeResult.PeerDescriptors.empty()) {
            YT_LOG_DEBUG("Peer suggestions received but ignored");
            return false;
        }

        bool addedNewPeers = false;
        for (const auto& peerDescriptor : probeResult.PeerDescriptors) {
            int blockIndex = peerDescriptor.block_index();
            for (auto peerNodeId : peerDescriptor.node_ids()) {
                auto maybeSuggestedDescriptor = NodeDirectory_->FindDescriptor(peerNodeId);
                if (!maybeSuggestedDescriptor) {
                    YT_LOG_DEBUG("Cannot resolve peer descriptor (NodeId: %v)",
                        peerNodeId);
                    continue;
                }

                if (auto suggestedAddress = maybeSuggestedDescriptor->FindAddress(Networks_)) {
                    if (AddPeer(
                        peerNodeId,
                        *suggestedAddress,
                        *maybeSuggestedDescriptor,
                        EPeerType::Peer,
                        /*nodeSuspicionMarkTime*/ std::nullopt))
                    {
                        addedNewPeers = true;
                    }

                    PeerBlocksMap_[*suggestedAddress].insert(blockIndex);
                    YT_LOG_DEBUG("Block peer descriptor received (Block: %v, SuggestedAddress: %v)",
                        blockIndex,
                        *suggestedAddress);

                    if (peerDescriptor.has_delivery_barier()) {
                        P2PDeliveryBarrier_[peerNodeId].emplace(
                            blockIndex,
                            peerDescriptor.delivery_barier());
                    }
                } else {
                    YT_LOG_WARNING("Peer suggestion ignored, required network is missing "
                        "(SuggestedAddress: %v, Networks: %v)",
                        maybeSuggestedDescriptor->GetDefaultAddress(),
                        Networks_);
                }
            }
        }

        if (probeResult.AllyReplicas) {
            MaybeUpdateSeeds(probeResult.AllyReplicas);
        }

        if (addedNewPeers) {
            SessionOptions_.ChunkReaderStatistics->P2PActivationCount.fetch_add(1, std::memory_order_relaxed);
        }

        return addedNewPeers;
    }

    void DoRequestBlocks()
    {
        auto reader = Reader_.Lock();
        if (!reader || IsCanceled()) {
            return;
        }

        auto blockIndexes = GetUnfetchedBlockIndexes();
        if (blockIndexes.empty()) {
            OnSessionSucceeded();
            return;
        }

        std::vector<TBlockWithCookie> cachedBlocks;
        std::vector<TBlockWithCookie> uncachedBlocks;

        const auto& blockCache = reader->BlockCache_;
        for (int blockIndex : blockIndexes) {
            TBlockId blockId(ChunkId_, blockIndex);
            const auto& readerConfig = reader->Config_;
            if (readerConfig->UseBlockCache) {
                if (reader->Config_->UseAsyncBlockCache) {
                    auto cookie = blockCache->GetBlockCookie(blockId, EBlockType::CompressedData);
                    if (cookie->IsActive()) {
                        uncachedBlocks.push_back(TBlockWithCookie(blockIndex, std::move(cookie)));
                    } else {
                        cachedBlocks.push_back(TBlockWithCookie(blockIndex, std::move(cookie)));
                    }
                } else {
                    auto block = blockCache->FindBlock(blockId, EBlockType::CompressedData);
                    if (block.Block) {
                        cachedBlocks.push_back(TBlockWithCookie(blockIndex, CreatePresetCachedBlockCookie(block)));
                    } else {
                        uncachedBlocks.push_back(TBlockWithCookie(blockIndex, /*cookie*/ nullptr));
                    }
                }
            } else {
                uncachedBlocks.push_back(TBlockWithCookie(blockIndex, /*cookie*/ nullptr));
            }
        }

        bool requestMoreBlocks = true;
        if (!uncachedBlocks.empty()) {
            auto candidates = FindCandidates(reader, uncachedBlocks);
            if (candidates.empty()) {
                OnPassCompleted();
                for (const auto& block : uncachedBlocks) {
                    if (const auto& cookie = block.Cookie) {
                        cookie->SetBlock(TError("No peer candidates were found"));
                    }
                }
                return;
            }
            requestMoreBlocks = FetchBlocksFromNodes(reader, uncachedBlocks, candidates);
        }

        FetchBlocksFromCache(cachedBlocks);

        if (requestMoreBlocks) {
            RequestBlocks();
        }
    }

    TPeerList FindCandidates(
        const TReplicationReaderPtr& reader,
        const std::vector<TBlockWithCookie>& blocksToFetch)
    {
        TPeerList candidates;

        int desiredPeerCount = GetDesiredPeerCount();
        while (std::ssize(candidates) < desiredPeerCount) {
            auto hasUnfetchedBlocks = [&] (const TString& address) {
                const auto& peerBlockIndexes = GetOrCrash(PeerBlocksMap_, address);

                for (const auto& block : blocksToFetch) {
                    int blockIndex = block.BlockIndex;
                    if (peerBlockIndexes.contains(blockIndex)) {
                        return true;
                    }
                }

                return false;
            };

            auto moreCandidates = PickPeerCandidates(
                reader,
                ReaderConfig_->ProbePeerCount,
                /* enableEarlyExit */ !ReaderConfig_->BlockRpcHedgingDelay,
                hasUnfetchedBlocks);

            if (moreCandidates.empty()) {
                break;
            }
            candidates.insert(candidates.end(), moreCandidates.begin(), moreCandidates.end());
        }

        return candidates;
    }

    //! Fetches blocks from nodes and adds them to block cache via cookies.
    //! Returns |True| if more blocks can be requested and |False| otherwise.
    bool FetchBlocksFromNodes(
        const TReplicationReaderPtr& reader,
        const std::vector<TBlockWithCookie>& blocks,
        const TPeerList& candidates)
    {
        auto cancelAll = [&] (const TError& error) {
            for (const auto& block : blocks) {
                // NB: Setting cookie twice is OK. Only the first value
                // will be used.
                if (const auto& cookie = block.Cookie) {
                    cookie->SetBlock(error);
                }
            }
        };

        std::vector<int> blockIndexes;
        blockIndexes.reserve(blocks.size());
        for (const auto& block : blocks) {
            blockIndexes.push_back(block.BlockIndex);
        }

        NProfiling::TWallTimer pickPeerTimer;

        // One extra request for actually getting blocks.
        // Hedging requests are disregarded.
        if (!SyncThrottle(RpsThrottler_, 1 + candidates.size())) {
            cancelAll(TError(
                NChunkClient::EErrorCode::ReaderThrottlingFailed,
                "Failed to apply throttling in reader"));
            return false;
        }

        auto peers = ProbeAndSelectBestPeers(
            candidates,
            GetDesiredPeerCount(),
            blockIndexes);

        SessionOptions_.ChunkReaderStatistics->PickPeerWaitTime.fetch_add(
            pickPeerTimer.GetElapsedValue(),
            std::memory_order_relaxed);

        IHedgingManagerPtr hedgingManager;
        if (SessionOptions_.HedgingManager) {
            hedgingManager = SessionOptions_.HedgingManager;
        } else if (ReaderConfig_->BlockRpcHedgingDelay) {
            hedgingManager = CreateSimpleHedgingManager(*ReaderConfig_->BlockRpcHedgingDelay);
        }

        std::optional<THedgingChannelOptions> hedgingOptions;
        if (hedgingManager) {
            hedgingOptions = THedgingChannelOptions{
                .HedgingManager = std::move(hedgingManager),
                .CancelPrimaryOnHedging = ReaderConfig_->CancelPrimaryBlockRpcRequestOnHedging,
            };
        }

        auto channel = MakePeersChannel(
            peers,
            hedgingOptions);
        if (!channel) {
            cancelAll(TError("No peers were selected"));
            return true;
        }

        if (ShouldThrottle(peers[0].Address, BytesThrottled_ == 0 && EstimatedSize_)) {
            // NB(psushin): This is preliminary throttling. The subsequent request may fail or return partial result.
            // In order not to throttle twice, we use BandwidthThrottled_ flag.
            // Still it protects us from bursty incoming traffic on the host.
            // If estimated size was not given, we fallback to post-throttling on actual received size.
            BytesThrottled_ = *EstimatedSize_;
            if (!SyncThrottle(BandwidthThrottler_, *EstimatedSize_)) {
                cancelAll(TError(
                    NChunkClient::EErrorCode::ReaderThrottlingFailed,
                    "Failed to apply throttling in reader"));
                return false;
            }
        }

        TDataNodeServiceProxy proxy(channel);
        proxy.SetDefaultTimeout(ReaderConfig_->BlockRpcTimeout);

        auto req = proxy.GetBlockSet();
        req->SetResponseHeavy(true);
        req->SetMultiplexingBand(SessionOptions_.MultiplexingBand);
        SetRequestWorkloadDescriptor(req, WorkloadDescriptor_);
        ToProto(req->mutable_chunk_id(), ChunkId_);
        ToProto(req->mutable_block_indexes(), blockIndexes);
        req->set_populate_cache(ReaderConfig_->PopulateCache);
        ToProto(req->mutable_read_session_id(), SessionOptions_.ReadSessionId);
        req->set_ally_replicas_revision(SeedReplicas_.Revision);

        FillP2PBarriers(req->mutable_wait_barriers(), peers, blockIndexes);

        NProfiling::TWallTimer dataWaitTimer;
        auto rspFuture = req->Invoke();
        SetSessionFuture(rspFuture.As<void>());
        auto rspOrError = WaitFor(rspFuture);
        SessionOptions_.ChunkReaderStatistics->DataWaitTime.fetch_add(
            dataWaitTimer.GetElapsedValue(),
            std::memory_order_relaxed);

        bool backup = IsBackup(rspOrError);
        const auto& respondedPeer = backup ? peers[1] : peers[0];

        if (!rspOrError.IsOK()) {
            auto wrappingError = TError(
                "Error fetching blocks from node %v",
                respondedPeer.Address);
            ProcessError(
                rspOrError,
                respondedPeer,
                wrappingError);
            cancelAll(wrappingError << rspOrError);
            return true;
        }

        const auto& rsp = rspOrError.Value();

        if (backup) {
            BanPeer(peers[0].Address, false);
        }

        SessionOptions_.ChunkReaderStatistics->DataBytesTransmitted.fetch_add(
            rsp->GetTotalSize(),
            std::memory_order_relaxed);
        reader->AccountTraffic(rsp->GetTotalSize(), respondedPeer.NodeDescriptor);

        auto probeResult = ParseProbeResponse(rsp);

        UpdatePeerBlockMap(probeResult);

        if (probeResult.NetThrottling || probeResult.DiskThrottling) {
            YT_LOG_DEBUG("Peer is throttling (Address: %v, NetThrottling: %v, DiskThrottling: %v)",
                respondedPeer.Address,
                probeResult.NetThrottling,
                probeResult.DiskThrottling);
        }

        if (rsp->has_chunk_reader_statistics()) {
            UpdateFromProto(&SessionOptions_.ChunkReaderStatistics, rsp->chunk_reader_statistics());
        }

        i64 bytesReceived = 0;
        int invalidBlockCount = 0;
        std::vector<int> receivedBlockIndexes;

        auto response = GetRpcAttachedBlocks(rsp, /* validateChecksums */ false);

        for (auto& block: response) {
            block.Data = TrackMemoryReference(Options_.MemoryReferenceTracker, std::move(block.Data));
        }

        for (int index = 0; index < std::ssize(response); ++index) {
            const auto& block = response[index];
            if (!block) {
                continue;
            }

            int blockIndex = req->block_indexes(index);
            auto blockId = TBlockId(ChunkId_, blockIndex);

            if (auto error = block.ValidateChecksum(); !error.IsOK()) {
                RegisterError(
                    TError(
                        NChunkClient::EErrorCode::BlockChecksumMismatch,
                        "Failed to validate received block checksum")
                        << TErrorAttribute("block_id", ToString(blockId))
                        << TErrorAttribute("peer", respondedPeer.Address)
                        << error,
                    /* raiseAlert */ true);

                ++invalidBlockCount;
                continue;
            }

            auto sourceDescriptor = ReaderOptions_->EnableP2P
                ? std::optional<TNodeDescriptor>(GetPeerDescriptor(respondedPeer.Address))
                : std::optional<TNodeDescriptor>(std::nullopt);

            auto& cookie = blocks[index].Cookie;
            if (cookie) {
                TCachedBlock cachedBlock(block);
                cookie->SetBlock(cachedBlock);
            } else if (reader->Config_->UseBlockCache) {
                reader->BlockCache_->PutBlock(blockId, EBlockType::CompressedData, block);
            }

            YT_VERIFY(Blocks_.emplace(blockIndex, block).second);
            bytesReceived += block.Size();
            TotalBytesReceived_ += block.Size();
            receivedBlockIndexes.push_back(blockIndex);
        }

        if (invalidBlockCount > 0) {
            BanPeer(respondedPeer.Address, false);
        }

        BanSeedIfUncomplete(rsp, respondedPeer.Address);

        if (bytesReceived > 0) {
            // Reinstall peer into peer queue, if some data was received.
            ReinstallPeer(respondedPeer.Address);
        }

        YT_LOG_DEBUG("Finished processing block response "
            "(Address: %v, PeerType: %v, BlocksReceived: %v, BytesReceived: %v, PeersSuggested: %v, InvalidBlockCount: %v)",
            respondedPeer.Address,
            respondedPeer.Type,
            MakeShrunkFormattableView(receivedBlockIndexes, TDefaultFormatter(), 3),
            bytesReceived,
            rsp->peer_descriptors_size(),
            invalidBlockCount);

        if (ShouldThrottle(respondedPeer.Address, TotalBytesReceived_ > BytesThrottled_)) {
            auto delta = TotalBytesReceived_ - BytesThrottled_;
            BytesThrottled_ = TotalBytesReceived_;
            if (!SyncThrottle(BandwidthThrottler_, delta)) {
                cancelAll(TError(
                    NChunkClient::EErrorCode::ReaderThrottlingFailed,
                    "Failed to apply throttling in reader"));
                return false;
            }
        }

        cancelAll(TError("Block was not sent by node"));
        return true;
    }

    void FillP2PBarriers(
        google::protobuf::RepeatedPtrField<NProto::TP2PBarrier>* barriers,
        const TPeerList& peerList,
        const std::vector<int>& blockIndexes)
    {
        // We need a way of sending two separate requests to the two different nodes.
        // And there is no way of doing this with hedging channel.
        // So we send all barriers to both nodes, and filter them in data node service.

        for (const auto& peer : peerList) {
            auto blockBarriers = P2PDeliveryBarrier_.find(peer.NodeId);
            if (blockBarriers == P2PDeliveryBarrier_.end()) {
                continue;
            }

            THashMap<TGuid, i64> maxBarrier;
            for (int blockIndex : blockIndexes) {
                auto it = blockBarriers->second.find(blockIndex);
                if (it == blockBarriers->second.end()) {
                    continue;
                }

                auto sessionId = FromProto<TGuid>(it->second.session_id());
                maxBarrier[sessionId] = std::max(maxBarrier[sessionId], it->second.iteration());
            }

            for (const auto& [sessionId, iteration] : maxBarrier) {
                auto wait = barriers->Add();

                ToProto(wait->mutable_session_id(), sessionId);
                wait->set_iteration(iteration);
                wait->set_if_node_id(peer.NodeId);
            }
        }
    }

    void FetchBlocksFromCache(const std::vector<TBlockWithCookie>& blocks)
    {
        std::vector<TFuture<TCachedBlock>> cachedBlockFutures;
        cachedBlockFutures.reserve(blocks.size());
        for (const auto& block : blocks) {
            cachedBlockFutures.push_back(block.Cookie->GetBlockFuture());
        }
        auto cachedBlocks = WaitForFast(AllSet(cachedBlockFutures))
            .ValueOrThrow();
        YT_VERIFY(cachedBlocks.size() == blocks.size());

        for (int index = 0; index < std::ssize(blocks); ++index) {
            int blockIndex = blocks[index].BlockIndex;
            const auto& blockOrError = cachedBlocks[index];
            if (blockOrError.IsOK()) {
                YT_LOG_DEBUG("Fetched block from block cache (BlockIndex: %v)",
                    blockIndex);

                const auto& block = blockOrError.Value().Block;
                YT_VERIFY(Blocks_.emplace(blockIndex, block).second);
                SessionOptions_.ChunkReaderStatistics->DataBytesReadFromCache.fetch_add(
                    block.Size(),
                    std::memory_order_relaxed);
            }
        }
    }

    void OnSessionSucceeded()
    {
        YT_LOG_DEBUG("All requested blocks are fetched");

        std::vector<TBlock> blocks;
        blocks.reserve(BlockIndexes_.size());
        for (int blockIndex : BlockIndexes_) {
            const auto& block = Blocks_[blockIndex];
            YT_VERIFY(block.Data);
            blocks.push_back(block);
        }
        Promise_.TrySet(std::vector<TBlock>(blocks));
    }

    void OnSessionFailed(bool fatal) override
    {
        auto error = BuildCombinedError(TError(
            NChunkClient::EErrorCode::ChunkBlockFetchFailed,
            "Error fetching blocks for chunk %v",
            ChunkId_));
        OnSessionFailed(fatal, error);
    }

    void OnSessionFailed(bool fatal, const TError& error) override
    {
        YT_LOG_DEBUG(error, "Reader session failed (Fatal: %v)", fatal);

        if (fatal) {
            SetReaderFailed();
        }

        Promise_.TrySet(error);
    }

    int GetDesiredPeerCount() const
    {
        return (SessionOptions_.HedgingManager || ReaderConfig_->BlockRpcHedgingDelay) ? 2 : 1;
    }

    void OnCanceled(const TError& error) override
    {
        auto wrappedError = TError(NYT::EErrorCode::Canceled, "ReadBlockSet session canceled")
            << error;
        TSessionBase::OnCanceled(wrappedError);
        Promise_.TrySet(wrappedError);
    }
};

TFuture<std::vector<TBlock>> TReplicationReader::ReadBlocks(
    const TClientChunkReadOptions& options,
    const std::vector<int>& blockIndexes,
    std::optional<i64> estimatedSize)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (blockIndexes.empty()) {
        return MakeFuture<std::vector<TBlock>>({});
    }

    auto session = New<TReadBlockSetSession>(
        this,
        options,
        blockIndexes,
        estimatedSize,
        BandwidthThrottler_,
        RpsThrottler_);
    return session->Run();
}

////////////////////////////////////////////////////////////////////////////////

class TReadBlockRangeSession
    : public TSessionBase
{
public:
    TReadBlockRangeSession(
        TReplicationReader* reader,
        const TClientChunkReadOptions& options,
        int firstBlockIndex,
        int blockCount,
        const std::optional<i64> estimatedSize,
        IThroughputThrottlerPtr bandwidthThrottler,
        IThroughputThrottlerPtr rpsThrottler)
        : TSessionBase(
            reader,
            options,
            std::move(bandwidthThrottler),
            std::move(rpsThrottler))
        , FirstBlockIndex_(firstBlockIndex)
        , BlockCount_(blockCount)
        , EstimatedSize_(estimatedSize)
    {
        YT_LOG_DEBUG("Will read block range (Blocks: %v-%v)",
            FirstBlockIndex_,
            FirstBlockIndex_ + BlockCount_ - 1);
    }

    TFuture<std::vector<TBlock>> Run()
    {
        if (BlockCount_ == 0) {
            return MakeFuture(std::vector<TBlock>());
        }
        Promise_.OnCanceled(BIND(&TReadBlockRangeSession::OnCanceled, MakeWeak(this)));
        StartTime_ = TInstant::Now();
        NextRetry();
        return Promise_;
    }

private:
    //! First block index to fetch.
    const int FirstBlockIndex_;

    //! Number of blocks to fetch.
    const int BlockCount_;

    std::optional<i64> EstimatedSize_;

    //! Promise representing the session.
    const TPromise<std::vector<TBlock>> Promise_ = NewPromise<std::vector<TBlock>>();

    //! Blocks that are fetched so far.
    std::vector<TBlock> FetchedBlocks_;

    i64 BytesThrottled_ = 0;


    void NextPass() override
    {
        if (!PrepareNextPass()) {
            return;
        }

        RequestBlocks();
    }

    void RequestBlocks()
    {
        SessionInvoker_->Invoke(
            BIND(&TReadBlockRangeSession::DoRequestBlocks, MakeStrong(this)));
    }

    void DoRequestBlocks()
    {
        auto reader = Reader_.Lock();
        if (!reader || IsCanceled()) {
            return;
        }

        YT_VERIFY(FetchedBlocks_.empty());

        auto candidates = PickPeerCandidates(
            reader,
            /* count */ 1,
            /* enableEarlyExit */ true);
        if (candidates.empty()) {
            OnPassCompleted();
            return;
        }

        const auto& peer = candidates.front();
        const auto& peerAddress = peer.Address;
        auto channel = GetChannel(peerAddress);
        if (!channel) {
            RequestBlocks();
            return;
        }

        YT_LOG_DEBUG("Requesting blocks from peer (Address: %v, Blocks: %v-%v, EstimatedSize: %v, BytesThrottled: %v)",
            peerAddress,
            FirstBlockIndex_,
            FirstBlockIndex_ + BlockCount_ - 1,
            EstimatedSize_,
            BytesThrottled_);

        if (ShouldThrottle(peerAddress, BytesThrottled_ == 0 && EstimatedSize_)) {
            // NB(psushin): This is preliminary throttling. The subsequent request may fail or return partial result.
            // In order not to throttle twice, we use BandwidthThrottled_ flag.
            // Still it protects us from bursty incoming traffic on the host.
            // If estimated size was not given, we fallback to post-throttling on actual received size.
            BytesThrottled_ = *EstimatedSize_;
            if (!SyncThrottle(BandwidthThrottler_, *EstimatedSize_)) {
                return;
            }
        }

        TDataNodeServiceProxy proxy(channel);
        proxy.SetDefaultTimeout(ReaderConfig_->BlockRpcTimeout);

        auto req = proxy.GetBlockRange();
        req->SetResponseHeavy(true);
        req->SetMultiplexingBand(SessionOptions_.MultiplexingBand);
        SetRequestWorkloadDescriptor(req, WorkloadDescriptor_);
        ToProto(req->mutable_chunk_id(), ChunkId_);
        req->set_first_block_index(FirstBlockIndex_);
        req->set_block_count(BlockCount_);

        NProfiling::TWallTimer dataWaitTimer;
        auto rspFuture = req->Invoke();
        SetSessionFuture(rspFuture.As<void>());
        auto rspOrError = WaitFor(rspFuture);
        SessionOptions_.ChunkReaderStatistics->DataWaitTime.fetch_add(
            dataWaitTimer.GetElapsedValue(),
            std::memory_order_relaxed);

        if (!rspOrError.IsOK()) {
            ProcessError(
                rspOrError,
                peer,
                TError(
                    "Error fetching blocks from node %v",
                    peerAddress));
            RequestBlocks();
            return;
        }

        const auto& rsp = rspOrError.Value();

        SessionOptions_.ChunkReaderStatistics->DataBytesTransmitted.fetch_add(
            rsp->GetTotalSize(),
            std::memory_order_relaxed);
        if (rsp->has_chunk_reader_statistics()) {
            UpdateFromProto(&SessionOptions_.ChunkReaderStatistics, rsp->chunk_reader_statistics());
        }

        auto blocks = GetRpcAttachedBlocks(rsp, /* validateChecksums */ false);

        int blocksReceived = 0;
        i64 bytesReceived = 0;

        for (const auto& block : blocks) {
            if (!block) {
                break;
            }

            if (auto error = block.ValidateChecksum(); !error.IsOK()) {
                RegisterError(
                    TError(
                        NChunkClient::EErrorCode::BlockChecksumMismatch,
                        "Failed to validate received block checksum")
                        << TErrorAttribute("block_id", ToString(TBlockId(ChunkId_, FirstBlockIndex_ + blocksReceived)))
                        << TErrorAttribute("peer", peerAddress)
                        << error,
                    /* raiseAlert */ true);

                BanPeer(peerAddress, false);
                FetchedBlocks_.clear();
                RequestBlocks();
                return;
            }

            blocksReceived += 1;
            bytesReceived += block.Size();
            TotalBytesReceived_ += block.Size();

            FetchedBlocks_.push_back(std::move(block));
        }

        BanSeedIfUncomplete(rsp, peerAddress);

        if (rsp->net_throttling() || rsp->disk_throttling()) {
            YT_LOG_DEBUG("Peer is throttling (Address: %v)", peerAddress);
        } else if (blocksReceived == 0) {
            YT_LOG_DEBUG("Peer has no relevant blocks (Address: %v)", peerAddress);
            BanPeer(peerAddress, false);
        } else {
            ReinstallPeer(peerAddress);
        }

        YT_LOG_DEBUG("Finished processing block response (Address: %v, BlocksReceived: %v-%v, BytesReceived: %v)",
            peerAddress,
            FirstBlockIndex_,
            FirstBlockIndex_ + blocksReceived - 1,
            bytesReceived);

        if (ShouldThrottle(peerAddress, TotalBytesReceived_ > BytesThrottled_)) {
            auto delta = TotalBytesReceived_ - BytesThrottled_;
            BytesThrottled_ = TotalBytesReceived_;
            if (!SyncThrottle(BandwidthThrottler_, delta)) {
                return;
            }
        }

        if (blocksReceived > 0) {
            OnSessionSucceeded();
        } else {
            RequestBlocks();
        }
    }

    void OnSessionSucceeded()
    {
        YT_LOG_DEBUG("Some blocks are fetched (Blocks: %v-%v)",
            FirstBlockIndex_,
            FirstBlockIndex_ + FetchedBlocks_.size() - 1);

        Promise_.TrySet(std::vector<TBlock>(FetchedBlocks_));
    }

    void OnSessionFailed(bool fatal) override
    {
        auto error = BuildCombinedError(TError(
            NChunkClient::EErrorCode::ChunkBlockFetchFailed,
            "Error fetching blocks for chunk %v",
            ChunkId_));
        OnSessionFailed(fatal, error);
    }

    void OnSessionFailed(bool fatal, const TError& error) override
    {
        YT_LOG_DEBUG(error, "Reader session failed (Fatal: %v)", fatal);

        if (fatal) {
            SetReaderFailed();
        }

        Promise_.TrySet(error);
    }

    void OnCanceled(const TError& error) override
    {
        auto wrappedError = TError(NYT::EErrorCode::Canceled, "ReadBlockRange session canceled")
            << error;
        TSessionBase::OnCanceled(wrappedError);
        Promise_.TrySet(wrappedError);
    }
};

TFuture<std::vector<TBlock>> TReplicationReader::ReadBlocks(
    const TClientChunkReadOptions& options,
    int firstBlockIndex,
    int blockCount,
    std::optional<i64> estimatedSize)
{
    VERIFY_THREAD_AFFINITY_ANY();
    YT_VERIFY(blockCount >= 0);

    if (blockCount == 0) {
        return MakeFuture<std::vector<TBlock>>({});
    }

    auto session = New<TReadBlockRangeSession>(
        this,
        options,
        firstBlockIndex,
        blockCount,
        estimatedSize,
        BandwidthThrottler_,
        RpsThrottler_);
    return session->Run();
}

////////////////////////////////////////////////////////////////////////////////

class TGetMetaSession
    : public TSessionBase
{
public:
    TGetMetaSession(
        TReplicationReader* reader,
        const TClientChunkReadOptions& options,
        const std::optional<int> partitionTag,
        const std::optional<std::vector<int>>& extensionTags)
        : TSessionBase(
            reader,
            options,
            reader->BandwidthThrottler_,
            reader->RpsThrottler_)
        , PartitionTag_(partitionTag)
        , ExtensionTags_(extensionTags)
    { }

    ~TGetMetaSession()
    {
        Promise_.TrySet(TError(NYT::EErrorCode::Canceled, "Reader destroyed"));
    }

    TFuture<TRefCountedChunkMetaPtr> Run()
    {
        StartTime_ = TInstant::Now();
        Promise_.OnCanceled(BIND(&TGetMetaSession::OnCanceled, MakeWeak(this)));
        NextRetry();
        return Promise_;
    }

private:
    const std::optional<int> PartitionTag_;
    const std::optional<std::vector<int>> ExtensionTags_;

    //! Promise representing the session.
    const TPromise<TRefCountedChunkMetaPtr> Promise_ = NewPromise<TRefCountedChunkMetaPtr>();


    void NextPass() override
    {
        if (!PrepareNextPass()) {
            return;
        }

        RequestMeta();
    }

    void RequestMeta()
    {
        // NB: strong ref here is the only reference that keeps session alive.
        SessionInvoker_->Invoke(
            BIND_NEW(&TGetMetaSession::DoRequestMeta, MakeStrong(this)));
    }

    void DoRequestMeta()
    {
        auto reader = Reader_.Lock();
        if (!reader || IsCanceled()) {
            return;
        }

        auto peers = PickPeerCandidates(
            reader,
            ReaderConfig_->MetaRpcHedgingDelay ? 2 : 1,
            /* enableEarlyExit */ false);
        if (peers.empty()) {
            OnPassCompleted();
            return;
        }

        std::optional<THedgingChannelOptions> hedgingOptions;
        if (ReaderConfig_->MetaRpcHedgingDelay) {
            hedgingOptions = THedgingChannelOptions{
                .HedgingManager = CreateSimpleHedgingManager(*ReaderConfig_->MetaRpcHedgingDelay),
                .CancelPrimaryOnHedging = false,
            };
        }
        auto channel = MakePeersChannel(peers, hedgingOptions);
        if (!channel) {
            RequestMeta();
            return;
        }

        YT_LOG_DEBUG("Requesting chunk meta (Addresses: %v)", peers);

        TDataNodeServiceProxy proxy(channel);
        proxy.SetDefaultTimeout(ReaderConfig_->MetaRpcTimeout);

        auto req = proxy.GetChunkMeta();
        req->SetResponseHeavy(true);
        req->SetMultiplexingBand(SessionOptions_.MultiplexingBand);
        SetRequestWorkloadDescriptor(req, WorkloadDescriptor_);
        req->set_enable_throttling(true);
        ToProto(req->mutable_chunk_id(), ChunkId_);
        req->set_all_extension_tags(!ExtensionTags_);
        if (PartitionTag_) {
            req->set_partition_tag(*PartitionTag_);
        }
        if (ExtensionTags_) {
            ToProto(req->mutable_extension_tags(), *ExtensionTags_);
        }
        req->set_supported_chunk_features(ToUnderlying(GetSupportedChunkFeatures()));

        NProfiling::TWallTimer dataWaitTimer;
        auto rspFuture = req->Invoke();
        SetSessionFuture(rspFuture.As<void>());
        auto rspOrError = WaitFor(rspFuture);
        SessionOptions_.ChunkReaderStatistics->DataWaitTime.fetch_add(
            dataWaitTimer.GetElapsedValue(),
            std::memory_order_relaxed);

        bool backup = IsBackup(rspOrError);
        const auto& respondedPeer = backup ? peers[1] : peers[0];

        if (!rspOrError.IsOK()) {
            ProcessError(
                rspOrError,
                respondedPeer,
                TError(
                    "Error fetching meta from node %v",
                    respondedPeer.Address));
            RequestMeta();
            return;
        }

        const auto& rsp = rspOrError.Value();

        if (backup) {
            BanPeer(peers[0].Address, false);
        }

        SessionOptions_.ChunkReaderStatistics->DataBytesTransmitted.fetch_add(
            rsp->GetTotalSize(),
            std::memory_order_relaxed);

        if (rsp->net_throttling()) {
            YT_LOG_DEBUG("Peer is throttling (Address: %v)", respondedPeer.Address);
            RequestMeta();
            return;
        }

        if (rsp->has_chunk_reader_statistics()) {
            UpdateFromProto(&SessionOptions_.ChunkReaderStatistics, rsp->chunk_reader_statistics());
        }

        TotalBytesReceived_ += rsp->ByteSize();
        OnSessionSucceeded(std::move(*rsp->mutable_chunk_meta()));
    }

    void OnSessionSucceeded(NProto::TChunkMeta&& chunkMeta)
    {
        YT_LOG_DEBUG("Chunk meta obtained");
        Promise_.TrySet(New<TRefCountedChunkMeta>(std::move(chunkMeta)));
    }

    void OnSessionFailed(bool fatal) override
    {
        auto error = BuildCombinedError(TError(
            NChunkClient::EErrorCode::ChunkMetaFetchFailed,
            "Error fetching meta for chunk %v",
            ChunkId_));
        OnSessionFailed(fatal, error);
    }

    void OnSessionFailed(bool fatal, const TError& error) override
    {
        if (fatal) {
            SetReaderFailed();
        }

        Promise_.TrySet(error);
    }

    void OnCanceled(const TError& error) override
    {
        auto wrappedError = TError(NYT::EErrorCode::Canceled, "GetMeta session canceled")
            << error;
        TSessionBase::OnCanceled(wrappedError);
        Promise_.TrySet(wrappedError);
    }
};

TFuture<TRefCountedChunkMetaPtr> TReplicationReader::FetchMeta(
    const TClientChunkReadOptions& options,
    std::optional<int> partitionTag,
    const std::optional<std::vector<int>>& extensionTags)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto session = New<TGetMetaSession>(
        this,
        options,
        partitionTag,
        extensionTags);
    return session->Run();
}

TFuture<TRefCountedChunkMetaPtr> TReplicationReader::GetMeta(
    const TClientChunkReadOptions& options,
    std::optional<int> partitionTag,
    const std::optional<std::vector<int>>& extensionTags)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!partitionTag && ChunkMetaCache_ && Config_->EnableChunkMetaCache) {
        return ChunkMetaCache_->Fetch(
            GetChunkId(),
            extensionTags,
            BIND(&TReplicationReader::FetchMeta, MakeStrong(this), options, std::nullopt));
    } else {
        return FetchMeta(options, partitionTag, extensionTags);
    }
}

////////////////////////////////////////////////////////////////////////////////

class TLookupRowsSession
    : public TSessionBase
{
public:
    TLookupRowsSession(
        TReplicationReader* reader,
        const TClientChunkReadOptions& options,
        TSharedRange<TLegacyKey> lookupKeys,
        TObjectId tableId,
        TRevision revision,
        TTableSchemaPtr tableSchema,
        const std::optional<i64> estimatedSize,
        const TColumnFilter& columnFilter,
        TTimestamp timestamp,
        NCompression::ECodec codecId,
        bool produceAllVersions,
        TTimestamp overrideTimestamp,
        bool enablePeerProbing,
        bool enableRejectsIfThrottling,
        IThroughputThrottlerPtr bandwidthThrottler,
        IThroughputThrottlerPtr rpsThrottler)
        : TSessionBase(
            reader,
            options,
            std::move(bandwidthThrottler),
            std::move(rpsThrottler))
        , LookupKeys_(std::move(lookupKeys))
        , TableId_(tableId)
        , Revision_(revision)
        , TableSchema_(std::move(tableSchema))
        , ReadSessionId_(options.ReadSessionId)
        , ColumnFilter_(columnFilter)
        , Timestamp_(timestamp)
        , CodecId_(codecId)
        , ProduceAllVersions_(produceAllVersions)
        , OverrideTimestamp_(overrideTimestamp)
        , EnablePeerProbing_(enablePeerProbing)
        , EnableRejectsIfThrottling_(enableRejectsIfThrottling)
    {
        Logger.AddTag("TableId: %v, Revision: %x",
            TableId_,
            Revision_);
        if (estimatedSize) {
            BytesToThrottle_ += std::max(0L, *estimatedSize);
        }

        auto writer = CreateWireProtocolWriter();
        writer->WriteUnversionedRowset(MakeRange(LookupKeys_));
        Keyset_ = writer->Finish();
    }

    ~TLookupRowsSession()
    {
        Promise_.TrySet(TError(NYT::EErrorCode::Canceled, "Reader destroyed"));
    }

    TFuture<TSharedRef> Run()
    {
        YT_VERIFY(!LookupKeys_.Empty());

        StartTime_ = NProfiling::GetInstant();
        Promise_.OnCanceled(BIND(&TLookupRowsSession::OnCanceled, MakeWeak(this)));
        NextRetry();
        return Promise_;
    }

private:
    using TLookupResponse = TIntrusivePtr<NRpc::TTypedClientResponse<NChunkClient::NProto::TRspLookupRows>>;

    const TSharedRange<TLegacyKey> LookupKeys_;
    const TObjectId TableId_;
    const TRevision Revision_;
    const TTableSchemaPtr TableSchema_;
    const TReadSessionId ReadSessionId_;
    const TColumnFilter ColumnFilter_;
    TTimestamp Timestamp_;
    const NCompression::ECodec CodecId_;
    const bool ProduceAllVersions_;
    const TTimestamp OverrideTimestamp_;
    const bool EnablePeerProbing_;
    const bool EnableRejectsIfThrottling_;

    //! Promise representing the session.
    const TPromise<TSharedRef> Promise_ = NewPromise<TSharedRef>();

    std::vector<TSharedRef> Keyset_;

    TSharedRef FetchedRowset_;

    i64 BytesToThrottle_ = 0;
    i64 BytesThrottled_ = 0;

    bool WaitedForSchemaForTooLong_ = false;

    int SinglePassIterationCount_;
    int CandidateIndex_;
    TPeerList SinglePassCandidates_;
    THashMap<TString, double> PeerAddressToThrottlingRate_;


    void NextPass() override
    {
        // Specific bounds for lookup.
        if (PassIndex_ >= ReaderConfig_->LookupRequestPassCount) {
            if (WaitedForSchemaForTooLong_) {
                RegisterError(TError(
                    NChunkClient::EErrorCode::WaitedForSchemaForTooLong,
                    "Some data node was healthy but was waiting for schema for too long; "
                    "probably other tablet node has failed"));
            }
            if (RetryIndex_ >= ReaderConfig_->LookupRequestRetryCount) {
                OnSessionFailed(true);
            } else {
                OnRetryFailed();
            }
            return;
        }

        if (!PrepareNextPass()) {
            return;
        }

        CandidateIndex_ = 0;
        SinglePassCandidates_.clear();
        SinglePassIterationCount_ = 0;
        PeerAddressToThrottlingRate_.clear();

        RequestRows();
    }

    void OnSessionFailed(bool fatal) override
    {
        auto error = BuildCombinedError(TError(
            NChunkClient::EErrorCode::RowsLookupFailed,
            "Error looking up rows in chunk %v",
            ChunkId_));
        OnSessionFailed(fatal, error);
    }

    void OnSessionFailed(bool fatal, const TError& error) override
    {
        if (fatal) {
            SetReaderFailed();
        }

        Promise_.TrySet(error);
    }

    TClosure CreateRequestCallback()
    {
        return BIND(&TLookupRowsSession::DoRequestRows, MakeStrong(this))
            .Via(SessionInvoker_);
    }

    void RequestRows()
    {
        CreateRequestCallback().Run();
    }

    void DoRequestRows()
    {
        auto reader = Reader_.Lock();
        if (!reader || IsCanceled()) {
            return;
        }

        if (SinglePassCandidates_.empty()) {
            NProfiling::TWallTimer pickPeerTimer;
            SinglePassCandidates_ = PickPeerCandidates(
                reader,
                ReaderConfig_->LookupRequestPeerCount,
                /* enableEarlyExit */ false);
            if (SinglePassCandidates_.empty()) {
                OnPassCompleted();
                return;
            }

            if (EnablePeerProbing_) {
                AsyncProbeAndSelectBestPeers(SinglePassCandidates_, SinglePassCandidates_.size(), {})
                    .Subscribe(BIND(
                        [
                            =,
                            this_ = MakeStrong(this),
                            pickPeerTimer = std::move(pickPeerTimer)
                        ] (const TErrorOr<TPeerList>& result) {
                            VERIFY_INVOKER_AFFINITY(SessionInvoker_);

                            SessionOptions_.ChunkReaderStatistics->PickPeerWaitTime.fetch_add(
                                pickPeerTimer.GetElapsedValue(),
                                std::memory_order_relaxed);

                            SinglePassCandidates_ = result.ValueOrThrow();
                            if (SinglePassCandidates_.empty()) {
                                OnPassCompleted();
                            } else {
                                DoRequestRows();
                            }
                        }).Via(SessionInvoker_));
                return;
            }
        }

        if (SinglePassIterationCount_ == ReaderConfig_->SinglePassIterationLimitForLookup) {
            // Additional post-throttling at the end of each pass.
            auto delta = BytesToThrottle_;
            BytesToThrottle_ = 0;
            BytesThrottled_ += delta;
            AsyncThrottle(BandwidthThrottler_, delta, BIND(&TLookupRowsSession::OnPassCompleted, MakeStrong(this)));
            return;
        }

        std::optional<TPeer> chosenPeer;
        while (CandidateIndex_ < std::ssize(SinglePassCandidates_)) {
            chosenPeer = SinglePassCandidates_[CandidateIndex_];
            if (!IsPeerBanned(chosenPeer->Address)) {
                break;
            }

            SinglePassCandidates_.erase(SinglePassCandidates_.begin() + CandidateIndex_);
        }

        if (CandidateIndex_ == std::ssize(SinglePassCandidates_)) {
            YT_LOG_DEBUG("Lookup replication reader is out of peers for lookup, will sleep for a while "
                "(CandidateCount: %v, SinglePassIterationCount: %v)",
                SinglePassCandidates_.size(),
                SinglePassIterationCount_);

            CandidateIndex_ = 0;
            ++SinglePassIterationCount_;
            SortCandidates();

            // All candidates (in current pass) are either
            // waiting for schema from other tablet nodes or are throttling.
            // So it's better to sleep for a while.
            TDelayedExecutor::Submit(
                CreateRequestCallback(),
                ReaderConfig_->LookupSleepDuration);
            return;
        }

        YT_VERIFY(chosenPeer);
        const auto& peerAddress = chosenPeer->Address;

        if (IsCanceled()) {
            return;
        }

        WaitedForSchemaForTooLong_ = false;

        YT_LOG_DEBUG("Sending lookup request to peer "
            "(Address: %v, CandidateIndex: %v, CandidateCount: %v, IterationCount: %v)",
            peerAddress,
            CandidateIndex_,
            SinglePassCandidates_.size(),
            SinglePassIterationCount_);

        if (ShouldThrottle(peerAddress, BytesThrottled_ == 0 && BytesToThrottle_)) {
            // NB(psushin): This is preliminary throttling. The subsequent request may fail or return partial result.
            // In order not to throttle twice, we use BandwidthThrottled_ flag.
            // Still it protects us from bursty incoming traffic on the host.
            // If estimated size was not given, we fallback to post-throttling on actual received size.
            std::swap(BytesThrottled_, BytesToThrottle_);
            if (!BandwidthThrottler_->IsOverdraft()) {
                BandwidthThrottler_->Acquire(BytesThrottled_);
            } else {
                AsyncThrottle(BandwidthThrottler_, BytesThrottled_, BIND(&TLookupRowsSession::RequestRows, MakeStrong(this)));
                return;
            }
        }

        ++CandidateIndex_;

        auto channel = GetChannel(peerAddress);
        if (!channel) {
            RequestRows();
            return;
        }
        RequestRowsFromPeer(channel, reader, *chosenPeer, false);
    }

    void RequestRowsFromPeer(
        const IChannelPtr& channel,
        const TReplicationReaderPtr& reader,
        const TPeer& chosenPeer,
        bool sendSchema)
    {
        TDataNodeServiceProxy proxy(channel);
        proxy.SetDefaultTimeout(ReaderConfig_->LookupRpcTimeout);

        auto req = proxy.LookupRows();
        req->SetResponseHeavy(true);
        req->SetMultiplexingBand(SessionOptions_.MultiplexingBand);
        req->SetMultiplexingParallelism(SessionOptions_.MultiplexingParallelism);
        SetRequestWorkloadDescriptor(req, WorkloadDescriptor_);
        ToProto(req->mutable_chunk_id(), ChunkId_);
        ToProto(req->mutable_read_session_id(), ReadSessionId_);
        req->set_timestamp(Timestamp_);
        req->set_compression_codec(ToProto<int>(CodecId_));
        ToProto(req->mutable_column_filter(), ColumnFilter_);
        req->set_produce_all_versions(ProduceAllVersions_);
        req->set_override_timestamp(OverrideTimestamp_);
        req->set_populate_cache(true);

        // NB: By default if peer is throttling it will immediately fail,
        // but if we have to request the same throttling peer again,
        // then we should stop iterating and make it finish the request.
        bool isPeerThrottling = PeerAddressToThrottlingRate_.contains(chosenPeer.Address);
        req->set_reject_if_throttling(EnableRejectsIfThrottling_ && !isPeerThrottling);
        if (isPeerThrottling) {
            YT_LOG_DEBUG("Lookup replication reader sends request to throttling peer "
                "(Address: %v, CandidateIndex: %v, IterationCount: %v, ThrottlingRate: %v)",
                chosenPeer.Address,
                CandidateIndex_ - 1,
                SinglePassIterationCount_,
                PeerAddressToThrottlingRate_[chosenPeer.Address]);
        }

        auto schemaData = req->mutable_schema_data();
        ToProto(schemaData->mutable_table_id(), TableId_);
        schemaData->set_revision(Revision_);
        schemaData->set_schema_size(TableSchema_->GetMemoryUsage());
        if (sendSchema) {
            ToProto(schemaData->mutable_schema(), *TableSchema_);
        }

        req->Attachments() = Keyset_;

        // NB: Throttling on table schema (if any) will be performed on response.
        BytesToThrottle_ += GetByteSize(req->Attachments());

        NProfiling::TWallTimer dataWaitTimer;
        req->Invoke()
            .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TDataNodeServiceProxy::TErrorOrRspLookupRowsPtr& rspOrError) {
                this_->OnResponse(
                    std::move(rspOrError),
                    std::move(channel),
                    std::move(dataWaitTimer),
                    std::move(reader),
                    std::move(chosenPeer),
                    sendSchema);
            }).Via(SessionInvoker_));
    }

    void OnResponse(
        const TDataNodeServiceProxy::TErrorOrRspLookupRowsPtr& rspOrError,
        const IChannelPtr& channel,
        NProfiling::TWallTimer dataWaitTimer,
        const TReplicationReaderPtr& reader,
        const TPeer& chosenPeer,
        bool sentSchema)
    {
        SessionOptions_.ChunkReaderStatistics->DataWaitTime.fetch_add(
            dataWaitTimer.GetElapsedValue(),
            std::memory_order_relaxed);

        const auto& peerAddress = chosenPeer.Address;

        if (!rspOrError.IsOK()) {
            ProcessError(
                rspOrError,
                chosenPeer,
                TError(
                    "Error fetching rows from node %v",
                    peerAddress));

            YT_LOG_WARNING("Data node lookup request failed "
                "(Address: %v, PeerType: %v, CandidateIndex: %v, IterationCount: %v, BytesToThrottle: %v)",
                peerAddress,
                chosenPeer.Type,
                CandidateIndex_ - 1,
                SinglePassIterationCount_,
                BytesToThrottle_);

            RequestRows();
            return;
        }

        const auto& response = rspOrError.Value();

        SessionOptions_.ChunkReaderStatistics->DataBytesTransmitted.fetch_add(
            response->GetTotalSize(),
            std::memory_order_relaxed);

        reader->AccountTraffic(
            response->GetTotalSize(),
            chosenPeer.NodeDescriptor);

        if (response->has_request_schema() && response->request_schema()) {
            YT_VERIFY(!response->fetched_rows());
            YT_VERIFY(!sentSchema);
            YT_LOG_DEBUG("Sending schema upon data node request "
                "(Address: %v, PeerType: %v, CandidateIndex: %v, IterationCount: %v)",
                peerAddress,
                chosenPeer.Type,
                CandidateIndex_ - 1,
                SinglePassIterationCount_);

            RequestRowsFromPeer(channel, reader, chosenPeer, true);
            return;
        }

        if (response->net_throttling() || response->disk_throttling()) {
            double throttlingRate =
                ReaderConfig_->NetQueueSizeFactor * response->net_queue_size() +
                ReaderConfig_->DiskQueueSizeFactor * response->disk_queue_size();
            YT_LOG_DEBUG("Peer is throttling on lookup (Address: %v, "
                "NetThrottling: %v, NetQueueSize: %v, DiskThrottling: %v, DiskQueueSize: %v, "
                "CandidateIndex: %v, IterationCount: %v, ThrottlingRate: %v)",
                peerAddress,
                response->net_throttling(),
                response->net_queue_size(),
                response->disk_throttling(),
                response->disk_queue_size(),
                CandidateIndex_ - 1,
                SinglePassIterationCount_,
                throttlingRate);
            YT_VERIFY(response->net_queue_size() > 0 || response->disk_queue_size());

            PeerAddressToThrottlingRate_[peerAddress] = throttlingRate;
        } else if (PeerAddressToThrottlingRate_.contains(peerAddress)) {
            PeerAddressToThrottlingRate_.erase(peerAddress);
        }

        if (!response->fetched_rows()) {
            if (response->rejected_due_to_throttling()) {
                YT_LOG_DEBUG("Peer rejected to execute request due to throttling (Address: %v)",
                    peerAddress);
                YT_VERIFY(response->net_throttling() || response->disk_throttling());
            } else {
                // NB(akozhikhov): If data node waits for schema from other tablet node,
                // then we switch to next data node in order to warm up as many schema caches as possible.
                YT_LOG_DEBUG("Data node is waiting for schema from other tablet "
                    "(Address: %v, PeerType: %v, CandidateIndex: %v, IterationCount: %v)",
                    peerAddress,
                    chosenPeer.Type,
                    CandidateIndex_ - 1,
                    SinglePassIterationCount_);

                WaitedForSchemaForTooLong_ = true;
            }

            RequestRows();
            return;
        }

        if (response->has_chunk_reader_statistics()) {
            UpdateFromProto(&SessionOptions_.ChunkReaderStatistics, response->chunk_reader_statistics());
        }

        ProcessAttachedVersionedRowset(response);

        if (ShouldThrottle(peerAddress, BytesToThrottle_ > 0)) {
            BytesThrottled_ += BytesToThrottle_;
            BandwidthThrottler_->Acquire(BytesToThrottle_);
            BytesToThrottle_ = 0;
        }

        OnSessionSucceeded();
    }

    void OnSessionSucceeded()
    {
        YT_LOG_DEBUG("Finished processing rows response "
            "(BytesThrottled: %v, CandidateIndex: %v, IterationCount: %v)",
            BytesThrottled_,
            CandidateIndex_ - 1,
            SinglePassIterationCount_);
        Promise_.TrySet(FetchedRowset_);
    }

    template <class TRspPtr>
    void ProcessAttachedVersionedRowset(const TRspPtr& response)
    {
        YT_VERIFY(!FetchedRowset_);
        FetchedRowset_ = response->Attachments()[0];
        TotalBytesReceived_ += FetchedRowset_.Size();
        BytesToThrottle_ += FetchedRowset_.Size();
    }

    // Sort is called after iterating over all candidates.
    void SortCandidates()
    {
        SortBy(SinglePassCandidates_, [&] (const TPeer& candidate) {
            auto throttlingRate = PeerAddressToThrottlingRate_.find(candidate.Address);
            return throttlingRate != PeerAddressToThrottlingRate_.end() ? throttlingRate->second : 0.;
        });
    }

    bool UpdatePeerBlockMap(const TPeerProbeResult& probeResult) override
    {
        if (probeResult.AllyReplicas) {
            MaybeUpdateSeeds(probeResult.AllyReplicas);
        }

        return false;
    }

    void OnCanceled(const TError& error) override
    {
        auto wrappedError = TError(NYT::EErrorCode::Canceled, "LookupRows session canceled")
            << error;
        TSessionBase::OnCanceled(wrappedError);
        Promise_.TrySet(wrappedError);
    }
};

TFuture<TSharedRef> TReplicationReader::LookupRows(
    const TClientChunkReadOptions& options,
    TSharedRange<TLegacyKey> lookupKeys,
    TObjectId tableId,
    TRevision revision,
    TTableSchemaPtr tableSchema,
    std::optional<i64> estimatedSize,
    const TColumnFilter& columnFilter,
    TTimestamp timestamp,
    NCompression::ECodec codecId,
    bool produceAllVersions,
    TTimestamp overrideTimestamp,
    bool enablePeerProbing,
    bool enableRejectsIfThrottling)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto session = New<TLookupRowsSession>(
        this,
        options,
        std::move(lookupKeys),
        tableId,
        revision,
        std::move(tableSchema),
        estimatedSize,
        columnFilter,
        timestamp,
        codecId,
        produceAllVersions,
        overrideTimestamp,
        enablePeerProbing,
        enableRejectsIfThrottling,
        BandwidthThrottler_,
        RpsThrottler_);
    return session->Run();
}

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TReplicationReaderWithOverridenThrottlers)

class TReplicationReaderWithOverridenThrottlers
    : public IChunkReaderAllowingRepair
    , public ILookupReader
{
public:
    TReplicationReaderWithOverridenThrottlers(
        TReplicationReaderPtr underlyingReader,
        IThroughputThrottlerPtr bandwidthThrottler,
        IThroughputThrottlerPtr rpsThrottler)
        : UnderlyingReader_(std::move(underlyingReader))
        , BandwidthThrottler_(std::move(bandwidthThrottler))
        , RpsThrottler_(std::move(rpsThrottler))
    { }

    TFuture<std::vector<TBlock>> ReadBlocks(
        const TClientChunkReadOptions& options,
        const std::vector<int>& blockIndexes,
        std::optional<i64> estimatedSize) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        if (blockIndexes.empty()) {
            return MakeFuture<std::vector<TBlock>>({});
        }

        auto session = New<TReadBlockSetSession>(
            UnderlyingReader_.Get(),
            options,
            blockIndexes,
            estimatedSize,
            BandwidthThrottler_,
            RpsThrottler_);
        return session->Run();
    }

    TFuture<std::vector<TBlock>> ReadBlocks(
        const TClientChunkReadOptions& options,
        int firstBlockIndex,
        int blockCount,
        std::optional<i64> estimatedSize) override
    {
        VERIFY_THREAD_AFFINITY_ANY();
        YT_VERIFY(blockCount >= 0);

        if (blockCount == 0) {
            return MakeFuture<std::vector<TBlock>>({});
        }

        auto session = New<TReadBlockRangeSession>(
            UnderlyingReader_.Get(),
            options,
            firstBlockIndex,
            blockCount,
            estimatedSize,
            BandwidthThrottler_,
            RpsThrottler_);
        return session->Run();
    }

    TFuture<TRefCountedChunkMetaPtr> GetMeta(
        const TClientChunkReadOptions& options,
        std::optional<int> partitionTag,
        const std::optional<std::vector<int>>& extensionTags) override
    {
        // NB: No network throttling is applied within GetMeta request.
        return UnderlyingReader_->GetMeta(options, partitionTag, extensionTags);
    }

    TFuture<TSharedRef> LookupRows(
        const TClientChunkReadOptions& options,
        TSharedRange<TLegacyKey> lookupKeys,
        TObjectId tableId,
        TRevision revision,
        TTableSchemaPtr tableSchema,
        std::optional<i64> estimatedSize,
        const TColumnFilter& columnFilter,
        TTimestamp timestamp,
        NCompression::ECodec codecId,
        bool produceAllVersions,
        TTimestamp overrideTimestamp,
        bool enablePeerProbing,
        bool enableRejectsIfThrottling) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto session = New<TLookupRowsSession>(
            UnderlyingReader_.Get(),
            options,
            std::move(lookupKeys),
            tableId,
            revision,
            std::move(tableSchema),
            estimatedSize,
            columnFilter,
            timestamp,
            codecId,
            produceAllVersions,
            overrideTimestamp,
            enablePeerProbing,
            enableRejectsIfThrottling,
            BandwidthThrottler_,
            RpsThrottler_);
        return session->Run();
    }

    TChunkId GetChunkId() const override
    {
        return UnderlyingReader_->GetChunkId();
    }

    TInstant GetLastFailureTime() const override
    {
        return UnderlyingReader_->GetLastFailureTime();
    }

    void SetSlownessChecker(TCallback<TError(i64, TDuration)> slownessChecker) override
    {
        UnderlyingReader_->SetSlownessChecker(std::move(slownessChecker));
    }

private:
    const TReplicationReaderPtr UnderlyingReader_;

    const IThroughputThrottlerPtr BandwidthThrottler_;
    const IThroughputThrottlerPtr RpsThrottler_;
};

DEFINE_REFCOUNTED_TYPE(TReplicationReaderWithOverridenThrottlers)

////////////////////////////////////////////////////////////////////////////////

} // namespace

////////////////////////////////////////////////////////////////////////////////

IChunkReaderAllowingRepairPtr CreateReplicationReader(
    TReplicationReaderConfigPtr config,
    TRemoteReaderOptionsPtr options,
    TChunkReaderHostPtr chunkReaderHost,
    TChunkId chunkId,
    const TChunkReplicaList& seedReplicas)
{
    return New<TReplicationReader>(
        std::move(config),
        std::move(options),
        std::move(chunkReaderHost),
        chunkId,
        seedReplicas);
}

////////////////////////////////////////////////////////////////////////////////

IChunkReaderAllowingRepairPtr CreateReplicationReaderThrottlingAdapter(
    const IChunkReaderPtr& underlyingReader,
    IThroughputThrottlerPtr bandwidthThrottler,
    IThroughputThrottlerPtr rpsThrottler)
{
    auto* underlyingReplicationReader = dynamic_cast<TReplicationReader*>(underlyingReader.Get());
    YT_VERIFY(underlyingReplicationReader);

    return New<TReplicationReaderWithOverridenThrottlers>(
        underlyingReplicationReader,
        std::move(bandwidthThrottler),
        std::move(rpsThrottler));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
