#include "table_commands.h"
#include "config.h"

#include <yt/ytlib/api/rowset.h>
#include <yt/ytlib/api/transaction.h>

#include <yt/ytlib/query_client/query_statistics.h>

#include <yt/ytlib/table_client/helpers.h>
#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/row_buffer.h>
#include <yt/ytlib/table_client/schemaful_writer.h>
#include <yt/ytlib/table_client/schemaless_chunk_reader.h>
#include <yt/ytlib/table_client/schemaless_chunk_writer.h>
#include <yt/ytlib/table_client/table_consumer.h>

#include <yt/ytlib/tablet_client/table_mount_cache.h>

#include <yt/ytlib/formats/config.h>
#include <yt/ytlib/formats/parser.h>

namespace NYT {
namespace NDriver {

using namespace NYson;
using namespace NYTree;
using namespace NFormats;
using namespace NChunkClient;
using namespace NQueryClient;
using namespace NConcurrency;
using namespace NTransactionClient;
using namespace NHiveClient;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NApi;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DriverLogger;

////////////////////////////////////////////////////////////////////////////////

TReadTableCommand::TReadTableCommand()
{
    RegisterParameter("path", Path);
    RegisterParameter("table_reader", TableReader)
        .Default(nullptr);
    RegisterParameter("control_attributes", ControlAttributes)
        .DefaultNew();
    RegisterParameter("unordered", Unordered)
        .Default(false);
}

void TReadTableCommand::OnLoaded()
{
    TCommandBase::OnLoaded();

    Path = Path.Normalize();
}

void TReadTableCommand::DoExecute(ICommandContextPtr context)
{
    Options.Ping = true;
    Options.Config = UpdateYsonSerializable(
        context->GetConfig()->TableReader,
        TableReader);

    auto reader = WaitFor(context->GetClient()->CreateTableReader(
        Path,
        Options))
        .ValueOrThrow();

    if (reader->GetTotalRowCount() > 0) {
        BuildYsonMapFluently(context->Request().ResponseParametersConsumer)
            .Item("start_row_index").Value(reader->GetTableRowIndex())
            .Item("approximate_row_count").Value(reader->GetTotalRowCount());
    } else {
        BuildYsonMapFluently(context->Request().ResponseParametersConsumer)
            .Item("approximate_row_count").Value(reader->GetTotalRowCount());
    }

    auto writer = CreateSchemalessWriterForFormat(
        context->GetOutputFormat(),
        reader->GetNameTable(),
        context->Request().OutputStream,
        false,
        ControlAttributes,
        0);

    PipeReaderToWriter(
        reader,
        writer,
        context->GetConfig()->ReadBufferRowCount);
}

//////////////////////////////////////////////////////////////////////////////////

TWriteTableCommand::TWriteTableCommand()
{
    RegisterParameter("path", Path);
    RegisterParameter("table_writer", TableWriter)
        .Default(nullptr);
}

void TWriteTableCommand::OnLoaded()
{
    TCommandBase::OnLoaded();

    Path = Path.Normalize();
}

void TWriteTableCommand::DoExecute(ICommandContextPtr context)
{
    auto transaction = AttachTransaction(context, false);

    auto config = UpdateYsonSerializable(
        context->GetConfig()->TableWriter,
        TableWriter);


    auto nameTable = New<TNameTable>();
    nameTable->SetEnableColumnNameValidation();

    auto options = New<TTableWriterOptions>();
    options->ValidateDuplicateIds = true;
    options->ValidateRowWeight = true;
    options->ValidateColumnCount = true;

    auto writer = CreateSchemalessTableWriter(
        config,
        options,
        Path,
        nameTable,
        context->GetClient(),
        transaction);

    WaitFor(writer->Open())
        .ThrowOnError();

    TWritingValueConsumer valueConsumer(
        writer,
        ConvertTo<TTypeConversionConfigPtr>(context->GetInputFormat().Attributes()));

    std::vector<IValueConsumer*> valueConsumers(1, &valueConsumer);
    TTableOutput output(CreateParserForFormat(
        context->GetInputFormat(),
        valueConsumers,
        0));

    PipeInputToOutput(context->Request().InputStream, &output, config->BlockSize);

    valueConsumer.Flush();

    WaitFor(writer->Close())
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

TMountTableCommand::TMountTableCommand()
{
    RegisterParameter("cell_id", Options.CellId)
        .Optional();
    RegisterParameter("freeze", Options.Freeze)
        .Optional();
}

void TMountTableCommand::DoExecute(ICommandContextPtr context)
{
    auto asyncResult = context->GetClient()->MountTable(
        Path.GetPath(),
        Options);
    WaitFor(asyncResult)
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

TUnmountTableCommand::TUnmountTableCommand()
{
    RegisterParameter("force", Options.Force)
        .Optional();
}

void TUnmountTableCommand::DoExecute(ICommandContextPtr context)
{
    auto asyncResult = context->GetClient()->UnmountTable(
        Path.GetPath(),
        Options);
    WaitFor(asyncResult)
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

void TRemountTableCommand::DoExecute(ICommandContextPtr context)
{
    auto asyncResult = context->GetClient()->RemountTable(
        Path.GetPath(),
        Options);
    WaitFor(asyncResult)
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

void TFreezeTableCommand::DoExecute(ICommandContextPtr context)
{
    auto asyncResult = context->GetClient()->FreezeTable(
        Path.GetPath(),
        Options);
    WaitFor(asyncResult)
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

void TUnfreezeTableCommand::DoExecute(ICommandContextPtr context)
{
    auto asyncResult = context->GetClient()->UnfreezeTable(
        Path.GetPath(),
        Options);
    WaitFor(asyncResult)
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

TReshardTableCommand::TReshardTableCommand()
{
    RegisterParameter("pivot_keys", PivotKeys)
        .Default();
    RegisterParameter("tablet_count", TabletCount)
        .Default()
        .GreaterThan(0);

    RegisterValidator([&] () {
        if (PivotKeys && TabletCount) {
            THROW_ERROR_EXCEPTION("Cannot specify both \"pivot_keys\" and \"tablet_count\"");
        }
        if (!PivotKeys && !TabletCount) {
            THROW_ERROR_EXCEPTION("Must specify either \"pivot_keys\" or \"tablet_count\"");
        }
    });
}

void TReshardTableCommand::DoExecute(ICommandContextPtr context)
{
    TFuture<void> asyncResult;
    if (PivotKeys) {
        asyncResult = context->GetClient()->ReshardTable(
            Path.GetPath(),
            *PivotKeys,
            Options);
    } else {
        asyncResult = context->GetClient()->ReshardTable(
            Path.GetPath(),
            *TabletCount,
            Options);
    }
    WaitFor(asyncResult)
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

TAlterTableCommand::TAlterTableCommand()
{
    RegisterParameter("path", Path);
    RegisterParameter("schema", Options.Schema)
        .Optional();
    RegisterParameter("dynamic", Options.Dynamic)
        .Optional();
}

void TAlterTableCommand::DoExecute(ICommandContextPtr context)
{
    auto asyncResult = context->GetClient()->AlterTable(
        Path.GetPath(),
        Options);
    WaitFor(asyncResult)
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

TSelectRowsCommand::TSelectRowsCommand()
{
    RegisterParameter("query", Query);
    RegisterParameter("timestamp", Options.Timestamp)
        .Optional();
    RegisterParameter("input_row_limit", Options.InputRowLimit)
        .Optional();
    RegisterParameter("output_row_limit", Options.OutputRowLimit)
        .Optional();
    RegisterParameter("range_expansion_limit", Options.RangeExpansionLimit)
        .Optional();
    RegisterParameter("fail_on_incomplete_result", Options.FailOnIncompleteResult)
        .Optional();
    RegisterParameter("verbose_logging", Options.VerboseLogging)
        .Optional();
    RegisterParameter("enable_code_cache", Options.EnableCodeCache)
        .Optional();
    RegisterParameter("max_subqueries", Options.MaxSubqueries)
        .Optional();
    RegisterParameter("workload_descriptor", Options.WorkloadDescriptor)
        .Optional();
}

void TSelectRowsCommand::DoExecute(ICommandContextPtr context)
{
    auto clientBase = GetClientBase(context);
    auto result = WaitFor(clientBase->SelectRows(Query, Options))
        .ValueOrThrow();

    const auto& rowset = result.Rowset;
    const auto& statistics = result.Statistics;

    auto format = context->GetOutputFormat();
    auto output = context->Request().OutputStream;
    auto writer = CreateSchemafulWriterForFormat(format, rowset->Schema(), output);

    writer->Write(rowset->Rows());

    WaitFor(writer->Close())
        .ThrowOnError();

    LOG_INFO("Query result statistics (RowsRead: %v, RowsWritten: %v, AsyncTime: %v, SyncTime: %v, ExecuteTime: %v, "
        "ReadTime: %v, WriteTime: %v, IncompleteInput: %v, IncompleteOutput: %v)",
        statistics.RowsRead,
        statistics.RowsWritten,
        statistics.AsyncTime.MilliSeconds(),
        statistics.SyncTime.MilliSeconds(),
        statistics.ExecuteTime.MilliSeconds(),
        statistics.ReadTime.MilliSeconds(),
        statistics.WriteTime.MilliSeconds(),
        statistics.IncompleteInput,
        statistics.IncompleteOutput);
}

////////////////////////////////////////////////////////////////////////////////

namespace {

std::vector<TUnversionedRow> ParseRows(
    ICommandContextPtr context,
    TTableWriterConfigPtr config,
    TBuildingValueConsumer* valueConsumer)
{
    std::vector<IValueConsumer*> valueConsumers(1, valueConsumer);
    TTableOutput output(CreateParserForFormat(
        context->GetInputFormat(),
        valueConsumers,
        0));

    auto input = CreateSyncAdapter(context->Request().InputStream);
    PipeInputToOutput(input.get(), &output, config->BlockSize);
    return valueConsumer->GetRows();
}

} // namespace

TInsertRowsCommand::TInsertRowsCommand()
{
    RegisterParameter("table_writer", TableWriter)
        .Default();
    RegisterParameter("path", Path);
    RegisterParameter("update", Update)
        .Default(false);
    RegisterParameter("aggregate", Aggregate)
        .Default(false);
}

void TInsertRowsCommand::DoExecute(ICommandContextPtr context)
{
    auto config = UpdateYsonSerializable(
        context->GetConfig()->TableWriter,
        TableWriter);

    auto tableMountCache = context->GetClient()->GetConnection()->GetTableMountCache();
    auto tableInfo = WaitFor(tableMountCache->GetTableInfo(Path.GetPath()))
        .ValueOrThrow();

    tableInfo->ValidateDynamic();

    if (!tableInfo->IsSorted() && Update) {
        THROW_ERROR_EXCEPTION("Cannot use \"update\" mode for ordered tables");
    }

    struct TInsertRowsBufferTag
    { };

    // Parse input data.
    TBuildingValueConsumer valueConsumer(
        tableInfo->Schemas[ETableSchemaKind::Write],
        ConvertTo<TTypeConversionConfigPtr>(context->GetInputFormat().Attributes()));
    valueConsumer.SetAggregate(Aggregate);
    valueConsumer.SetTreatMissingAsNull(!Update);

    auto rows = ParseRows(context, config, &valueConsumer);
    auto rowBuffer = New<TRowBuffer>(TInsertRowsBufferTag());
    auto capturedRows = rowBuffer->Capture(rows);
    auto rowRange = MakeSharedRange(
        std::vector<TUnversionedRow>(capturedRows.begin(), capturedRows.end()),
        std::move(rowBuffer));

    // Run writes.
    auto transaction = GetTransaction(context);

    transaction->WriteRows(
        Path.GetPath(),
        valueConsumer.GetNameTable(),
        std::move(rowRange));

    if (ShouldCommitTransaction()) {
        WaitFor(transaction->Commit())
            .ThrowOnError();
    }
}

////////////////////////////////////////////////////////////////////////////////

TLookupRowsCommand::TLookupRowsCommand()
{
    RegisterParameter("table_writer", TableWriter)
        .Default();
    RegisterParameter("path", Path);
    RegisterParameter("column_names", ColumnNames)
        .Default();
    RegisterParameter("timestamp", Options.Timestamp)
        .Optional();
    RegisterParameter("keep_missing_rows", Options.KeepMissingRows)
        .Optional();
}

void TLookupRowsCommand::DoExecute(ICommandContextPtr context)
{
    auto tableMountCache = context->GetClient()->GetConnection()->GetTableMountCache();
    auto asyncTableInfo = tableMountCache->GetTableInfo(Path.GetPath());
    auto tableInfo = WaitFor(asyncTableInfo)
        .ValueOrThrow();

    tableInfo->ValidateDynamic();

    auto config = UpdateYsonSerializable(
        context->GetConfig()->TableWriter,
        TableWriter);

    struct TLookupRowsBufferTag
    { };

    // Parse input data.
    TBuildingValueConsumer valueConsumer(
        tableInfo->Schemas[ETableSchemaKind::Lookup],
        ConvertTo<TTypeConversionConfigPtr>(context->GetInputFormat().Attributes()));
    auto keys = ParseRows(context, config, &valueConsumer);
    auto rowBuffer = New<TRowBuffer>(TLookupRowsBufferTag());
    auto capturedKeys = rowBuffer->Capture(keys);
    auto mutableKeyRange = MakeSharedRange(std::move(capturedKeys), std::move(rowBuffer));
    auto keyRange = TSharedRange<TUnversionedRow>(
        static_cast<const TUnversionedRow*>(mutableKeyRange.Begin()),
        static_cast<const TUnversionedRow*>(mutableKeyRange.End()),
        mutableKeyRange.GetHolder());
    auto nameTable = valueConsumer.GetNameTable();

    if (ColumnNames) {
        Options.ColumnFilter.All = false;
        for (const auto& name : *ColumnNames) {
            auto maybeIndex = nameTable->FindId(name);
            if (!maybeIndex) {
                if (!tableInfo->Schemas[ETableSchemaKind::Primary].FindColumn(name)) {
                    THROW_ERROR_EXCEPTION("No such column %Qv",
                        name);
                }
                maybeIndex = nameTable->GetIdOrRegisterName(name);
            }
            Options.ColumnFilter.Indexes.push_back(*maybeIndex);
        }
    }

    // Run lookup.
    auto clientBase = GetClientBase(context);
    auto asyncRowset = clientBase->LookupRows(
        Path.GetPath(),
        std::move(nameTable),
        std::move(keyRange),
        Options);
    auto rowset = WaitFor(asyncRowset)
        .ValueOrThrow();

    auto format = context->GetOutputFormat();
    auto output = context->Request().OutputStream;
    auto writer = CreateSchemafulWriterForFormat(format, rowset->Schema(), output);

    writer->Write(rowset->Rows());

    WaitFor(writer->Close())
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

TDeleteRowsCommand::TDeleteRowsCommand()
{
    RegisterParameter("table_writer", TableWriter)
        .Default();
    RegisterParameter("path", Path);
}

void TDeleteRowsCommand::DoExecute(ICommandContextPtr context)
{
    auto config = UpdateYsonSerializable(
        context->GetConfig()->TableWriter,
        TableWriter);

    auto tableMountCache = context->GetClient()->GetConnection()->GetTableMountCache();
    auto asyncTableInfo = tableMountCache->GetTableInfo(Path.GetPath());
    auto tableInfo = WaitFor(asyncTableInfo)
        .ValueOrThrow();

    tableInfo->ValidateDynamic();

    struct TDeleteRowsBufferTag
    { };

    // Parse input data.
    TBuildingValueConsumer valueConsumer(
        tableInfo->Schemas[ETableSchemaKind::Delete],
        ConvertTo<TTypeConversionConfigPtr>(context->GetInputFormat().Attributes()));
    auto keys = ParseRows(context, config, &valueConsumer);
    auto rowBuffer = New<TRowBuffer>(TDeleteRowsBufferTag());
    auto capturedKeys = rowBuffer->Capture(keys);
    auto keyRange = MakeSharedRange(
        std::vector<TKey>(capturedKeys.begin(), capturedKeys.end()),
        std::move(rowBuffer));

    // Run deletes.
    auto transaction = GetTransaction(context);

    transaction->DeleteRows(
        Path.GetPath(),
        valueConsumer.GetNameTable(),
        std::move(keyRange));

    if (ShouldCommitTransaction()) {
        WaitFor(transaction->Commit())
            .ThrowOnError();
    }
}

////////////////////////////////////////////////////////////////////////////////

TTrimRowsCommand::TTrimRowsCommand()
{
    RegisterParameter("path", Path);
    RegisterParameter("tablet_index", TabletIndex);
    RegisterParameter("trimmed_row_count", TrimmedRowCount);
}

void TTrimRowsCommand::DoExecute(ICommandContextPtr context)
{
    auto client = context->GetClient();
    auto asyncResult = client->TrimTable(
        Path.GetPath(),
        TabletIndex,
        TrimmedRowCount,
        Options);
    WaitFor(asyncResult)
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

TEnableTableReplicaCommand::TEnableTableReplicaCommand()
{
    RegisterParameter("replica_id", ReplicaId);
}

void TEnableTableReplicaCommand::DoExecute(ICommandContextPtr context)
{
    auto client = context->GetClient();
    auto asyncResult = client->EnableTableReplica(
        ReplicaId,
        Options);
    WaitFor(asyncResult)
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

TDisableTableReplicaCommand::TDisableTableReplicaCommand()
{
    RegisterParameter("replica_id", ReplicaId);
}

void TDisableTableReplicaCommand::DoExecute(ICommandContextPtr context)
{
    auto client = context->GetClient();
    auto asyncResult = client->DisableTableReplica(
        ReplicaId,
        Options);
    WaitFor(asyncResult)
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
