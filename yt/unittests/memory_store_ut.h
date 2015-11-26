
#include "stdafx.h"
#include "framework.h"
#include "versioned_table_client_ut.h"

#include <yt/core/misc/nullable.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/ytlib/table_client/public.h>
#include <yt/ytlib/table_client/schema.h>
#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/writer.h>
#include <yt/ytlib/table_client/schemaful_chunk_reader.h>
#include <yt/ytlib/table_client/schemaful_chunk_writer.h>
#include <yt/ytlib/table_client/versioned_row.h>
#include <yt/ytlib/table_client/unversioned_row.h>
#include <yt/ytlib/table_client/versioned_reader.h>

#include <yt/ytlib/chunk_client/config.h>
#include <yt/ytlib/chunk_client/memory_reader.h>
#include <yt/ytlib/chunk_client/memory_writer.h>

#include <yt/ytlib/tablet_client/public.h>
#include <yt/ytlib/tablet_client/config.h>

#include <ytlib/query_client/column_evaluator.h>
#include <ytlib/query_client/config.h>

#include <yt/server/tablet_node/public.h>
#include <yt/server/tablet_node/config.h>
#include <yt/server/tablet_node/tablet.h>
#include <yt/server/tablet_node/tablet_manager.h>
#include <yt/server/tablet_node/transaction.h>
#include <yt/server/tablet_node/dynamic_memory_store.h>

namespace NYT {
namespace NTabletNode {
namespace {

using namespace NTabletClient;
using namespace NTableClient;
using namespace NObjectClient;
using namespace NTransactionClient;
using namespace NQueryClient;
using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TMemoryStoreTestBase
    : public ::testing::Test
{
protected:
    virtual void SetUp() override
    {
        auto schema = GetSchema();

        for (const auto& column : schema.Columns()) {
            NameTable_->RegisterName(column.Name);
        }

        auto columnEvaluatorCache = New<TColumnEvaluatorCache>(
            New<TColumnEvaluatorCacheConfig>(),
            CreateBuiltinFunctionRegistry());

        Tablet_.reset(new TTablet(
            New<TTableMountConfig>(),
            New<TTabletWriterOptions>(),
            NullTabletId,
            NullObjectId,
            nullptr,
            columnEvaluatorCache,
            schema,
            schema.GetKeyColumns(),
            MinKey(),
            MaxKey(),
            GetAtomicity()));
        Tablet_->CreateInitialPartition();
        Tablet_->StartEpoch(nullptr);
    }

    virtual TTableSchema GetSchema() const
    {
        // NB: Key columns must go first.
        TTableSchema schema;
        schema.Columns().push_back(TColumnSchema("key", EValueType::Int64, Null, Null, Null, ESortOrder::Ascending));
        schema.Columns().push_back(TColumnSchema("a", EValueType::Int64));
        schema.Columns().push_back(TColumnSchema("b", EValueType::Double));
        schema.Columns().push_back(TColumnSchema("c", EValueType::String));
        return schema;
    }

    virtual EAtomicity GetAtomicity() const
    {
        return EAtomicity::Full;
    }

    TUnversionedOwningRow BuildRow(const Stroka& yson, bool treatMissingAsNull = true)
    {
        return NTableClient::BuildRow(yson, Tablet_->KeyColumns(), Tablet_->Schema(), treatMissingAsNull);
    }

    TTimestamp GenerateTimestamp()
    {
        return CurrentTimestamp_++;
    }


    std::unique_ptr<TTransaction> StartTransaction(TTimestamp startTimestamp = NullTimestamp)
    {
        std::unique_ptr<TTransaction> transaction(new TTransaction(TTransactionId::Create()));
        transaction->SetStartTimestamp(startTimestamp == NullTimestamp ? GenerateTimestamp() : startTimestamp);
        transaction->SetState(ETransactionState::Active);
        return transaction;
    }

    void PrepareTransaction(TTransaction* transaction)
    {
        EXPECT_EQ(ETransactionState::Active, transaction->GetState());
        transaction->SetPrepareTimestamp(GenerateTimestamp());
        transaction->SetState(ETransactionState::TransientCommitPrepared);
    }

    NTransactionClient::TTimestamp CommitTransaction(TTransaction* transaction)
    {
        EXPECT_EQ(ETransactionState::TransientCommitPrepared, transaction->GetState());
        transaction->SetCommitTimestamp(GenerateTimestamp());
        transaction->SetState(ETransactionState::Committed);
        return transaction->GetCommitTimestamp();
    }

    void AbortTransaction(TTransaction* transaction)
    {
        transaction->SetState(ETransactionState::Aborted);
    }


    bool AreRowsEqual(const TUnversionedOwningRow& row, const Stroka& yson)
    {
        return AreRowsEqual(row, yson.c_str());
    }

    bool AreRowsEqual(const TUnversionedOwningRow& row, const char* yson)
    {
        if (!row && !yson) {
            return true;
        }

        if (!row || !yson) {
            return false;
        }

        auto expectedRowParts = ConvertTo<yhash_map<Stroka, INodePtr>>(
            TYsonString(yson, EYsonType::MapFragment));

        for (int index = 0; index < row.GetCount(); ++index) {
            const auto& value = row[index];
            const auto& name = NameTable_->GetName(value.Id);
            auto it = expectedRowParts.find(name);
            switch (value.Type) {
                case EValueType::Int64:
                    if (it == expectedRowParts.end()) {
                        return false;
                    }
                    if (it->second->GetValue<i64>() != value.Data.Int64) {
                        return false;
                    }
                    break;

                case EValueType::Uint64:
                    if (it == expectedRowParts.end()) {
                        return false;
                    }
                    if (it->second->GetValue<ui64>() != value.Data.Uint64) {
                        return false;
                    }
                    break;

                case EValueType::Double:
                    if (it == expectedRowParts.end()) {
                        return false;
                    }
                    if (it->second->GetValue<double>() != value.Data.Double) {
                        return false;
                    }
                    break;

                case EValueType::String:
                    if (it == expectedRowParts.end()) {
                        return false;
                    }
                    if (it->second->GetValue<Stroka>() != Stroka(value.Data.String, value.Length)) {
                        return false;
                    }
                    break;

                case EValueType::Null:
                    if (it != expectedRowParts.end()) {
                        return false;
                    }
                    break;

                default:
                    YUNREACHABLE();
            }
        }

        return true;
    }

    TUnversionedOwningRow LookupRow(IStorePtr store, const TOwningKey& key, TTimestamp timestamp)
    {
        std::vector<TKey> lookupKeys(1, key.Get());
        auto sharedLookupKeys = MakeSharedRange(std::move(lookupKeys), key);
        auto lookupReader = store->CreateReader(sharedLookupKeys, timestamp, TColumnFilter());

        lookupReader->Open()
            .Get()
            .ThrowOnError();

        std::vector<TVersionedRow> rows;
        rows.reserve(1);

        EXPECT_TRUE(lookupReader->Read(&rows));
        EXPECT_EQ(1, rows.size());
        auto row = rows.front();
        if (!row) {
            return TUnversionedOwningRow();
        }

        EXPECT_LE(row.GetWriteTimestampCount(), 1);
        EXPECT_LE(row.GetDeleteTimestampCount(), 1);
        if (row.GetWriteTimestampCount() == 0) {
            return TUnversionedOwningRow();
        }

        TUnversionedOwningRowBuilder builder;
        
        int keyCount = static_cast<int>(Tablet_->KeyColumns().size());
        int schemaColumnCount = static_cast<int>(Tablet_->Schema().Columns().size());

        // Keys
        const auto* keys = row.BeginKeys();
        for (int id = 0; id < keyCount; ++id) {
            builder.AddValue(keys[id]);
        }

        // Fixed values
        int versionedIndex = 0;
        for (int id = keyCount; id < schemaColumnCount; ++id) {
            if (versionedIndex < row.GetValueCount() && row.BeginValues()[versionedIndex].Id == id) {
                builder.AddValue(row.BeginValues()[versionedIndex++]);
            } else {
                builder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, id));
            }
        }

        return builder.FinishRow();
    }

    const TLockDescriptor& GetLock(TDynamicRow row, int index = TDynamicRow::PrimaryLockIndex)
    {
        return row.BeginLocks(Tablet_->KeyColumns().size())[index];
    }


    TTimestamp CurrentTimestamp_ = 10000; // some reasonable starting point
    TNameTablePtr NameTable_ = New<TNameTable>();
    std::unique_ptr<TTablet> Tablet_;

};

///////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NTabletNode
} // namespace NYT

