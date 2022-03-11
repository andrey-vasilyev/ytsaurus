#pragma once

#include "chunk_reader.h"
#include "chunk_reader_options.h"
#include "chunk_reader_memory_manager.h"

#include <yt/yt/client/chunk_client/data_statistics.h>

#include <yt/yt/core/actions/future.h>

#include <yt/yt/core/compression/public.h>

#include <yt/yt/core/concurrency/async_semaphore.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/misc/lazy_ptr.h>
#include <yt/yt/core/misc/property.h>
#include <yt/yt/core/misc/ref.h>

#include <yt/yt/core/profiling/public.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

//! For a sequence of block indexes, fetches and uncompresses these blocks in the given order.
/*!
 *  Internally, blocks are prefetched obeying a given memory limit.
 */
class TBlockFetcher
    : public virtual TRefCounted
{
public:
    struct TBlockInfo
    {
        int ReaderIndex = -1;
        int BlockIndex = -1;
        int Priority = 0;

        i64 UncompressedDataSize = 0;
    };

    TBlockFetcher(
        TBlockFetcherConfigPtr config,
        std::vector<TBlockInfo> blockInfos,
        TChunkReaderMemoryManagerPtr memoryManager,
        std::vector<IChunkReaderPtr> chunkReaders,
        IBlockCachePtr blockCache,
        NCompression::ECodec codecId,
        double compressionRatio,
        const TClientChunkReadOptions& chunkReadOptions);

    ~TBlockFetcher();

    //! Returns |true| if there are requested blocks that were not fetched enough times.
    bool HasMoreBlocks() const;

    //! Returns uncompressed size of block with given index.
    i64 GetBlockSize(int readerIndex, int blockIndex) const;
    i64 GetBlockSize(int blockIndex) const;

    //! Asynchronously fetches the block with given index.
    /*!
     *  It is not allowed to fetch the block more times than it has been requested.
     *  If an error occurs during fetching then the whole session is failed.
     */
    TFuture<TBlock> FetchBlock(int readerIndex, int blockIndex);
    TFuture<TBlock> FetchBlock(int blockIndex);

    //! Returns true if all blocks are fetched and false otherwise.
    bool IsFetchingCompleted() const;

    //! Returns total uncompressed size of read blocks.
    i64 GetUncompressedDataSize() const;

    //! Returns total compressed size of read blocks.
    i64 GetCompressedDataSize() const;

    //! Returns codec and CPU time spent in compression.
    TCodecDuration GetDecompressionTime() const;

private:
    const TBlockFetcherConfigPtr Config_;
    std::vector<TBlockInfo> BlockInfos_;
    const std::vector<IChunkReaderPtr> ChunkReaders_;
    const IBlockCachePtr BlockCache_;
    const IInvokerPtr CompressionInvoker_;
    const IInvokerPtr ReaderInvoker_;
    const double CompressionRatio_;
    const TChunkReaderMemoryManagerPtr MemoryManager_;
    NCompression::ICodec* const Codec_;
    const TClientChunkReadOptions ChunkReadOptions_;
    NLogging::TLogger Logger;

    std::atomic<i64> UncompressedDataSize_ = 0;
    std::atomic<i64> CompressedDataSize_ = 0;
    std::atomic<NProfiling::TCpuDuration> DecompressionTime_ = 0;

    //! (ReaderIndex, BlockIndex) -> WindowIndex.
    THashMap<std::pair<int, int>, int> BlockDescriptorToWindowIndex_;

    TFuture<TMemoryUsageGuardPtr> FetchNextGroupMemoryFuture_;

    struct TWindowSlot
    {
        // Created lazily in GetBlockPromise.
        YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, BlockPromiseLock);
        TPromise<TBlock> BlockPromise;

        std::atomic<int> RemainingFetches = 0;

        TMemoryUsageGuardPtr MemoryUsageGuard;

        std::atomic_flag FetchStarted = ATOMIC_FLAG_INIT;
    };

    std::unique_ptr<TWindowSlot[]> Window_;

    int TotalRemainingFetches_ = 0;
    std::atomic<i64> TotalRemainingSize_ = 0;
    int FirstUnfetchedWindowIndex_ = 0;
    bool FetchingCompleted_ = false;

    struct TBlockDescriptor
    {
        int ReaderIndex;
        int BlockIndex;

        bool operator==(const TBlockDescriptor&) const = default;
    };

    void FetchNextGroup(const TErrorOr<TMemoryUsageGuardPtr>& memoryUsageGuardOrError);

    void RequestBlocks(
        std::vector<int> windowIndexes,
        std::vector<TBlockDescriptor> blockDescriptor,
        i64 uncompressedSize);

    void OnGotBlocks(
        int readerIndex,
        std::vector<int> windowIndexes,
        std::vector<int> blockIndices,
        TErrorOr<std::vector<TBlock>>&& blocksOrError);

    void DecompressBlocks(
        std::vector<int> windowIndexes,
        std::vector<TBlock> compressedBlocks);

    void MarkFailedBlocks(
        const std::vector<int>& windowIndexes,
        const TError& error);

    void ReleaseBlocks(const std::vector<int>& windowIndexes);

    static TPromise<TBlock> GetBlockPromise(TWindowSlot& windowSlot);
    static void ResetBlockPromise(TWindowSlot& windowSlot);
};

DEFINE_REFCOUNTED_TYPE(TBlockFetcher)

////////////////////////////////////////////////////////////////////////////////

class TSequentialBlockFetcher
    : public virtual TRefCounted
    , private TBlockFetcher
{
public:
    TSequentialBlockFetcher(
        TBlockFetcherConfigPtr config,
        std::vector<TBlockInfo> blockInfos,
        TChunkReaderMemoryManagerPtr memoryManager,
        std::vector<IChunkReaderPtr> chunkReaders,
        IBlockCachePtr blockCache,
        NCompression::ECodec codecId,
        double compressionRatio,
        const TClientChunkReadOptions& chunkReadOptions);

    TFuture<TBlock> FetchNextBlock();

    i64 GetNextBlockSize() const;

    using TBlockFetcher::HasMoreBlocks;
    using TBlockFetcher::IsFetchingCompleted;
    using TBlockFetcher::GetUncompressedDataSize;
    using TBlockFetcher::GetCompressedDataSize;
    using TBlockFetcher::GetDecompressionTime;

private:
    std::vector<TBlockInfo> OriginalOrderBlockInfos_;
    int CurrentIndex_ = 0;
};

DEFINE_REFCOUNTED_TYPE(TSequentialBlockFetcher)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
