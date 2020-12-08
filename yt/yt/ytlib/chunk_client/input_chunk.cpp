#include "input_chunk.h"

#include <yt/library/erasure/codec.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>

#include <yt/client/object_client/helpers.h>

#include <yt/library/erasure/codec.h>

#include <yt/core/misc/numeric_helpers.h>

namespace NYT::NChunkClient {

using namespace NTableClient;
using namespace NTabletClient;
using namespace NObjectClient;
using namespace NNodeTrackerClient;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

TInputChunkBase::TInputChunkBase(const NProto::TChunkSpec& chunkSpec)
    : ChunkId_(FromProto<TChunkId>(chunkSpec.chunk_id()))
    , TableIndex_(chunkSpec.table_index())
    , ErasureCodec_(NErasure::ECodec(chunkSpec.erasure_codec()))
    , TableRowIndex_(chunkSpec.table_row_index())
    , RangeIndex_(chunkSpec.range_index())
    , TabletIndex_(chunkSpec.tablet_index())
{
    SetReplicaList(FromProto<TChunkReplicaList>(chunkSpec.replicas()));

    const auto& chunkMeta = chunkSpec.chunk_meta();
    if (auto miscExt = FindProtoExtension<NProto::TMiscExt>(chunkMeta.extensions())) {
        TotalUncompressedDataSize_ = miscExt->uncompressed_data_size();

        // NB(psushin): we don't use overrides from master, since we can do the same estimates ourself.
        TotalDataWeight_ = miscExt->has_data_weight() && miscExt->data_weight() > 0
            ? miscExt->data_weight()
            : TotalUncompressedDataSize_;

        TotalRowCount_ = miscExt->row_count();

        CompressedDataSize_ = miscExt->compressed_data_size();

        MaxBlockSize_ = miscExt->has_max_block_size()
            ? miscExt->max_block_size()
            : DefaultMaxBlockSize;
        UniqueKeys_ = miscExt->unique_keys();
    }

    if (IsDynamicStore()) {
        // TODO(ifsmirnov): See YT-12212 for reasonable estimates.
        TableChunkFormat_ = ETableChunkFormat::SchemalessHorizontal;
        TotalDataWeight_ = 1;
        TotalRowCount_ = 1;
        CompressedDataSize_ = 1;
        MaxBlockSize_ = DefaultMaxBlockSize;

        UniqueKeys_ = TypeFromId(ChunkId_) == EObjectType::SortedDynamicTabletStore;
        TabletId_ = FromProto<TTabletId>(chunkSpec.tablet_id());
    } else {
        YT_VERIFY(EChunkType(chunkMeta.type()) == EChunkType::Table);
        TableChunkFormat_ = CheckedEnumCast<ETableChunkFormat>(chunkMeta.version());
    }
}

TChunkReplicaList TInputChunkBase::GetReplicaList() const
{
    TChunkReplicaList replicas;

    replicas.reserve(MaxInputChunkReplicaCount);
    for (auto replica : Replicas_) {
        if (replica.GetNodeId() != InvalidNodeId) {
            replicas.push_back(replica);
        }
    }
    return replicas;
}

void TInputChunkBase::SetReplicaList(const TChunkReplicaList& replicas)
{
    Replicas_.fill(TChunkReplica());
    for (int index = 0; index < replicas.size(); ++index) {
        auto replica = replicas[index];
        if (ErasureCodec_ == NErasure::ECodec::None) {
            if (index < MaxInputChunkReplicaCount) {
                Replicas_[index] = replica;
            }
        } else {
            int erasureIndex = replica.GetReplicaIndex();
            YT_VERIFY(erasureIndex < MaxInputChunkReplicaCount);
            Replicas_[erasureIndex] = replica;
        }
    }
}

bool TInputChunkBase::IsDynamicStore() const
{
    auto type = TypeFromId(ChunkId_);
    return type == EObjectType::SortedDynamicTabletStore ||
        type == EObjectType::OrderedDynamicTabletStore;
}

// Intentionally used.
void TInputChunkBase::CheckOffsets()
{
    static_assert(offsetof(TInputChunkBase, ChunkId_) == 0, "invalid offset");
    static_assert(offsetof(TInputChunkBase, Replicas_) == 16, "invalid offset");
    static_assert(offsetof(TInputChunkBase, TableIndex_) == 80, "invalid offset");
    static_assert(offsetof(TInputChunkBase, ErasureCodec_) == 84, "invalid offset");
    static_assert(offsetof(TInputChunkBase, TableRowIndex_) == 88, "invalid offset");
    static_assert(offsetof(TInputChunkBase, RangeIndex_) == 96, "invalid offset");
    static_assert(offsetof(TInputChunkBase, TableChunkFormat_) == 100, "invalid offset");
    static_assert(offsetof(TInputChunkBase, ChunkIndex_) == 104, "invalid offset");
    static_assert(offsetof(TInputChunkBase, TabletIndex_) == 112, "invalid offset");
    static_assert(offsetof(TInputChunkBase, TabletId_) == 120, "invalid offset");
    static_assert(offsetof(TInputChunkBase, TotalUncompressedDataSize_) == 136, "invalid offset");
    static_assert(offsetof(TInputChunkBase, TotalRowCount_) == 144, "invalid offset");
    static_assert(offsetof(TInputChunkBase, CompressedDataSize_) == 152, "invalid offset");
    static_assert(offsetof(TInputChunkBase, TotalDataWeight_) == 160, "invalid offset");
    static_assert(offsetof(TInputChunkBase, MaxBlockSize_) == 168, "invalid offset");
    static_assert(offsetof(TInputChunkBase, UniqueKeys_) == 176, "invalid offset");
    static_assert(offsetof(TInputChunkBase, ColumnSelectivityFactor_) == 184, "invalid offset");
    static_assert(sizeof(TInputChunkBase) == 192, "invalid sizeof");
}

////////////////////////////////////////////////////////////////////////////////

TInputChunk::TInputChunk(const NProto::TChunkSpec& chunkSpec)
    : TInputChunkBase(chunkSpec)
    , LowerLimit_(chunkSpec.has_lower_limit()
        ? std::make_unique<TLegacyReadLimit>(chunkSpec.lower_limit())
        : nullptr)
    , UpperLimit_(chunkSpec.has_upper_limit()
        ? std::make_unique<TLegacyReadLimit>(chunkSpec.upper_limit())
        : nullptr)
    , BoundaryKeys_(FindBoundaryKeys(chunkSpec.chunk_meta()))
    , PartitionsExt_(HasProtoExtension<NTableClient::NProto::TPartitionsExt>(chunkSpec.chunk_meta().extensions())
        ? std::make_unique<NTableClient::NProto::TPartitionsExt>(
            GetProtoExtension<NTableClient::NProto::TPartitionsExt>(chunkSpec.chunk_meta().extensions()))
        : nullptr)
    , HeavyColumnarStatisticsExt_(HasProtoExtension<NTableClient::NProto::THeavyColumnStatisticsExt>(chunkSpec.chunk_meta().extensions())
        ? std::make_unique<NTableClient::NProto::THeavyColumnStatisticsExt>(
            GetProtoExtension<NTableClient::NProto::THeavyColumnStatisticsExt>(chunkSpec.chunk_meta().extensions()))
        : nullptr)
{
    if (TypeFromId(ChunkId_) == EObjectType::SortedDynamicTabletStore) {
        BoundaryKeys_ = std::make_unique<TOwningBoundaryKeys>();
        BoundaryKeys_->MinKey = LowerLimit_ && LowerLimit_->HasLegacyKey() ? LowerLimit_->GetLegacyKey() : MinKey();
        BoundaryKeys_->MaxKey = UpperLimit_ && UpperLimit_->HasLegacyKey() ? UpperLimit_->GetLegacyKey() : MaxKey();
    }
}

void TInputChunk::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, static_cast<TInputChunkBase&>(*this));
    Persist<TUniquePtrSerializer<>>(context, LowerLimit_);
    Persist<TUniquePtrSerializer<>>(context, UpperLimit_);
    Persist<TUniquePtrSerializer<>>(context, BoundaryKeys_);
    Persist<TUniquePtrSerializer<>>(context, PartitionsExt_);

    // COMPAT(gritukan)
    if (context.GetVersion() >= 300303) {
        Persist<TUniquePtrSerializer<>>(context, HeavyColumnarStatisticsExt_);
    }
}

size_t TInputChunk::SpaceUsed() const
{
    return
       sizeof(*this) +
       (LowerLimit_ ? LowerLimit_->SpaceUsed() : 0) +
       (UpperLimit_ ? UpperLimit_->SpaceUsed() : 0) +
       (BoundaryKeys_ ? BoundaryKeys_->SpaceUsed() : 0) +
       (PartitionsExt_ ? PartitionsExt_->SpaceUsed() : 0) +
       (HeavyColumnarStatisticsExt_ ? HeavyColumnarStatisticsExt_->SpaceUsed() : 0);
}

//! Returns |false| iff the chunk has nontrivial limits.
bool TInputChunk::IsCompleteChunk() const
{
    return
        (!LowerLimit_ || IsTrivial(*LowerLimit_)) &&
        (!UpperLimit_ || IsTrivial(*UpperLimit_));
}

//! Returns |true| iff the chunk is complete and is large enough.
bool TInputChunk::IsLargeCompleteChunk(i64 desiredChunkSize) const
{
    if (!IsCompleteChunk()) {
        return false;
    }

    // ChunkSequenceWriter may actually produce a chunk a bit smaller than desiredChunkSize,
    // so we have to be more flexible here.
    return 0.9 * CompressedDataSize_ >= desiredChunkSize;
}

void TInputChunk::ReleaseBoundaryKeys()
{
    BoundaryKeys_.reset();
}

void TInputChunk::ReleasePartitionsExt()
{
    PartitionsExt_.reset();
}

void TInputChunk::ReleaseHeavyColumnarStatisticsExt()
{
    HeavyColumnarStatisticsExt_.reset();
}

i64 TInputChunk::GetRowCount() const
{
    i64 lowerRowIndex = LowerLimit_ && LowerLimit_->HasRowIndex()
        ? LowerLimit_->GetRowIndex()
        : 0;

    i64 upperRowIndex = UpperLimit_ && UpperLimit_->HasRowIndex()
        ? UpperLimit_->GetRowIndex()
        : TotalRowCount_;

    auto rowCount = std::max(0l, upperRowIndex - lowerRowIndex);
    YT_VERIFY(rowCount <= TotalRowCount_);
    return rowCount;
}

i64 TInputChunk::GetDataWeight() const
{
    auto rowCount = GetRowCount();
    auto rowSelectivityFactor = static_cast<double>(rowCount) / TotalRowCount_;
    return std::max<i64>(std::ceil(TotalDataWeight_ * ColumnSelectivityFactor_ * rowSelectivityFactor), rowCount);
}

i64 TInputChunk::GetUncompressedDataSize() const
{
    return ApplySelectivityFactors(TotalUncompressedDataSize_);
}

i64 TInputChunk::GetCompressedDataSize() const
{
    return ApplySelectivityFactors(CompressedDataSize_);
}

i64 TInputChunk::ApplySelectivityFactors(i64 dataSize) const
{
    auto rowCount = GetRowCount();
    auto rowSelectivityFactor = static_cast<double>(rowCount) / TotalRowCount_;
    i64 result;
    if (TableChunkFormat_ == ETableChunkFormat::UnversionedColumnar ||
        TableChunkFormat_ == ETableChunkFormat::VersionedColumnar)
    {
        result = std::ceil(dataSize * ColumnSelectivityFactor_ * rowSelectivityFactor);
    } else {
        result = std::ceil(dataSize * rowSelectivityFactor);
    }
    result = std::max<i64>(result, rowCount);
    return std::max<i64>(result, 1);
}

////////////////////////////////////////////////////////////////////////////////

//! ToProto is used to pass chunk specs to job proxy as part of TTableInputSpec.
void ToProto(NProto::TChunkSpec* chunkSpec, const TInputChunkPtr& inputChunk, EDataSourceType dataSourceType)
{
    ToProto(chunkSpec->mutable_chunk_id(), inputChunk->ChunkId_);
    const auto& replicas = inputChunk->GetReplicaList();
    ToProto(chunkSpec->mutable_replicas(), replicas);
    if (inputChunk->TableIndex_ >= 0) {
        chunkSpec->set_table_index(inputChunk->TableIndex_);
    }

    if (inputChunk->ErasureCodec_ != NErasure::ECodec::None) {
        chunkSpec->set_erasure_codec(static_cast<int>(inputChunk->ErasureCodec_));
    }

    if (inputChunk->TableRowIndex_ > 0) {
        chunkSpec->set_table_row_index(inputChunk->TableRowIndex_);
    }

    if (inputChunk->RangeIndex_ > 0) {
        chunkSpec->set_range_index(inputChunk->RangeIndex_);
    }

    if (inputChunk->ChunkIndex_ > 0) {
        chunkSpec->set_chunk_index(inputChunk->ChunkIndex_);
    }

    if (inputChunk->TabletIndex_ >= 0) {
        chunkSpec->set_tablet_index(inputChunk->TabletIndex_);
    }

    if (inputChunk->LowerLimit_) {
        ToProto(chunkSpec->mutable_lower_limit(), *inputChunk->LowerLimit_);
    }
    if (inputChunk->UpperLimit_) {
        ToProto(chunkSpec->mutable_upper_limit(), *inputChunk->UpperLimit_);
    }

    chunkSpec->mutable_chunk_meta()->set_type(static_cast<int>(EChunkType::Table));
    chunkSpec->mutable_chunk_meta()->set_version(static_cast<int>(inputChunk->TableChunkFormat_));
    chunkSpec->mutable_chunk_meta()->mutable_extensions();
}

TString ToString(const TInputChunkPtr& inputChunk)
{
    TString boundaryKeys;
    if (inputChunk->BoundaryKeys()) {
        boundaryKeys = Format(
            "MinKey: %v, MaxKey: %v",
            inputChunk->BoundaryKeys()->MinKey,
            inputChunk->BoundaryKeys()->MaxKey);
    }
    return Format(
        "{ChunkId: %v, Replicas: %v, TableIndex: %v, ErasureCodec: %v, TableRowIndex: %v, "
        "RangeIndex: %v, ChunkIndex: %v, TabletIndex: %v, TableChunkFormat: %v, UncompressedDataSize: %v, RowCount: %v, "
        "CompressedDataSize: %v, DataWeight: %v, MaxBlockSize: %v, LowerLimit: %v, UpperLimit: %v, "
        "BoundaryKeys: {%v}, PartitionsExt: {%v}}",
        inputChunk->ChunkId(),
        JoinToString(inputChunk->Replicas()),
        inputChunk->GetTableIndex(),
        inputChunk->GetErasureCodec(),
        inputChunk->GetTableRowIndex(),
        inputChunk->GetRangeIndex(),
        inputChunk->GetChunkIndex(),
        inputChunk->GetTabletIndex(),
        inputChunk->GetTableChunkFormat(),
        inputChunk->GetUncompressedDataSize(),
        inputChunk->GetRowCount(),
        inputChunk->GetCompressedDataSize(),
        inputChunk->GetDataWeight(),
        inputChunk->GetMaxBlockSize(),
        inputChunk->LowerLimit() ? std::make_optional(*inputChunk->LowerLimit()) : std::nullopt,
        inputChunk->UpperLimit() ? std::make_optional(*inputChunk->UpperLimit()) : std::nullopt,
        inputChunk->BoundaryKeys() ? boundaryKeys : "",
        inputChunk->PartitionsExt() ? inputChunk->PartitionsExt()->ShortDebugString() : "");
}

////////////////////////////////////////////////////////////////////////////////

bool IsUnavailable(const TInputChunkPtr& inputChunk, bool checkParityParts)
{
    return IsUnavailable(inputChunk->GetReplicaList(), inputChunk->GetErasureCodec(), checkParityParts);
}

TChunkId EncodeChunkId(const TInputChunkPtr& inputChunk, TNodeId nodeId)
{
    auto replicaIt = std::find_if(
        inputChunk->Replicas().begin(),
        inputChunk->Replicas().end(),
        [=] (TChunkReplica replica) {
            return replica.GetNodeId() == nodeId;
        });
    YT_VERIFY(replicaIt != inputChunk->Replicas().end());

    TChunkIdWithIndexes chunkIdWithIndexes(
        inputChunk->ChunkId(),
        replicaIt->GetReplicaIndex(),
        0 /* mediumIndex */);
    return EncodeChunkId(chunkIdWithIndexes);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
