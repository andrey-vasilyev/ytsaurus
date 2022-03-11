#include "versioned_chunk_reader.h"
#include "column_block_manager.h"
#include "segment_readers.h"
#include "read_span_refiner.h"
#include "rowset_builder.h"
#include "dispatch_by_type.h"
#include "prepared_meta.h"

#include <yt/yt/ytlib/chunk_client/block.h>

#include <yt/yt/ytlib/chunk_client/block_fetcher.h>

#include <yt/yt/ytlib/table_client/cached_versioned_chunk_meta.h>
#include <yt/yt/ytlib/table_client/chunk_column_mapping.h>
#include <yt/yt/ytlib/table_client/hunks.h>

#include <yt/yt/ytlib/query_client/coordination_helpers.h>

#include <yt/yt/client/table_client/config.h>

#include <yt/yt/ytlib/table_client/versioned_chunk_reader.h>
#include <yt/yt/ytlib/table_client/private.h>

#include <yt/yt/core/misc/tls_guard.h>

namespace NYT::NNewTableClient {

using NTableChunkFormat::NProto::TColumnMeta;

using NProfiling::TValueIncrementingTimingGuard;
using NProfiling::TWallTimer;

using NChunkClient::IBlockCachePtr;
using NChunkClient::TBlockFetcherPtr;
using NChunkClient::IChunkReaderPtr;

using NChunkClient::TChunkReaderMemoryManager;
using NChunkClient::TChunkReaderMemoryManagerOptions;
using NChunkClient::TClientChunkReadOptions;

using NTableClient::TChunkReaderConfigPtr;
using NTableClient::TColumnFilter;

using NTableClient::IVersionedReader;
using NTableClient::IVersionedReaderPtr;
using NTableClient::TVersionedRow;
using NTableClient::TChunkReaderPerformanceCountersPtr;

using NChunkClient::TCodecStatistics;
using NChunkClient::TChunkId;
using NChunkClient::NProto::TDataStatistics;

using NTableClient::IVersionedRowBatchPtr;
using NTableClient::TRowBatchReadOptions;
using NTableClient::CreateEmptyVersionedRowBatch;

using NTableClient::TTableSchemaPtr;

using NTableClient::TChunkColumnMapping;
using NTableClient::TChunkColumnMappingPtr;

////////////////////////////////////////////////////////////////////////////////

std::vector<EValueType> GetKeyTypes(const TTableSchemaPtr& tableSchema)
{
    std::vector<EValueType> keyTypes;

    auto keyColumnCount = tableSchema->GetKeyColumnCount();
    const auto& schemaColumns = tableSchema->Columns();

    for (int keyColumnIndex = 0; keyColumnIndex < keyColumnCount; ++keyColumnIndex) {
        auto type = schemaColumns[keyColumnIndex].GetWireType();
        keyTypes.push_back(type);
    }

    return keyTypes;
}

std::vector<TValueSchema> GetValuesSchema(
    const TTableSchemaPtr& tableSchema,
    TRange<TColumnIdMapping> valueIdMapping)
{
    std::vector<TValueSchema> valueSchema;

    const auto& schemaColumns = tableSchema->Columns();
    for (const auto& idMapping : valueIdMapping) {
        auto type = schemaColumns[idMapping.ReaderSchemaIndex].GetWireType();
        valueSchema.push_back(TValueSchema{
            type,
            ui16(idMapping.ReaderSchemaIndex),
            schemaColumns[idMapping.ReaderSchemaIndex].Aggregate().has_value()});
    }

    return valueSchema;
}

///////////////////////////////////////////////////////////////////////////////

// Need to build all read windows at once to prepare block indexes for block fetcher.
template <class TItem, class TPredicate>
std::vector<TSpanMatching> DoBuildReadWindows(
    const TCachedVersionedChunkMetaPtr& chunkMeta,
    TRange<TItem> items,
    TPredicate pred)
{
    const auto& blockLastKeys = chunkMeta->BlockLastKeys();
    const auto& blockMeta = chunkMeta->DataBlockMeta();

    std::vector<TSpanMatching> readWindows;

    using NQueryClient::GroupItemsByShards;

    // Block last keys may repeat.

    // TODO(lukyan): Fix repeated block last keys other way.
    ui32 checkLastLimit = 0;

    GroupItemsByShards(items, blockLastKeys, pred, [&] (
        const TLegacyKey* shardIt,
        const TItem* itemIt,
        const TItem* itemItEnd)
    {
        if (shardIt == blockLastKeys.end()) {
            auto chunkRowCount = chunkMeta->Misc().row_count();
            TReadSpan controlSpan(itemIt - items.begin(), itemItEnd - items.begin());
            // Add window for ranges after chunk end bound. It will be used go generate sentinel rows for lookup.
            readWindows.emplace_back(TReadSpan(chunkRowCount, chunkRowCount), controlSpan);
            return;
        }

        auto shardIndex = shardIt - blockLastKeys.begin();
        auto upperRowLimit = blockMeta->data_blocks(shardIndex).chunk_row_count();

        // TODO(lukyan): Rewrite calculation of start index.
        while (shardIndex > 0 && blockMeta->data_blocks(shardIndex - 1).chunk_row_count() == upperRowLimit) {
            --shardIndex;
        }

        ui32 startIndex = shardIndex > 0 ? blockMeta->data_blocks(shardIndex - 1).chunk_row_count() : 0;

        if (startIndex < checkLastLimit) {
            return;
        }

        YT_VERIFY(itemIt != itemItEnd);

        TReadSpan controlSpan(itemIt - items.begin(), itemItEnd - items.begin());
        readWindows.emplace_back(TReadSpan(startIndex, upperRowLimit), controlSpan);

        checkLastLimit = upperRowLimit;
    });

    return readWindows;
}

std::vector<TSpanMatching> BuildReadWindows(
    TRange<TRowRange> keyRanges,
    const TCachedVersionedChunkMetaPtr& chunkMeta,
    int keyColumnCount)
{
    struct TPredicate
    {
        const int CommonKeyPrefix;
        const int KeyColumnCount;

        bool operator() (const TRowRange* itemIt, const TLegacyKey* shardIt) const
        {
            return !TestKeyWithWidening(
                ToKeyRef(*shardIt, CommonKeyPrefix),
                MakeKeyBoundRef(itemIt->second, true, KeyColumnCount));
        }

        bool operator() (const TLegacyKey* shardIt, const TRowRange* itemIt) const
        {
            return !TestKeyWithWidening(
                ToKeyRef(*shardIt, CommonKeyPrefix),
                MakeKeyBoundRef(itemIt->first, false, KeyColumnCount));
        }
    };

    for (auto rowRange : keyRanges) {
        YT_VERIFY(rowRange.first <= rowRange.second);
    }

    return DoBuildReadWindows(
        chunkMeta,
        keyRanges,
        TPredicate{chunkMeta->GetChunkKeyColumnCount(), keyColumnCount});
}

std::vector<TSpanMatching> BuildReadWindows(
    TRange<TLegacyKey> keys,
    const TCachedVersionedChunkMetaPtr& chunkMeta,
    int /*keyColumnCount*/)
{
    // Strong typedef.
    struct TItem
        : public TLegacyKey
    {
        using TLegacyKey::TLegacyKey;
    };

    struct TPredicate
    {
        const int CommonKeyPrefix;

        bool operator() (const TItem* itemIt, const TLegacyKey* shardIt) const
        {
            return CompareWithWidening(
                ToKeyRef(*shardIt, CommonKeyPrefix),
                ToKeyRef(*itemIt)) >= 0;
        }

        bool operator() (const TLegacyKey* shardIt, const TItem* itemIt) const
        {
            return CompareWithWidening(
                ToKeyRef(*shardIt, CommonKeyPrefix),
                ToKeyRef(*itemIt)) < 0;
        }
    };

    return DoBuildReadWindows(
        chunkMeta,
        MakeRange(static_cast<const TItem*>(keys.begin()), keys.size()),
        TPredicate{chunkMeta->GetChunkKeyColumnCount()});
}

////////////////////////////////////////////////////////////////////////////////

class TVersionedChunkReader
    : public TBlockWindowManager
{
public:
    TVersionedChunkReader(
        TRefCountedDataBlockMetaPtr blockMeta,
        std::vector<std::unique_ptr<TGroupBlockHolder>> columnBlockHolders,
        TBlockFetcherPtr blockFetcher,
        std::unique_ptr<IRowsetBuilder> rowsetBuilder,
        std::vector<TSpanMatching>&& windowsList,
        TReaderStatisticsPtr readerStatistics)
        : TBlockWindowManager(
            std::move(columnBlockHolders),
            std::move(blockMeta),
            std::move(blockFetcher),
            readerStatistics)
        , RowsetBuilder_(std::move(rowsetBuilder))
        , WindowsList_(std::move(windowsList))
    {
        std::reverse(WindowsList_.begin(), WindowsList_.end());
    }

    bool ReadRows(std::vector<TMutableVersionedRow>* rows, ui32 readCount, ui64* dataWeigth)
    {
        YT_VERIFY(rows->empty());
        rows->assign(readCount, TMutableVersionedRow());

        RowsetBuilder_->ClearBuffer();
        ui32 offset = 0;

        while (offset < readCount) {
            if (!RowsetBuilder_->IsReadListEmpty()) {
                // Read rows within window.
                offset += RowsetBuilder_->ReadRowsByList(
                    rows->data() + offset,
                    readCount - offset,
                    dataWeigth,
                    ReaderStatistics_.Get());
                // Segment limit reached, readCount reached, or read list exhausted.
            } else if (WindowsList_.empty()) {
                rows->resize(offset);
                return false;
            } else if (!UpdateWindow()) {
                break;
            }
        }

        rows->resize(offset);
        return true;
    }

    TChunkedMemoryPool* GetPool()
    {
        return RowsetBuilder_->GetPool();
    }

private:
    std::unique_ptr<IRowsetBuilder> RowsetBuilder_;
    TTmpBuffers LocalBuffers_;

    // Vector is used as a stack. No need to keep ReadRangeIndex and RowIndex.
    // Also use stack for WindowsList and do not keep NextWindowIndex.
    std::vector<TSpanMatching> WindowsList_;

    // Returns false if need wait for ready event.
    bool UpdateWindow()
    {
        YT_VERIFY(!WindowsList_.empty());

        if (!TryUpdateWindow(WindowsList_.back().Chunk.Lower)) {
            return false;
        }

        {
            TValueIncrementingTimingGuard<TWallTimer> timingGuard(&ReaderStatistics_->BuildRangesTime);
            YT_VERIFY(RowsetBuilder_->IsReadListEmpty());

            // Update read list.
            RowsetBuilder_->BuildReadListForWindow(WindowsList_.back());
            WindowsList_.pop_back();
        }

        return true;
    }
};

////////////////////////////////////////////////////////////////////////////////

// TReaderWrapper implements IVersionedReader interface.
// It calculates performance counters and provides data and decompression statistics.
class TReaderWrapper
    : public IVersionedReader
    , public TVersionedChunkReader
{
public:
    TReaderWrapper(
        TIntrusivePtr<TPreparedChunkMeta> preparedMeta,
        TCachedVersionedChunkMetaPtr chunkMeta,
        std::unique_ptr<bool[]> columnHunkFlags,
        std::vector<std::unique_ptr<TGroupBlockHolder>> columnBlockHolders,
        TBlockFetcherPtr blockFetcher,
        std::unique_ptr<IRowsetBuilder> rowsetBuilder,
        std::vector<TSpanMatching>&& windowsList,
        TChunkReaderPerformanceCountersPtr performanceCounters,
        bool lookup,
        TReaderStatisticsPtr readerStatistics)
        : TVersionedChunkReader(
            chunkMeta->DataBlockMeta(),
            std::move(columnBlockHolders),
            std::move(blockFetcher),
            std::move(rowsetBuilder),
            std::move(windowsList),
            std::move(readerStatistics))
        , PreparedMeta_(std::move(preparedMeta))
        , PerformanceCounters_(std::move(performanceCounters))
        , ChunkMeta_(std::move(chunkMeta))
        , ColumnHunkFlags_(std::move(columnHunkFlags))
        , Lookup_(lookup)
    { }

    TFuture<void> Open() override
    {
        return VoidFuture;
    }

    IVersionedRowBatchPtr Read(const TRowBatchReadOptions& options) override
    {
        YT_VERIFY(options.MaxRowsPerRead > 0);

        std::vector<TMutableVersionedRow> rows;
        rows.reserve(options.MaxRowsPerRead);

        bool hasMore = Read(&rows);
        if (!hasMore) {
            YT_VERIFY(rows.empty());
            return nullptr;
        }

        if (rows.empty()) {
            return CreateEmptyVersionedRowBatch();
        }

        auto* rowsData = rows.data();
        auto rowsSize = rows.size();
        return CreateBatchFromVersionedRows(MakeSharedRange(
            TRange<TVersionedRow>(rowsData, rowsSize),
            std::move(rows),
            MakeStrong(this)));
    }

    TFuture<void> GetReadyEvent() const override
    {
        return TVersionedChunkReader::GetReadyEvent();
    }

    // TODO(lukyan): Provide statistics object to BlockFetcher.
    TDataStatistics GetDataStatistics() const override
    {
        if (!BlockFetcher_) {
            return TDataStatistics();
        }

        TDataStatistics dataStatistics;
        dataStatistics.set_chunk_count(1);
        dataStatistics.set_uncompressed_data_size(BlockFetcher_->GetUncompressedDataSize());
        dataStatistics.set_compressed_data_size(BlockFetcher_->GetCompressedDataSize());
        dataStatistics.set_row_count(RowCount_);
        dataStatistics.set_data_weight(DataWeight_);
        return dataStatistics;
    }

    TCodecStatistics GetDecompressionStatistics() const override
    {
        return BlockFetcher_
            ? TCodecStatistics().Append(BlockFetcher_->GetDecompressionTime())
            : TCodecStatistics();
    }

    bool IsFetchingCompleted() const override
    {
        if (!BlockFetcher_) {
            return true;
        }

        return BlockFetcher_->IsFetchingCompleted();
    }

    std::vector<TChunkId> GetFailedChunkIds() const override
    {
        return {};
    }

private:
    // Hold reference to prepared meta.
    const TIntrusivePtr<TPreparedChunkMeta> PreparedMeta_;
    const TChunkReaderPerformanceCountersPtr PerformanceCounters_;
    const TCachedVersionedChunkMetaPtr ChunkMeta_;

    const std::unique_ptr<bool[]> ColumnHunkFlags_;

    // TODO(lukyan): Use performance counters adapter and increment counters uniformly and remove this flag.
    const bool Lookup_;

    i64 RowCount_ = 0;
    i64 DataWeight_ = 0;

    bool Read(std::vector<TMutableVersionedRow>* rows)
    {
        ui64 dataWeight = 0;

        rows->clear();
        bool hasMore = ReadRows(
            rows,
            rows->capacity(),
            &dataWeight);

        if (ColumnHunkFlags_) {
            auto* pool = GetPool();
            for (auto row : *rows) {
                GlobalizeHunkValuesAndSetHunkFlag(
                    pool,
                    ChunkMeta_,
                    ColumnHunkFlags_.get(),
                    row);
            }
        }

        i64 rowCount = 0;
        for (auto row : *rows) {
            if (row) {
                ++rowCount;
            }
        }

        RowCount_ += rowCount;
        DataWeight_ += dataWeight;

        if (Lookup_) {
            PerformanceCounters_->StaticChunkRowLookupCount += rowCount;
            PerformanceCounters_->StaticChunkRowLookupDataWeightCount += dataWeight;
        } else {
            PerformanceCounters_->StaticChunkRowReadCount += rowCount;
            PerformanceCounters_->StaticChunkRowReadDataWeightCount += dataWeight;
        }

        if (hasMore) {
            return true;
        }

        // Actually does not have more but caller expect true if rows are not empty.
        return !rows->empty();
    }
};

bool IsKeys(const TSharedRange<TRowRange>&)
{
    return false;
}

bool IsKeys(const TSharedRange<TLegacyKey>&)
{
    return true;
}

std::vector<TBlockFetcher::TBlockInfo> BuildBlockInfos(
    std::vector<TRange<ui32>> groupBlockIndexes,
    TRange<TSpanMatching> windows,
    const TRefCountedDataBlockMetaPtr& blockMetas)
{
    auto groupCount = groupBlockIndexes.size();
    std::vector<ui32> perGroupBlockRowLimits(groupCount, 0);

    std::vector<TBlockFetcher::TBlockInfo> blockInfos;
    for (auto window : windows) {
        auto startRowIndex = window.Chunk.Lower;

        for (ui16 groupId = 0; groupId < groupCount; ++groupId) {
            if (startRowIndex < perGroupBlockRowLimits[groupId]) {
                continue;
            }

            auto& blockIndexes = groupBlockIndexes[groupId];

            auto blockIt = ExponentialSearch(blockIndexes.begin(), blockIndexes.end(), [&] (auto blockIt) {
                const auto& blockMeta = blockMetas->data_blocks(*blockIt);
                return blockMeta.chunk_row_count() <= startRowIndex;
            });

            blockIndexes = blockIndexes.Slice(blockIt - blockIndexes.begin(), blockIndexes.size());

            if (blockIt != blockIndexes.end()) {
                const auto& blockMeta = blockMetas->data_blocks(*blockIt);
                perGroupBlockRowLimits[groupId] = blockMeta.chunk_row_count();

                blockInfos.push_back({
                    .ReaderIndex = 0,
                    .BlockIndex = static_cast<int>(*blockIt),
                    .Priority = static_cast<int>(blockMeta.chunk_row_count() - blockMeta.row_count()),
                    .UncompressedDataSize = blockMeta.uncompressed_size()
                });
            }
        }
    }

    return blockInfos;
}

////////////////////////////////////////////////////////////////////////////////

template <class TItem>
IVersionedReaderPtr CreateVersionedChunkReader(
    TSharedRange<TItem> readItems,
    TTimestamp timestamp,
    TCachedVersionedChunkMetaPtr chunkMeta,
    const TTableSchemaPtr& tableSchema,
    const TColumnFilter& columnFilter,
    const TChunkColumnMappingPtr& chunkColumnMapping,
    IBlockCachePtr blockCache,
    const TChunkReaderConfigPtr config,
    IChunkReaderPtr underlyingReader,
    TChunkReaderPerformanceCountersPtr performanceCounters,
    const TClientChunkReadOptions& chunkReadOptions,
    bool produceAll,
    TReaderStatisticsPtr readerStatistics)
{
    if (!readerStatistics) {
        readerStatistics = New<TReaderStatistics>();
    }

    TWallTimer BuildReadWindowsTimer;
    auto preparedChunkMeta = chunkMeta->GetPreparedChunkMeta();

    auto windowsList = BuildReadWindows(readItems, chunkMeta, tableSchema->GetKeyColumnCount());
    readerStatistics->BuildReadWindowsTime = BuildReadWindowsTimer.GetElapsedTime();

    auto valuesIdMapping = chunkColumnMapping
        ? chunkColumnMapping->BuildVersionedSimpleSchemaIdMapping(columnFilter)
        : TChunkColumnMapping(tableSchema, chunkMeta->GetChunkSchema())
            .BuildVersionedSimpleSchemaIdMapping(columnFilter);

    TWallTimer createColumnBlockHoldersTimer;
    auto groupIds = GetGroupsIds(*preparedChunkMeta, chunkMeta->GetChunkKeyColumnCount(), valuesIdMapping);

    auto groupBlockHolders = CreateGroupBlockHolders(*preparedChunkMeta, groupIds);
    readerStatistics->CreateColumnBlockHoldersTime = createColumnBlockHoldersTimer.GetElapsedTime();

    std::vector<TRange<ui32>> groupBlockIds;
    for (const auto& blockHolder : groupBlockHolders) {
        groupBlockIds.push_back(blockHolder->GetBlockIds());
    }

    TWallTimer buildBlockInfosTimer;
    auto blockInfos = BuildBlockInfos(std::move(groupBlockIds), windowsList, chunkMeta->DataBlockMeta());
    readerStatistics->BuildBlockInfosTime = buildBlockInfosTimer.GetElapsedTime();

    auto blockCount = blockInfos.size();
    size_t uncompressedBlocksSize = 0;

    for (const auto& blockInfo : blockInfos) {
        uncompressedBlocksSize += blockInfo.UncompressedDataSize;
    }

    TBlockFetcherPtr blockFetcher;
    if (!blockInfos.empty()) {
        TWallTimer createBlockFetcherTimer;
        auto memoryManager = New<TChunkReaderMemoryManager>(TChunkReaderMemoryManagerOptions(config->WindowSize));

        blockFetcher = New<TBlockFetcher>(
            config,
            std::move(blockInfos),
            memoryManager,
            std::vector{underlyingReader},
            blockCache,
            CheckedEnumCast<NCompression::ECodec>(chunkMeta->Misc().compression_codec()),
            static_cast<double>(chunkMeta->Misc().compressed_data_size()) / chunkMeta->Misc().uncompressed_data_size(),
            chunkReadOptions);
        readerStatistics->CreateBlockFetcherTime = createBlockFetcherTimer.GetElapsedTime();
    }

    auto keyTypes = GetKeyTypes(tableSchema);
    auto valueSchema = GetValuesSchema(tableSchema, valuesIdMapping);

    const auto& Logger = NTableClient::TableClientLogger;

    YT_LOG_DEBUG("Creating rowset builder (ReadItemCount: %v, BlockCount: %v, UncompressedBlocksSize: %v, KeyTypes: %v, ValueTypes: %v)",
        readItems.size(),
        blockCount,
        uncompressedBlocksSize,
        keyTypes,
        MakeFormattableView(valueSchema, [] (TStringBuilderBase* builder, TValueSchema valueSchema) {
            builder->AppendFormat("%v", valueSchema.Type);
        }));

    if (timestamp == NTableClient::AllCommittedTimestamp) {
        produceAll = true;
    }

    YT_VERIFY(produceAll || timestamp != NTableClient::AllCommittedTimestamp);

    std::vector<std::pair<const TBlockRef*, ui16>> blockRefs;

    std::vector<TColumnBase> columnInfos;
    for (int index = 0; index < std::ssize(keyTypes); ++index) {
        const TBlockRef* blockRef = nullptr;
        if (index < chunkMeta->GetChunkKeyColumnCount()) {
            auto groupId = preparedChunkMeta->GroupIdPerColumn[index];
            auto blockHolderIndex = LowerBound(groupIds.begin(), groupIds.end(), groupId) - groupIds.begin();
            blockRef = groupBlockHolders[blockHolderIndex].get();
        }

        columnInfos.emplace_back(
            blockRef,
            preparedChunkMeta->ColumnIndexInGroup[index]);
    }

    for (auto [chunkSchemaIndex, readerSchemaIndex] : valuesIdMapping) {
        auto groupId = preparedChunkMeta->GroupIdPerColumn[chunkSchemaIndex];
        auto blockHolderIndex = LowerBound(groupIds.begin(), groupIds.end(), groupId) - groupIds.begin();
        const auto* blockRef = groupBlockHolders[blockHolderIndex].get();
        columnInfos.emplace_back(
            blockRef,
            preparedChunkMeta->ColumnIndexInGroup[chunkSchemaIndex]);
    }

    {
        // Timestamp column info.
        auto groupId = preparedChunkMeta->GroupIdPerColumn.back();
        auto blockHolderIndex = LowerBound(groupIds.begin(), groupIds.end(), groupId) - groupIds.begin();
        const auto* blockRef = groupBlockHolders[blockHolderIndex].get();
        columnInfos.emplace_back(
            blockRef,
            preparedChunkMeta->ColumnIndexInGroup.back());
    }

    auto rowsetBuilder = CreateRowsetBuilder(readItems, keyTypes, valueSchema, columnInfos, timestamp, produceAll);

    std::unique_ptr<bool[]> columnHunkFlags;
    const auto& chunkSchema = chunkMeta->GetChunkSchema();
    if (chunkSchema->HasHunkColumns()) {
        columnHunkFlags.reset(new bool[tableSchema->GetColumnCount()]());
        for (auto [chunkColumnId, tableColumnId] : valuesIdMapping) {
            columnHunkFlags[tableColumnId] = chunkSchema->Columns()[chunkColumnId].MaxInlineHunkSize().has_value();
        }
    }

    return New<TReaderWrapper>(
        std::move(preparedChunkMeta),
        std::move(chunkMeta),
        std::move(columnHunkFlags),
        std::move(groupBlockHolders),
        std::move(blockFetcher),
        std::move(rowsetBuilder),
        std::move(windowsList),
        std::move(performanceCounters),
        IsKeys(readItems),
        readerStatistics);
}

template
IVersionedReaderPtr CreateVersionedChunkReader<TRowRange>(
    TSharedRange<TRowRange> readItems,
    TTimestamp timestamp,
    TCachedVersionedChunkMetaPtr chunkMeta,
    const TTableSchemaPtr& tableSchema,
    const TColumnFilter& columnFilter,
    const TChunkColumnMappingPtr& chunkColumnMapping,
    IBlockCachePtr blockCache,
    const TChunkReaderConfigPtr config,
    IChunkReaderPtr underlyingReader,
    TChunkReaderPerformanceCountersPtr performanceCounters,
    const TClientChunkReadOptions& chunkReadOptions,
    bool produceAll,
    TReaderStatisticsPtr readerStatistics);

template
IVersionedReaderPtr CreateVersionedChunkReader<TLegacyKey>(
    TSharedRange<TLegacyKey> readItems,
    TTimestamp timestamp,
    TCachedVersionedChunkMetaPtr chunkMeta,
    const TTableSchemaPtr& tableSchema,
    const TColumnFilter& columnFilter,
    const TChunkColumnMappingPtr& chunkColumnMapping,
    IBlockCachePtr blockCache,
    const TChunkReaderConfigPtr config,
    IChunkReaderPtr underlyingReader,
    TChunkReaderPerformanceCountersPtr performanceCounters,
    const TClientChunkReadOptions& chunkReadOptions,
    bool produceAll,
    TReaderStatisticsPtr readerStatistics);

////////////////////////////////////////////////////////////////////////////////

TSharedRange<TRowRange> ClipRanges(
    TSharedRange<TRowRange> ranges,
    TUnversionedRow lower,
    TUnversionedRow upper,
    THolderPtr holder)
{
    auto startIt = ranges.begin();
    auto endIt = ranges.end();

    if (lower) {
        startIt = BinarySearch(startIt, endIt, [&] (auto it) {
            return it->second <= lower;
        });
    }

    if (upper) {
        endIt = BinarySearch(startIt, endIt, [&] (auto it) {
            return it->first < upper;
        });
    }

    if (startIt != endIt) {
        std::vector<TRowRange> items(startIt, endIt);
        if (lower && lower > items.front().first) {
            items.front().first = lower;
        }

        if (upper && upper < items.back().second) {
            items.back().second = upper;
        }

        return MakeSharedRange(
            std::move(items),
            std::move(ranges.ReleaseHolder()),
            std::move(holder));
    } else {
        return TSharedRange<TRowRange>(); // Empty ranges.
    }
}

TSharedRange<TRowRange> ConvertLegacyRanges(
    NTableClient::TLegacyOwningKey lowerLimit,
    NTableClient::TLegacyOwningKey upperLimit)
{
    // Workaround for case with invalid read limits (lower is greater than upper: [0#1, 1#<Min>] .. [0#1])
    // Test: test_traverse_table_with_alter_and_ranges_stress
    return MakeSingletonRowRange(lowerLimit, std::max(lowerLimit, upperLimit));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NNewTableClient
