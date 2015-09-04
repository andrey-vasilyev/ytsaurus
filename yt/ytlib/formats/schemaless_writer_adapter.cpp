#include "stdafx.h"

#include "schemaless_writer_adapter.h"

#include <ytlib/new_table_client/name_table.h>

#include <core/actions/future.h>

#include <core/concurrency/async_stream.h>

#include <core/misc/error.h>

#include <core/yson/consumer.h>

#include <core/ytree/fluent.h>

namespace NYT {
namespace NFormats {

using namespace NVersionedTableClient;
using namespace NConcurrency;
using namespace NYson;
using namespace NYTree;


////////////////////////////////////////////////////////////////////////////////

TSchemalessWriterAdapter::TSchemalessWriterAdapter(
    const TFormat& format,
    TNameTablePtr nameTable,
    IAsyncOutputStreamPtr output,
    bool enableContextSaving,
    bool enableKeySwitch,
    int keyColumnCount)
    : TContextSavingMixin(enableContextSaving, CreateSyncAdapter(std::move(output)))
    , NameTable_(nameTable)
    , EnableKeySwitch_(enableKeySwitch)
    , KeyColumnCount_(keyColumnCount)
{
    Consumer_ = CreateConsumerForFormat(format, EDataType::Tabular, GetOutputStream());
}

TFuture<void> TSchemalessWriterAdapter::Open()
{
    return VoidFuture;
}

bool TSchemalessWriterAdapter::Write(const std::vector<TUnversionedRow> &rows)
{
    try {
        for (const auto& row : rows) {
            if (EnableKeySwitch_) {
                try {
                    if (CurrentKey_ && CompareRows(row, CurrentKey_, KeyColumnCount_)) {
                        WriteControlAttribute(EControlAttribute::KeySwitch, true);
                    }
                    CurrentKey_ = row;
                } catch (const std::exception& ex) {
                    // COMPAT(psushin): composite values are not comparable anymore.
                    THROW_ERROR_EXCEPTION("Cannot inject key switch into output stream") << ex;
                }
            }

            ConsumeRow(row);
            TryFlushBuffer();
        }

        TryFlushBuffer();

        if (EnableKeySwitch_ && CurrentKey_) {
            LastKey_ = GetKeyPrefix(CurrentKey_, KeyColumnCount_);
            CurrentKey_ = LastKey_.Get();
        }
    } catch (const std::exception& ex) {
        Error_ = TError(ex);
        return false;
    }

    return true;
}

TFuture<void> TSchemalessWriterAdapter::GetReadyEvent()
{
    return MakeFuture(Error_);
}

TFuture<void> TSchemalessWriterAdapter::Close()
{
    try {
        TContextSavingMixin::Close();
    } catch (const std::exception& ex) {
        Error_ = TError(ex);
    }

    return MakeFuture(Error_);
}

TNameTablePtr TSchemalessWriterAdapter::GetNameTable() const
{
    return NameTable_;
}

bool TSchemalessWriterAdapter::IsSorted() const
{
    return false;
}

TBlob TSchemalessWriterAdapter::GetContext() const
{
    return TContextSavingMixin::GetContext();
}

void TSchemalessWriterAdapter::WriteTableIndex(int tableIndex)
{
    WriteControlAttribute(EControlAttribute::TableIndex, tableIndex);
}

void TSchemalessWriterAdapter::WriteRangeIndex(i32 rangeIndex)
{
    WriteControlAttribute(EControlAttribute::RangeIndex, rangeIndex);
}

void TSchemalessWriterAdapter::WriteRowIndex(i64 rowIndex)
{
    WriteControlAttribute(EControlAttribute::RowIndex, rowIndex);
}

template <class T>
void TSchemalessWriterAdapter::WriteControlAttribute(
    EControlAttribute controlAttribute,
    T value)
{
    BuildYsonListFluently(Consumer_.get())
        .Item()
        .BeginAttributes()
            .Item(FormatEnum(controlAttribute)).Value(value)
        .EndAttributes()
        .Entity();
}

void TSchemalessWriterAdapter::ConsumeRow(const TUnversionedRow& row)
{
    Consumer_->OnListItem();
    Consumer_->OnBeginMap();
    for (auto* it = row.Begin(); it != row.End(); ++it) {
        auto& value = *it;

        Consumer_->OnKeyedItem(NameTable_->GetName(value.Id));
        switch (value.Type) {
            case EValueType::Int64:
                Consumer_->OnInt64Scalar(value.Data.Int64);
                break;
            case EValueType::Uint64:
                Consumer_->OnUint64Scalar(value.Data.Uint64);
                break;
            case EValueType::Double:
                Consumer_->OnDoubleScalar(value.Data.Double);
                break;
            case EValueType::Boolean:
                Consumer_->OnBooleanScalar(value.Data.Boolean);
                break;
            case EValueType::String:
                Consumer_->OnStringScalar(TStringBuf(value.Data.String, value.Length));
                break;
            case EValueType::Null:
                Consumer_->OnEntity();
                break;
            case EValueType::Any:
                Consumer_->OnRaw(TStringBuf(value.Data.String, value.Length), EYsonType::Node);
                break;
            default:
                YUNREACHABLE();
        }
    }
    Consumer_->OnEndMap();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
