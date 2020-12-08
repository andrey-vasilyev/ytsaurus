#pragma once

#include "chunk_meta_extensions.h"
#include "timing_reader.h"

#include <yt/client/chunk_client/reader_base.h>
#include <yt/client/chunk_client/read_limit.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/block_fetcher.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TChunkReaderBase
    : public virtual NChunkClient::IReaderBase
    , public TTimingReaderBase
{
public:
    TChunkReaderBase(
        NChunkClient::TBlockFetcherConfigPtr config,
        NChunkClient::IChunkReaderPtr underlyingReader,
        NChunkClient::IBlockCachePtr blockCache,
        const NChunkClient::TClientBlockReadOptions& blockReadOptions,
        const NChunkClient::TChunkReaderMemoryManagerPtr& memoryManager = nullptr);

    ~TChunkReaderBase();

    virtual NChunkClient::NProto::TDataStatistics GetDataStatistics() const override;

    virtual NChunkClient::TCodecStatistics GetDecompressionStatistics() const override;

    virtual bool IsFetchingCompleted() const override;

    virtual std::vector<NChunkClient::TChunkId> GetFailedChunkIds() const override;

protected:
    const NChunkClient::TBlockFetcherConfigPtr Config_;
    const NChunkClient::IBlockCachePtr BlockCache_;
    const NChunkClient::IChunkReaderPtr UnderlyingReader_;
    const NChunkClient::TClientBlockReadOptions BlockReadOptions_;

    NChunkClient::TSequentialBlockFetcherPtr SequentialBlockFetcher_;
    NChunkClient::TChunkReaderMemoryManagerPtr MemoryManager_;
    TFuture<NChunkClient::TBlock> CurrentBlock_;

    bool BlockEnded_ = false;
    bool InitFirstBlockNeeded_ = false;
    bool InitNextBlockNeeded_ = false;

    bool CheckRowLimit_ = false;
    bool CheckKeyLimit_ = false;

    TChunkedMemoryPool MemoryPool_;

    bool BeginRead();
    bool OnBlockEnded();

    TFuture<void> DoOpen(
        std::vector<NChunkClient::TBlockFetcher::TBlockInfo> blockSequence,
        const NChunkClient::NProto::TMiscExt& miscExt);

    int GetBlockIndexByKey(
        TLegacyKey key,
        const TSharedRange<TLegacyKey>& blockIndexKeys,
        std::optional<int> keyColumnCount) const;

    void CheckBlockUpperKeyLimit(
        TLegacyKey blockLastKey,
        TLegacyKey upperLimit,
        std::optional<int> keyColumnCount = std::nullopt);

    void CheckBlockUpperLimits(
        i64 blockChunkRowCount,
        TLegacyKey blockLastKey,
        const NChunkClient::TLegacyReadLimit& upperLimit,
        std::optional<int> keyColumnCount = std::nullopt);

    // These methods return min block index, satisfying the lower limit.
    int ApplyLowerRowLimit(const NProto::TBlockMetaExt& blockMeta, const NChunkClient::TLegacyReadLimit& lowerLimit) const;
    int ApplyLowerKeyLimit(const TSharedRange<TLegacyKey>& blockIndexKeys, const NChunkClient::TLegacyReadLimit& lowerLimit, std::optional<int> keyColumnCount = std::nullopt) const;

    // These methods return max block index, satisfying the upper limit.
    int ApplyUpperRowLimit(const NProto::TBlockMetaExt& blockMeta, const NChunkClient::TLegacyReadLimit& upperLimit) const;
    int ApplyUpperKeyLimit(const TSharedRange<TLegacyKey>& blockIndexKeys, const NChunkClient::TLegacyReadLimit& upperLimit, std::optional<int> keyColumnCount = std::nullopt) const;

    virtual void InitFirstBlock() = 0;
    virtual void InitNextBlock() = 0;

private:
    NLogging::TLogger Logger;

    TLegacyKey WidenKey(const TLegacyKey& key, std::optional<int> keyColumnCount, TChunkedMemoryPool* pool) const;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
