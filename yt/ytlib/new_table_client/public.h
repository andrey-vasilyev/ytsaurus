#pragma once

#include <core/misc/common.h>
#include <core/misc/enum.h>

#include <ytlib/transaction_client/public.h>

#include <ytlib/table_client/public.h>

namespace NYT {
namespace NVersionedTableClient {

////////////////////////////////////////////////////////////////////////////////

using NTransactionClient::TTimestamp;
using NTransactionClient::NullTimestamp;

using NTableClient::TKeyColumns;

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(ERowsetType,
    (Simple)
    (Versioned)   // With timestamps and tombstones
);

DECLARE_ENUM(EColumnType,
    (TheBottom)
    (Integer)
    (Double)
    (String)
    (Any)
    (Null)
);

struct TRowValue;
struct TRowHeader;
class TRow;
class TOwningRow;

struct TColumnSchema;
class TTableSchema;

class TNameTable;
typedef TIntrusivePtr<TNameTable> TNameTablePtr;

class TBlockWriter;

class TChunkWriter;
typedef TIntrusivePtr<TChunkWriter> TChunkWriterPtr;

struct IReader;
typedef TIntrusivePtr<IReader> IReaderPtr;

struct IWriter;
typedef TIntrusivePtr<IWriter> IWriterPtr;

class TChunkWriterConfig;
typedef TIntrusivePtr<TChunkWriterConfig> TChunkWriterConfigPtr;

class TChunkReaderConfig;
typedef TIntrusivePtr<TChunkReaderConfig> TChunkReaderConfigPtr;

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
