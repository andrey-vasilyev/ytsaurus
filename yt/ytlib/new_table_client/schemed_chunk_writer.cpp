#include "stdafx.h"

#include "schemed_chunk_writer.h"

#include "schemed_block_writer.h"
#include "chunk_meta_extensions.h"
#include "config.h"
#include "name_table.h"
#include "private.h"
#include "schema.h"
#include "schemed_writer.h"
#include "unversioned_row.h"
#include "writer.h"

#include <ytlib/table_client/chunk_meta_extensions.h>

#include <ytlib/chunk_client/async_writer.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>
#include <ytlib/chunk_client/dispatcher.h>
#include <ytlib/chunk_client/encoding_writer.h>

#include <core/concurrency/fiber.h>

#include <core/misc/error.h>

namespace NYT {
namespace NVersionedTableClient {

using namespace NChunkClient;
using namespace NConcurrency;

using NChunkClient::NProto::TChunkMeta;
using NProto::TBlockMetaExt;

static const int TimestampIndex = 0;

////////////////////////////////////////////////////////////////////////////////

class TChunkWriter
    : public IWriter
    , public ISchemedWriter
{
public:
    TChunkWriter(
        TChunkWriterConfigPtr config,
        TChunkWriterOptionsPtr options,
        IAsyncWriterPtr asyncWriter);

    // ToDo(psushin): deprecated.
    virtual void Open(
        TNameTablePtr nameTable,
        const TTableSchema& schema,
        const TKeyColumns& keyColumns = TKeyColumns()) final override;

    // ToDo(psushin): deprecated.
    virtual void WriteValue(const TUnversionedValue& value) final override;

    // ToDo(psushin): deprecated.
    virtual bool EndRow() final override;

    virtual TAsyncError Open(
        const TTableSchema& schema,
        const TNullable<TKeyColumns>& keyColumns) final override;

    virtual bool Write(const std::vector<TUnversionedRow>& rows) final override;

    virtual TAsyncError GetReadyEvent() final override;

    virtual TAsyncError Close() final override;

    virtual i64 GetRowIndex() const final override;

private:
    struct TColumnDescriptor
    {
        TColumnDescriptor()
            : IndexInBlock(-1)
            , OutputIndex(-1)
            , Type(EValueType::Null)
            , IsKeyPart(false)
            , PreviousValue()
        { }

        int IndexInBlock;
        int OutputIndex;
        EValueType Type;
        // Used for versioned rowsets.
        bool IsKeyPart;

        union {
            i64 Integer;
            double Double;
            struct {
                const char* String;
                size_t Length;
            };
        } PreviousValue;

    };

    TChunkWriterConfigPtr Config;
    TEncodingWriterOptionsPtr Options;
    IAsyncWriterPtr UnderlyingWriter;

    std::vector<int> KeyIds;
    TNameTablePtr InputNameTable;
    TNameTablePtr OutputNameTable;

    // Vector is indexed by InputNameTable indexes.
    std::vector<TColumnDescriptor> ColumnDescriptors;

    TEncodingWriterPtr EncodingWriter;

    std::unique_ptr<TBlockWriter> PreviousBlock;
    std::unique_ptr<TBlockWriter> CurrentBlock;

    bool IsNewKey;

    i64 RowIndex;
    i64 LargestBlockSize;

    // Column sizes for block writer.
    std::vector<int> ColumnSizes;

    TChunkMeta Meta;
    TBlockMetaExt BlockMetaExt;
    TTableSchema Schema;

    void DoClose(TAsyncErrorPromise result);
    void FlushPreviousBlock();

};

////////////////////////////////////////////////////////////////////////////////

TChunkWriter::TChunkWriter(
    TChunkWriterConfigPtr config,
    TEncodingWriterOptionsPtr options,
    IAsyncWriterPtr asyncWriter)
    : Config(config)
    , Options(options)
    , UnderlyingWriter(asyncWriter)
    , OutputNameTable(New<TNameTable>())
    , EncodingWriter(New<TEncodingWriter>(config, options, asyncWriter))
    , IsNewKey(false)
    , RowIndex(0)
    , LargestBlockSize(0)
{ }

TAsyncError TChunkWriter::Open(
   const TTableSchema& schema,
   const TNullable<TKeyColumns>& keyColumns)
{
    auto nameTable = New<TNameTable>();
    for (int i = 0; i < schema.Columns().size(); ++i) {
        YCHECK(i == nameTable->RegisterName(schema.Columns()[i].Name));
    }
    Open(nameTable, schema, *keyColumns);
    return MakeFuture(TError());
}

void TChunkWriter::Open(
    TNameTablePtr nameTable,
    const TTableSchema& schema,
    const TKeyColumns& keyColumns)
{
    Schema = schema;
    InputNameTable = nameTable;

    // Integers and Doubles align at 8 bytes (stores the whole value),
    // while String and Any align at 4 bytes (stores just offset to value).
    // To ensure proper alignment during reading, move all integer and
    // double columns to the front.
    std::sort(
        Schema.Columns().begin(),
        Schema.Columns().end(),
        [] (const TColumnSchema& lhs, const TColumnSchema& rhs) {
            auto isFront = [] (const TColumnSchema& schema) {
                return schema.Type == EValueType::Integer || schema.Type == EValueType::Double;
            };
            if (isFront(lhs) && !isFront(rhs)) {
                return true;
            }
            if (!isFront(lhs) && isFront(rhs)) {
                return false;
            }
            return &lhs < &rhs;
        }
    );

    ColumnDescriptors.resize(InputNameTable->GetSize());

    for (const auto& column : Schema.Columns()) {
        TColumnDescriptor descriptor;
        descriptor.IndexInBlock = ColumnSizes.size();
        descriptor.OutputIndex = OutputNameTable->RegisterName(column.Name);
        descriptor.Type = column.Type;

        if (column.Type == EValueType::String || column.Type == EValueType::Any) {
            ColumnSizes.push_back(4);
        } else {
            ColumnSizes.push_back(8);
        }

        auto id = InputNameTable->GetId(column.Name);
        ColumnDescriptors[id] = descriptor;
    }

    for (const auto& column : keyColumns) {
        auto id = InputNameTable->GetId(column);
        KeyIds.push_back(id);

        auto& descriptor = ColumnDescriptors[id];
        YCHECK(descriptor.IndexInBlock >= 0);
        YCHECK(descriptor.Type != EValueType::Any);
        descriptor.IsKeyPart = true;
    }

    CurrentBlock.reset(new TBlockWriter(ColumnSizes));
}

bool TChunkWriter::Write(const std::vector<TUnversionedRow>& rows)
{
    bool res = true;
    for (const auto& row : rows) {
        for (auto it = row.Begin(); it != row.End(); ++it) {
            WriteValue(*it);
        }
        res = EndRow();
    }

    return res;
}

void TChunkWriter::WriteValue(const TUnversionedValue& value)
{
    if (ColumnDescriptors.size() <= value.Id) {
        ColumnDescriptors.resize(value.Id + 1);
    }

    auto& columnDescriptor = ColumnDescriptors[value.Id];

    if (columnDescriptor.Type == EValueType::Null) {
        // Uninitialized column becomes variable.
        columnDescriptor.Type = EValueType::TheBottom;
        columnDescriptor.OutputIndex =
            OutputNameTable->RegisterName(InputNameTable->GetName(value.Id));
    }

    switch (columnDescriptor.Type) {
        case EValueType::Integer:
            YASSERT(value.Type == EValueType::Integer || value.Type == EValueType::Null);
            CurrentBlock->WriteInteger(value, columnDescriptor.IndexInBlock);
            if (columnDescriptor.IsKeyPart) {
                if (value.Data.Integer != columnDescriptor.PreviousValue.Integer) {
                    IsNewKey = true;
                }
                columnDescriptor.PreviousValue.Integer = value.Data.Integer;
            }
            break;

        case EValueType::Double:
            YASSERT(value.Type == EValueType::Double || value.Type == EValueType::Null);
            CurrentBlock->WriteDouble(value, columnDescriptor.IndexInBlock);
            if (columnDescriptor.IsKeyPart) {
                if (value.Data.Double != columnDescriptor.PreviousValue.Double) {
                    IsNewKey = true;
                }
                columnDescriptor.PreviousValue.Double = value.Data.Double;
            }
            break;

        case EValueType::String:
            YASSERT(value.Type == EValueType::String || value.Type == EValueType::Null);
            if (columnDescriptor.IsKeyPart) {
                auto newKey = CurrentBlock->WriteKeyString(value, columnDescriptor.IndexInBlock);
                auto oldKey = TStringBuf(
                    columnDescriptor.PreviousValue.String,
                    columnDescriptor.PreviousValue.Length);
                if (newKey != oldKey) {
                    IsNewKey = true;
                }
                columnDescriptor.PreviousValue.String = newKey.data();
                columnDescriptor.PreviousValue.Length = newKey.length();
            } else {
                CurrentBlock->WriteString(value, columnDescriptor.IndexInBlock);
            }
            break;

        case EValueType::Any:
            CurrentBlock->WriteAny(value, columnDescriptor.IndexInBlock);
            break;

        // Variable column.
        case EValueType::TheBottom:
            CurrentBlock->WriteVariable(value, columnDescriptor.OutputIndex);
            break;

        default:
            YUNREACHABLE();
    }
}

bool TChunkWriter::EndRow()
{

    CurrentBlock->EndRow();

    if (PreviousBlock) {
        FlushPreviousBlock();

        /*
        if (!KeyIds.empty()) {
            auto* key = IndexExt.add_keys();
            for (const auto& id : KeyIds) {
                auto* part = key->add_parts();
                const auto& column = ColumnDescriptors[id];
                part->set_type(column.Type);
                switch (column.Type) {
                    case EValueType::Integer:
                        part->set_int_value(column.PreviousValue.Integer);
                        break;
                    case EValueType::Double:
                        part->set_double_value(column.PreviousValue.Double);
                        break;
                    case EValueType::String:
                        part->set_str_value(
                            column.PreviousValue.String,
                            column.PreviousValue.Length);
                        break;
                    default:
                        YUNREACHABLE();
                }
            }
        }*/
    }

    if (CurrentBlock->GetSize() > Config->BlockSize) {
        YCHECK(PreviousBlock.get() == nullptr);
        PreviousBlock.swap(CurrentBlock);
        CurrentBlock.reset(new TBlockWriter(ColumnSizes));
    }

    ++RowIndex;
    return EncodingWriter->IsReady();
}

TAsyncError TChunkWriter::GetReadyEvent()
{
    return EncodingWriter->GetReadyEvent();
}

TAsyncError TChunkWriter::Close()
{
    auto result = NewPromise<TError>();

    TDispatcher::Get()->GetWriterInvoker()->Invoke(BIND(
        &TChunkWriter::DoClose,
        MakeWeak(this),
        result));

    return result;
}

i64 TChunkWriter::GetRowIndex() const 
{
    return RowIndex;
}

void TChunkWriter::DoClose(TAsyncErrorPromise result)
{
    if (CurrentBlock->GetSize() > 0) {
        YCHECK(PreviousBlock == nullptr);
        PreviousBlock.swap(CurrentBlock);
    }

    if (PreviousBlock) {
        FlushPreviousBlock();
    }

    {
        auto error = WaitFor(EncodingWriter->AsyncFlush());
        if (!error.IsOK()) {
            result.Set(error);
            return;
        }
    }

    Meta.set_type(EChunkType::Table);
    Meta.set_version(FormatVersion);

    SetProtoExtension(Meta.mutable_extensions(), BlockMetaExt);
    SetProtoExtension(Meta.mutable_extensions(), NYT::ToProto<NProto::TTableSchemaExt>(Schema));

    NProto::TNameTableExt nameTableExt;
    ToProto(&nameTableExt, OutputNameTable);
    SetProtoExtension(Meta.mutable_extensions(), nameTableExt);

    NChunkClient::NProto::TMiscExt miscExt;
    if (KeyIds.empty()) {
        miscExt.set_sorted(false);
    } else {
        miscExt.set_sorted(true);

//      SetProtoExtension(Meta.mutable_extensions(), IndexExt);
        NTableClient::NProto::TKeyColumnsExt keyColumnsExt;
        for (int id : KeyIds) {
            keyColumnsExt.add_names(InputNameTable->GetName(id));
        }
        SetProtoExtension(Meta.mutable_extensions(), keyColumnsExt);
    }

    miscExt.set_uncompressed_data_size(EncodingWriter->GetUncompressedSize());
    miscExt.set_compressed_data_size(EncodingWriter->GetCompressedSize());
    miscExt.set_meta_size(Meta.ByteSize());
    miscExt.set_compression_codec(Options->CompressionCodec);
    miscExt.set_row_count(RowIndex);
    miscExt.set_max_block_size(LargestBlockSize);
    SetProtoExtension(Meta.mutable_extensions(), miscExt);

    auto error = WaitFor(UnderlyingWriter->AsyncClose(Meta));
    result.Set(error);
}

void TChunkWriter::FlushPreviousBlock()
{
    auto block = PreviousBlock->FlushBlock();
    EncodingWriter->WriteBlock(std::move(block.Data));
    block.Meta.set_chunk_row_count(GetRowIndex());
    *BlockMetaExt.add_entries() = block.Meta;
    if (block.Meta.block_size() > LargestBlockSize) {
        LargestBlockSize = block.Meta.block_size();
    }
    PreviousBlock.reset();
}

////////////////////////////////////////////////////////////////////////////////

IWriterPtr CreateChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    IAsyncWriterPtr asyncWriter)
{
    return New<TChunkWriter>(config, options, asyncWriter);
}

////////////////////////////////////////////////////////////////////////////////

ISchemedWriterPtr CreateSchemedChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    NChunkClient::IAsyncWriterPtr asyncWriter)
{
    return New<TChunkWriter>(config, options, asyncWriter);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
