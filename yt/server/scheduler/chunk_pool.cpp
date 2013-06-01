#include "stdafx.h"
#include "chunk_pool.h"
#include "private.h"

#include <ytlib/misc/id_generator.h>
#include <ytlib/misc/property.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <ytlib/chunk_client/chunk_spec.h>
#include <ytlib/table_client/chunk_meta_extensions.h>

namespace NYT {
namespace NScheduler {

using namespace NNodeTrackerClient;
using namespace NChunkServer;
using namespace NChunkClient;
using namespace NChunkClient::NProto;

////////////////////////////////////////////////////////////////////

void AddStripeToList(
    const TChunkStripePtr& stripe,
    const TNodeDirectoryPtr& nodeDirectory,
    i64 stripeDataSize,
    i64 stripeRowCount,
    const TChunkStripeListPtr& list,
    const TNullable<Stroka>& address)
{
    list->Stripes.push_back(stripe);
    list->TotalDataSize += stripeDataSize;
    list->TotalRowCount += stripeRowCount;

    list->TotalChunkCount += stripe->ChunkSlices.size();
    if (address) {
        FOREACH (const auto& chunkSlice, stripe->ChunkSlices) {
            bool isLocal = false;
            FOREACH (ui32 protoReplica, chunkSlice->GetChunkSpec()->replicas()) {
                auto replica = FromProto<NChunkClient::TChunkReplica>(protoReplica);
                const auto& descriptor = nodeDirectory->GetDescriptor(replica);
                if (descriptor.Address == *address && chunkSlice->GetLocality(replica.GetIndex()) > 0) {
                    isLocal = true;
                    break;
                }
            }
            if (isLocal) {
                ++list->LocalChunkCount;
            } else {
                ++list->NonLocalChunkCount;
            }
        }
    } else {
        list->NonLocalChunkCount += stripe->ChunkSlices.size();
    }
}

////////////////////////////////////////////////////////////////////

TChunkStripe::TChunkStripe()
    : WaitingChunkCount(0)
{ }

TChunkStripe::TChunkStripe(TChunkSlicePtr chunkSlice)
    : WaitingChunkCount(0)
{
    ChunkSlices.push_back(chunkSlice);
}

TChunkStripe::TChunkStripe(const TChunkStripe& other)
{
    FOREACH (const auto& chunkSlice, other.ChunkSlices) {
        ChunkSlices.push_back(New<TChunkSlice>(*chunkSlice));
    }
}

TChunkStripeStatistics TChunkStripe::GetStatistics() const
{
    TChunkStripeStatistics result;

    FOREACH (const auto& chunkSlice, ChunkSlices) {
        result.DataSize += chunkSlice->GetDataSize();
        result.RowCount += chunkSlice->GetRowCount();
        ++result.ChunkCount;
        result.MaxBlockSize = std::max(result.MaxBlockSize, chunkSlice->GetMaxBlockSize());
    }

    return result;
}

TChunkStripeStatistics operator + (
    const TChunkStripeStatistics& lhs,
    const TChunkStripeStatistics& rhs)
{
    TChunkStripeStatistics result;
    result.ChunkCount = lhs.ChunkCount + rhs.ChunkCount;
    result.DataSize = lhs.DataSize + rhs.DataSize;
    result.RowCount = lhs.RowCount + rhs.RowCount;
    result.MaxBlockSize = std::max(lhs.MaxBlockSize, rhs.MaxBlockSize);
    return result;
}

TChunkStripeStatistics& operator += (
    TChunkStripeStatistics& lhs,
    const TChunkStripeStatistics& rhs)
{
    lhs.ChunkCount += rhs.ChunkCount;
    lhs.DataSize += rhs.DataSize;
    lhs.RowCount += rhs.RowCount;
    lhs.MaxBlockSize = std::max(lhs.MaxBlockSize, rhs.MaxBlockSize);
    return lhs;
}

TChunkStripeStatisticsVector AggregateStatistics(
    const TChunkStripeStatisticsVector& statistics)
{
    TChunkStripeStatistics sum;
    FOREACH (const auto& stat, statistics) {
        sum += stat;
    }
    return TChunkStripeStatisticsVector(1, sum);
}

////////////////////////////////////////////////////////////////////

TChunkStripeList::TChunkStripeList()
    : IsApproximate(false)
    , TotalDataSize(0)
    , TotalRowCount(0)
    , TotalChunkCount(0)
    , LocalChunkCount(0)
    , NonLocalChunkCount(0)
{ }

TChunkStripeStatisticsVector TChunkStripeList::GetStatistics() const
{
    TChunkStripeStatisticsVector result;
    result.reserve(Stripes.size());
    FOREACH (const auto& stripe, Stripes) {
        result.push_back(stripe->GetStatistics());
    }
    return result;
}

TChunkStripeStatistics TChunkStripeList::GetAggregateStatistics() const
{
    TChunkStripeStatistics result;
    result.ChunkCount = TotalChunkCount;
    if (IsApproximate) {
        result.RowCount = TotalRowCount * ApproximateSizesBoostFactor;
        result.DataSize = TotalDataSize * ApproximateSizesBoostFactor;
    } else {
        result.RowCount = TotalRowCount;
        result.DataSize = TotalDataSize;
    }
    return result;
}

////////////////////////////////////////////////////////////////////

class TChunkPoolInputBase
    : public virtual IChunkPoolInput
{
public:
    explicit TChunkPoolInputBase(TNodeDirectoryPtr nodeDirectory)
        : NodeDirectory(nodeDirectory)
        , Finished(false)
    { }

    // IChunkPoolInput implementation.

    virtual void Finish() override
    {
        Finished = true;
    }

protected:
    TNodeDirectoryPtr NodeDirectory;
    bool Finished;

};

////////////////////////////////////////////////////////////////////

class TSuspendableStripe
{
    DEFINE_BYVAL_RW_PROPERTY(IChunkPoolOutput::TCookie, ExtractedCookie);

public:
    TSuspendableStripe()
        : ExtractedCookie_(IChunkPoolOutput::NullCookie)
        , Suspended(false)
    { }

    explicit TSuspendableStripe(TChunkStripePtr stripe)
        : ExtractedCookie_(IChunkPoolOutput::NullCookie)
        , Stripe(std::move(stripe))
        , Suspended(false)
        , Statistics(Stripe->GetStatistics())
    { }

    const TChunkStripePtr& GetStripe() const
    {
        return Stripe;
    }

    const TChunkStripeStatistics& GetStatistics() const
    {
        return Statistics;
    }

    void Suspend()
    {
        YCHECK(Stripe);
        YCHECK(!Suspended);

        Suspended = true;
    }

    bool IsSuspended() const
    {
        return Suspended;
    }

    void Resume(TChunkStripePtr stripe)
    {
        YCHECK(Stripe);
        YCHECK(Suspended);

        // NB: do not update statistics on resume to preserve counters.
        Suspended = false;
        Stripe = stripe;
    }

private:
    TChunkStripePtr Stripe;
    bool Suspended;
    TChunkStripeStatistics Statistics;

};

////////////////////////////////////////////////////////////////////

class TChunkPoolOutputBase
    : public virtual IChunkPoolOutput
{
public:
    TChunkPoolOutputBase()
        : DataSizeCounter(0)
        , RowCounter(0)
    { }

    virtual i64 GetTotalDataSize() const override
    {
        return DataSizeCounter.GetTotal();
    }

    virtual i64 GetRunningDataSize() const override
    {
        return DataSizeCounter.GetRunning();
    }

    virtual i64 GetCompletedDataSize() const override
    {
        return DataSizeCounter.GetCompleted();
    }

    virtual i64 GetPendingDataSize() const override
    {
        return DataSizeCounter.GetPending();
    }

    virtual i64 GetTotalRowCount() const override
    {
        return RowCounter.GetTotal();
    }

protected:
    TProgressCounter DataSizeCounter;
    TProgressCounter RowCounter;
    TProgressCounter JobCounter;

};

////////////////////////////////////////////////////////////////////

class TAtomicChunkPool
    : public TChunkPoolInputBase
    , public TChunkPoolOutputBase
    , public IChunkPool
{
public:
    // IChunkPoolInput implementation.

    explicit TAtomicChunkPool(TNodeDirectoryPtr nodeDirectory)
        : TChunkPoolInputBase(nodeDirectory)
        , SuspendedStripeCount(0)
    {
        JobCounter.Set(1);
    }

    virtual IChunkPoolInput::TCookie Add(TChunkStripePtr stripe) override
    {
        YCHECK(!Finished);
        YCHECK(!ExtractedList);

        auto cookie = static_cast<int>(Stripes.size());

        TSuspendableStripe suspendableStripe(stripe);
        Stripes.push_back(suspendableStripe);

        DataSizeCounter.Increment(suspendableStripe.GetStatistics().DataSize);
        RowCounter.Increment(suspendableStripe.GetStatistics().RowCount);

        UpdateLocality(stripe, +1);

        return cookie;
    }

    virtual void Suspend(IChunkPoolInput::TCookie cookie) override
    {
        ++SuspendedStripeCount;
        auto& suspendableStripe = Stripes[cookie];
        Stripes[cookie].Suspend();
        UpdateLocality(suspendableStripe.GetStripe(), -1);
    }

    virtual void Resume(IChunkPoolInput::TCookie cookie, TChunkStripePtr stripe) override
    {
        auto& suspendableStripe = Stripes[cookie];
        suspendableStripe.Resume(stripe);
        --SuspendedStripeCount;
        UpdateLocality(suspendableStripe.GetStripe(), +1);
    }

    // IChunkPoolOutput implementation.

    virtual TChunkStripeStatisticsVector GetApproximateStripeStatistics() const override
    {
        TChunkStripeStatisticsVector result;
        result.reserve(Stripes.size());
        FOREACH (const auto& suspendableStripe, Stripes) {
            auto stripe = suspendableStripe.GetStripe();
            result.push_back(stripe->GetStatistics());
        }
        return result;
    }

    virtual bool IsCompleted() const override
    {
        return Finished &&
               JobCounter.GetCompleted() == 1;
    }

    virtual int GetTotalJobCount() const override
    {
        return 1;
    }

    virtual int GetPendingJobCount() const override
    {
        return
            Finished &&
            SuspendedStripeCount == 0 &&
            JobCounter.GetPending() == 1
            ? 1 : 0;
    }

    virtual i64 GetLocality(const Stroka& address) const override
    {
        if (ExtractedList) {
            return 0;
        }

        auto it = AddressToLocality.find(address);
        return it == AddressToLocality.end() ? 0 : it->second;
    }

    virtual IChunkPoolOutput::TCookie Extract(const Stroka& address) override
    {
        YCHECK(Finished);
        YCHECK(SuspendedStripeCount == 0);

        if (GetPendingJobCount() == 0) {
            return IChunkPoolOutput::NullCookie;
        }

        ExtractedList = New<TChunkStripeList>();
        FOREACH (const auto& suspendableStripe, Stripes) {
            auto stripe = suspendableStripe.GetStripe();
            auto stat = stripe->GetStatistics();
            AddStripeToList(stripe, NodeDirectory, stat.DataSize, stat.RowCount, ExtractedList, address);
        }

        JobCounter.Start(1);
        DataSizeCounter.Start(DataSizeCounter.GetTotal());
        RowCounter.Start(RowCounter.GetTotal());

        return 0;
    }

    virtual TChunkStripeListPtr GetStripeList(IChunkPoolOutput::TCookie cookie) override
    {
        YCHECK(cookie == 0);
        YCHECK(ExtractedList);
        YCHECK(Finished);

        return ExtractedList;
    }

    virtual void Completed(IChunkPoolOutput::TCookie cookie) override
    {
        YCHECK(cookie == 0);
        YCHECK(ExtractedList);
        YCHECK(Finished);

        JobCounter.Completed(1);
        DataSizeCounter.Completed(DataSizeCounter.GetTotal());
        RowCounter.Completed(RowCounter.GetTotal());

        ExtractedList = nullptr;
    }

    virtual void Failed(IChunkPoolOutput::TCookie cookie) override
    {
        YCHECK(cookie == 0);
        YCHECK(ExtractedList);
        YCHECK(Finished);

        JobCounter.Failed(1);
        DataSizeCounter.Failed(DataSizeCounter.GetTotal());
        RowCounter.Failed(RowCounter.GetTotal());

        ExtractedList = nullptr;
    }

    virtual void Aborted(IChunkPoolOutput::TCookie cookie) override
    {
        YCHECK(cookie == 0);
        YCHECK(ExtractedList);
        YCHECK(Finished);

        JobCounter.Aborted(1);
        DataSizeCounter.Aborted(DataSizeCounter.GetTotal());
        RowCounter.Aborted(RowCounter.GetTotal());

        ExtractedList = nullptr;
    }

    virtual void Lost(IChunkPoolOutput::TCookie cookie) override
    {
        YCHECK(cookie == 0);
        YCHECK(!ExtractedList);
        YCHECK(Finished);

        JobCounter.Lost(1);
        DataSizeCounter.Lost(DataSizeCounter.GetTotal());
        RowCounter.Lost(RowCounter.GetTotal());
    }

private:
    std::vector<TSuspendableStripe> Stripes;

    yhash_map<Stroka, i64> AddressToLocality;
    TChunkStripeListPtr ExtractedList;

    int SuspendedStripeCount;

    void UpdateLocality(TChunkStripePtr stripe, int delta)
    {
        FOREACH (const auto& chunkSlice, stripe->ChunkSlices) {
            FOREACH (ui32 protoReplica, chunkSlice->GetChunkSpec()->replicas()) {
                auto replica = FromProto<NChunkClient::TChunkReplica>(protoReplica);
                const auto& descriptor = NodeDirectory->GetDescriptor(replica);
                i64 localityDelta = chunkSlice->GetLocality(replica.GetIndex()) * delta;
                AddressToLocality[descriptor.Address] += localityDelta;
            }
        }
    }

};

std::unique_ptr<IChunkPool> CreateAtomicChunkPool(TNodeDirectoryPtr nodeDirectory)
{
    return std::unique_ptr<IChunkPool>(new TAtomicChunkPool(nodeDirectory));
}

////////////////////////////////////////////////////////////////////

class TUnorderedChunkPool
    : public TChunkPoolInputBase
    , public TChunkPoolOutputBase
    , public IChunkPool
{
public:
    explicit TUnorderedChunkPool(
        TNodeDirectoryPtr nodeDirectory,
        int jobCount)
        : TChunkPoolInputBase(nodeDirectory)
        , SuspendedStripeCount(0)
        , UnavailableLostCookieCount(0)
    {
        JobCounter.Set(jobCount);
    }

    // IChunkPoolInput implementation.

    virtual IChunkPoolInput::TCookie Add(TChunkStripePtr stripe) override
    {
        YCHECK(!Finished);

        auto cookie = Stripes.size();

        TSuspendableStripe suspendableStripe(stripe);
        Stripes.push_back(suspendableStripe);

        DataSizeCounter.Increment(suspendableStripe.GetStatistics().DataSize);
        RowCounter.Increment(suspendableStripe.GetStatistics().RowCount);

        Register(cookie);

        return cookie;
    }

    virtual void Suspend(IChunkPoolInput::TCookie cookie) override
    {
        auto& suspendableStripe = Stripes[cookie];
        suspendableStripe.Suspend();

        auto outputCookie = suspendableStripe.GetExtractedCookie();
        if (outputCookie == IChunkPoolOutput::NullCookie) {
            Unregister(cookie);
            ++SuspendedStripeCount;
        } else {
            auto it = ExtractedLists.find(outputCookie);
            YCHECK(it != ExtractedLists.end());
            auto& extractedStripeList = it->second;

            if (LostCookies.find(outputCookie) != LostCookies.end() &&
                extractedStripeList.UnavailableStripeCount == 0)
            {
                ++UnavailableLostCookieCount;
            }
            ++extractedStripeList.UnavailableStripeCount;
        }
    }

    virtual void Resume(IChunkPoolInput::TCookie cookie, TChunkStripePtr stripe) override
    {
        auto& suspendableStripe = Stripes[cookie];
        suspendableStripe.Resume(stripe);

        auto outputCookie = suspendableStripe.GetExtractedCookie();
        if (outputCookie == IChunkPoolOutput::NullCookie) {
            Register(cookie);
            --SuspendedStripeCount;
        } else {
            auto it = ExtractedLists.find(outputCookie);
            YCHECK(it != ExtractedLists.end());
            auto& extractedStripeList = it->second;
            --extractedStripeList.UnavailableStripeCount;

            if (LostCookies.find(outputCookie) != LostCookies.end() &&
                extractedStripeList.UnavailableStripeCount == 0)
            {
                --UnavailableLostCookieCount;
            }
        }
    }

    // IChunkPoolOutput implementation.

    virtual bool IsCompleted() const override
    {
        return Finished &&
               LostCookies.empty() &&
               SuspendedStripeCount == 0 &&
               PendingGlobalStripes.empty() &&
               JobCounter.GetRunning() == 0;
    }

    virtual int GetTotalJobCount() const override
    {
        return IsCompleted() ? JobCounter.GetCompleted() : JobCounter.GetTotal();
    }

    virtual int GetPendingJobCount() const override
    {
        // NB: Pending data size can be zero while JobCounter indicates
        // that some jobs are pending. This may happen due to unevenness
        // of workload partitioning and cause the task to start less jobs than
        // suggested.

        return (PendingGlobalStripes.empty() && UnavailableLostCookieCount == LostCookies.size())
            ? 0
            : JobCounter.GetPending();
    }

    virtual TChunkStripeStatisticsVector GetApproximateStripeStatistics() const override
    {
        if (!ExtractedLists.empty()) {
            TChunkStripeStatisticsVector result;
            FOREACH(const auto& index, ExtractedLists.begin()->second.StripeIndexes) {
                result.push_back(Stripes[index].GetStripe()->GetStatistics());
            }
            return result;
        }

        TChunkStripeStatistics stat;
        // Typically unordered pool has one chunk per stripe.
        // NB: Cannot estimate MaxBlockSize to fill stat field here.
        stat.ChunkCount = std::max(
            static_cast<i64>(1),
            static_cast<i64>(PendingGlobalStripes.size()) / GetPendingJobCount());
        stat.DataSize = std::max(
            static_cast<i64>(1),
            GetPendingDataSize() / GetPendingJobCount());
        stat.RowCount = std::max(
            static_cast<i64>(1),
            GetTotalRowCount() / GetTotalJobCount());

        TChunkStripeStatisticsVector result;
        result.push_back(stat);
        return result;
    }

    virtual i64 GetLocality(const Stroka& address) const override
    {
        auto it = PendingLocalChunks.find(address);
        return it == PendingLocalChunks.end() ? 0 : it->second.Locality;
    }

    virtual IChunkPoolOutput::TCookie Extract(const Stroka& address) override
    {
        YCHECK(Finished);

        if (GetPendingJobCount() == 0) {
            return IChunkPoolOutput::NullCookie;
        }

        TChunkStripeListPtr list;
        IChunkPoolOutput::TCookie cookie;

        if (LostCookies.size() == UnavailableLostCookieCount) {
            cookie = OutputCookieGenerator.Next();
            auto pair = ExtractedLists.insert(std::make_pair(cookie, TExtractedStripeList()));
            YCHECK(pair.second);
            auto& extractedStripeList = pair.first->second;

            list = New<TChunkStripeList>();
            extractedStripeList.StripeList = list;

            // Take local chunks first.
            auto it = PendingLocalChunks.find(address);
            if (it != PendingLocalChunks.end()) {
                const auto& entry = it->second;
                AddAndUnregisterStripes(
                    extractedStripeList,
                    cookie,
                    entry.StripeIndexes.begin(),
                    entry.StripeIndexes.end(),
                    address);
            }

            // Take non-local chunks.
            AddAndUnregisterStripes(
                extractedStripeList,
                cookie,
                PendingGlobalStripes.begin(),
                PendingGlobalStripes.end(),
                address);
        } else {
            auto lostIt = LostCookies.begin();
            while (true) {
                cookie = *lostIt;
                auto it = ExtractedLists.find(cookie);
                YCHECK(it != ExtractedLists.end());
                if (it->second.UnavailableStripeCount == 0) {
                    LostCookies.erase(lostIt);
                    YCHECK(ReplayCookies.insert(cookie).second);
                    list = GetStripeList(cookie);
                    break;
                }
                YCHECK(++lostIt != LostCookies.end());
            }
        }

        JobCounter.Start(1);
        DataSizeCounter.Start(list->TotalDataSize);
        RowCounter.Start(list->TotalRowCount);

        return cookie;
    }

    virtual TChunkStripeListPtr GetStripeList(IChunkPoolOutput::TCookie cookie) override
    {
        auto it = ExtractedLists.find(cookie);
        YCHECK(it != ExtractedLists.end());
        return it->second.StripeList;
    }

    virtual void Completed(IChunkPoolOutput::TCookie cookie) override
    {
        auto list = GetStripeList(cookie);

        JobCounter.Completed(1);
        DataSizeCounter.Completed(list->TotalDataSize);
        RowCounter.Completed(list->TotalRowCount);

        // NB: may fail.
        ReplayCookies.erase(cookie);
    }

    virtual void Failed(IChunkPoolOutput::TCookie cookie) override
    {
        auto it = ExtractedLists.find(cookie);
        YCHECK(it != ExtractedLists.end());
        auto& extractedStripeList = it->second;
        auto list = extractedStripeList.StripeList;

        JobCounter.Failed(1);
        DataSizeCounter.Failed(list->TotalDataSize);
        RowCounter.Failed(list->TotalRowCount);

        ReinstallStripeList(extractedStripeList, cookie);
    }

    virtual void Aborted(IChunkPoolOutput::TCookie cookie) override
    {
        auto it = ExtractedLists.find(cookie);
        YCHECK(it != ExtractedLists.end());
        auto& extractedStripeList = it->second;
        auto list = extractedStripeList.StripeList;

        JobCounter.Aborted(1);
        DataSizeCounter.Aborted(list->TotalDataSize);
        RowCounter.Aborted(list->TotalRowCount);

        ReinstallStripeList(extractedStripeList, cookie);
    }

    virtual void Lost(IChunkPoolOutput::TCookie cookie) override
    {
        auto it = ExtractedLists.find(cookie);
        YCHECK(it != ExtractedLists.end());
        auto& extractedStripeList = it->second;
        auto list = extractedStripeList.StripeList;

        // No need to respect locality for restarted jobs.
        list->NonLocalChunkCount += list->LocalChunkCount;
        list->LocalChunkCount = 0;

        JobCounter.Lost(1);
        DataSizeCounter.Lost(list->TotalDataSize);
        RowCounter.Lost(list->TotalRowCount);

        YCHECK(LostCookies.insert(cookie).second);
        if (extractedStripeList.UnavailableStripeCount > 0) {
            ++UnavailableLostCookieCount;
        }
    }

private:
    std::vector<TSuspendableStripe> Stripes;

    //! Indexes in #Stripes.
    yhash_set<int> PendingGlobalStripes;

    i64 SuspendedStripeCount;
    int UnavailableLostCookieCount;

    struct TLocalityEntry
    {
        TLocalityEntry()
            : Locality(0)
        { }

        //! The total locality associated with this address.
        i64 Locality;

        //! Indexes in #Stripes.
        yhash_set<int> StripeIndexes;
    };

    struct TExtractedStripeList
    {
        TExtractedStripeList()
            : UnavailableStripeCount(0)
        { }

        int UnavailableStripeCount;
        std::vector<int> StripeIndexes;
        TChunkStripeListPtr StripeList;
    };

    yhash_map<Stroka, TLocalityEntry> PendingLocalChunks;

    TIdGenerator OutputCookieGenerator;

    yhash_map<IChunkPoolOutput::TCookie, TExtractedStripeList> ExtractedLists;

    yhash_set<IChunkPoolOutput::TCookie> LostCookies;
    yhash_set<IChunkPoolOutput::TCookie> ReplayCookies;


    void Register(int stripeIndex)
    {
        auto& suspendableStripe = Stripes[stripeIndex];
        YCHECK(suspendableStripe.GetExtractedCookie() == IChunkPoolOutput::NullCookie);

        auto stripe = suspendableStripe.GetStripe();
        FOREACH (const auto& chunkSlice, stripe->ChunkSlices) {
            FOREACH (ui32 protoReplica, chunkSlice->GetChunkSpec()->replicas()) {
                auto replica = FromProto<NChunkClient::TChunkReplica>(protoReplica);

                auto locality = chunkSlice->GetLocality(replica.GetIndex());
                if (locality > 0) {
                    const auto& descriptor = NodeDirectory->GetDescriptor(replica);
                    auto& entry = PendingLocalChunks[descriptor.Address];
                    // NB: do not check that stripe is unique, it may have already been inserted,
                    // since different replicas may reside on the same node during rebalancing.
                    entry.StripeIndexes.insert(stripeIndex);
                    entry.Locality += locality;
                }
            }
        }

        YCHECK(PendingGlobalStripes.insert(stripeIndex).second);
    }

    void Unregister(int stripeIndex)
    {
        auto& suspendableStripe = Stripes[stripeIndex];

        auto stripe = suspendableStripe.GetStripe();
        FOREACH (const auto& chunkSlice, stripe->ChunkSlices) {
            FOREACH (ui32 protoReplica, chunkSlice->GetChunkSpec()->replicas()) {
                auto replica = FromProto<NChunkClient::TChunkReplica>(protoReplica);
                auto locality = chunkSlice->GetLocality(replica.GetIndex());
                if (locality > 0) {
                    const auto& descriptor = NodeDirectory->GetDescriptor(replica);
                    auto& entry = PendingLocalChunks[descriptor.Address];
                    auto it = entry.StripeIndexes.find(stripeIndex);
                    if (it != entry.StripeIndexes.end()) {
                        entry.StripeIndexes.erase(it);
                    }
                    entry.Locality -= locality;
                }
            }
        }

        YCHECK(PendingGlobalStripes.erase(stripeIndex) == 1);
    }

    template <class TIterator>
    void AddAndUnregisterStripes(
        TExtractedStripeList& extractedStripeList,
        IChunkPoolOutput::TCookie cookie,
        const TIterator& begin,
        const TIterator& end,
        const Stroka& address)
    {
        i64 idealDataSizePerJob = std::max(static_cast<i64>(1), DataSizeCounter.GetPending() / JobCounter.GetPending());

        auto& list = extractedStripeList.StripeList;
        size_t oldSize = list->Stripes.size();
        for (auto it = begin; it != end && list->TotalDataSize < idealDataSizePerJob; ++it) {
            auto stripeIndex = *it;
            extractedStripeList.StripeIndexes.push_back(stripeIndex);

            auto& suspendableStripe = Stripes[stripeIndex];
            suspendableStripe.SetExtractedCookie(cookie);
            auto stat = suspendableStripe.GetStatistics();
            AddStripeToList(
                suspendableStripe.GetStripe(),
                NodeDirectory,
                stat.DataSize,
                stat.RowCount,
                list,
                address);
        }
        size_t newSize = list->Stripes.size();

        for (size_t index = oldSize; index < newSize; ++index) {
            Unregister(extractedStripeList.StripeIndexes[index]);
        }
    }

    void ReinstallStripeList(const TExtractedStripeList& extractedStripeList, IChunkPoolOutput::TCookie cookie)
    {
        auto replayIt = ReplayCookies.find(cookie);
        if (replayIt == ReplayCookies.end()) {
            FOREACH (const auto& stripeIndex, extractedStripeList.StripeIndexes) {
                auto& suspendableStripe = Stripes[stripeIndex];
                suspendableStripe.SetExtractedCookie(IChunkPoolOutput::NullCookie);
                if (!suspendableStripe.IsSuspended()) {
                    Register(stripeIndex);
                }
            }
            YCHECK(ExtractedLists.erase(cookie) == 1);
        } else {
            ReplayCookies.erase(replayIt);
            YCHECK(LostCookies.insert(cookie).second);
            if (extractedStripeList.UnavailableStripeCount > 0) {
                ++UnavailableLostCookieCount;
            }
        }
    }

};

std::unique_ptr<IChunkPool> CreateUnorderedChunkPool(
    TNodeDirectoryPtr nodeDirectory,
    int jobCount)
{
    return std::unique_ptr<IChunkPool>(new TUnorderedChunkPool(
        nodeDirectory,
        jobCount));
}

////////////////////////////////////////////////////////////////////

class TShuffleChunkPool
    : public TChunkPoolInputBase
    , public IShuffleChunkPool
{
public:
    TShuffleChunkPool(
        TNodeDirectoryPtr nodeDirectory,
        int partitionCount,
        i64 dataSizeThreshold)
        : TChunkPoolInputBase(nodeDirectory)
        , DataSizeThreshold(dataSizeThreshold)
    {
        Outputs.resize(partitionCount);
        for (int index = 0; index < partitionCount; ++index) {
            Outputs[index].reset(new TOutput(this, index));
        }
    }

    // IShuffleChunkPool implementation.

    virtual IChunkPoolInput* GetInput() override
    {
        return this;
    }

    virtual IChunkPoolOutput* GetOutput(int partitionIndex) override
    {
        return ~Outputs[partitionIndex];
    }

    // IChunkPoolInput implementation.

    virtual IChunkPoolInput::TCookie Add(TChunkStripePtr stripe) override
    {
        YCHECK(!Finished);

        auto cookie = static_cast<int>(InputStripes.size());

        TInputStripe inputStripe;
        inputStripe.ElementaryIndexBegin = static_cast<int>(ElementaryStripes.size());

        FOREACH (const auto& chunkSlice, stripe->ChunkSlices) {
            int elementaryIndex = static_cast<int>(ElementaryStripes.size());
            auto elementaryStripe = New<TChunkStripe>(chunkSlice);
            ElementaryStripes.push_back(elementaryStripe);

            auto partitionsExt = GetProtoExtension<NTableClient::NProto::TPartitionsExt>(
                chunkSlice->GetChunkSpec()->extensions());
            YCHECK(partitionsExt.partitions_size() == Outputs.size());

            for (int index = 0; index < static_cast<int>(Outputs.size()); ++index) {
                const auto& partitionAttributes = partitionsExt.partitions(index);
                Outputs[index]->AddStripe(
                    elementaryIndex,
                    partitionAttributes.uncompressed_data_size(),
                    partitionAttributes.row_count());
            }

            RemoveProtoExtension<NTableClient::NProto::TPartitionsExt>(
                chunkSlice->GetChunkSpec()->mutable_extensions());
        }

        inputStripe.ElementaryIndexEnd = static_cast<int>(ElementaryStripes.size());
        InputStripes.push_back(inputStripe);

        return cookie;
    }

    virtual void Suspend(IChunkPoolInput::TCookie cookie) override
    {
        const auto& inputStripe = InputStripes[cookie];
        for (int index = inputStripe.ElementaryIndexBegin; index < inputStripe.ElementaryIndexEnd; ++index) {
            FOREACH (const auto& output, Outputs) {
                output->SuspendStripe(index);
            }
        }
    }

    virtual void Resume(IChunkPoolInput::TCookie cookie, TChunkStripePtr stripe) override
    {
        // Remove all partition extensions.
        FOREACH (auto chunkSlice, stripe->ChunkSlices) {
            RemoveProtoExtension<NTableClient::NProto::TPartitionsExt>(
                chunkSlice->GetChunkSpec()->mutable_extensions());
        }

        // Although the sizes and even the row count may have changed (mind unordered reader and
        // possible undetermined mappers in partition jobs), we ignore it and use counter values
        // from the initial stripes, hoping that nobody will recognize it. This may lead to
        // incorrect memory consumption estimates but significant bias is very unlikely.
        const auto& inputStripe = InputStripes[cookie];
        int stripeCount = inputStripe.ElementaryIndexEnd - inputStripe.ElementaryIndexBegin;
        int limit = std::min(static_cast<int>(stripe->ChunkSlices.size()), stripeCount - 1);

        // Fill the initial range of elementary stripes with new chunks (one per stripe).
        for (int index = 0; index < limit; ++index) {
            auto chunkSlice = stripe->ChunkSlices[index];
            int elementaryIndex = index + inputStripe.ElementaryIndexBegin;
            ElementaryStripes[elementaryIndex] = New<TChunkStripe>(chunkSlice);
        }

        // Cleanup the rest of elementary stripes.
        for (int elementaryIndex = inputStripe.ElementaryIndexBegin + limit;
            elementaryIndex < inputStripe.ElementaryIndexEnd;
            ++elementaryIndex)
        {
            ElementaryStripes[elementaryIndex] = New<TChunkStripe>();
        }

        // Put remaining chunks (if any) into the last stripe.
        auto& lastElementaryStripe = ElementaryStripes[inputStripe.ElementaryIndexBegin + limit];
        for (int index = limit; index < static_cast<int>(stripe->ChunkSlices.size()); ++index) {
            auto chunkSlice = stripe->ChunkSlices[index];
            lastElementaryStripe->ChunkSlices.push_back(chunkSlice);
        }

        for (int elementaryIndex = inputStripe.ElementaryIndexBegin;
            elementaryIndex < inputStripe.ElementaryIndexEnd;
            ++elementaryIndex)
        {
            FOREACH (const auto& output, Outputs) {
                output->ResumeStripe(elementaryIndex);
            }
        }
    }

    virtual void Finish() override
    {
        if (Finished)
            return;

        TChunkPoolInputBase::Finish();

        FOREACH (const auto& output, Outputs) {
            output->FinishInput();
        }
    }

private:
    class TOutput
        : public TChunkPoolOutputBase
    {
    public:
        explicit TOutput(
            TShuffleChunkPool* owner,
            int partitionIndex)
            : Owner(owner)
            , PartitionIndex(partitionIndex)
        {
            AddNewRun();
        }

        struct TStripeInfo
        {
            TStripeInfo()
                : DataSize(0)
                , RowCount(0)
            { }

            i64 DataSize;
            i64 RowCount;
        };

        void AddStripe(int elementaryIndex, i64 dataSize, i64 rowCount)
        {
            auto* run = &Runs.back();
            if (run->TotalDataSize > 0  && run->TotalDataSize + dataSize > Owner->DataSizeThreshold) {
                SealLastRun();
                AddNewRun();
                run = &Runs.back();
            }

            YCHECK(elementaryIndex == run->ElementaryIndexEnd);
            run->ElementaryIndexEnd = elementaryIndex + 1;
            run->TotalDataSize += dataSize;
            run->TotalRowCount += rowCount;

            DataSizeCounter.Increment(dataSize);
            RowCounter.Increment(rowCount);
        }

        void SuspendStripe(int elementaryIndex)
        {
            auto* run = FindRun(elementaryIndex);
            if (run) {
                run->IsApproximate = true;
                ++run->SuspendCount;
                UpdatePendingRunSet(*run);
            }
        }

        void ResumeStripe(int elementaryIndex)
        {
            auto* run = FindRun(elementaryIndex);
            if (run) {
                --run->SuspendCount;
                YCHECK(run->SuspendCount >= 0);
                UpdatePendingRunSet(*run);
            }
        }

        void FinishInput()
        {
            auto& lastRun = Runs.back();
            if (lastRun.TotalDataSize > 0) {
                SealLastRun();
            } else {
                Runs.pop_back();
            }
        }

        // IChunkPoolOutput implementation.

        virtual TChunkStripeStatisticsVector GetApproximateStripeStatistics() const override
        {
            YCHECK(!Runs.empty());
            YCHECK(GetPendingJobCount() > 0);

            TChunkStripeStatisticsVector result(1);

            // This is the next run to be given by #Extract.
            auto it = PendingRuns.begin();
            auto cookie = *it;
            auto& run = Runs[cookie];

            auto& stat = result.front();

            // NB: cannot estimate MaxBlockSize here.
            stat.ChunkCount = run.ElementaryIndexEnd - run.ElementaryIndexBegin;
            stat.DataSize = run.TotalDataSize;
            stat.RowCount = run.TotalRowCount;

            if (run.IsApproximate) {
                stat.DataSize *= ApproximateSizesBoostFactor;
                stat.RowCount *= ApproximateSizesBoostFactor;
            }

            return result;
        }

        virtual bool IsCompleted() const override
        {
            return Owner->Finished &&
                   JobCounter.GetCompleted() == Runs.size();
        }

        virtual int GetTotalJobCount() const override
        {
            return static_cast<int>(Runs.size());
        }

        virtual int GetPendingJobCount() const override
        {
            return static_cast<int>(PendingRuns.size());
        }

        virtual i64 GetLocality(const Stroka& address) const override
        {
            UNUSED(address);
            YUNREACHABLE();
        }

        virtual TCookie Extract(const Stroka& address) override
        {
            if (GetPendingJobCount() == 0) {
                return NullCookie;
            }

            auto it = PendingRuns.begin();
            auto cookie = *it;
            PendingRuns.erase(it);

            auto& run = Runs[cookie];
            YCHECK(run.State == ERunState::Pending);
            run.State = ERunState::Running;

            JobCounter.Start(1);
            DataSizeCounter.Start(run.TotalDataSize);
            RowCounter.Start(run.TotalRowCount);

            return cookie;
        }

        virtual TChunkStripeListPtr GetStripeList(TCookie cookie) override
        {
            const auto& run = Runs[cookie];

            auto list = New<TChunkStripeList>();
            list->PartitionTag = PartitionIndex;
            for (int index = run.ElementaryIndexBegin; index < run.ElementaryIndexEnd; ++index) {
                auto stripe = Owner->ElementaryStripes[index];
                list->Stripes.push_back(stripe);
                list->TotalChunkCount += stripe->ChunkSlices.size();
            }

            // NB: never ever make TotalDataSize and TotalBoostFactor approximate.
            // Otherwise sort data size and row counters will be severely corrupted
            list->TotalDataSize = run.TotalDataSize;
            list->TotalRowCount = run.TotalRowCount;

            list->LocalChunkCount = 0;
            list->NonLocalChunkCount = list->TotalChunkCount;

            list->IsApproximate = run.IsApproximate;

            return list;
        }

        virtual void Completed(TCookie cookie) override
        {
            auto& run = Runs[cookie];
            YCHECK(run.State == ERunState::Running);
            run.State = ERunState::Completed;

            JobCounter.Completed(1);
            DataSizeCounter.Completed(run.TotalDataSize);
            RowCounter.Completed(run.TotalRowCount);
        }

        virtual void Failed(TCookie cookie) override
        {
            auto& run = Runs[cookie];
            YCHECK(run.State == ERunState::Running);
            run.State = ERunState::Pending;

            UpdatePendingRunSet(run);

            JobCounter.Failed(1);
            DataSizeCounter.Failed(run.TotalDataSize);
            RowCounter.Failed(run.TotalRowCount);
        }

        virtual void Aborted(TCookie cookie) override
        {
            auto& run = Runs[cookie];
            YCHECK(run.State == ERunState::Running);
            run.State = ERunState::Pending;

            UpdatePendingRunSet(run);

            JobCounter.Aborted(1);
            DataSizeCounter.Aborted(run.TotalDataSize);
            RowCounter.Aborted(run.TotalRowCount);
        }

        virtual void Lost(TCookie cookie) override
        {
            auto& run = Runs[cookie];
            YCHECK(run.State == ERunState::Completed);
            run.State = ERunState::Pending;

            UpdatePendingRunSet(run);

            JobCounter.Lost(1);
            DataSizeCounter.Lost(run.TotalDataSize);
            RowCounter.Lost(run.TotalRowCount);
        }

    private:
        friend class TShuffleChunkPool;

        TShuffleChunkPool* Owner;
        int PartitionIndex;

        DECLARE_ENUM(ERunState,
            (Initializing)
            (Pending)
            (Running)
            (Completed)
        );

        struct TRun
        {
            TRun()
                : ElementaryIndexBegin(0)
                , ElementaryIndexEnd(0)
                , TotalDataSize(0)
                , TotalRowCount(0)
                , SuspendCount(0)
                , State(ERunState::Initializing)
                , IsApproximate(false)
            { }

            int ElementaryIndexBegin;
            int ElementaryIndexEnd;
            i64 TotalDataSize;
            i64 TotalRowCount;
            int SuspendCount;
            ERunState State;
            bool IsApproximate;
        };

        std::vector<TRun> Runs;
        yhash_set<TCookie> PendingRuns;


        void UpdatePendingRunSet(const TRun& run)
        {
            TCookie cookie = &run - Runs.data();
            if (run.State == ERunState::Pending && run.SuspendCount == 0) {
                PendingRuns.insert(cookie);
            } else {
                PendingRuns.erase(cookie);
            }
        }

        void AddNewRun()
        {
            TRun run;
            run.ElementaryIndexBegin = Runs.empty() ? 0 : Runs.back().ElementaryIndexEnd;
            run.ElementaryIndexEnd = run.ElementaryIndexBegin;
            Runs.push_back(run);
        }

        TRun* FindRun(int elementaryIndex)
        {
            if (Runs.empty() || elementaryIndex >= Runs.back().ElementaryIndexEnd) {
                return nullptr;
            }

            int lo = 0;
            int hi = static_cast<int>(Runs.size());
            while (lo + 1 < hi) {
                int mid = (lo + hi) / 2;
                const auto& run = Runs[mid];
                if (run.ElementaryIndexBegin <= elementaryIndex) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }

            auto& run = Runs[lo];
            YCHECK(run.ElementaryIndexBegin <= elementaryIndex && run.ElementaryIndexEnd > elementaryIndex);
            return &run;
        }

        void SealLastRun()
        {
            auto& run = Runs.back();
            YCHECK(run.TotalDataSize > 0);
            YCHECK(run.State == ERunState::Initializing);
            run.State = ERunState::Pending;
            UpdatePendingRunSet(run);
        }

    };

    i64 DataSizeThreshold;

    // One should use std::unique_ptr with care :)
    std::vector< std::unique_ptr<TOutput> > Outputs;

    struct TInputStripe
    {
        int ElementaryIndexBegin;
        int ElementaryIndexEnd;
    };

    std::vector<TInputStripe> InputStripes;
    std::vector<TChunkStripePtr> ElementaryStripes;
};

std::unique_ptr<IShuffleChunkPool> CreateShuffleChunkPool(
    TNodeDirectoryPtr nodeDirectory,
    int partitionCount,
    i64 dataSizeThreshold)
{
    return std::unique_ptr<IShuffleChunkPool>(new TShuffleChunkPool(
        nodeDirectory,
        partitionCount,
        dataSizeThreshold));
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

