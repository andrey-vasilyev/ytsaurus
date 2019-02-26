#pragma once

#include "public.h"
#include "unversioned_writer.h"

#include <yt/client/api/table_reader.h>

#include <yt/core/concurrency/async_stream.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

IUnversionedWriterPtr CreateSchemalessFromApiWriterAdapter(
    NApi::ITableWriterPtr underlyingWriter);

NApi::ITableWriterPtr CreateApiFromSchemalessWriterAdapter(
    IUnversionedWriterPtr underlyingWriter);

////////////////////////////////////////////////////////////////////////////////

struct TPipeReaderToWriterOptions
{
    i64 BufferRowCount = 0;
    bool ValidateValues = false;
    NConcurrency::IThroughputThrottlerPtr Throttler;
    // Used only for testing.
    TDuration PipeDelay;
};

void PipeReaderToWriter(
    NApi::ITableReaderPtr reader,
    IUnversionedRowsetWriterPtr writer,
    const TPipeReaderToWriterOptions& options);

void PipeInputToOutput(
    IInputStream* input,
    IOutputStream* output,
    i64 bufferBlockSize);

void PipeInputToOutput(
    NConcurrency::IAsyncInputStreamPtr input,
    IOutputStream* output,
    i64 bufferBlockSize);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
